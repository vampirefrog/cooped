// Convex brush geometry: a brush is the intersection of half-spaces (planes).
// buildBrushMesh() runs the CSG hull build — clip each face plane by every other
// half-space to get the face polygon, then triangulate. Coordinates are Z-up,
// world-space, Quake-scale float units (see DESIGN.md §17).
#pragma once

#include <cstdint>
#include <vector>

#include <bx/math.h>

struct Plane {
	bx::Vec3 n;               // outward unit normal
	float    d;               // plane: dot(n, p) == d  (interior is dot(n, p) <= d)
	uint32_t textureId = 0;   // per-face texture index
	// Per-face UV alignment (applied to the planar projection).
	float uScale = 1.0f, vScale = 1.0f;   // larger = texture appears bigger (fewer repeats)
	float uOffset = 0.0f, vOffset = 0.0f; // in tiles
	float rotation = 0.0f;                // degrees

	Plane() : n(0.0f, 0.0f, 0.0f), d(0.0f) {}
	Plane(const bx::Vec3& _n, float _d) : n(_n), d(_d) {}
};

struct BrushVertex {
	float x, y, z;     // position
	float nx, ny, nz;  // normal
	float u, v;        // texture coords (planar-projected per face)
};

struct Brush {
	uint32_t id = 0;  // stable, server-assigned identifier (0 = unassigned)
	std::vector<Plane> planes;
	float color[3] = {0.8f, 0.8f, 0.8f};
};

// Index range for one face's triangles within a brush mesh (for per-face texture binding).
struct FaceRange {
	uint32_t firstIndex;
	uint32_t numIndices;
	int      face;  // index into Brush::planes
};

// Axis-aligned box brush (6 planes).
Brush makeBox(const bx::Vec3& center, const bx::Vec3& halfExtent, float r, float g, float b);

// CSG hull build: convex brush -> triangle mesh (world-space positions + per-face normals + UVs).
// outFaces records each face's index range so the renderer can bind a per-face texture.
void buildBrushMesh(const Brush& brush, std::vector<BrushVertex>& outVerts,
                    std::vector<uint16_t>& outIndices, std::vector<FaceRange>& outFaces);

struct RayHit {
	bool  hit  = false;
	float t    = 0.0f;   // distance along the ray to the nearest surface
	int   face = -1;     // index of the face (plane) the ray entered through
};

// Ray vs convex brush (slab clip against the half-spaces). Ray dir need not be normalized.
RayHit rayBrushIntersect(const bx::Vec3& ro, const bx::Vec3& rd, const Brush& brush);

// Translate a brush by delta (shift every plane's distance).
void translateBrush(Brush& brush, const bx::Vec3& delta);

// Convex polygon for one face (its plane clipped by every other half-space).
// Empty if the face is degenerate (e.g. clipped away).
std::vector<bx::Vec3> brushFacePolygon(const Brush& brush, size_t faceIndex);

// Triangle-list mesh for a single face, positions pushed out along the normal by 'bias'
// (used for the edit-mode face highlight overlay).
void buildFaceMesh(const Brush& brush, size_t faceIndex, float bias,
                   std::vector<BrushVertex>& outTris);
