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

#include <cmath>

#include "brush.h"
#include "map_io.h"
#include "navmesh.h"
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

// --- server-driven AI agents that wander the Recast navmesh (broadcast like player presence) ---
NavMesh g_nav;
bool g_navDirty = false;          // geometry changed; rebuild the navmesh (throttled in the loop)
uint32_t g_nextAgentId = 1;
struct Agent {
	uint32_t id = 0;
	float pos[3] = {0, 0, 0};
	float yaw = 0.0f;
	std::vector<float> path;      // flattened xyz straight-path waypoints (cooped coords)
	size_t wp = 0;                // index of the current target waypoint
	uint32_t hp = 30;             // shot down at 0 -> respawn (kAgentHp)
};
std::vector<Agent> g_agents;
constexpr float kAgentSpeed = 90.0f;    // world units / second
constexpr float kWanderRadius = 450.0f; // how far an agent picks its next goal
constexpr uint32_t kAgentHp = 30;       // shots-to-kill * kShotDamage
constexpr uint32_t kShotDamage = 10;    // one hitscan shot
const bx::Vec3 kAgentHalf(12.0f, 12.0f, 18.0f);  // a touch wider than the avatar for forgiving aim
void resolveShot(const std::vector<Brush>& brushes, const bx::Vec3& origin, const bx::Vec3& dir);

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
	if ((MsgType)data[0] == MsgType::PlayerFire) {
		ByteReader r(data, len);
		r.u8();
		const bx::Vec3 o = r.vec3();
		const bx::Vec3 d = r.vec3();
		if (r.ok) resolveShot(brushes, o, d);
		return;
	}
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
		// Geometry edits also invalidate the navmesh (rebuilt, throttled, in the main loop).
		if (mt == MsgType::CreateBrush || mt == MsgType::DeleteBrush ||
		    mt == MsgType::TranslateBrush || mt == MsgType::SetPlaneD) g_navDirty = true;
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

// Give an agent a fresh random goal and a navmesh path to it (waypoint 0 is its start).
void agentPickGoal(Agent& a) {
	a.path.clear();
	a.wp = 0;
	float goal[3];
	if (!g_nav.randomPointAround(a.pos, kWanderRadius, goal)) return;
	std::vector<float> pts;
	if (g_nav.findPath(a.pos, goal, pts) && pts.size() >= 6) { a.path = std::move(pts); a.wp = 1; }
}

void spawnAgents(int n) {
	g_agents.clear();
	if (!g_nav.valid()) return;
	const float center[3] = {0, 0, 0};
	for (int i = 0; i < n; ++i) {
		float p[3];
		if (!g_nav.randomPointAround(center, 700.0f, p)) continue;
		Agent a;
		a.id = g_nextAgentId++;
		a.pos[0] = p[0]; a.pos[1] = p[1]; a.pos[2] = p[2];
		agentPickGoal(a);
		g_agents.push_back(std::move(a));
	}
}

// Teleport an agent to a fresh random navmesh point and reset its HP/path.
void respawnAgent(Agent& a) {
	const float center[3] = {0, 0, 0};
	float p[3];
	if (g_nav.randomPointAround(center, 700.0f, p)) { a.pos[0] = p[0]; a.pos[1] = p[1]; a.pos[2] = p[2]; }
	a.hp = kAgentHp;
	a.path.clear();
	a.wp = 0;
	agentPickGoal(a);
}

// Ray vs axis-aligned box (slab method). tHit is the entry distance along dir (>0 for ahead).
bool rayAabb(const bx::Vec3& o, const bx::Vec3& d, const bx::Vec3& cmin, const bx::Vec3& cmax, float& tHit) {
	const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
	const float lo[3] = {cmin.x, cmin.y, cmin.z}, hi[3] = {cmax.x, cmax.y, cmax.z};
	float tmin = 0.0f, tmax = 1.0e30f;
	for (int i = 0; i < 3; ++i) {
		if (std::fabs(dd[i]) < 1.0e-8f) { if (oo[i] < lo[i] || oo[i] > hi[i]) return false; continue; }
		float t1 = (lo[i] - oo[i]) / dd[i], t2 = (hi[i] - oo[i]) / dd[i];
		if (t1 > t2) std::swap(t1, t2);
		if (t1 > tmin) tmin = t1;
		if (t2 < tmax) tmax = t2;
		if (tmin > tmax) return false;
	}
	if (tmax < 0.0f) return false;
	tHit = (tmin >= 0.0f) ? tmin : tmax;
	return tHit > 0.0f;
}

// Server-authoritative hitscan resolution: pick the nearest agent the shot reaches before any
// brush occludes it; apply damage; respawn if killed. (Player-vs-player is intentionally skipped.)
void resolveShot(const std::vector<Brush>& brushes, const bx::Vec3& origin, const bx::Vec3& dir) {
	float tBrush = 1.0e30f;
	for (const Brush& b : brushes) {
		const RayHit h = rayBrushIntersect(origin, dir, b);
		if (h.hit && h.t > 0.0f && h.t < tBrush) tBrush = h.t;
	}
	int hitIdx = -1;
	float tAgent = 1.0e30f;
	for (size_t i = 0; i < g_agents.size(); ++i) {
		const Agent& a = g_agents[i];
		const bx::Vec3 c(a.pos[0], a.pos[1], a.pos[2] + kAgentHalf.z);
		const bx::Vec3 mn = bx::sub(c, kAgentHalf), mx = bx::add(c, kAgentHalf);
		float t;
		if (rayAabb(origin, dir, mn, mx, t) && t < tAgent) { tAgent = t; hitIdx = (int)i; }
	}
	if (hitIdx < 0 || tAgent >= tBrush) return;  // missed, or a brush is in the way
	Agent& a = g_agents[hitIdx];
	if (a.hp <= kShotDamage) { printf("agent %u killed; respawning\n", a.id); respawnAgent(a); }
	else { a.hp -= kShotDamage; printf("agent %u hit (hp %u)\n", a.id, a.hp); }
}

// Advance each agent along its path; pick a new goal when it arrives or its path runs out.
void stepAgents(float dt) {
	for (Agent& a : g_agents) {
		const size_t N = a.path.size() / 3;
		if (N < 2 || a.wp >= N) { agentPickGoal(a); continue; }
		const float* w = &a.path[a.wp * 3];
		const float dx = w[0] - a.pos[0], dy = w[1] - a.pos[1], dz = w[2] - a.pos[2];
		const float d = std::sqrt(dx * dx + dy * dy);
		if (d > 1.0e-3f) a.yaw = std::atan2(dy, dx);
		const float step = kAgentSpeed * dt;
		if (d <= step) { a.pos[0] = w[0]; a.pos[1] = w[1]; a.pos[2] = w[2]; ++a.wp; }
		else { a.pos[0] += dx / d * step; a.pos[1] += dy / d * step; a.pos[2] += dz * (step / d); }
	}
	// Soft separation: keep agents from overlapping each other and the players. Pure XY pushback
	// after movement — the agent yields, so the player can shove through, and agents don't stack.
	const float kAgentR = 12.0f, kPlayerR = 18.0f;
	const float minA = kAgentR * 2.0f, minP = kAgentR + kPlayerR;
	for (int it = 0; it < 2; ++it) {           // 2 relaxation passes is plenty for this density
		for (size_t i = 0; i < g_agents.size(); ++i) {
			for (size_t j = i + 1; j < g_agents.size(); ++j) {
				float dx = g_agents[j].pos[0] - g_agents[i].pos[0];
				float dy = g_agents[j].pos[1] - g_agents[i].pos[1];
				const float d2 = dx * dx + dy * dy;
				if (d2 < minA * minA && d2 > 1.0e-4f) {
					const float dist = std::sqrt(d2), push = (minA - dist) * 0.5f;
					dx /= dist; dy /= dist;
					g_agents[i].pos[0] -= dx * push; g_agents[i].pos[1] -= dy * push;
					g_agents[j].pos[0] += dx * push; g_agents[j].pos[1] += dy * push;
				}
			}
			for (const auto& kv : g_players) {
				if (!kv.second.hasState) continue;
				const bx::Vec3& p = kv.second.pos;
				float dx = g_agents[i].pos[0] - p.x, dy = g_agents[i].pos[1] - p.y;
				const float d2 = dx * dx + dy * dy;
				if (d2 < minP * minP && d2 > 1.0e-4f) {
					const float dist = std::sqrt(d2), push = (minP - dist);
					dx /= dist; dy /= dist;
					g_agents[i].pos[0] += dx * push; g_agents[i].pos[1] += dy * push;
				}
			}
		}
	}
}

std::vector<uint8_t> buildAgentStates() {
	ByteWriter w;
	w.u8((uint8_t)MsgType::AgentStates);
	w.u32((uint32_t)g_agents.size());
	for (const Agent& a : g_agents) {
		w.u32(a.id);
		w.f32(a.pos[0]); w.f32(a.pos[1]); w.f32(a.pos[2]);
		w.f32(a.yaw);
		w.u32(a.hp);
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
				if (g_nav.valid()) sendToChannel(m.dc, msgNavMesh(g_nav.debugTriangles()));
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

	if (g_nav.build(brushes)) { spawnAgents(5); printf("navmesh built; %zu AI agents wandering\n", g_agents.size()); }
	else printf("navmesh build failed (no walkable area?)\n");

	auto lastSave = std::chrono::steady_clock::now();
	auto lastStateBroadcast = std::chrono::steady_clock::now();
	auto lastNavBuild = std::chrono::steady_clock::now();
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
					if (g_nav.valid()) sendENet(ev.peer, msgNavMesh(g_nav.debugTriangles()));
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

		const auto now = std::chrono::steady_clock::now();

		// Rebuild the navmesh after geometry edits (throttled to once a second to survive drags).
		if (g_navDirty && std::chrono::duration_cast<std::chrono::seconds>(now - lastNavBuild).count() >= 1) {
			g_nav.build(brushes);
			g_navDirty = false;
			lastNavBuild = now;
			if (g_nav.valid()) broadcastAll(server, msgNavMesh(g_nav.debugTriangles()));  // refresh overlay
		}

		// Step the AI and broadcast everyone (players + agents) ~20 Hz so clients can render them.
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStateBroadcast).count() >= 50) {
			const float dt = std::chrono::duration<float>(now - lastStateBroadcast).count();
			stepAgents(dt);
			if (!g_players.empty()) broadcastAll(server, buildPlayerStates());
			if (!g_agents.empty()) broadcastAll(server, buildAgentStates());
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
