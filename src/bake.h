// GI lightmap bake (native only; the bake uses xatlas + Embree). On Emscripten this isn't built.
//
// The bake is split into phases so it can run locally (one machine, all cores) or be distributed
// across nodes: every node deterministically rebuilds the SAME patch list + BVH from the scene
// (bakeBuild), so only per-round radiosity slices need to be exchanged. bakeGather computes the
// bounce term for a contiguous patch range [lo,hi); bakeAssemble turns the final radiosity into
// the RGBM atlas. Because a patch's gather is computed identically wherever it runs, a distributed
// bake is bit-for-bit equal to a local one.
#pragma once

#include <cstddef>
#include <vector>

#include "brush.h"

struct BakeResult {
	bool ok = false;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	uint32_t chartCount = 0;
	std::vector<uint8_t> pixels;   // RGBA8 lightmap (RGBM-encoded), atlasWidth*atlasHeight*4
	std::vector<float> vertexUV;   // normalized lightmap UV, 2 per world-soup vertex
	uint32_t vertexCount = 0;      // number of world-soup vertices (built in brush order)
};

constexpr int kBakeBounces = 2;  // radiosity gather rounds (coordinator and local path agree)

// --- distributable phases -------------------------------------------------------------------
struct BakeSolver;  // opaque: unwrapped atlas, rasterized direct light, patch list, and BVH

// Unwrap + rasterize direct light + build the patch BVH. Returns null on failure (empty world,
// unwrap failure). texelsPerUnit sets atlas density (higher = sharper, bake cost ~quadratic).
BakeSolver* bakeBuild(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                      float texelsPerUnit = 1.0f / 32.0f);
void bakeDestroy(BakeSolver*);

size_t bakeSolverPatchCount(const BakeSolver*);
// Fill rad (3 floats per patch) with the direct-light emission — the round-0 radiosity.
void bakeSolverInitRadiosity(const BakeSolver*, std::vector<float>& rad);
// Gather the albedo-weighted bounce term for receivers [lo,hi) from the full previous radiosity
// (radIn, 3*P floats). Writes 3*(hi-lo) floats to out (out[0] = patch lo). Multithreaded.
void bakeGather(const BakeSolver*, const float* radIn, size_t P, size_t lo, size_t hi,
                float* out, unsigned threads);
// Turn the final radiosity (3*P floats) into the RGBM atlas + per-vertex UVs.
BakeResult bakeAssemble(const BakeSolver*, const float* radiosity, size_t P);

// --- convenience: full local bake (build + bounce rounds across all cores + assemble) ---------
BakeResult bakeLightmaps(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                         float texelsPerUnit = 1.0f / 32.0f);
