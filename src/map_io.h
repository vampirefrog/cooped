// Shared CMAP (cooped map) binary IO, used by the dedicated server and the headless bake tool.
// CMAP v3 layout: magic 'CMAP', u32 version, u32 nextId, brushes, lights, u8 hasLightmap, [lightmap].
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "protocol.h"

inline bool saveCmap(const char* path, const std::vector<Brush>& brushes,
                     const std::vector<Light>& lights, const LightmapData& lm, uint32_t nextId) {
	ByteWriter w;
	w.u8('C'); w.u8('M'); w.u8('A'); w.u8('P');
	w.u32(3);
	w.u32(nextId);
	w.u32((uint32_t)brushes.size());
	for (const Brush& b : brushes) writeBrush(w, b);
	w.u32((uint32_t)lights.size());
	for (const Light& l : lights) writeLight(w, l);
	const bool hasLm = lm.valid();
	w.u8(hasLm ? 1 : 0);
	if (hasLm) writeLightmap(w, lm);
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(w.data.data(), 1, w.data.size(), f) == w.data.size();
	fclose(f);
	return ok;
}

inline bool loadCmap(const char* path, std::vector<Brush>& brushes, std::vector<Light>& lights,
                     LightmapData& lm, uint32_t& nextId) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> buf(size > 0 ? (size_t)size : 0);
	const size_t got = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
	fclose(f);
	if (got != buf.size() || buf.size() < 16) return false;
	ByteReader r(buf.data(), buf.size());
	if (r.u8() != 'C' || r.u8() != 'M' || r.u8() != 'A' || r.u8() != 'P') return false;
	const uint32_t version = r.u32();
	nextId = r.u32();
	const uint32_t n = r.u32();
	std::vector<Brush> loaded;
	for (uint32_t i = 0; i < n && r.ok; ++i) loaded.push_back(readBrush(r));
	if (!r.ok || loaded.size() != n) return false;
	const uint32_t ln = r.u32();
	std::vector<Light> loadedLights;
	for (uint32_t i = 0; i < ln && r.ok; ++i) loadedLights.push_back(readLight(r));
	if (!r.ok || loadedLights.size() != ln) return false;
	if (version >= 3 && r.u8() == 1) {  // optional baked lightmap
		LightmapData got2 = readLightmap(r);
		if (r.ok && got2.valid()) lm = std::move(got2);
	}
	brushes = std::move(loaded);
	lights = std::move(loadedLights);
	return true;
}
