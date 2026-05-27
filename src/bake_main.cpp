// cooped_bake — headless distributed lightmap baker (native only). No window, no renderer.
//
//   Worker:       cooped_bake --worker [port]            (default port 27700)
//   Coordinator:  cooped_bake <map.cmap> [options]
//     --workers host:port,host:port   distribute across these workers (+ this machine)
//     --density N                     units per texel (default 32; smaller = finer/sharper)
//     --threads T                     threads this node uses (default = all cores)
//     --out path                      write to a different file (default: overwrite the map)
//
// Every node rebuilds the identical patch list + BVH from the scene, so only per-round radiosity
// slices cross the wire and the distributed result equals a local bake bit-for-bit. The coordinator
// writes the baked lightmap back into the CMAP; the dedicated server then loads and ships it.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "bake.h"
#include "bake_proto.h"
#include "brush.h"
#include "map_io.h"
#include "protocol.h"

namespace {

constexpr uint16_t kDefaultWorkerPort = 27700;

unsigned allCores() { const unsigned hw = std::thread::hardware_concurrency(); return hw ? hw : 1u; }

// ---- worker: serve a coordinator: build the solver on Job, gather a slice each Round ----------
int runWorker(uint16_t port) {
	if (enet_initialize() != 0) { fprintf(stderr, "enet_initialize failed\n"); return 1; }
	ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = port;
	ENetHost* host = enet_host_create(&addr, 1, 1, 0, 0);
	if (!host) { fprintf(stderr, "worker: listen failed on udp:%u\n", port); enet_deinitialize(); return 1; }
	printf("cooped_bake worker: listening on udp:%u (%u cores)\n", port, allCores());

	BakeSolver* solver = nullptr;
	uint32_t myLo = 0, myHi = 0;
	size_t P = 0;
	ENetEvent ev;
	while (enet_host_service(host, &ev, 1000) >= 0) {
		if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
			ByteReader r(ev.packet->data, ev.packet->dataLength);
			switch ((BakeMsg)r.u8()) {
				case BakeMsg::Job: {
					const float tpu = r.f32();
					myLo = r.u32(); myHi = r.u32();
					const uint32_t nb = r.u32();
					std::vector<Brush> brushes;
					for (uint32_t i = 0; i < nb && r.ok; ++i) brushes.push_back(readBrush(r));
					const uint32_t nl = r.u32();
					std::vector<Light> lights;
					for (uint32_t i = 0; i < nl && r.ok; ++i) lights.push_back(readLight(r));
					if (solver) { bakeDestroy(solver); solver = nullptr; }
					solver = r.ok ? bakeBuild(brushes, lights, tpu) : nullptr;
					P = bakeSolverPatchCount(solver);
					printf("worker: job range [%u,%u) of %zu patches\n", myLo, myHi, P);
					const std::vector<uint8_t> rdy = bakeMsgReady((uint32_t)P);
					enet_peer_send(ev.peer, 0, enet_packet_create(rdy.data(), rdy.size(), ENET_PACKET_FLAG_RELIABLE));
					break;
				}
				case BakeMsg::Round: {
					const uint32_t round = r.u32();
					const uint32_t n = r.u32();
					const uint8_t* fp = r.take((size_t)n * sizeof(float));
					if (solver && fp && n == (uint32_t)(P * 3)) {
						std::vector<float> rad(n);
						memcpy(rad.data(), fp, (size_t)n * sizeof(float));  // copy out (alignment)
						const uint32_t lo = (uint32_t)bx::min<size_t>(myLo, P), hi = (uint32_t)bx::min<size_t>(myHi, P);
						std::vector<float> g((size_t)(hi - lo) * 3, 0.0f);
						if (hi > lo) bakeGather(solver, rad.data(), P, lo, hi, g.data(), allCores());
						const std::vector<uint8_t> msg = bakeMsgPartial(round, lo, hi, g.data(), g.size());
						enet_peer_send(ev.peer, 0, enet_packet_create(msg.data(), msg.size(), ENET_PACKET_FLAG_RELIABLE));
					}
					break;
				}
				default: break;
			}
			enet_packet_destroy(ev.packet);
		} else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
			if (solver) { bakeDestroy(solver); solver = nullptr; }
			printf("worker: coordinator disconnected; ready for next job\n");
		}
	}
	if (solver) bakeDestroy(solver);
	enet_host_destroy(host);
	enet_deinitialize();
	return 0;
}

// ---- coordinator ------------------------------------------------------------------------------
struct WorkerLink {
	ENetPeer* peer = nullptr;
	uint32_t lo = 0, hi = 0;   // this worker's receiver range
	bool ready = false;        // handshook and agrees on patch count
	bool gotPartial = false;   // returned its slice this round
};

std::vector<std::pair<std::string, uint16_t>> parseWorkers(const std::string& list) {
	std::vector<std::pair<std::string, uint16_t>> out;
	size_t pos = 0;
	while (pos <= list.size()) {
		const size_t comma = list.find(',', pos);
		const std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		if (!item.empty()) {
			const size_t colon = item.find(':');
			if (colon != std::string::npos)
				out.push_back({item.substr(0, colon), (uint16_t)atoi(item.substr(colon + 1).c_str())});
		}
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	return out;
}

int runCoordinator(const char* mapPath, const char* outPath, const std::string& workerList,
                   float texelsPerUnit, unsigned threads) {
	std::vector<Brush> brushes;
	std::vector<Light> lights;
	LightmapData oldLm;
	uint32_t nextId = 1;
	if (!loadCmap(mapPath, brushes, lights, oldLm, nextId)) { fprintf(stderr, "can't load map '%s'\n", mapPath); return 1; }
	printf("loaded '%s': %zu brushes, %zu lights\n", mapPath, brushes.size(), lights.size());

	const auto t0 = std::chrono::steady_clock::now();
	BakeSolver* solver = bakeBuild(brushes, lights, texelsPerUnit);
	if (!solver) { fprintf(stderr, "bake build failed (empty world?)\n"); return 1; }
	const size_t P = bakeSolverPatchCount(solver);

	// --- connect to workers + assign contiguous patch ranges (node 0 = this coordinator) ---
	const auto addrs = parseWorkers(workerList);
	ENetHost* client = nullptr;
	std::vector<WorkerLink> workers;
	const size_t nodes = addrs.size() + 1;
	auto chunk = [&](size_t idx) { return (uint32_t)((P * idx) / nodes); };
	const uint32_t coordLo = chunk(0), coordHi = chunk(1);

	if (!addrs.empty()) {
		if (enet_initialize() != 0) { fprintf(stderr, "enet_initialize failed\n"); bakeDestroy(solver); return 1; }
		client = enet_host_create(nullptr, addrs.size(), 1, 0, 0);
		for (size_t k = 0; k < addrs.size(); ++k) {
			ENetAddress ea; enet_address_set_host(&ea, addrs[k].first.c_str()); ea.port = addrs[k].second;
			WorkerLink w;
			w.peer = enet_host_connect(client, &ea, 1, 0);
			w.lo = chunk(k + 1); w.hi = chunk(k + 2);
			workers.push_back(w);
		}
		// Wait for connections (best-effort).
		ENetEvent ev;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		size_t connected = 0;
		while (connected < workers.size() && std::chrono::steady_clock::now() < deadline)
			if (enet_host_service(client, &ev, 100) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) ++connected;
		// Send each connected worker its job, then await Ready (patch-count agreement).
		for (WorkerLink& w : workers)
			if (w.peer->state == ENET_PEER_STATE_CONNECTED) {
				const auto job = bakeMsgJob(brushes, lights, texelsPerUnit, w.lo, w.hi);
				enet_peer_send(w.peer, 0, enet_packet_create(job.data(), job.size(), ENET_PACKET_FLAG_RELIABLE));
			}
		enet_host_flush(client);
		size_t need = 0;
		for (const WorkerLink& w : workers) if (w.peer->state == ENET_PEER_STATE_CONNECTED) ++need;
		size_t got = 0;
		deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (got < need && std::chrono::steady_clock::now() < deadline) {
			if (enet_host_service(client, &ev, 100) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
				ByteReader r(ev.packet->data, ev.packet->dataLength);
				if ((BakeMsg)r.u8() == BakeMsg::Ready) {
					const uint32_t pc = r.u32();
					for (WorkerLink& w : workers)
						if (w.peer == ev.peer) {
							w.ready = (pc == (uint32_t)P);
							if (!w.ready) printf("worker patch mismatch (%u vs %zu): covering its range locally\n", pc, P);
							++got;
							break;
						}
				}
				enet_packet_destroy(ev.packet);
			}
		}
	}

	// --- bounce rounds: scatter radiosity, each node gathers its range, gather partials, sync ---
	std::vector<float> rad; bakeSolverInitRadiosity(solver, rad);
	const std::vector<float> direct = rad;
	for (uint32_t round = 0; round < (uint32_t)kBakeBounces; ++round) {
		std::vector<float> gathered(P * 3, 0.0f);
		for (WorkerLink& w : workers) {
			w.gotPartial = false;
			if (w.ready && w.hi > w.lo) {
				const auto msg = bakeMsgRound(round, rad);
				enet_peer_send(w.peer, 0, enet_packet_create(msg.data(), msg.size(), ENET_PACKET_FLAG_RELIABLE));
			}
		}
		if (client) enet_host_flush(client);

		// This node computes its own slice while the workers compute theirs.
		if (coordHi > coordLo) bakeGather(solver, rad.data(), P, coordLo, coordHi, gathered.data() + (size_t)coordLo * 3, threads);

		if (client) {
			size_t pending = 0;
			for (const WorkerLink& w : workers) if (w.ready && w.hi > w.lo) ++pending;
			ENetEvent ev;
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
			while (pending > 0 && std::chrono::steady_clock::now() < deadline) {
				if (enet_host_service(client, &ev, 100) > 0) {
					if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
						ByteReader r(ev.packet->data, ev.packet->dataLength);
						if ((BakeMsg)r.u8() == BakeMsg::Partial) {
							const uint32_t rd = r.u32(), lo = r.u32(), hi = r.u32(), n = r.u32();
							const uint8_t* fp = r.take((size_t)n * sizeof(float));
							if (rd == round && fp)
								for (WorkerLink& w : workers)
									if (w.peer == ev.peer && !w.gotPartial && lo == w.lo && hi == w.hi) {
										memcpy(gathered.data() + (size_t)lo * 3, fp, (size_t)n * sizeof(float));
										w.gotPartial = true; --pending;
										break;
									}
						}
						enet_packet_destroy(ev.packet);
					} else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
						for (WorkerLink& w : workers) if (w.peer == ev.peer) w.ready = false;
					}
				}
			}
			// Any worker that didn't deliver (slow/dead/mismatch): cover its range locally so the
			// result is always complete and identical to a single-machine bake.
			for (const WorkerLink& w : workers)
				if (w.hi > w.lo && !w.gotPartial) {
					printf("round %u: covering range [%u,%u) locally\n", round, w.lo, w.hi);
					bakeGather(solver, rad.data(), P, w.lo, w.hi, gathered.data() + (size_t)w.lo * 3, threads);
				}
		}
		for (size_t i = 0; i < P * 3; ++i) rad[i] = direct[i] + gathered[i];
	}

	// --- assemble + write the lightmap back into the map ---
	BakeResult res = bakeAssemble(solver, rad.data(), P);
	bakeDestroy(solver);
	LightmapData lm;
	lm.w = res.atlasWidth; lm.h = res.atlasHeight;
	lm.pixels = std::move(res.pixels); lm.vertexUV = std::move(res.vertexUV);
	const char* dst = outPath ? outPath : mapPath;
	const bool saved = saveCmap(dst, brushes, lights, lm, nextId);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (saved) printf("baked %zu patches across %zu node(s) in %.0f ms -> '%s' (%ux%u)\n", P, nodes, ms, dst, lm.w, lm.h);
	else fprintf(stderr, "save '%s' failed\n", dst);

	if (client) {
		for (WorkerLink& w : workers) if (w.peer) enet_peer_disconnect_now(w.peer, 0);
		enet_host_destroy(client);
		enet_deinitialize();
	}
	return saved ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc >= 2 && strcmp(argv[1], "--worker") == 0) {
		const uint16_t port = (argc > 2) ? (uint16_t)atoi(argv[2]) : kDefaultWorkerPort;
		return runWorker(port);
	}
	if (argc < 2) {
		fprintf(stderr,
		        "usage:\n"
		        "  %s --worker [port]\n"
		        "  %s <map.cmap> [--workers h:p,h:p] [--density N] [--threads T] [--out path]\n",
		        argv[0], argv[0]);
		return 2;
	}
	const char* mapPath = argv[1];
	const char* outPath = nullptr;
	std::string workers;
	float texelsPerUnit = 1.0f / 32.0f;
	unsigned threads = allCores();
	for (int i = 2; i < argc; ++i) {
		if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) workers = argv[++i];
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPath = argv[++i];
		else if (strcmp(argv[i], "--density") == 0 && i + 1 < argc) { const int n = atoi(argv[++i]); if (n > 0) texelsPerUnit = 1.0f / (float)n; }
		else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) { const int t = atoi(argv[++i]); if (t > 0) threads = (unsigned)t; }
		else fprintf(stderr, "ignoring unknown arg: %s\n", argv[i]);
	}
	return runCoordinator(mapPath, outPath, workers, texelsPerUnit, threads);
}
