// GI lightmap bake (native only; the bake uses xatlas + later Embree/radiosity).
// On Emscripten this is a stub — bakes run on native machines (Embree doesn't target wasm).
#pragma once

#include <vector>

#include "brush.h"

struct BakeResult {
	bool ok = false;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	uint32_t chartCount = 0;
	std::vector<uint8_t> pixels;   // RGBA8 lightmap (tonemapped), atlasWidth*atlasHeight*4
	std::vector<float> vertexUV;   // normalized lightmap UV, 2 per world-soup vertex
	uint32_t vertexCount = 0;      // number of world-soup vertices (== brushes built in order)
};

// Unwrap the world (xatlas) and bake shadowed direct light + a radiosity bounce (Embree) into a
// lightmap, returning the RGBM RGBA8 texels + per-vertex lightmap UVs for the renderer to sample.
// texelsPerUnit sets the atlas density (higher = sharper, but bake cost grows ~quadratically).
BakeResult bakeLightmaps(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                         float texelsPerUnit = 1.0f / 32.0f);
