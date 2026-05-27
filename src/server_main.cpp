// cooped dedicated server (Milestones 5 + 7).
// Authoritative brush map. Speaks BOTH transports in one process (no gateway):
//   * ENet (UDP)            for native clients
//   * WebRTC DataChannels   for browser clients, with WebSocket signaling (libdatachannel)
// Edit ops from either transport are applied and rebroadcast to every client.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <enet/enet.h>
#include <rtc/rtc.hpp>

#include "brush.h"
#include "map_io.h"
#include "protocol.h"

namespace {

constexpr uint16_t kDefaultPort = 27500;  // ENet UDP; WebSocket signaling = this + 1

uint32_t g_nextId = 1;
LightmapData g_lightmap;                     // latest bake: shipped to joiners, persisted, cleared on edits
bool g_dirty = false;                       // map changed since last save
volatile std::sig_atomic_t g_running = 1;   // cleared by SIGINT/SIGTERM for a clean save-on-exit

void onSignal(int) { g_running = 0; }

// Persist / restore the authoritative map (shared CMAP IO; carries the baked lightmap too).
bool saveMap(const char* path, const std::vector<Brush>& brushes, const std::vector<Light>& lights) {
	return saveCmap(path, brushes, lights, g_lightmap, g_nextId);
}

bool loadMap(const char* path, std::vector<Brush>& brushes, std::vector<Light>& lights) {
	return loadCmap(path, brushes, lights, g_lightmap, g_nextId);
}

// --- WebRTC peer registry + a thread-safe inbound queue (libdatachannel callbacks run on
//     their own threads; the main loop drains the queue and owns the brush map). -----------
struct RtcPeer {
	std::shared_ptr<rtc::WebSocket> ws;
	std::shared_ptr<rtc::PeerConnection> pc;
};

enum class InKind { Join, Data, Leave };
struct InMsg {
	InKind kind;
	std::shared_ptr<rtc::DataChannel> dc;
	std::vector<uint8_t> bytes;
};

std::mutex g_mtx;
std::vector<std::shared_ptr<RtcPeer>> g_peers;                   // keeps ws/pc alive
std::vector<std::shared_ptr<rtc::DataChannel>> g_openChannels;   // for broadcast (guarded by g_mtx)
std::queue<InMsg> g_inbound;

// Player presence (all touched on the main thread only — ENet handlers and drainRtc run there).
struct PlayerInfo {
	uint32_t id = 0;
	bx::Vec3 pos = bx::Vec3(0, 0, 0);
	float yaw = 0.0f, pitch = 0.0f;
	bool hasState = false;
};
uint32_t g_nextPlayerId = 1;
std::map<uint32_t, PlayerInfo> g_players;              // id -> latest state
std::map<rtc::DataChannel*, uint32_t> g_chanId;        // WebRTC channel -> player id

Brush* findBrush(std::vector<Brush>& brushes, uint32_t id) {
	for (Brush& b : brushes) if (b.id == id) return &b;
	return nullptr;
}

void buildScene(std::vector<Brush>& brushes) {
	auto add = [&](Brush b) { b.id = g_nextId++; brushes.push_back(std::move(b)); };
	add(makeBox({0, 0, -8}, {512, 512, 8}, 0.45f, 0.47f, 0.50f));
	add(makeBox({0, 0, 32}, {32, 32, 32}, 0.80f, 0.30f, 0.25f));
	add(makeBox({128, 0, 24}, {24, 48, 24}, 0.30f, 0.65f, 0.35f));
}

void sendToChannel(const std::shared_ptr<rtc::DataChannel>& dc, const std::vector<uint8_t>& msg) {
	if (dc && dc->isOpen()) dc->send(reinterpret_cast<const std::byte*>(msg.data()), msg.size());
}

// Send to every client on both transports.
void broadcastAll(ENetHost* host, const std::vector<uint8_t>& msg) {
	ENetPacket* pkt = enet_packet_create(msg.data(), msg.size(), ENET_PACKET_FLAG_RELIABLE);
	enet_host_broadcast(host, 0, pkt);
	std::lock_guard<std::mutex> lk(g_mtx);
	for (const auto& dc : g_openChannels) sendToChannel(dc, msg);
}

// Apply an op to the authoritative map; returns the message to rebroadcast (empty if invalid).
std::vector<uint8_t> applyOp(std::vector<Brush>& brushes, std::vector<Light>& lights,
                             const uint8_t* data, size_t len) {
	ByteReader r(data, len);
	switch ((MsgType)r.u8()) {
		case MsgType::CreateLight: {
			Light l = readLight(r);
			if (!r.ok) return {};
			l.id = g_nextId++;
			lights.push_back(l);
			return msgCreateLight(l);
		}
		case MsgType::DeleteLight: {
			const uint32_t id = r.u32();
			if (!r.ok) return {};
			for (size_t i = 0; i < lights.size(); ++i)
				if (lights[i].id == id) { lights.erase(lights.begin() + i); break; }
			return msgDeleteLight(id);
		}
		case MsgType::SetLightPos: {
			const uint32_t id = r.u32();
			const bx::Vec3 pos = r.vec3();
			if (!r.ok) return {};
			for (Light& l : lights) if (l.id == id) { l.pos = pos; break; }
			return msgSetLightPos(id, pos);
		}
		case MsgType::CreateBrush: {
			Brush b = readBrush(r);
			if (!r.ok) return {};
			b.id = g_nextId++;
			brushes.push_back(b);
			return msgCreateBrush(b);
		}
		case MsgType::DeleteBrush: {
			const uint32_t id = r.u32();
			if (!r.ok) return {};
			for (size_t i = 0; i < brushes.size(); ++i)
				if (brushes[i].id == id) { brushes.erase(brushes.begin() + i); break; }
			return msgDeleteBrush(id);
		}
		case MsgType::TranslateBrush: {
			const uint32_t id = r.u32();
			const bx::Vec3 d = r.vec3();
			if (!r.ok) return {};
			if (Brush* b = findBrush(brushes, id)) translateBrush(*b, d);
			return msgTranslateBrush(id, d);
		}
		case MsgType::SetPlaneD: {
			const uint32_t id = r.u32();
			const uint32_t face = r.u32();
			const float d = r.f32();
			if (!r.ok) return {};
			if (Brush* b = findBrush(brushes, id))
				if (face < b->planes.size()) b->planes[face].d = d;
			return msgSetPlaneD(id, face, d);
		}
		case MsgType::SetFaceTexture: {
			const uint32_t id = r.u32();
			const uint32_t face = r.u32();
			const uint32_t texId = r.u32();
			if (!r.ok) return {};
			if (Brush* b = findBrush(brushes, id))
				if (face < b->planes.size()) b->planes[face].textureId = texId;
			return msgSetFaceTexture(id, face, texId);
		}
		case MsgType::SetFaceUV: {
			const uint32_t id = r.u32();
			const uint32_t face = r.u32();
			const float us = r.f32(), vs = r.f32(), uo = r.f32(), vo = r.f32(), rot = r.f32();
			if (!r.ok) return {};
			if (Brush* b = findBrush(brushes, id))
				if (face < b->planes.size()) {
					Plane& pl = b->planes[face];
					pl.uScale = us; pl.vScale = vs; pl.uOffset = uo; pl.vOffset = vo; pl.rotation = rot;
				}
			return msgSetFaceUV(id, face, us, vs, uo, vo, rot);
		}
		case MsgType::Lightmap: {
			LightmapData lm = readLightmap(r);
			if (!r.ok || !lm.valid()) return {};
			g_lightmap = std::move(lm);
			return msgLightmap(g_lightmap);  // rebroadcast to everyone (and persist via g_dirty)
		}
		default:
			return {};
	}
}

void sendENet(ENetPeer* peer, const std::vector<uint8_t>& msg) {
	enet_peer_send(peer, 0, enet_packet_create(msg.data(), msg.size(), ENET_PACKET_FLAG_RELIABLE));
}

// Dispatch a message from a client: player-state updates the presence map; everything else is
// an edit op (applied + rebroadcast). `senderId` is the sender's assigned player id.
void handleClientMessage(std::vector<Brush>& brushes, std::vector<Light>& lights, uint32_t senderId,
                         const uint8_t* data, size_t len, ENetHost* host) {
	if (len < 1) return;
	if ((MsgType)data[0] == MsgType::PlayerState) {
		ByteReader r(data, len);
		r.u8();
		const bx::Vec3 p = r.vec3();
		const float yaw = r.f32(), pitch = r.f32();
		if (!r.ok) return;
		auto it = g_players.find(senderId);
		if (it != g_players.end()) {
			it->second.pos = p;
			it->second.yaw = yaw;
			it->second.pitch = pitch;
			it->second.hasState = true;
		}
		return;
	}
	const std::vector<uint8_t> rb = applyOp(brushes, lights, data, len);
	if (!rb.empty()) {
		broadcastAll(host, rb);
		g_dirty = true;
		// Any geometry/light change makes the stored bake stale; texture swaps don't affect it.
		const MsgType mt = (MsgType)data[0];
		if (mt != MsgType::Lightmap && mt != MsgType::SetFaceTexture) g_lightmap = LightmapData{};
	}
}

// Build the PlayerStates broadcast from everyone who has reported a position.
std::vector<uint8_t> buildPlayerStates() {
	ByteWriter w;
	w.u8((uint8_t)MsgType::PlayerStates);
	uint32_t count = 0;
	for (const auto& kv : g_players) if (kv.second.hasState) ++count;
	w.u32(count);
	for (const auto& kv : g_players) {
		const PlayerInfo& p = kv.second;
		if (!p.hasState) continue;
		w.u32(p.id);
		w.vec3(p.pos);
		w.f32(p.yaw);
		w.f32(p.pitch);
	}
	return w.data;
}

// Parse a signaling line from a browser: "offer\n<sdp>" or "CAND\n<mid>\n<candidate>".
void handleSignaling(const std::shared_ptr<rtc::PeerConnection>& pc, const std::string& s) {
	const size_t nl = s.find('\n');
	if (nl == std::string::npos) return;
	const std::string type = s.substr(0, nl);
	const std::string rest = s.substr(nl + 1);
	if (type == "offer") {
		pc->setRemoteDescription(rtc::Description(rest, "offer"));
	} else if (type == "CAND") {
		const size_t nl2 = rest.find('\n');
		if (nl2 == std::string::npos) return;
		pc->addRemoteCandidate(rtc::Candidate(rest.substr(nl2 + 1), rest.substr(0, nl2)));
	}
}

void onWebSocketClient(std::shared_ptr<rtc::WebSocket> ws) {
	auto peer = std::make_shared<RtcPeer>();
	peer->ws = ws;

	rtc::Configuration cfg;
	cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
	peer->pc = std::make_shared<rtc::PeerConnection>(cfg);

	std::weak_ptr<rtc::WebSocket> wsw = ws;
	peer->pc->onLocalDescription([wsw](rtc::Description desc) {
		if (auto w = wsw.lock()) w->send(std::string(desc.typeString()) + "\n" + std::string(desc));
	});
	peer->pc->onLocalCandidate([wsw](rtc::Candidate cand) {
		if (auto w = wsw.lock()) w->send("CAND\n" + cand.mid() + "\n" + cand.candidate());
	});
	peer->pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
		dc->onOpen([dc]() {
			std::lock_guard<std::mutex> lk(g_mtx);
			g_openChannels.push_back(dc);
			g_inbound.push({InKind::Join, dc, {}});
		});
		dc->onMessage([dc](rtc::message_variant msg) {
			if (!std::holds_alternative<rtc::binary>(msg)) return;
			const rtc::binary& b = std::get<rtc::binary>(msg);
			std::vector<uint8_t> bytes(b.size());
			if (!b.empty()) memcpy(bytes.data(), b.data(), b.size());
			std::lock_guard<std::mutex> lk(g_mtx);
			g_inbound.push({InKind::Data, dc, std::move(bytes)});
		});
		dc->onClosed([dc]() {
			std::lock_guard<std::mutex> lk(g_mtx);
			for (size_t i = 0; i < g_openChannels.size(); ++i)
				if (g_openChannels[i] == dc) { g_openChannels.erase(g_openChannels.begin() + i); break; }
			g_inbound.push({InKind::Leave, dc, {}});
		});
	});

	auto pcw = std::weak_ptr<rtc::PeerConnection>(peer->pc);
	ws->onMessage([pcw](rtc::message_variant msg) {
		if (std::holds_alternative<std::string>(msg))
			if (auto pc = pcw.lock()) handleSignaling(pc, std::get<std::string>(msg));
	});
	ws->onClosed([peer]() {
		std::lock_guard<std::mutex> lk(g_mtx);
		for (size_t i = 0; i < g_peers.size(); ++i)
			if (g_peers[i] == peer) { g_peers.erase(g_peers.begin() + i); break; }
	});

	std::lock_guard<std::mutex> lk(g_mtx);
	g_peers.push_back(peer);
}

// Drain WebRTC inbound events on the main thread (which owns `brushes`).
void drainRtc(std::vector<Brush>& brushes, std::vector<Light>& lights, ENetHost* host) {
	std::queue<InMsg> local;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		std::swap(local, g_inbound);
	}
	while (!local.empty()) {
		InMsg m = std::move(local.front());
		local.pop();
		switch (m.kind) {
			case InKind::Join: {
				const uint32_t pid = g_nextPlayerId++;
				g_chanId[m.dc.get()] = pid;
				g_players[pid] = PlayerInfo{pid};
				printf("browser client joined as player %u (%zu brushes)\n", pid, brushes.size());
				sendToChannel(m.dc, msgAssignId(pid, kProtocolVersion));
				sendToChannel(m.dc, msgSnapshot(brushes, lights));
				if (g_lightmap.valid()) sendToChannel(m.dc, msgLightmap(g_lightmap));
				break;
			}
			case InKind::Data: {
				auto it = g_chanId.find(m.dc.get());
				handleClientMessage(brushes, lights, it != g_chanId.end() ? it->second : 0,
				                    m.bytes.data(), m.bytes.size(), host);
				break;
			}
			case InKind::Leave: {
				auto it = g_chanId.find(m.dc.get());
				if (it != g_chanId.end()) { g_players.erase(it->second); g_chanId.erase(it); }
				printf("browser client left\n");
				break;
			}
		}
	}
}

}  // namespace

int main(int argc, char** argv) {
	// Usage: cooped_server [port] [cert.pem key.pem]
	// With a cert+key, signaling is wss:// (TLS) so an https page (e.g. GitHub Pages) can connect.
	const uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : kDefaultPort;
	const uint16_t wsPort = port + 1;
	const char* certPem = (argc > 3) ? argv[2] : nullptr;
	const char* keyPem = (argc > 3) ? argv[3] : nullptr;

	if (enet_initialize() != 0) { fprintf(stderr, "enet_initialize failed\n"); return 1; }
	atexit(enet_deinitialize);

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = port;
	ENetHost* server = enet_host_create(&addr, 32, 2, 0, 0);
	if (!server) { fprintf(stderr, "enet_host_create failed on port %u\n", port); return 1; }

	rtc::InitLogger(rtc::LogLevel::Warning);
	rtc::WebSocketServerConfiguration wsCfg;
	wsCfg.port = wsPort;
	if (certPem && keyPem) {
		wsCfg.enableTls = true;
		wsCfg.certificatePemFile = certPem;
		wsCfg.keyPemFile = keyPem;
	}
	rtc::WebSocketServer wsServer(wsCfg);
	wsServer.onClient(onWebSocketClient);

	const char* mapPath = getenv("COOPED_MAP");
	if (!mapPath) mapPath = "cooped.map";

	std::vector<Brush> brushes;
	std::vector<Light> lights;
	if (loadMap(mapPath, brushes, lights))
		printf("loaded map '%s' (%zu brushes, %zu lights)\n", mapPath, brushes.size(), lights.size());
	else
		{ buildScene(brushes); g_dirty = true; }  // persist the fresh sandbox so tools can bake it

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	printf("cooped server: ENet udp:%u, WebRTC signaling %s:%u  (%zu brushes, map='%s')\n", port,
	       wsCfg.enableTls ? "wss" : "ws", wsPort, brushes.size(), mapPath);

	auto lastSave = std::chrono::steady_clock::now();
	auto lastStateBroadcast = std::chrono::steady_clock::now();
	while (g_running) {
		ENetEvent ev;
		while (enet_host_service(server, &ev, 10) > 0) {
			switch (ev.type) {
				case ENET_EVENT_TYPE_CONNECT: {
					const uint32_t pid = g_nextPlayerId++;
					ev.peer->data = (void*)(uintptr_t)pid;
					g_players[pid] = PlayerInfo{pid};
					printf("native client connected as player %u\n", pid);
					sendENet(ev.peer, msgAssignId(pid, kProtocolVersion));
					sendENet(ev.peer, msgSnapshot(brushes, lights));
					if (g_lightmap.valid()) sendENet(ev.peer, msgLightmap(g_lightmap));
					break;
				}
				case ENET_EVENT_TYPE_RECEIVE: {
					const uint32_t pid = (uint32_t)(uintptr_t)ev.peer->data;
					handleClientMessage(brushes, lights, pid, ev.packet->data, ev.packet->dataLength, server);
					enet_packet_destroy(ev.packet);
					break;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
					g_players.erase((uint32_t)(uintptr_t)ev.peer->data);
					printf("native client disconnected\n");
					break;
				default:
					break;
			}
		}
		drainRtc(brushes, lights, server);

		// Broadcast everyone's position ~20 Hz so clients can render each other.
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStateBroadcast).count() >= 50) {
			if (!g_players.empty()) broadcastAll(server, buildPlayerStates());
			lastStateBroadcast = now;
		}

		// Autosave at most every 5s when the map has changed.
		if (g_dirty && std::chrono::duration_cast<std::chrono::seconds>(now - lastSave).count() >= 5) {
			if (saveMap(mapPath, brushes, lights)) { g_dirty = false; printf("autosaved '%s'\n", mapPath); }
			lastSave = now;
		}
	}

	if (g_dirty) printf(saveMap(mapPath, brushes, lights) ? "saved '%s' on exit\n" : "save failed\n", mapPath);
	enet_host_destroy(server);
	return 0;
}
