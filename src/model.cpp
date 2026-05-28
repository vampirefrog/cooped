#include "model.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <bgfx/bgfx.h>
#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/file.h>

#include "brush.h"  // BrushVertex layout used everywhere

#include <ufbx.h>

void Model::destroy() {
	if (bgfx::isValid(vbh)) { bgfx::destroy(vbh); vbh = BGFX_INVALID_HANDLE; }
	if (bgfx::isValid(ibh)) { bgfx::destroy(ibh); ibh = BGFX_INVALID_HANDLE; }
	if (bgfx::isValid(texture)) { bgfx::destroy(texture); texture = BGFX_INVALID_HANDLE; }
	numIndices = 0;
}

namespace {

bgfx::TextureHandle loadDiffuse(const char* path) {
	if (!path || !*path) return BGFX_INVALID_HANDLE;
	FILE* f = fopen(path, "rb");
	if (!f) return BGFX_INVALID_HANDLE;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> buf(size > 0 ? (size_t)size : 0);
	const size_t got = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
	fclose(f);
	if (got != buf.size() || buf.empty()) return BGFX_INVALID_HANDLE;
	// Static allocator: bimg stores a pointer to it inside the ImageContainer (must outlive it).
	static bx::DefaultAllocator alloc;
	bimg::ImageContainer* img = bimg::imageParse(&alloc, buf.data(), (uint32_t)buf.size(),
	                                             bimg::TextureFormat::RGBA8);
	if (!img) return BGFX_INVALID_HANDLE;
	// Copy into a bgfx-owned buffer so we can free img immediately (avoids async-release lifetime).
	const bgfx::TextureHandle h = bgfx::createTexture2D(
	    (uint16_t)img->m_width, (uint16_t)img->m_height, false, 1, bgfx::TextureFormat::RGBA8, 0,
	    bgfx::copy(img->m_data, img->m_size));
	bimg::imageFree(img);
	return h;
}

}  // namespace

Model loadFbxModel(const char* path, const bgfx::VertexLayout& layout, const char* texturePath,
                   float targetHeight, float yawOffset) {
	Model m;
	ufbx_load_opts opts = {};
	// cooped is left-handed Z-up (X-right, Y-forward, Z-up) — see physics + projection.
	opts.target_axes = ufbx_axes_left_handed_z_up;
	opts.target_unit_meters = 1.0f;
	opts.generate_missing_normals = true;
	ufbx_error err;
	ufbx_scene* scene = ufbx_load_file(path, &opts, &err);
	if (!scene) { printf("ufbx '%s' load failed: %s\n", path, err.description.data); return m; }

	std::vector<BrushVertex> verts;
	std::vector<uint32_t> idx;
	float bmin[3] = {1.0e30f, 1.0e30f, 1.0e30f}, bmax[3] = {-1.0e30f, -1.0e30f, -1.0e30f};

	for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
		ufbx_mesh* msh = scene->meshes.data[mi];
		const bool hasUv = msh->uv_sets.count > 0;
		uint32_t tri[64 * 3];
		for (size_t fi = 0; fi < msh->faces.count; ++fi) {
			const ufbx_face f = msh->faces.data[fi];
			if (f.num_indices < 3) continue;
			const uint32_t nt = (uint32_t)ufbx_triangulate_face(tri, sizeof(tri) / sizeof(tri[0]), msh, f);
			for (uint32_t k = 0; k < nt * 3; ++k) {
				const uint32_t c = tri[k];
				const ufbx_vec3 p = ufbx_get_vertex_vec3(&msh->vertex_position, c);
				const ufbx_vec3 n = ufbx_get_vertex_vec3(&msh->vertex_normal, c);
				const ufbx_vec2 uv = hasUv ? ufbx_get_vertex_vec2(&msh->vertex_uv, c) : ufbx_vec2{0, 0};
				BrushVertex v{(float)p.x, (float)p.y, (float)p.z,
				              (float)n.x, (float)n.y, (float)n.z,
				              (float)uv.x, 1.0f - (float)uv.y,   // FBX UVs are commonly Y-down
				              0.0f, 0.0f};
				if (v.x < bmin[0]) bmin[0] = v.x; if (v.x > bmax[0]) bmax[0] = v.x;
				if (v.y < bmin[1]) bmin[1] = v.y; if (v.y > bmax[1]) bmax[1] = v.y;
				if (v.z < bmin[2]) bmin[2] = v.z; if (v.z > bmax[2]) bmax[2] = v.z;
				idx.push_back((uint32_t)verts.size());
				verts.push_back(v);
			}
		}
	}
	ufbx_free_scene(scene);
	if (verts.empty()) { printf("fbx '%s': no triangles\n", path); return m; }

	// Centre XY, drop feet to z=0, scale to targetHeight, then rotate around Z by yawOffset so
	// the model's authored "front" aligns with +X (cooped's yaw=0 direction).
	const float cx = (bmin[0] + bmax[0]) * 0.5f;
	const float cy = (bmin[1] + bmax[1]) * 0.5f;
	const float h = bmax[2] - bmin[2];
	const float s = (h > 1.0e-3f) ? (targetHeight / h) : 1.0f;
	const float cz = std::cos(yawOffset), sz = std::sin(yawOffset);
	for (BrushVertex& v : verts) {
		float x = (v.x - cx) * s, y = (v.y - cy) * s;
		v.x = x * cz - y * sz; v.y = x * sz + y * cz;
		v.z = (v.z - bmin[2]) * s;
		const float nx = v.nx * cz - v.ny * sz, ny = v.nx * sz + v.ny * cz;
		v.nx = nx; v.ny = ny;
	}
	m.bsCenter[0] = 0; m.bsCenter[1] = 0; m.bsCenter[2] = targetHeight * 0.5f;
	m.bsRadius = 0.5f * targetHeight * 1.5f;

	m.vbh = bgfx::createVertexBuffer(
	    bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(BrushVertex))), layout);
	m.ibh = bgfx::createIndexBuffer(
	    bgfx::copy(idx.data(), uint32_t(idx.size() * sizeof(uint32_t))), BGFX_BUFFER_INDEX32);
	m.numIndices = (uint32_t)idx.size();
	m.texture = loadDiffuse(texturePath);
	printf("fbx '%s': %zu verts, %zu idx, scaled %.3fx, tex %s\n", path, verts.size(), idx.size(), s,
	       bgfx::isValid(m.texture) ? "ok" : "missing");
	return m;
}
