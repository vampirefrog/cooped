// Convex brush geometry: a brush is the intersection of half-spaces (planes).
// buildBrushMesh() runs the CSG hull build — clip each face plane by every other
// half-space to get the face polygon, then triangulate. Coordinates are Z-up,
// world-space, Quake-scale float units (see DESIGN.md §17).
#pragma once

#include <cstdint>
#include <vector>

#include <bx/math.h>

struct Plane {
	bx::Vec3 n;  // outward unit normal
	float    d;  // plane: dot(n, p) == d  (interior is dot(n, p) <= d)

	Plane() : n(0.0f, 0.0f, 0.0f), d(0.0f) {}
	Plane(const bx::Vec3& _n, float _d) : n(_n), d(_d) {}
};

struct BrushVertex {
	float x, y, z;     // position
	float nx, ny, nz;  // normal
};

struct Brush {
	std::vector<Plane> planes;
	float color[3] = {0.8f, 0.8f, 0.8f};
};

// Axis-aligned box brush (6 planes).
Brush makeBox(const bx::Vec3& center, const bx::Vec3& halfExtent, float r, float g, float b);

// CSG hull build: convex brush -> triangle mesh (world-space positions + per-face normals).
void buildBrushMesh(const Brush& brush, std::vector<BrushVertex>& outVerts,
                    std::vector<uint16_t>& outIndices);
