// Static (no skeleton, no animation) 3D model loaded from FBX via ufbx, drawn with the world
// shader. Vertices are pre-normalized at load time so the model is centred in XY and its feet sit
// at z=0 — per-instance transform is just T(pos) * Rz(yaw).
#pragma once

#include <bgfx/bgfx.h>

struct Model {
	bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;
	uint32_t numIndices = 0;
	bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;  // diffuse; whiteTex used if invalid
	float bsCenter[3] = {0, 0, 0};                       // bounding sphere (pre-normalization)
	float bsRadius = 1.0f;
	bool ok() const { return bgfx::isValid(vbh) && bgfx::isValid(ibh) && numIndices > 0; }
	void destroy();
};

// Load an FBX into the given vertex layout (must include Position, Normal, TexCoord0, TexCoord1).
// targetHeight scales the model so its bounding-box height matches this many world units.
// yawOffset rotates the model around Z at load time so its authored "front" aligns with +X (the
// cooped yaw=0 direction) — typically -pi/2 for FBX models authored facing +Y. The diffuse
// texture is read from texturePath (Meshy ships a sibling PNG).
Model loadFbxModel(const char* path, const bgfx::VertexLayout& layout, const char* texturePath,
                   float targetHeight = 32.0f, float yawOffset = 0.0f);
