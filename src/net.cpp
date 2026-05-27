#include "net.h"

#if !defined(__EMSCRIPTEN__)

#include <enet/enet.h>

struct NetClient {
	ENetHost* host = nullptr;
	ENetPeer* peer = nullptr;
	bool connected = false;
};

bool netGlobalInit() { return enet_initialize() == 0; }

NetClient* netConnect(const char* host, uint16_t port) {
	NetClient* c = new NetClient();
	c->host = enet_host_create(nullptr, 1, 2, 0, 0);  // client: 1 outgoing connection, 2 channels
	if (!c->host) { delete c; return nullptr; }
	ENetAddress addr;
	enet_address_set_host(&addr, host);
	addr.port = port;
	c->peer = enet_host_connect(c->host, &addr, 2, 0);
	if (!c->peer) { enet_host_destroy(c->host); delete c; return nullptr; }
	return c;
}

void netService(NetClient* c, std::vector<NetEvent>& out) {
	if (!c) return;
	ENetEvent ev;
	while (enet_host_service(c->host, &ev, 0) > 0) {
		switch (ev.type) {
			case ENET_EVENT_TYPE_CONNECT:
				c->connected = true;
				out.push_back({NetEvent::Connected, {}});
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				out.push_back({NetEvent::Data,
				               std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength)});
				enet_packet_destroy(ev.packet);
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				c->connected = false;
				out.push_back({NetEvent::Disconnected, {}});
				break;
			default:
				break;
		}
	}
}

void netSend(NetClient* c, const uint8_t* data, size_t len, bool reliable) {
	if (!c || !c->peer) return;
	ENetPacket* pkt = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	enet_peer_send(c->peer, reliable ? 0 : 1, pkt);
	enet_host_flush(c->host);
}

void netDisconnect(NetClient* c) {
	if (!c) return;
	if (c->peer) enet_peer_disconnect_now(c->peer, 0);
	if (c->host) enet_host_destroy(c->host);
	delete c;
}

bool netIsConnected(NetClient* c) { return c && c->connected; }

#else  // Emscripten: no UDP yet (WebRTC transport lands in Milestone 7)

bool netGlobalInit() { return true; }
NetClient* netConnect(const char*, uint16_t) { return nullptr; }
void netService(NetClient*, std::vector<NetEvent>&) {}
void netSend(NetClient*, const uint8_t*, size_t, bool) {}
void netDisconnect(NetClient*) {}
bool netIsConnected(NetClient*) { return false; }

#endif
