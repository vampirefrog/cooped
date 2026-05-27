#include "bake.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
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

inline uint8_t to8(float v) { return (uint8_t)(bx::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

const float kRgbmRange = 8.0f;  // RGBM alpha multiplier range (shader decodes rgb * a * this)

}  // namespace

// Everything the bounce + assembly need, rebuilt deterministically from the scene on every node.
struct BakeSolver {
	uint32_t W = 0, H = 0, chartCount = 0, vertexCount = 0;
	std::vector<float> lm;         // W*H*3 direct light (base; radiosity overwrites covered texels)
	std::vector<uint8_t> covered;  // W*H
	std::vector<float> vertexUV;   // 2 per soup vertex
	struct Patch { bx::Vec3 pos, nrm, alb; uint32_t ti; };
	std::vector<Patch> patches;
	std::vector<bx::Vec3> direct;  // emission per patch (= lm sampled at the patch texel)
	float patchArea = 0.0f;
	RTCDevice dev = nullptr;
	RTCScene scene = nullptr;
	~BakeSolver() {
		if (scene) rtcReleaseScene(scene);
		if (dev) rtcReleaseDevice(dev);
	}
};

BakeSolver* bakeBuild(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                      float texelsPerUnit) {
	// --- gather the world triangle soup (positions + per-vertex face normals + brush albedo) ---
	std::vector<float> positions, normals, albedos;
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
			albedos.push_back(b.color[0]); albedos.push_back(b.color[1]); albedos.push_back(b.color[2]);
		}
		for (uint16_t i : idx) indices.push_back(base + (uint32_t)i);
	}
	if (indices.empty()) return nullptr;

	// --- unwrap into a lightmap atlas (xatlas; deterministic for a given input) ---
	xatlas::Atlas* atlas = xatlas::Create();
	xatlas::MeshDecl decl;
	decl.vertexCount = (uint32_t)(positions.size() / 3);
	decl.vertexPositionData = positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)indices.size();
	decl.indexData = indices.data();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success) { xatlas::Destroy(atlas); return nullptr; }
	xatlas::PackOptions packOpts;
	packOpts.texelsPerUnit = texelsPerUnit;
	xatlas::Generate(atlas, xatlas::ChartOptions(), packOpts);
	const uint32_t W = atlas->width, H = atlas->height;
	if (W == 0 || H == 0 || atlas->meshCount == 0) { xatlas::Destroy(atlas); return nullptr; }
	const xatlas::Mesh& m = atlas->meshes[0];

	BakeSolver* s = new BakeSolver();
	s->W = W; s->H = H; s->chartCount = atlas->chartCount;
	s->vertexCount = (uint32_t)(positions.size() / 3);
	s->vertexUV.assign(s->vertexCount * 2, 0.0f);
	for (uint32_t i = 0; i < m.vertexCount; ++i) {
		const xatlas::Vertex& v = m.vertexArray[i];
		s->vertexUV[v.xref * 2 + 0] = v.uv[0] / (float)W;
		s->vertexUV[v.xref * 2 + 1] = v.uv[1] / (float)H;
	}

	// --- Embree BVH over the soup (shadow + visibility rays) ---
	s->dev = rtcNewDevice(nullptr);
	s->scene = rtcNewScene(s->dev);
	RTCGeometry geom = rtcNewGeometry(s->dev, RTC_GEOMETRY_TYPE_TRIANGLE);
	const uint32_t nv = (uint32_t)(positions.size() / 3), nt = (uint32_t)(indices.size() / 3);
	float* vb = (float*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
	                                            3 * sizeof(float), nv);
	memcpy(vb, positions.data(), positions.size() * sizeof(float));
	unsigned* ib = (unsigned*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
	                                                  3 * sizeof(unsigned), nt);
	memcpy(ib, indices.data(), indices.size() * sizeof(uint32_t));
	rtcCommitGeometry(geom);
	rtcAttachGeometry(s->scene, geom);
	rtcReleaseGeometry(geom);
	rtcCommitScene(s->scene);

	auto occluded = [&](const bx::Vec3& o, const bx::Vec3& d, float dist) -> bool {
		RTCRay ray{};
		ray.org_x = o.x; ray.org_y = o.y; ray.org_z = o.z;
		ray.dir_x = d.x; ray.dir_y = d.y; ray.dir_z = d.z;
		ray.tnear = 0.05f; ray.tfar = dist; ray.mask = 0xffffffffu;
		rtcOccluded1(s->scene, &ray);
		return ray.tfar < 0.0f;
	};

	// --- rasterize shadowed direct light per texel ---
	const bx::Vec3 sunDir = bx::normalize(bx::Vec3(0.35f, 0.25f, 0.9f));
	const float sunInt = 0.8f;
	const bx::Vec3 skyAmbient(0.20f, 0.24f, 0.32f);
	s->lm.assign(W * H * 3, 0.0f);
	s->covered.assign(W * H, 0);
	std::vector<float> tpos(W * H * 3, 0.0f), tnrm(W * H * 3, 0.0f), talb(W * H * 3, 0.0f);

	for (uint32_t t = 0; t + 2 < m.indexCount; t += 3) {
		const xatlas::Vertex& a = m.vertexArray[m.indexArray[t]];
		const xatlas::Vertex& b = m.vertexArray[m.indexArray[t + 1]];
		const xatlas::Vertex& c = m.vertexArray[m.indexArray[t + 2]];
		const bx::Vec3 w0 = vat(positions, a.xref), w1 = vat(positions, b.xref), w2 = vat(positions, c.xref);
		const bx::Vec3 n = bx::normalize(vat(normals, a.xref));
		const bx::Vec3 alb = vat(albedos, a.xref);
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
				s->lm[o] = col.x; s->lm[o + 1] = col.y; s->lm[o + 2] = col.z;
				tpos[o] = wp.x; tpos[o + 1] = wp.y; tpos[o + 2] = wp.z;
				tnrm[o] = n.x;  tnrm[o + 1] = n.y;  tnrm[o + 2] = n.z;
				talb[o] = alb.x; talb[o + 1] = alb.y; talb[o + 2] = alb.z;
				s->covered[ty * W + tx] = 1;
			}
	}

	// --- patch list + direct emission (one patch per covered texel) ---
	const float texel = 1.0f / texelsPerUnit;
	s->patchArea = texel * texel;
	for (uint32_t i = 0; i < W * H; ++i) {
		if (!s->covered[i]) continue;
		const uint32_t o = i * 3;
		s->patches.push_back({{tpos[o], tpos[o + 1], tpos[o + 2]},
		                      {tnrm[o], tnrm[o + 1], tnrm[o + 2]},
		                      {talb[o], talb[o + 1], talb[o + 2]}, i});
	}
	s->direct.assign(s->patches.size(), bx::Vec3(0, 0, 0));
	for (size_t i = 0; i < s->patches.size(); ++i) {
		const uint32_t o = s->patches[i].ti * 3;
		s->direct[i] = bx::Vec3(s->lm[o], s->lm[o + 1], s->lm[o + 2]);
	}

	xatlas::Destroy(atlas);
	return s;
}

void bakeDestroy(BakeSolver* s) { delete s; }

size_t bakeSolverPatchCount(const BakeSolver* s) { return s ? s->patches.size() : 0; }

void bakeSolverInitRadiosity(const BakeSolver* s, std::vector<float>& rad) {
	rad.resize(s->patches.size() * 3);
	for (size_t i = 0; i < s->patches.size(); ++i) {
		rad[i * 3 + 0] = s->direct[i].x; rad[i * 3 + 1] = s->direct[i].y; rad[i * 3 + 2] = s->direct[i].z;
	}
}

void bakeGather(const BakeSolver* s, const float* radIn, size_t P, size_t lo, size_t hi,
                float* out, unsigned threads) {
	const float kPi = 3.14159265358979f;
	// One receiver patch is independent of the others given the previous round's radiosity, so a
	// range is a clean parallel-for (rtcOccluded1 is safe to call concurrently on a committed scene).
	const auto work = [&](size_t a, size_t b) {
		for (size_t i = a; i < b; ++i) {
			const BakeSolver::Patch& pi = s->patches[i];
			bx::Vec3 sum(0, 0, 0);
			for (size_t j = 0; j < P; ++j) {
				if (j == i) continue;
				const BakeSolver::Patch& pj = s->patches[j];
				const bx::Vec3 d = bx::sub(pj.pos, pi.pos);
				const float dist2 = bx::dot(d, d);
				if (dist2 < 1.0f) continue;
				const float dist = bx::sqrt(dist2);
				const bx::Vec3 dir = bx::mul(d, 1.0f / dist);
				const float cosi = bx::dot(pi.nrm, dir);
				if (cosi <= 0.0f) continue;
				const float cosj = -bx::dot(pj.nrm, dir);
				if (cosj <= 0.0f) continue;
				// Disk-approx form factor; +patchArea regularizes the near-field singularity.
				const float ff = (cosi * cosj) * s->patchArea / (kPi * dist2 + s->patchArea);
				if (ff < 1.0e-5f) continue;
				const bx::Vec3 o = bx::add(pi.pos, bx::mul(pi.nrm, 0.1f));
				RTCRay ray{};
				ray.org_x = o.x; ray.org_y = o.y; ray.org_z = o.z;
				ray.dir_x = dir.x; ray.dir_y = dir.y; ray.dir_z = dir.z;
				ray.tnear = 0.05f; ray.tfar = dist - 0.2f; ray.mask = 0xffffffffu;
				rtcOccluded1(s->scene, &ray);
				if (ray.tfar < 0.0f) continue;
				sum = bx::add(sum, bx::mul(bx::Vec3(radIn[j * 3], radIn[j * 3 + 1], radIn[j * 3 + 2]), ff));
			}
			const size_t k = (i - lo) * 3;
			out[k + 0] = sum.x * pi.alb.x; out[k + 1] = sum.y * pi.alb.y; out[k + 2] = sum.z * pi.alb.z;
		}
	};
	const size_t N = hi - lo;
	const unsigned nthreads = (N < 512) ? 1u : bx::max(1u, threads);
	if (nthreads <= 1) { work(lo, hi); return; }
	std::vector<std::thread> pool;
	const size_t chunk = (N + nthreads - 1) / nthreads;
	for (unsigned t = 0; t < nthreads; ++t) {
		const size_t a = lo + (size_t)t * chunk, b = bx::min(hi, a + chunk);
		if (a >= b) break;
		pool.emplace_back(work, a, b);
	}
	for (std::thread& th : pool) th.join();
}

BakeResult bakeAssemble(const BakeSolver* s, const float* rad, size_t P) {
	BakeResult r;
	const uint32_t W = s->W, H = s->H;
	std::vector<float> lm = s->lm;  // base direct light; overwrite covered texels with final radiosity
	for (size_t i = 0; i < P && i < s->patches.size(); ++i) {
		const uint32_t o = s->patches[i].ti * 3;
		lm[o] = rad[i * 3 + 0]; lm[o + 1] = rad[i * 3 + 1]; lm[o + 2] = rad[i * 3 + 2];
	}

	// Dilate covered texels into their neighbours a couple of times (reduces chart-edge seams).
	std::vector<uint8_t> covered = s->covered;
	for (int pass = 0; pass < 2; ++pass) {
		std::vector<uint8_t> cov2 = covered;
		for (int y = 0; y < (int)H; ++y)
			for (int x = 0; x < (int)W; ++x) {
				if (covered[y * W + x]) continue;
				const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (auto& d : nb) {
					const int nx = x + d[0], ny = y + d[1];
					if (nx < 0 || ny < 0 || nx >= (int)W || ny >= (int)H || !covered[ny * W + nx]) continue;
					const int so = (ny * W + nx) * 3, o = (y * W + x) * 3;
					lm[o] = lm[so]; lm[o + 1] = lm[so + 1]; lm[o + 2] = lm[so + 2];
					cov2[y * W + x] = 1;
					break;
				}
			}
		covered.swap(cov2);
	}

	// RGBM encode (shader decodes + tonemaps, so bright/bounced light isn't clipped here).
	r.pixels.resize(W * H * 4);
	for (uint32_t i = 0; i < W * H; ++i) {
		const float rr = lm[i * 3 + 0], gg = lm[i * 3 + 1], bb = lm[i * 3 + 2];
		float mm = bx::max(bx::max(rr, gg, bb) / kRgbmRange, 1.0f / 255.0f);
		mm = bx::ceil(bx::clamp(mm, 0.0f, 1.0f) * 255.0f) / 255.0f;
		const float inv = 1.0f / (mm * kRgbmRange);
		r.pixels[i * 4 + 0] = to8(rr * inv);
		r.pixels[i * 4 + 1] = to8(gg * inv);
		r.pixels[i * 4 + 2] = to8(bb * inv);
		r.pixels[i * 4 + 3] = to8(mm);
	}
	r.vertexUV = s->vertexUV;
	r.vertexCount = s->vertexCount;
	r.ok = true;
	r.atlasWidth = W;
	r.atlasHeight = H;
	r.chartCount = s->chartCount;
	return r;
}

BakeResult bakeLightmaps(const std::vector<Brush>& brushes, const std::vector<Light>& lights,
                         float texelsPerUnit) {
	const auto tStart = std::chrono::steady_clock::now();
	BakeSolver* s = bakeBuild(brushes, lights, texelsPerUnit);
	if (!s) return {};
	const size_t P = bakeSolverPatchCount(s);

	unsigned hw = bx::max(1u, std::thread::hardware_concurrency());
	if (const char* e = getenv("COOPED_BAKE_THREADS")) { const int v = atoi(e); if (v > 0) hw = (unsigned)v; }

	std::vector<float> rad; bakeSolverInitRadiosity(s, rad);
	const std::vector<float> direct = rad;
	for (int pass = 0; pass < kBakeBounces; ++pass) {
		std::vector<float> gathered(P * 3, 0.0f);
		bakeGather(s, rad.data(), P, 0, P, gathered.data(), hw);
		for (size_t i = 0; i < P * 3; ++i) rad[i] = direct[i] + gathered[i];
	}

	double bounced = 0.0;
	for (size_t i = 0; i < P; ++i)
		bounced += bx::length(bx::sub(bx::Vec3(rad[i * 3], rad[i * 3 + 1], rad[i * 3 + 2]),
		                              bx::Vec3(direct[i * 3], direct[i * 3 + 1], direct[i * 3 + 2])));
	printf("[bake] radiosity: %zu patches, %d bounces, %u threads, avg indirect %.3f\n",
	       P, kBakeBounces, (P < 512 ? 1u : hw), P ? bounced / P : 0.0);

	BakeResult r = bakeAssemble(s, rad.data(), P);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
	printf("[bake] %u charts, lightmap %u x %u, %zu lights, %.0f ms\n", r.chartCount, r.atlasWidth, r.atlasHeight, lights.size(), ms);
	bakeDestroy(s);
	return r;
}
