// Client-side network transport (ENet, native only). On Emscripten these are stubs
// so the web build compiles as single-player until the WebRTC transport (Milestone 7).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct NetClient;  // opaque

struct NetEvent {
	enum Type { Connected, Disconnected, Data } type;
	std::vector<uint8_t> data;  // for Data
};

bool netGlobalInit();
// Connect to host:port. Returns null on failure or on Emscripten.
NetClient* netConnect(const char* host, uint16_t port);
// Poll the connection; appends connect/disconnect/data events.
void netService(NetClient* c, std::vector<NetEvent>& out);
void netSend(NetClient* c, const uint8_t* data, size_t len, bool reliable);
void netDisconnect(NetClient* c);
bool netIsConnected(NetClient* c);
