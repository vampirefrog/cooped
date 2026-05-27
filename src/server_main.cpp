// cooped dedicated server (Milestone 5).
// Holds the authoritative brush map; sends a full snapshot on join; applies edit ops
// (assigning brush IDs for creates) and rebroadcasts them to every client.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <enet/enet.h>

#include "brush.h"
#include "protocol.h"

namespace {

constexpr uint16_t kDefaultPort = 27500;

uint32_t g_nextId = 1;

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

void broadcast(ENetHost* host, const std::vector<uint8_t>& msg) {
	ENetPacket* pkt = enet_packet_create(msg.data(), msg.size(), ENET_PACKET_FLAG_RELIABLE);
	enet_host_broadcast(host, 0, pkt);
}

// Apply an incoming op to the authoritative map and return the message to rebroadcast
// to all clients (empty if the op was invalid).
std::vector<uint8_t> applyOp(std::vector<Brush>& brushes, const uint8_t* data, size_t len) {
	ByteReader r(data, len);
	const MsgType type = (MsgType)r.u8();
	switch (type) {
		case MsgType::CreateBrush: {
			Brush b = readBrush(r);
			if (!r.ok) return {};
			b.id = g_nextId++;  // server assigns the authoritative id
			brushes.push_back(b);
			printf("create brush id=%u (%zu total)\n", b.id, brushes.size());
			return msgCreateBrush(b);
		}
		case MsgType::DeleteBrush: {
			const uint32_t id = r.u32();
			if (!r.ok) return {};
			for (size_t i = 0; i < brushes.size(); ++i)
				if (brushes[i].id == id) { brushes.erase(brushes.begin() + i); break; }
			printf("delete brush id=%u\n", id);
			return msgDeleteBrush(id);
		}
		case MsgType::TranslateBrush: {
			const uint32_t id = r.u32();
			const bx::Vec3 delta = r.vec3();
			if (!r.ok) return {};
			if (Brush* b = findBrush(brushes, id)) translateBrush(*b, delta);
			return msgTranslateBrush(id, delta);
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

}  // namespace

int main(int argc, char** argv) {
	const uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : kDefaultPort;

	if (enet_initialize() != 0) {
		fprintf(stderr, "enet_initialize failed\n");
		return 1;
	}
	atexit(enet_deinitialize);

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = port;
	ENetHost* server = enet_host_create(&addr, 16 /*clients*/, 2 /*channels*/, 0, 0);
	if (!server) {
		fprintf(stderr, "enet_host_create failed on port %u\n", port);
		return 1;
	}

	std::vector<Brush> brushes;
	buildScene(brushes);
	printf("cooped server listening on udp:%u  (%zu brushes)\n", port, brushes.size());

	for (;;) {
		ENetEvent ev;
		while (enet_host_service(server, &ev, 100) > 0) {
			switch (ev.type) {
				case ENET_EVENT_TYPE_CONNECT: {
					printf("client connected (%x:%u)\n", ev.peer->address.host, ev.peer->address.port);
					const std::vector<uint8_t> snap = msgSnapshot(brushes);
					ENetPacket* pkt = enet_packet_create(snap.data(), snap.size(), ENET_PACKET_FLAG_RELIABLE);
					enet_peer_send(ev.peer, 0, pkt);
					break;
				}
				case ENET_EVENT_TYPE_RECEIVE: {
					const std::vector<uint8_t> rebroadcast =
					    applyOp(brushes, ev.packet->data, ev.packet->dataLength);
					enet_packet_destroy(ev.packet);
					if (!rebroadcast.empty()) broadcast(server, rebroadcast);
					break;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
					printf("client disconnected\n");
					break;
				default:
					break;
			}
		}
	}
	// unreachable; enet_host_destroy(server);
}
