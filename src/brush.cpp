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

void buildBrushMesh(const Brush& brush, std::vector<BrushVertex>& outVerts,
                    std::vector<uint16_t>& outIndices) {
	const std::vector<Plane>& planes = brush.planes;
	for (size_t i = 0; i < planes.size(); ++i) {
		std::vector<bx::Vec3> poly = initialFacePolygon(planes[i]);
		for (size_t j = 0; j < planes.size() && !poly.empty(); ++j) {
			if (j != i) clipByPlane(poly, planes[j]);
		}
		if (poly.size() < 3) continue;  // degenerate / clipped away

		const bx::Vec3& nrm = planes[i].n;
		const uint16_t base = static_cast<uint16_t>(outVerts.size());
		for (const bx::Vec3& p : poly) {
			outVerts.push_back({p.x, p.y, p.z, nrm.x, nrm.y, nrm.z});
		}
		for (size_t k = 1; k + 1 < poly.size(); ++k) {  // triangle fan
			outIndices.push_back(base);
			outIndices.push_back(static_cast<uint16_t>(base + k));
			outIndices.push_back(static_cast<uint16_t>(base + k + 1));
		}
	}
}
