#include "brush.h"

namespace {

constexpr float kQuadSpan = 1.0e5f;  // initial face polygon half-size before clipping
constexpr float kEps      = 0.01f;   // clip tolerance (Quake-scale units)

inline bx::Vec3 scale(const bx::Vec3& v, float s) { return bx::mul(v, s); }

// Signed distance of p from a plane (>0 outside the interior half-space).
inline float planeDist(const Plane& pl, const bx::Vec3& p) { return bx::dot(pl.n, p) - pl.d; }

// A large quad lying on the plane, wound around the plane normal.
std::vector<bx::Vec3> initialFacePolygon(const Plane& pl) {
	const bx::Vec3 n = pl.n;
	const float ax = bx::abs(n.x), ay = bx::abs(n.y), az = bx::abs(n.z);
	bx::Vec3 axis = (ax <= ay && ax <= az) ? bx::Vec3(1.0f, 0.0f, 0.0f)
	              : (ay <= az)             ? bx::Vec3(0.0f, 1.0f, 0.0f)
	                                       : bx::Vec3(0.0f, 0.0f, 1.0f);
	const bx::Vec3 u = bx::normalize(bx::cross(n, axis));
	const bx::Vec3 v = bx::cross(n, u);
	const bx::Vec3 o = scale(n, pl.d);  // a point on the plane
	const bx::Vec3 us = scale(u, kQuadSpan);
	const bx::Vec3 vs = scale(v, kQuadSpan);
	return {
		bx::sub(bx::sub(o, us), vs),
		bx::sub(bx::add(o, us), vs),
		bx::add(bx::add(o, us), vs),
		bx::add(bx::sub(o, us), vs),
	};
}

// Sutherland-Hodgman: keep the part of poly inside the half-space dot(n,p) <= d.
void clipByPlane(std::vector<bx::Vec3>& poly, const Plane& pl) {
	if (poly.empty()) return;
	std::vector<bx::Vec3> out;
	out.reserve(poly.size() + 4);
	const size_t n = poly.size();
	for (size_t i = 0; i < n; ++i) {
		const bx::Vec3& a = poly[i];
		const bx::Vec3& b = poly[(i + 1) % n];
		const float da = planeDist(pl, a);
		const float db = planeDist(pl, b);
		const bool aIn = da <= kEps;
		const bool bIn = db <= kEps;
		if (aIn) out.push_back(a);
		if (aIn != bIn) {
			const float t = da / (da - db);
			out.push_back(bx::add(a, scale(bx::sub(b, a), t)));
		}
	}
	poly.swap(out);
}

}  // namespace

std::vector<bx::Vec3> brushFacePolygon(const Brush& brush, size_t faceIndex) {
	const std::vector<Plane>& planes = brush.planes;
	std::vector<bx::Vec3> poly = initialFacePolygon(planes[faceIndex]);
	for (size_t j = 0; j < planes.size() && !poly.empty(); ++j) {
		if (j != faceIndex) clipByPlane(poly, planes[j]);
	}
	if (poly.size() < 3) poly.clear();
	return poly;
}

void buildFaceMesh(const Brush& brush, size_t faceIndex, float bias,
                   std::vector<BrushVertex>& outTris) {
	const std::vector<bx::Vec3> poly = brushFacePolygon(brush, faceIndex);
	if (poly.size() < 3) return;
	const bx::Vec3& n = brush.planes[faceIndex].n;
	const bx::Vec3 off = scale(n, bias);
	for (size_t k = 1; k + 1 < poly.size(); ++k) {  // triangle fan
		const bx::Vec3 a = bx::add(poly[0], off);
		const bx::Vec3 b = bx::add(poly[k], off);
		const bx::Vec3 c = bx::add(poly[k + 1], off);
		outTris.push_back({a.x, a.y, a.z, n.x, n.y, n.z});
		outTris.push_back({b.x, b.y, b.z, n.x, n.y, n.z});
		outTris.push_back({c.x, c.y, c.z, n.x, n.y, n.z});
	}
}

Brush makeBox(const bx::Vec3& c, const bx::Vec3& h, float r, float g, float b) {
	Brush brush;
	brush.color[0] = r;
	brush.color[1] = g;
	brush.color[2] = b;
	brush.planes = {
		{bx::Vec3( 1.0f,  0.0f,  0.0f), c.x + h.x},
		{bx::Vec3(-1.0f,  0.0f,  0.0f), -(c.x - h.x)},
		{bx::Vec3( 0.0f,  1.0f,  0.0f), c.y + h.y},
		{bx::Vec3( 0.0f, -1.0f,  0.0f), -(c.y - h.y)},
		{bx::Vec3( 0.0f,  0.0f,  1.0f), c.z + h.z},
		{bx::Vec3( 0.0f,  0.0f, -1.0f), -(c.z - h.z)},
	};
	return brush;
}

RayHit rayBrushIntersect(const bx::Vec3& ro, const bx::Vec3& rd, const Brush& brush) {
	float tmin = -1.0e30f;
	float tmax = 1.0e30f;
	int enterFace = -1;
	for (int i = 0; i < (int)brush.planes.size(); ++i) {
		const Plane& pl = brush.planes[i];
		const float denom = bx::dot(pl.n, rd);
		const float dist = bx::dot(pl.n, ro) - pl.d;  // >0 outside the interior
		if (bx::abs(denom) < 1.0e-6f) {
			if (dist > 0.0f) return {};  // parallel and outside this half-space
			continue;
		}
		const float t = -dist / denom;
		if (denom < 0.0f) {            // ray entering this half-space
			if (t > tmin) { tmin = t; enterFace = i; }
		} else {                       // ray exiting this half-space
			if (t < tmax) tmax = t;
		}
		if (tmin > tmax) return {};
	}
	const float t = (tmin > 1.0e-4f) ? tmin : tmax;  // nearest surface ahead of the origin
	if (t < 1.0e-4f) return {};
	return {true, t, (tmin > 1.0e-4f) ? enterFace : -1};
}

void translateBrush(Brush& brush, const bx::Vec3& delta) {
	for (Plane& pl : brush.planes) pl.d += bx::dot(pl.n, delta);
}

void buildBrushMesh(const Brush& brush, std::vector<BrushVertex>& outVerts,
                    std::vector<uint16_t>& outIndices) {
	for (size_t i = 0; i < brush.planes.size(); ++i) {
		const std::vector<bx::Vec3> poly = brushFacePolygon(brush, i);
		if (poly.size() < 3) continue;  // degenerate / clipped away

		const bx::Vec3& nrm = brush.planes[i].n;
		const uint16_t base = static_cast<uint16_t>(outVerts.size());
		// Planar texture projection from the face's dominant axis (one tile per 64 units).
		const float ax = bx::abs(nrm.x), ay = bx::abs(nrm.y), az = bx::abs(nrm.z);
		const float s = 1.0f / 64.0f;
		for (const bx::Vec3& p : poly) {
			float u, v;
			if (az >= ax && az >= ay)      { u = p.x * s; v = p.y * s; }  // floors / ceilings
			else if (ax >= ay)             { u = p.y * s; v = p.z * s; }  // ±X walls
			else                           { u = p.x * s; v = p.z * s; }  // ±Y walls
			outVerts.push_back({p.x, p.y, p.z, nrm.x, nrm.y, nrm.z, u, v});
		}
		for (size_t k = 1; k + 1 < poly.size(); ++k) {  // triangle fan
			outIndices.push_back(base);
			outIndices.push_back(static_cast<uint16_t>(base + k));
			outIndices.push_back(static_cast<uint16_t>(base + k + 1));
		}
	}
}
