// Wire protocol for the distributed lightmap bake (coordinator <-> headless workers, over ENet).
// Each node rebuilds the same patch list from the scene, so only the scene (once) and per-round
// radiosity slices travel the wire. Reuses the brush/light encoding from protocol.h.
#pragma once

#include <cstdint>
#include <vector>

#include "protocol.h"

enum class BakeMsg : uint8_t {
	Job     = 1,  // coordinator->worker: scene + density + this worker's receiver range [lo,hi)
	Ready   = 2,  // worker->coordinator: built ok, my patch count (coordinator checks agreement)
	Round   = 3,  // coordinator->worker: round index + full previous radiosity (3*P floats)
	Partial = 4,  // worker->coordinator: round index + [lo,hi) + gathered slice (3*(hi-lo) floats)
};

inline std::vector<uint8_t> bakeMsgJob(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                                       float texelsPerUnit, uint32_t lo, uint32_t hi) {
	ByteWriter w;
	w.u8((uint8_t)BakeMsg::Job);
	w.f32(texelsPerUnit);
	w.u32(lo); w.u32(hi);
	w.u32((uint32_t)brushes.size());
	for (const Brush& b : brushes) writeBrush(w, b);
	w.u32((uint32_t)lights.size());
	for (const Light& l : lights) writeLight(w, l);
	return w.data;
}

inline std::vector<uint8_t> bakeMsgReady(uint32_t patchCount) {
	ByteWriter w;
	w.u8((uint8_t)BakeMsg::Ready);
	w.u32(patchCount);
	return w.data;
}

inline std::vector<uint8_t> bakeMsgRound(uint32_t round, const std::vector<float>& rad) {
	ByteWriter w;
	w.u8((uint8_t)BakeMsg::Round);
	w.u32(round);
	w.u32((uint32_t)rad.size());
	w.bytes(rad.data(), rad.size() * sizeof(float));
	return w.data;
}

inline std::vector<uint8_t> bakeMsgPartial(uint32_t round, uint32_t lo, uint32_t hi,
                                           const float* g, size_t n) {
	ByteWriter w;
	w.u8((uint8_t)BakeMsg::Partial);
	w.u32(round);
	w.u32(lo); w.u32(hi);
	w.u32((uint32_t)n);
	w.bytes(g, n * sizeof(float));
	return w.data;
}
