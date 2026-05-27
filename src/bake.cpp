#include "bake.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <xatlas.h>

// GI step 1: unwrap the world's brush geometry into a lightmap atlas with xatlas.
// (Next: rasterize charts -> per-texel world pos/normal, ray-trace direct light with Embree,
//  then radiosity bounce, store as an HDR lightmap, and sample it at runtime.)
BakeResult bakeLightmaps(const std::vector<Brush>& brushes) {
	std::vector<float> positions;     // xyz per vertex (world space)
	std::vector<uint32_t> indices;
	for (const Brush& b : brushes) {
		std::vector<BrushVertex> verts;
		std::vector<uint16_t> idx;
		std::vector<FaceRange> faces;
		buildBrushMesh(b, verts, idx, faces);
		const uint32_t base = (uint32_t)(positions.size() / 3);
		for (const BrushVertex& v : verts) {
			positions.push_back(v.x);
			positions.push_back(v.y);
			positions.push_back(v.z);
		}
		for (uint16_t i : idx) indices.push_back(base + (uint32_t)i);
	}

	BakeResult r;
	if (indices.empty()) return r;

	xatlas::Atlas* atlas = xatlas::Create();
	xatlas::MeshDecl decl;
	decl.vertexCount = (uint32_t)(positions.size() / 3);
	decl.vertexPositionData = positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)indices.size();
	decl.indexData = indices.data();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success) {
		xatlas::Destroy(atlas);
		return r;
	}

	xatlas::ChartOptions chartOpts;
	xatlas::PackOptions packOpts;
	packOpts.texelsPerUnit = 1.0f / 32.0f;  // coarse for now (~1 luxel / 32 world units)
	xatlas::Generate(atlas, chartOpts, packOpts);

	r.ok = true;
	r.atlasWidth = atlas->width;
	r.atlasHeight = atlas->height;
	r.chartCount = atlas->chartCount;
	printf("[bake] xatlas: %u charts, atlas %u x %u\n", atlas->chartCount, atlas->width, atlas->height);
	xatlas::Destroy(atlas);
	return r;
}
