// Wire protocol for collaborative editing (Milestone 5).
// Server is authoritative: it owns brush IDs, applies ops, and rebroadcasts.
// Little-endian raw encoding (x86 + wasm are both LE). Header-only so the client
// and the dedicated server share it without an extra translation unit.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "brush.h"

// Bump on any wire-format change. The server sends it in AssignId; the client compares and
// surfaces a clear mismatch instead of silently rendering a garbled map.
constexpr uint32_t kProtocolVersion = 4;

enum class MsgType : uint8_t {
	Snapshot       = 1,  // server->client: full map
	CreateBrush    = 2,  // client->server: geometry (id=0); server->clients: with assigned id
	DeleteBrush    = 3,  // id
	TranslateBrush = 4,  // id + delta
	SetPlaneD      = 5,  // id + faceIndex + new plane distance (push/pull result)
	AssignId       = 6,  // server->client: your player id
	PlayerState    = 7,  // client->server: my pos + yaw + pitch
	PlayerStates   = 8,  // server->clients: [count]{id,pos,yaw,pitch} of everyone
	SetFaceTexture = 9,  // brush id + face index + texture index
	SetFaceUV      = 10, // brush id + face index + uScale,vScale,uOffset,vOffset,rotation
	CreateLight    = 11, // client: pos+color+radius (id=0); server: with assigned id
	DeleteLight    = 12, // light id
};

struct ByteWriter {
	std::vector<uint8_t> data;
	void u8(uint8_t v) { data.push_back(v); }
	void u32(uint32_t v) { const uint8_t* p = (const uint8_t*)&v; data.insert(data.end(), p, p + 4); }
	void f32(float v) { const uint8_t* p = (const uint8_t*)&v; data.insert(data.end(), p, p + 4); }
	void vec3(const bx::Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
};

struct ByteReader {
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;
	ByteReader(const uint8_t* d, size_t n) : p(d), end(d + n) {}
	uint8_t u8() { if (p + 1 > end) { ok = false; return 0; } return *p++; }
	uint32_t u32() { uint32_t v = 0; if (p + 4 > end) { ok = false; return 0; } memcpy(&v, p, 4); p += 4; return v; }
	float f32() { float v = 0; if (p + 4 > end) { ok = false; return 0; } memcpy(&v, p, 4); p += 4; return v; }
	bx::Vec3 vec3() { const float x = f32(), y = f32(), z = f32(); return bx::Vec3(x, y, z); }
};

inline void writeBrush(ByteWriter& w, const Brush& b) {
	w.u32(b.id);
	w.f32(b.color[0]); w.f32(b.color[1]); w.f32(b.color[2]);
	w.u32((uint32_t)b.planes.size());
	for (const Plane& pl : b.planes) {
		w.vec3(pl.n); w.f32(pl.d); w.u32(pl.textureId);
		w.f32(pl.uScale); w.f32(pl.vScale); w.f32(pl.uOffset); w.f32(pl.vOffset); w.f32(pl.rotation);
	}
}

inline Brush readBrush(ByteReader& r) {
	Brush b;
	b.id = r.u32();
	b.color[0] = r.f32(); b.color[1] = r.f32(); b.color[2] = r.f32();
	const uint32_t n = r.u32();
	for (uint32_t i = 0; i < n && r.ok; ++i) {
		const bx::Vec3 nrm = r.vec3();
		const float d = r.f32();
		Plane pl(nrm, d);
		pl.textureId = r.u32();
		pl.uScale = r.f32(); pl.vScale = r.f32();
		pl.uOffset = r.f32(); pl.vOffset = r.f32();
		pl.rotation = r.f32();
		b.planes.push_back(pl);
	}
	return b;
}

inline void writeLight(ByteWriter& w, const Light& l) {
	w.u32(l.id);
	w.vec3(l.pos);
	w.f32(l.color[0]); w.f32(l.color[1]); w.f32(l.color[2]);
	w.f32(l.radius);
}

inline Light readLight(ByteReader& r) {
	Light l;
	l.id = r.u32();
	l.pos = r.vec3();
	l.color[0] = r.f32(); l.color[1] = r.f32(); l.color[2] = r.f32();
	l.radius = r.f32();
	return l;
}

inline std::vector<uint8_t> msgSnapshot(const std::vector<Brush>& brushes,
                                        const std::vector<Light>& lights) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::Snapshot);
	w.u32((uint32_t)brushes.size());
	for (const Brush& b : brushes) writeBrush(w, b);
	w.u32((uint32_t)lights.size());
	for (const Light& l : lights) writeLight(w, l);
	return w.data;
}

inline std::vector<uint8_t> msgCreateLight(const Light& l) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::CreateLight);
	writeLight(w, l);
	return w.data;
}

inline std::vector<uint8_t> msgDeleteLight(uint32_t id) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::DeleteLight);
	w.u32(id);
	return w.data;
}

inline std::vector<uint8_t> msgCreateBrush(const Brush& b) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::CreateBrush);
	writeBrush(w, b);
	return w.data;
}

inline std::vector<uint8_t> msgDeleteBrush(uint32_t id) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::DeleteBrush);
	w.u32(id);
	return w.data;
}

inline std::vector<uint8_t> msgTranslateBrush(uint32_t id, const bx::Vec3& delta) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::TranslateBrush);
	w.u32(id);
	w.vec3(delta);
	return w.data;
}

inline std::vector<uint8_t> msgSetPlaneD(uint32_t id, uint32_t face, float d) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::SetPlaneD);
	w.u32(id);
	w.u32(face);
	w.f32(d);
	return w.data;
}

inline std::vector<uint8_t> msgAssignId(uint32_t id, uint32_t protocolVersion) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::AssignId);
	w.u32(id);
	w.u32(protocolVersion);
	return w.data;
}

inline std::vector<uint8_t> msgPlayerState(const bx::Vec3& pos, float yaw, float pitch) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::PlayerState);
	w.vec3(pos);
	w.f32(yaw);
	w.f32(pitch);
	return w.data;
}

inline std::vector<uint8_t> msgSetFaceTexture(uint32_t id, uint32_t face, uint32_t textureId) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::SetFaceTexture);
	w.u32(id);
	w.u32(face);
	w.u32(textureId);
	return w.data;
}

inline std::vector<uint8_t> msgSetFaceUV(uint32_t id, uint32_t face, float uScale, float vScale,
                                         float uOffset, float vOffset, float rotation) {
	ByteWriter w;
	w.u8((uint8_t)MsgType::SetFaceUV);
	w.u32(id);
	w.u32(face);
	w.f32(uScale); w.f32(vScale); w.f32(uOffset); w.f32(vOffset); w.f32(rotation);
	return w.data;
}
