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

#else  // Emscripten: WebRTC DataChannel via the browser's RTCPeerConnection + WebSocket signaling.

#include <cstring>

#include <emscripten.h>

// JS glue. Server URL: ?server=<ws/wss url> if given; else ws://<host>:27501 ONLY on an http
// page (local dev). On https with no ?server=, stays single-player (a ws:// would be blocked as
// mixed content). Returns 1 if a connection was started, 0 otherwise. Never throws.
// Incoming datachannel messages are queued in Module.coopRx (array of Uint8Array); C drains them.
EM_JS(int, coop_js_connect, (), {
	var server = (new URLSearchParams(location.search)).get('server');
	var url;
	if (server) url = server;
	else if (location.protocol === 'http:') url = 'ws://' + (location.hostname || 'localhost') + ':27501';
	else return 0;  // https + no ?server= -> single-player (avoid insecure ws:// mixed content)
	try {
		Module.coopState = 1;  // 0=disconnected, 1=connecting, 2=connected
		Module.coopRx = [];
		var ws = new WebSocket(url);
		var pc = new RTCPeerConnection({iceServers: [{urls: 'stun:stun.l.google.com:19302'}]});
		var dc = pc.createDataChannel('data', {ordered: true});
		dc.binaryType = 'arraybuffer';
		Module.coopWS = ws; Module.coopPC = pc; Module.coopDC = dc;

		dc.onopen = function() { Module.coopState = 2; };
		dc.onclose = function() { Module.coopState = 0; };
		dc.onmessage = function(e) { Module.coopRx.push(new Uint8Array(e.data)); };
		pc.onicecandidate = function(e) {
			if (e.candidate && ws.readyState === 1)
				ws.send('CAND\n' + (e.candidate.sdpMid || '0') + '\n' + e.candidate.candidate);
		};
		ws.onopen = function() {
			pc.createOffer()
			  .then(function(o) { return pc.setLocalDescription(o); })
			  .then(function() { ws.send('offer\n' + pc.localDescription.sdp); });
		};
		ws.onmessage = function(ev) {
			var s = ev.data, nl = s.indexOf('\n'), type = s.substring(0, nl), rest = s.substring(nl + 1);
			if (type === 'answer') {
				pc.setRemoteDescription({type: 'answer', sdp: rest});
			} else if (type === 'CAND') {
				var nl2 = rest.indexOf('\n');
				pc.addIceCandidate({candidate: rest.substring(nl2 + 1), sdpMid: rest.substring(0, nl2)});
			}
		};
		ws.onerror = function() { if (Module.coopState !== 2) Module.coopState = 0; };
		ws.onclose = function() { if (Module.coopState !== 2) Module.coopState = 0; };
		return 1;
	} catch (e) {
		Module.coopState = 0;
		return 0;
	}
});
EM_JS(int, coop_js_state, (), { return Module.coopState || 0; });
EM_JS(int, coop_js_rxcount, (), { return Module.coopRx ? Module.coopRx.length : 0; });
EM_JS(int, coop_js_rxsize, (), { return (Module.coopRx && Module.coopRx.length) ? Module.coopRx[0].length : 0; });
EM_JS(void, coop_js_rxpop, (uint8_t* ptr), { HEAPU8.set(Module.coopRx.shift(), ptr); });
EM_JS(void, coop_js_send, (const uint8_t* ptr, int len), {
	if (Module.coopDC && Module.coopDC.readyState === 'open') Module.coopDC.send(HEAPU8.slice(ptr, ptr + len));
});

struct NetClient {
	bool reportedConnected = false;
	bool reportedDisconnected = false;
};

bool netGlobalInit() { return true; }

NetClient* netConnect(const char*, uint16_t) {  // URL resolved in JS (query param / default)
	if (coop_js_connect() == 0) return nullptr;  // no server configured -> single-player
	return new NetClient();
}

void netService(NetClient* c, std::vector<NetEvent>& out) {
	if (!c) return;
	const int st = coop_js_state();
	if (st == 2 && !c->reportedConnected) {
		c->reportedConnected = true;
		out.push_back({NetEvent::Connected, {}});
	}
	if (st == 0 && c->reportedConnected && !c->reportedDisconnected) {
		c->reportedDisconnected = true;
		out.push_back({NetEvent::Disconnected, {}});
	}
	while (coop_js_rxcount() > 0) {
		const int n = coop_js_rxsize();
		std::vector<uint8_t> buf(n > 0 ? n : 1);
		coop_js_rxpop(buf.data());
		if (n > 0) { buf.resize(n); out.push_back({NetEvent::Data, std::move(buf)}); }
	}
}

void netSend(NetClient*, const uint8_t* data, size_t len, bool) { coop_js_send(data, (int)len); }
void netDisconnect(NetClient* c) { delete c; }
bool netIsConnected(NetClient* c) { return c && coop_js_state() == 2; }

#endif
