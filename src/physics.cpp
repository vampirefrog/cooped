#include "physics.h"

namespace {

constexpr float kSurfEps   = 0.03f;  // back-off from surfaces to avoid re-penetration
constexpr float kStepH     = 18.0f;  // max stair height the player can step up
constexpr float kFloorDotZ = 0.7f;   // normal.z above this counts as walkable floor

inline bx::Vec3 scl(const bx::Vec3& v, float s) { return bx::mul(v, s); }
inline float horizDist(const bx::Vec3& a, const bx::Vec3& b) {
	const float dx = a.x - b.x, dy = a.y - b.y;
	return bx::sqrt(dx * dx + dy * dy);
}

// One collide-and-slide pass: move pos by vel*dt, clipping/sliding off up to 4 planes.
void slide(const std::vector<Brush>& brushes, bx::Vec3& pos, bx::Vec3& vel,
           const bx::Vec3& half, float dt, bool& onGround) {
	float timeLeft = dt;
	for (int it = 0; it < 4; ++it) {
		const bx::Vec3 delta = scl(vel, timeLeft);
		if (bx::length(delta) < 1.0e-5f) break;
		const TraceResult tr = traceBox(brushes, pos, delta, half);
		if (tr.fraction > 0.0f && !tr.startSolid) pos = bx::add(pos, scl(delta, tr.fraction));
		if (!tr.hit) break;
		if (tr.normal.z > kFloorDotZ) onGround = true;
		pos = bx::add(pos, scl(tr.normal, kSurfEps));        // nudge off the plane
		vel = bx::sub(vel, scl(tr.normal, bx::dot(vel, tr.normal)));  // slide
		timeLeft -= timeLeft * tr.fraction;
		if (timeLeft <= 0.0f) break;
	}
}

}  // namespace

TraceResult traceBox(const std::vector<Brush>& brushes, const bx::Vec3& start,
                     const bx::Vec3& delta, const bx::Vec3& half) {
	TraceResult best;
	const bx::Vec3 end = bx::add(start, delta);

	for (const Brush& brush : brushes) {
		float enter = 0.0f, leave = 1.0f;
		bx::Vec3 enterN(0, 0, 1);
		bool gotEnter = false, startedOutside = false, missed = false;
		float maxDS = -1.0e30f;
		bx::Vec3 maxDSN(0, 0, 1);

		for (const Plane& pl : brush.planes) {
			const float dexp = pl.d + (half.x * bx::abs(pl.n.x) + half.y * bx::abs(pl.n.y) +
			                           half.z * bx::abs(pl.n.z));
			const float dS = bx::dot(pl.n, start) - dexp;  // >0 = outside this expanded half-space
			const float dE = bx::dot(pl.n, end) - dexp;
			if (dS > maxDS) { maxDS = dS; maxDSN = pl.n; }
			if (dS > 0.0f) startedOutside = true;
			if (dS > 0.0f && dE > 0.0f) { missed = true; break; }  // segment fully outside the brush
			if (dS <= 0.0f && dE <= 0.0f) continue;                 // fully inside this half-space
			const float t = dS / (dS - dE);
			if (dS > 0.0f) {            // crossing inward
				if (t > enter) { enter = t; enterN = pl.n; gotEnter = true; }
			} else {                   // crossing outward
				if (t < leave) leave = t;
			}
		}
		if (missed) continue;

		if (!startedOutside) {  // box centre began inside this brush
			best.startSolid = true;
			best.hit = true;
			best.fraction = 0.0f;
			best.normal = maxDSN;  // least-penetrating plane: push out along it
			return best;
		}
		if (gotEnter && enter <= leave && enter >= 0.0f && enter < best.fraction) {
			best.fraction = enter;
			best.normal = enterN;
			best.hit = true;
		}
	}
	return best;
}

bool movePlayer(const std::vector<Brush>& brushes, bx::Vec3& pos, bx::Vec3& vel,
                const bx::Vec3& half, float dt) {
	const bx::Vec3 startPos = pos, startVel = vel;

	// (1) plain slide
	bx::Vec3 p1 = pos, v1 = vel;
	bool g1 = false;
	slide(brushes, p1, v1, half, dt, g1);

	// (2) step-up attempt: rise up to kStepH, slide, then drop back down
	const TraceResult up = traceBox(brushes, startPos, {0, 0, kStepH}, half);
	bx::Vec3 pUp = bx::add(startPos, bx::Vec3(0, 0, kStepH * up.fraction));
	bx::Vec3 pStep = pUp, vStep = startVel;
	bool gStep = false;
	slide(brushes, pStep, vStep, half, dt, gStep);
	const TraceResult down = traceBox(brushes, pStep, {0, 0, -kStepH}, half);
	pStep = bx::add(pStep, bx::Vec3(0, 0, -kStepH * down.fraction));
	const bool steppedOntoFloor = down.hit && down.normal.z > kFloorDotZ;

	// Keep whichever advanced further horizontally (prefer the step if it landed on floor).
	if (steppedOntoFloor && horizDist(startPos, pStep) > horizDist(startPos, p1) + 0.01f) {
		pos = pStep;
		vel = vStep;
		vel.z = v1.z;  // don't keep the upward step velocity
		return true;
	}
	pos = p1;
	vel = v1;
	return g1;
}
