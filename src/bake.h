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
};

// Step 1: unwrap the world's brush faces into a lightmap atlas (UVs). Bake fills come next.
BakeResult bakeLightmaps(const std::vector<Brush>& brushes);
