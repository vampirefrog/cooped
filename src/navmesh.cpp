#include "navmesh.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

namespace {

// cooped (x,y,z, Z-up) <-> Recast (x,y,z, Y-up) is a Y/Z swap, which is its own inverse.
inline void swapYZ(const float in[3], float out[3]) { out[0] = in[0]; out[1] = in[2]; out[2] = in[1]; }

float frand() { return (float)rand() / ((float)RAND_MAX + 1.0f); }

// Agent + voxel sizing in cooped world units (Quake scale). Boxes are 64 tall, so a ~56-tall
// agent that can only climb 16 treats them as walls and paths around their footprints.
constexpr float kCellSize = 8.0f, kCellHeight = 4.0f;
constexpr float kAgentHeight = 56.0f, kAgentRadius = 16.0f, kAgentClimb = 16.0f;

}  // namespace

NavMesh::~NavMesh() { destroy(); }

void NavMesh::destroy() {
	if (m_query) { dtFreeNavMeshQuery(m_query); m_query = nullptr; }
	if (m_navMesh) { dtFreeNavMesh(m_navMesh); m_navMesh = nullptr; }
}

bool NavMesh::build(const std::vector<Brush>& brushes) {
	destroy();
	m_debugTris.clear();

	// World triangle soup in Recast (Y-up) space.
	std::vector<float> verts;
	std::vector<int> tris;
	for (const Brush& b : brushes) {
		std::vector<BrushVertex> bv;
		std::vector<uint16_t> bi;
		std::vector<FaceRange> faces;
		buildBrushMesh(b, bv, bi, faces);
		const int base = (int)(verts.size() / 3);
		for (const BrushVertex& v : bv) { verts.push_back(v.x); verts.push_back(v.z); verts.push_back(v.y); }
		for (uint16_t i : bi) tris.push_back(base + (int)i);
	}
	const int nverts = (int)(verts.size() / 3), ntris = (int)(tris.size() / 3);
	if (ntris == 0) return false;

	rcConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.cs = kCellSize;
	cfg.ch = kCellHeight;
	cfg.walkableSlopeAngle = 45.0f;
	cfg.walkableHeight = (int)ceilf(kAgentHeight / cfg.ch);
	cfg.walkableClimb = (int)floorf(kAgentClimb / cfg.ch);
	cfg.walkableRadius = (int)ceilf(kAgentRadius / cfg.cs);
	cfg.maxEdgeLen = (int)(12.0f);
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = (int)rcSqr(8);
	cfg.mergeRegionArea = (int)rcSqr(20);
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = cfg.cs * 6.0f;
	cfg.detailSampleMaxError = cfg.ch * 1.0f;
	rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
	rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

	rcContext ctx(false);
	rcHeightfield* solid = rcAllocHeightfield();
	if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
		{ if (solid) rcFreeHeightField(solid); return false; }

	std::vector<unsigned char> areas(ntris, 0);
	rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
	if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *solid, cfg.walkableClimb))
		{ rcFreeHeightField(solid); return false; }

	rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
	rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

	rcCompactHeightfield* chf = rcAllocCompactHeightfield();
	if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
		{ rcFreeHeightField(solid); if (chf) rcFreeCompactHeightfield(chf); return false; }
	rcFreeHeightField(solid);

	if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) ||
	    !rcBuildDistanceField(&ctx, *chf) ||
	    !rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
		{ rcFreeCompactHeightfield(chf); return false; }

	rcContourSet* cset = rcAllocContourSet();
	if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
		{ rcFreeCompactHeightfield(chf); if (cset) rcFreeContourSet(cset); return false; }

	rcPolyMesh* pmesh = rcAllocPolyMesh();
	if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
		{ rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); if (pmesh) rcFreePolyMesh(pmesh); return false; }

	rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
	if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
		{ rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); if (dmesh) rcFreePolyMeshDetail(dmesh); return false; }
	rcFreeCompactHeightfield(chf);
	rcFreeContourSet(cset);

	// Capture the detail-mesh triangles (cooped coords) for the client's debug overlay.
	for (int i = 0; i < dmesh->nmeshes; ++i) {
		const unsigned int* msh = &dmesh->meshes[i * 4];
		const unsigned int bverts = msh[0], btris = msh[2], ntris = msh[3];
		for (unsigned int j = 0; j < ntris; ++j) {
			const unsigned char* t = &dmesh->tris[(btris + j) * 4];
			for (int k = 0; k < 3; ++k) {
				float c[3]; swapYZ(&dmesh->verts[(bverts + t[k]) * 3], c);
				m_debugTris.push_back(c[0]); m_debugTris.push_back(c[1]); m_debugTris.push_back(c[2]);
			}
		}
	}

	// Mark every walkable poly with flag 1 (the query filter includes it).
	for (int i = 0; i < pmesh->npolys; ++i)
		if (pmesh->areas[i] == RC_WALKABLE_AREA) { pmesh->areas[i] = 0; pmesh->flags[i] = 1; }

	dtNavMeshCreateParams params;
	memset(&params, 0, sizeof(params));
	params.verts = pmesh->verts;
	params.vertCount = pmesh->nverts;
	params.polys = pmesh->polys;
	params.polyAreas = pmesh->areas;
	params.polyFlags = pmesh->flags;
	params.polyCount = pmesh->npolys;
	params.nvp = pmesh->nvp;
	params.detailMeshes = dmesh->meshes;
	params.detailVerts = dmesh->verts;
	params.detailVertsCount = dmesh->nverts;
	params.detailTris = dmesh->tris;
	params.detailTriCount = dmesh->ntris;
	params.walkableHeight = kAgentHeight;
	params.walkableRadius = kAgentRadius;
	params.walkableClimb = kAgentClimb;
	rcVcopy(params.bmin, pmesh->bmin);
	rcVcopy(params.bmax, pmesh->bmax);
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;

	unsigned char* navData = nullptr;
	int navDataSize = 0;
	const bool created = dtCreateNavMeshData(&params, &navData, &navDataSize);
	rcFreePolyMesh(pmesh);
	rcFreePolyMeshDetail(dmesh);
	if (!created) return false;

	m_navMesh = dtAllocNavMesh();
	if (!m_navMesh || dtStatusFailed(m_navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA)))
		{ dtFree(navData); destroy(); return false; }
	m_query = dtAllocNavMeshQuery();
	if (!m_query || dtStatusFailed(m_query->init(m_navMesh, 2048))) { destroy(); return false; }
	return true;
}

bool NavMesh::randomPointAround(const float around[3], float radius, float out[3]) const {
	if (!m_query) return false;
	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);
	float c[3]; swapYZ(around, c);
	const float ext[3] = {64.0f, 512.0f, 64.0f};
	dtPolyRef cref = 0; float cnap[3];
	if (dtStatusFailed(m_query->findNearestPoly(c, ext, &filter, &cref, cnap)) || !cref) return false;
	dtPolyRef rref = 0; float pt[3];
	if (dtStatusFailed(m_query->findRandomPointAroundCircle(cref, cnap, radius, &filter, frand, &rref, pt)) || !rref)
		return false;
	swapYZ(pt, out);
	return true;
}

bool NavMesh::findPath(const float start[3], const float end[3], std::vector<float>& outPts) const {
	outPts.clear();
	if (!m_query) return false;
	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);
	float s[3], e[3]; swapYZ(start, s); swapYZ(end, e);
	const float ext[3] = {64.0f, 512.0f, 64.0f};
	dtPolyRef sref = 0, eref = 0; float snap[3], enap[3];
	m_query->findNearestPoly(s, ext, &filter, &sref, snap);
	m_query->findNearestPoly(e, ext, &filter, &eref, enap);
	if (!sref || !eref) return false;

	dtPolyRef polys[256];
	int npolys = 0;
	if (dtStatusFailed(m_query->findPath(sref, eref, snap, enap, &filter, polys, &npolys, 256)) || npolys == 0)
		return false;
	// If the path didn't reach the end poly it's partial; aim for the last poly's nearest point.
	float target[3]; dtVcopy(target, enap);
	if (polys[npolys - 1] != eref) m_query->closestPointOnPoly(polys[npolys - 1], enap, target, nullptr);

	float straight[256 * 3];
	unsigned char flags[256];
	dtPolyRef refs[256];
	int ns = 0;
	if (dtStatusFailed(m_query->findStraightPath(snap, target, polys, npolys, straight, flags, refs, &ns, 256)) || ns == 0)
		return false;
	outPts.reserve(ns * 3);
	for (int i = 0; i < ns; ++i) {
		float p[3]; swapYZ(&straight[i * 3], p);
		outPts.push_back(p[0]); outPts.push_back(p[1]); outPts.push_back(p[2]);
	}
	return true;
}
