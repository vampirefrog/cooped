#include "bake.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <bx/math.h>
#include <embree4/rtcore.h>
#include <xatlas.h>

namespace {

// 2D edge function (signed area * 2) for barycentric rasterization in atlas space.
inline float edge(float ax, float ay, float bx_, float by, float cx, float cy) {
	return (bx_ - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

inline bx::Vec3 vat(const std::vector<float>& a, uint32_t i) { return {a[i * 3], a[i * 3 + 1], a[i * 3 + 2]}; }

inline uint8_t tonemap(float v) {  // exposure tonemap -> 8-bit
	const float c = 1.0f - bx::exp(-v * 1.2f);
	const int i = (int)(bx::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
	return (uint8_t)i;
}

}  // namespace

BakeResult bakeLightmaps(const std::vector<Brush>& brushes, const std::vector<Light>& lights) {
	// --- gather the world triangle soup (positions + per-vertex face normals) ---
	std::vector<float> positions, normals;
	std::vector<uint32_t> indices;
	for (const Brush& b : brushes) {
		std::vector<BrushVertex> verts;
		std::vector<uint16_t> idx;
		std::vector<FaceRange> faces;
		buildBrushMesh(b, verts, idx, faces);
		const uint32_t base = (uint32_t)(positions.size() / 3);
		for (const BrushVertex& v : verts) {
			positions.push_back(v.x);  positions.push_back(v.y);  positions.push_back(v.z);
			normals.push_back(v.nx);   normals.push_back(v.ny);   normals.push_back(v.nz);
		}
		for (uint16_t i : idx) indices.push_back(base + (uint32_t)i);
	}
	BakeResult r;
	if (indices.empty()) return r;

	// --- unwrap into a lightmap atlas (xatlas) ---
	xatlas::Atlas* atlas = xatlas::Create();
	xatlas::MeshDecl decl;
	decl.vertexCount = (uint32_t)(positions.size() / 3);
	decl.vertexPositionData = positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)indices.size();
	decl.indexData = indices.data();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success) { xatlas::Destroy(atlas); return r; }
	xatlas::PackOptions packOpts;
	packOpts.texelsPerUnit = 1.0f / 32.0f;  // coarse for now
	xatlas::Generate(atlas, xatlas::ChartOptions(), packOpts);
	const uint32_t W = atlas->width, H = atlas->height;
	if (W == 0 || H == 0 || atlas->meshCount == 0) { xatlas::Destroy(atlas); return r; }
	const xatlas::Mesh& m = atlas->meshes[0];

	// Per-soup-vertex normalized lightmap UV (last writer wins across chart-duplicated verts).
	r.vertexCount = (uint32_t)(positions.size() / 3);
	r.vertexUV.assign(r.vertexCount * 2, 0.0f);
	for (uint32_t i = 0; i < m.vertexCount; ++i) {
		const xatlas::Vertex& v = m.vertexArray[i];
		r.vertexUV[v.xref * 2 + 0] = v.uv[0] / (float)W;
		r.vertexUV[v.xref * 2 + 1] = v.uv[1] / (float)H;
	}

	// --- Embree BVH over the soup (for shadow rays) ---
	RTCDevice dev = rtcNewDevice(nullptr);
	RTCScene scene = rtcNewScene(dev);
	RTCGeometry geom = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_TRIANGLE);
	const uint32_t nv = (uint32_t)(positions.size() / 3), nt = (uint32_t)(indices.size() / 3);
	float* vb = (float*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
	                                            3 * sizeof(float), nv);
	memcpy(vb, positions.data(), positions.size() * sizeof(float));
	unsigned* ib = (unsigned*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
	                                                  3 * sizeof(unsigned), nt);
	memcpy(ib, indices.data(), indices.size() * sizeof(uint32_t));
	rtcCommitGeometry(geom);
	rtcAttachGeometry(scene, geom);
	rtcReleaseGeometry(geom);
	rtcCommitScene(scene);

	auto occluded = [&](const bx::Vec3& o, const bx::Vec3& d, float dist) -> bool {
		RTCRay ray{};
		ray.org_x = o.x; ray.org_y = o.y; ray.org_z = o.z;
		ray.dir_x = d.x; ray.dir_y = d.y; ray.dir_z = d.z;
		ray.tnear = 0.05f; ray.tfar = dist; ray.mask = 0xffffffffu;
		rtcOccluded1(scene, &ray);
		return ray.tfar < 0.0f;
	};

	// --- bake constants ---
	const bx::Vec3 sunDir = bx::normalize(bx::Vec3(0.35f, 0.25f, 0.9f));
	const float sunInt = 0.8f;
	const bx::Vec3 skyAmbient(0.20f, 0.24f, 0.32f);

	std::vector<float> lm(W * H * 3, 0.0f);
	std::vector<uint8_t> covered(W * H, 0);

	for (uint32_t t = 0; t + 2 < m.indexCount; t += 3) {
		const xatlas::Vertex& a = m.vertexArray[m.indexArray[t]];
		const xatlas::Vertex& b = m.vertexArray[m.indexArray[t + 1]];
		const xatlas::Vertex& c = m.vertexArray[m.indexArray[t + 2]];
		const bx::Vec3 w0 = vat(positions, a.xref), w1 = vat(positions, b.xref), w2 = vat(positions, c.xref);
		const bx::Vec3 n = bx::normalize(vat(normals, a.xref));
		const float area = edge(a.uv[0], a.uv[1], b.uv[0], b.uv[1], c.uv[0], c.uv[1]);
		if (bx::abs(area) < 1.0e-6f) continue;
		int minx = (int)bx::floor(bx::min(a.uv[0], b.uv[0], c.uv[0])), maxx = (int)bx::ceil(bx::max(a.uv[0], b.uv[0], c.uv[0]));
		int miny = (int)bx::floor(bx::min(a.uv[1], b.uv[1], c.uv[1])), maxy = (int)bx::ceil(bx::max(a.uv[1], b.uv[1], c.uv[1]));
		minx = bx::max(minx, 0); miny = bx::max(miny, 0);
		maxx = bx::min(maxx, (int)W - 1); maxy = bx::min(maxy, (int)H - 1);
		for (int ty = miny; ty <= maxy; ++ty)
			for (int tx = minx; tx <= maxx; ++tx) {
				const float px = tx + 0.5f, py = ty + 0.5f;
				float b0 = edge(b.uv[0], b.uv[1], c.uv[0], c.uv[1], px, py) / area;
				float b1 = edge(c.uv[0], c.uv[1], a.uv[0], a.uv[1], px, py) / area;
				float b2 = edge(a.uv[0], a.uv[1], b.uv[0], b.uv[1], px, py) / area;
				if (b0 < -0.01f || b1 < -0.01f || b2 < -0.01f) continue;
				bx::Vec3 wp = bx::add(bx::add(bx::mul(w0, b0), bx::mul(w1, b1)), bx::mul(w2, b2));
				const bx::Vec3 origin = bx::add(wp, bx::mul(n, 0.1f));
				bx::Vec3 col = skyAmbient;
				const float sndl = bx::dot(n, sunDir);
				if (sndl > 0.0f && !occluded(origin, sunDir, 1.0e6f))
					col = bx::add(col, bx::Vec3(sunInt * sndl, sunInt * sndl, sunInt * sndl));
				for (const Light& L : lights) {
					const bx::Vec3 toL = bx::sub(L.pos, wp);
					const float dist = bx::length(toL);
					if (dist < 1.0e-3f || dist >= L.radius) continue;
					const bx::Vec3 dir = bx::mul(toL, 1.0f / dist);
					const float ndl = bx::dot(n, dir);
					if (ndl <= 0.0f) continue;
					float atten = 1.0f - dist / L.radius;
					atten *= atten;
					if (!occluded(origin, dir, dist - 0.2f)) {
						const float k = ndl * atten;
						col = bx::add(col, bx::Vec3(L.color[0] * k, L.color[1] * k, L.color[2] * k));
					}
				}
				const int o = (ty * (int)W + tx) * 3;
				lm[o] = col.x; lm[o + 1] = col.y; lm[o + 2] = col.z;
				covered[ty * W + tx] = 1;
			}
	}

	// Dilate covered texels into their neighbours a couple of times (reduces chart-edge seams).
	for (int pass = 0; pass < 2; ++pass) {
		std::vector<uint8_t> cov2 = covered;
		for (int y = 0; y < (int)H; ++y)
			for (int x = 0; x < (int)W; ++x) {
				if (covered[y * W + x]) continue;
				const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (auto& d : nb) {
					const int nx = x + d[0], ny = y + d[1];
					if (nx < 0 || ny < 0 || nx >= (int)W || ny >= (int)H || !covered[ny * W + nx]) continue;
					const int s = (ny * W + nx) * 3, o = (y * W + x) * 3;
					lm[o] = lm[s]; lm[o + 1] = lm[s + 1]; lm[o + 2] = lm[s + 2];
					cov2[y * W + x] = 1;
					break;
				}
			}
		covered.swap(cov2);
	}

	// Tonemap to an RGBA8 lightmap texture the renderer samples.
	r.pixels.resize(W * H * 4);
	for (uint32_t i = 0; i < W * H; ++i) {
		r.pixels[i * 4 + 0] = tonemap(lm[i * 3 + 0]);
		r.pixels[i * 4 + 1] = tonemap(lm[i * 3 + 1]);
		r.pixels[i * 4 + 2] = tonemap(lm[i * 3 + 2]);
		r.pixels[i * 4 + 3] = 255;
	}

	rtcReleaseScene(scene);
	rtcReleaseDevice(dev);
	r.ok = true;
	r.atlasWidth = W;
	r.atlasHeight = H;
	r.chartCount = atlas->chartCount;
	printf("[bake] %u charts, lightmap %u x %u, %zu lights\n", atlas->chartCount, W, H, lights.size());
	xatlas::Destroy(atlas);
	return r;
}
