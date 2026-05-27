// Quake-style player physics: swept axis-aligned box vs convex brushes.
// The player is an AABB; collision uses the Minkowski trick (expand each brush plane
// outward by the box's support, then trace the box centre as a point).
#pragma once

#include <vector>

#include <bx/math.h>

#include "brush.h"

struct TraceResult {
	float    fraction = 1.0f;                 // how far along delta we can move (0..1)
	bx::Vec3 normal   = bx::Vec3(0, 0, 1);    // surface normal at the hit
	bool     hit      = false;
	bool     startSolid = false;              // the box started embedded in a brush
};

// Sweep an AABB (centre `start`, half-extents `half`) by `delta` against all brushes.
TraceResult traceBox(const std::vector<Brush>& brushes, const bx::Vec3& start,
                     const bx::Vec3& delta, const bx::Vec3& half);

// Move `pos` by `vel*dt` with collide-and-slide + stair step-up; updates pos & vel.
// Returns true if the player is standing on a floor afterwards.
bool movePlayer(const std::vector<Brush>& brushes, bx::Vec3& pos, bx::Vec3& vel,
                const bx::Vec3& half, float dt);
