// cooped dedicated server (Milestones 5 + 7).
// Authoritative brush map. Speaks BOTH transports in one process (no gateway):
//   * ENet (UDP)            for native clients
//   * WebRTC DataChannels   for browser clients, with WebSocket signaling (libdatachannel)
// Edit ops from either transport are applied and rebroadcast to every client.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <enet/enet.h>
#include <rtc/rtc.hpp>

#include "brush.h"
#include "protocol.h"

namespace {

constexpr uint16_t kDefaultPort = 27500;  // ENet UDP; WebSocket signaling = this + 1

uint32_t g_nextId = 1;

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
std::vector<std::shared_ptr<rtc::DataChannel>> g_openChannels;   // for broadcast
std::queue<InMsg> g_inbound;

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
std::vector<uint8_t> applyOp(std::vector<Brush>& brushes, const uint8_t* data, size_t len) {
	ByteReader r(data, len);
	switch ((MsgType)r.u8()) {
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
		default:
			return {};
	}
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
void drainRtc(std::vector<Brush>& brushes, ENetHost* host) {
	std::queue<InMsg> local;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		std::swap(local, g_inbound);
	}
	while (!local.empty()) {
		InMsg m = std::move(local.front());
		local.pop();
		switch (m.kind) {
			case InKind::Join:
				printf("browser client joined (%zu brushes)\n", brushes.size());
				sendToChannel(m.dc, msgSnapshot(brushes));
				break;
			case InKind::Data: {
				const std::vector<uint8_t> rb = applyOp(brushes, m.bytes.data(), m.bytes.size());
				if (!rb.empty()) broadcastAll(host, rb);
				break;
			}
			case InKind::Leave:
				printf("browser client left\n");
				break;
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

	std::vector<Brush> brushes;
	buildScene(brushes);
	printf("cooped server: ENet udp:%u, WebRTC signaling %s:%u  (%zu brushes)\n", port,
	       wsCfg.enableTls ? "wss" : "ws", wsPort, brushes.size());

	for (;;) {
		ENetEvent ev;
		while (enet_host_service(server, &ev, 10) > 0) {
			switch (ev.type) {
				case ENET_EVENT_TYPE_CONNECT: {
					printf("native client connected\n");
					const std::vector<uint8_t> snap = msgSnapshot(brushes);
					ENetPacket* pkt = enet_packet_create(snap.data(), snap.size(), ENET_PACKET_FLAG_RELIABLE);
					enet_peer_send(ev.peer, 0, pkt);
					break;
				}
				case ENET_EVENT_TYPE_RECEIVE: {
					const std::vector<uint8_t> rb = applyOp(brushes, ev.packet->data, ev.packet->dataLength);
					enet_packet_destroy(ev.packet);
					if (!rb.empty()) broadcastAll(server, rb);
					break;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
					printf("native client disconnected\n");
					break;
				default:
					break;
			}
		}
		drainRtc(brushes, server);
	}
}
