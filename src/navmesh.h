// Recast/Detour navmesh built from the brush world, for server-side AI pathfinding (native only).
// cooped is Z-up; Recast/Detour are Y-up, so positions are Y/Z-swapped at this boundary.
#pragma once

#include <vector>

#include "brush.h"

class dtNavMesh;
class dtNavMeshQuery;

class NavMesh {
public:
	~NavMesh();
	// Build (or rebuild) the navmesh from the world brushes. Returns false if no walkable area.
	bool build(const std::vector<Brush>& brushes);
	bool valid() const { return m_query != nullptr; }

	// Random reachable point within `radius` of `around` (cooped coords). false if none found.
	bool randomPointAround(const float around[3], float radius, float out[3]) const;
	// Straight path from start to end (cooped coords) as flattened xyz waypoints. false if none.
	bool findPath(const float start[3], const float end[3], std::vector<float>& outPts) const;

	// Walkable surface as a triangle soup (cooped coords, 9 floats per triangle) for debug draw.
	const std::vector<float>& debugTriangles() const { return m_debugTris; }

private:
	void destroy();
	dtNavMesh* m_navMesh = nullptr;
	dtNavMeshQuery* m_query = nullptr;
	std::vector<float> m_debugTris;
};
