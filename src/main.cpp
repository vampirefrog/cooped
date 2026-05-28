// cooped — milestone 3
// In-game editor: E toggles edit/play, a grid-snapped fly camera, crosshair brush
// targeting + click-select, create/delete/move brushes, and undo/redo. The brush
// list is the source of truth; meshes are rebuilt when it changes.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/math.h>

#include <cstdio>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#include "brush.h"
#include "net.h"
#include "physics.h"
#include "protocol.h"
#include "model.h"
#if !defined(__EMSCRIPTEN__)
#include "bake.h"
#endif

#include "shaders/generated/glsl/vs_world.sc.bin.h"
#include "shaders/generated/essl/vs_world.sc.bin.h"
#include "shaders/generated/spirv/vs_world.sc.bin.h"
#include "shaders/generated/glsl/fs_world.sc.bin.h"
#include "shaders/generated/essl/fs_world.sc.bin.h"
#include "shaders/generated/spirv/fs_world.sc.bin.h"
#include "shaders/generated/glsl/vs_sky.sc.bin.h"
#include "shaders/generated/essl/vs_sky.sc.bin.h"
#include "shaders/generated/spirv/vs_sky.sc.bin.h"
#include "shaders/generated/glsl/fs_sky.sc.bin.h"
#include "shaders/generated/essl/fs_sky.sc.bin.h"
#include "shaders/generated/spirv/fs_sky.sc.bin.h"

namespace {

constexpr float kGridExtent = 2048.0f;  // half-size of the rendered grid
constexpr int   kMaxLights  = 32;        // must match fs_world.sc
constexpr float kLightHalf  = 16.0f;     // half-size of a light's editable wireframe cube
const float kPlaceColors[5][3] = {
    {1.0f, 1.0f, 1.0f}, {1.0f, 0.4f, 0.3f}, {0.4f, 1.0f, 0.5f}, {0.5f, 0.6f, 1.0f}, {1.0f, 0.85f, 0.5f}};

// Player physics constants (Quake-scale units).
const bx::Vec3 kPlayerHalf(16.0f, 16.0f, 28.0f);  // 32 x 32 x 56 AABB
constexpr float kEyeOffset = 20.0f;   // eye height above the player centre
constexpr float kGravity   = 800.0f;   // jump felt fine — leave vertical alone
constexpr float kJumpSpeed = 270.0f;
constexpr float kMaxSpeed  = 320.0f;
constexpr float kGroundAccel = 18.0f;  // reach/change speed fast (was 10 — felt floaty horizontally)
constexpr float kAirAccel    = 8.0f;   // strong air steering (was 1 = near-locked momentum)
constexpr float kFriction    = 12.0f;  // stop quickly when keys released (was 6 — slidey)

bgfx::ShaderHandle makeShader(const uint8_t* data, uint32_t size) {
	return bgfx::createShader(bgfx::copy(data, size));
}

// Load + decode an image file (jpg/png) into an RGBA8 texture. Invalid handle on failure.
bgfx::TextureHandle loadTexture(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return BGFX_INVALID_HANDLE;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> buf(size > 0 ? (size_t)size : 0);
	const size_t got = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
	fclose(f);
	if (got != buf.size() || buf.empty()) return BGFX_INVALID_HANDLE;

	static bx::DefaultAllocator alloc;
	bimg::ImageContainer* img =
	    bimg::imageParse(&alloc, buf.data(), (uint32_t)buf.size(), bimg::TextureFormat::RGBA8);
	if (!img) return BGFX_INVALID_HANDLE;
	bgfx::TextureHandle h = bgfx::createTexture2D(
	    (uint16_t)img->m_width, (uint16_t)img->m_height, false, 1, bgfx::TextureFormat::RGBA8, 0,
	    bgfx::copy(img->m_data, img->m_size));
	bimg::imageFree(img);
	return h;
}

bgfx::ProgramHandle createWorldProgram() {
	const uint8_t *vs, *fs;
	uint32_t vsn, fsn;
	switch (bgfx::getRendererType()) {
		case bgfx::RendererType::Vulkan:
			vs = vs_world_spv;  vsn = sizeof(vs_world_spv);  fs = fs_world_spv;  fsn = sizeof(fs_world_spv);  break;
		case bgfx::RendererType::OpenGLES:
			vs = vs_world_essl; vsn = sizeof(vs_world_essl); fs = fs_world_essl; fsn = sizeof(fs_world_essl); break;
		default:
			vs = vs_world_glsl; vsn = sizeof(vs_world_glsl); fs = fs_world_glsl; fsn = sizeof(fs_world_glsl); break;
	}
	return bgfx::createProgram(makeShader(vs, vsn), makeShader(fs, fsn), true);
}

bgfx::ProgramHandle createSkyProgram() {
	const uint8_t *vs, *fs;
	uint32_t vsn, fsn;
	switch (bgfx::getRendererType()) {
		case bgfx::RendererType::Vulkan:
			vs = vs_sky_spv;  vsn = sizeof(vs_sky_spv);  fs = fs_sky_spv;  fsn = sizeof(fs_sky_spv);  break;
		case bgfx::RendererType::OpenGLES:
			vs = vs_sky_essl; vsn = sizeof(vs_sky_essl); fs = fs_sky_essl; fsn = sizeof(fs_sky_essl); break;
		default:
			vs = vs_sky_glsl; vsn = sizeof(vs_sky_glsl); fs = fs_sky_glsl; fsn = sizeof(fs_sky_glsl); break;
	}
	return bgfx::createProgram(makeShader(vs, vsn), makeShader(fs, fsn), true);
}

// Load a Radiance .hdr equirectangular map as a half-float texture (V clamped for the poles).
bgfx::TextureHandle loadHdrTexture(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return BGFX_INVALID_HANDLE;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> buf(size > 0 ? (size_t)size : 0);
	const size_t got = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
	fclose(f);
	if (got != buf.size() || buf.empty()) return BGFX_INVALID_HANDLE;
	static bx::DefaultAllocator alloc;
	bimg::ImageContainer* img =
	    bimg::imageParse(&alloc, buf.data(), (uint32_t)buf.size(), bimg::TextureFormat::RGBA16F);
	if (!img) return BGFX_INVALID_HANDLE;
	bgfx::TextureHandle h = bgfx::createTexture2D(
	    (uint16_t)img->m_width, (uint16_t)img->m_height, false, 1, bgfx::TextureFormat::RGBA16F,
	    BGFX_SAMPLER_V_CLAMP, bgfx::copy(img->m_data, img->m_size));
	bimg::imageFree(img);
	return h;
}

// A distinct-ish color per player id.
void colorFromId(uint32_t id, float out[4]) {
	out[0] = 0.35f + 0.6f * ((id * 73 % 100) / 100.0f);
	out[1] = 0.35f + 0.6f * ((id * 151 % 100) / 100.0f);
	out[2] = 0.35f + 0.6f * ((id * 199 % 100) / 100.0f);
	out[3] = 1.0f;
}

struct RemotePlayer {
	bx::Vec3 pos = bx::Vec3(0, 0, 0);
	float yaw = 0.0f, pitch = 0.0f;
};

struct AiAgent {  // server-driven wandering AI; pos is its feet on the navmesh
	bx::Vec3 pos = bx::Vec3(0, 0, 0);
	float yaw = 0.0f;
	uint32_t hp = 0;
};

const bx::Vec3 kAgentHalf(10.0f, 10.0f, 16.0f);  // a smaller cube than the player avatar
constexpr uint32_t kAgentMaxHp = 30;             // mirror the server (hp / kAgentMaxHp = bar fill)

struct Mesh {
	bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;
	uint32_t numIndices = 0;
	std::vector<FaceRange> faces;  // per-face index ranges (for per-face texture binding)
	float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};

	void destroy() {
		if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
		if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
		vbh = BGFX_INVALID_HANDLE;
		ibh = BGFX_INVALID_HANDLE;
		numIndices = 0;
	}
};

struct Camera {
	bx::Vec3 pos{0.0f, -360.0f, 160.0f};
	float yaw = bx::kPiHalf, pitch = -0.35f;
	bx::Vec3 forward() const {
		const float cp = bx::cos(pitch);
		return {cp * bx::cos(yaw), cp * bx::sin(yaw), bx::sin(pitch)};
	}
};

struct App {
	SDL_Window* window = nullptr;
	uint32_t width = 1280, height = 720;
	bool bgfxInitialized = false;

	bgfx::VertexLayout layout;
	bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle u_albedo = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
	std::vector<bgfx::TextureHandle> textures;            // loaded texture set
	bgfx::TextureHandle whiteTex = BGFX_INVALID_HANDLE;   // 1x1 white (untextured draws)
	// HDR skybox.
	bgfx::ProgramHandle skyProgram = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle skyTex[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
	int skyIdx = 0;
	bgfx::UniformHandle s_sky = BGFX_INVALID_HANDLE, u_skyParams = BGFX_INVALID_HANDLE;
	bgfx::VertexBufferHandle fullscreen = BGFX_INVALID_HANDLE;  // fullscreen triangle
	// Lighting uniforms.
	bgfx::UniformHandle u_lightParams = BGFX_INVALID_HANDLE;     // x=count, y=ambient
	bgfx::UniformHandle u_sunDir = BGFX_INVALID_HANDLE;          // xyz dir, w intensity
	bgfx::UniformHandle u_lightPosRadius = BGFX_INVALID_HANDLE;  // [kMaxLights] xyz+radius
	bgfx::UniformHandle u_lightColor = BGFX_INVALID_HANDLE;      // [kMaxLights] rgb+intensity
	std::vector<Light> lights;
	// Baked lightmap (from F6). When present, brushes sample it instead of dynamic lighting.
	bgfx::TextureHandle lightmapTex = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle s_lightmap = BGFX_INVALID_HANDLE;
	bool hasLightmap = false;
#if !defined(__EMSCRIPTEN__)
	// Async F6 bake: runs on a worker thread so the UI doesn't freeze; result uploaded on the
	// main thread when done (bgfx is single-threaded). Native only.
	std::thread bakeThread;
	std::atomic<bool> bakeRunning{false};   // a bake is in flight (ignore new F6)
	std::atomic<bool> bakeDone{false};       // worker finished; main thread should upload bakeResult
	BakeResult bakeResult;                    // written by the worker, read by main after bakeDone
#endif
	bgfx::VertexBufferHandle lightWire = BGFX_INVALID_HANDLE;   // unit wireframe cube (line list)
	uint32_t lightWireVerts = 0;
	int placeColorIdx = 0;                                      // current light color to place

	// Light targeting + Sauerbraten-style drag.
	int targetedLight = -1;                       // light whose cube the crosshair is on
	bx::Vec3 targetedLightNormal = bx::Vec3(0, 0, 1);
	uint32_t dragLightId = 0;                     // light being dragged (0 = none)
	bx::Vec3 dragPlaneN = bx::Vec3(0, 0, 1);      // drag plane normal (clicked face)
	bx::Vec3 dragPlanePoint = bx::Vec3(0, 0, 0);  // a fixed point on the drag plane
	bx::Vec3 dragOffset = bx::Vec3(0, 0, 0);      // grab point minus light centre

	std::vector<Brush> brushes;   // source of truth
	std::vector<Mesh>  meshes;    // rebuilt from brushes when dirty
	Mesh grid;

	std::vector<std::vector<Brush>> undoStack, redoStack;

	// Networking (Milestone 5). When online, edits are sent to the server and applied on echo.
	NetClient* net = nullptr;
	bool online = false;
	bool selectOnNextCreate = false;  // select the next CreateBrush echo as ours
	uint32_t localNextId = 1;         // id source for single-player (offline) brushes

	// Player presence.
	uint32_t myPlayerId = 0;                          // assigned by the server
	bool versionMismatch = false;                     // server protocol version != ours
	std::map<uint32_t, RemotePlayer> remotePlayers;   // other players, by id
	Mesh avatarMesh;                                  // body-sized box for rendering them
	std::map<uint32_t, AiAgent> agents;               // server-driven AI agents, by id
	Mesh agentMesh;                                   // fallback cube if a character model is missing
	std::vector<Model> npcModels;                     // characters; each agent picks one by id
	bgfx::VertexBufferHandle navDbgVbh = BGFX_INVALID_HANDLE;  // navmesh wireframe (line list)
	uint32_t navDbgVerts = 0;
	bool showNavMesh = false;                         // N toggles the navmesh debug overlay
	// Hitscan firing (play mode, LMB): cooldown + a brief tracer line for visual feedback.
	uint64_t nextFireNs = 0;
	uint64_t tracerExpireNs = 0;
	bx::Vec3 tracerStart = bx::Vec3(0, 0, 0);
	bx::Vec3 tracerEnd = bx::Vec3(0, 0, 0);
	uint64_t lastStateSendNs = 0;

	Camera cam;
	// Play-mode player body (collision/gravity); in edit mode cam.pos is a free-fly camera.
	bx::Vec3 playerPos{0, 0, 64};
	bx::Vec3 playerVel{0, 0, 0};
	bool onGround = false;

	bool editMode = true;
	uint32_t selectedId = 0;     // selected brush id (0 = none) — id-based so it survives net edits
	int  selectedFace = -1;      // selected face on that brush (the push/pull target)
	int  targeted = -1;          // brush index under the crosshair this frame
	int  targetedFace = -1;      // face of that brush under the crosshair
	uint32_t ppId = 0; int ppFace = -1;  // active push/pull face, for undo coalescing
	float gridStep = 64.0f;
	float lightmapTexelsPerUnit = 1.0f / 32.0f;  // bake density (H + wheel); 1/this = units per texel

	float mouseDx = 0.0f, mouseDy = 0.0f;
	uint64_t lastTicksNs = 0;
};

float snapToGrid(float v, float step) { return bx::round(v / step) * step; }

// Index of the brush with the given id, or -1.
int indexOfId(const std::vector<Brush>& brushes, uint32_t id) {
	if (id == 0) return -1;
	for (int i = 0; i < (int)brushes.size(); ++i)
		if (brushes[i].id == id) return i;
	return -1;
}

// Quake acceleration: ramp velocity toward wishDir up to wishSpeed.
void accelerate(bx::Vec3& vel, const bx::Vec3& wishDir, float wishSpeed, float accel, float dt) {
	const float current = bx::dot(vel, wishDir);
	const float add = wishSpeed - current;
	if (add <= 0.0f) return;
	float accelSpeed = accel * dt * wishSpeed;
	if (accelSpeed > add) accelSpeed = add;
	vel = bx::add(vel, bx::mul(wishDir, accelSpeed));
}

Mesh buildMeshFromBrush(const Brush& brush, const bgfx::VertexLayout& layout) {
	std::vector<BrushVertex> verts;
	std::vector<uint16_t> indices;
	std::vector<FaceRange> faces;
	buildBrushMesh(brush, verts, indices, faces);
	Mesh m;
	if (!verts.empty() && !indices.empty()) {
		m.vbh = bgfx::createVertexBuffer(
		    bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(BrushVertex))), layout);
		m.ibh = bgfx::createIndexBuffer(
		    bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(uint16_t))));
		m.numIndices = uint32_t(indices.size());
		m.faces = std::move(faces);
	}
	m.color[0] = brush.color[0];
	m.color[1] = brush.color[1];
	m.color[2] = brush.color[2];
	return m;
}

void rebuildSceneMeshes(App* app) {
	for (Mesh& m : app->meshes) m.destroy();
	app->meshes.clear();
	app->meshes.reserve(app->brushes.size());
	for (const Brush& b : app->brushes) app->meshes.push_back(buildMeshFromBrush(b, app->layout));
	app->hasLightmap = false;  // meshes rebuilt without lightmap UVs -> bake is stale
}

// Rebuild all brush meshes with lightmap UVs from a bake (soup-vertex order matches the bake).
void rebuildSceneMeshesLightmap(App* app, const std::vector<float>& vertexUV) {
	for (Mesh& m : app->meshes) m.destroy();
	app->meshes.clear();
	app->meshes.reserve(app->brushes.size());
	uint32_t soupBase = 0;
	for (const Brush& b : app->brushes) {
		std::vector<BrushVertex> verts;
		std::vector<uint16_t> indices;
		std::vector<FaceRange> faces;
		buildBrushMesh(b, verts, indices, faces);
		for (uint32_t k = 0; k < verts.size(); ++k) {
			const uint32_t s = soupBase + k;
			if (s * 2 + 1 < vertexUV.size()) { verts[k].lu = vertexUV[s * 2]; verts[k].lv = vertexUV[s * 2 + 1]; }
		}
		soupBase += (uint32_t)verts.size();
		Mesh m;
		if (!verts.empty() && !indices.empty()) {
			m.vbh = bgfx::createVertexBuffer(
			    bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(BrushVertex))), app->layout);
			m.ibh = bgfx::createIndexBuffer(
			    bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(uint16_t))));
			m.numIndices = uint32_t(indices.size());
			m.faces = std::move(faces);
		}
		m.color[0] = b.color[0]; m.color[1] = b.color[1]; m.color[2] = b.color[2];
		app->meshes.push_back(std::move(m));
	}
}

// Upload an RGBM lightmap atlas and rebind the meshes' lightmap UVs (used by a local F6 bake
// and by a lightmap received from the server — the latter works on web, no Embree needed).
void applyLightmapTexture(App* app, uint32_t w, uint32_t h, const std::vector<uint8_t>& pixels,
                          const std::vector<float>& vertexUV) {
	if (pixels.size() != (size_t)w * h * 4) return;
	if (bgfx::isValid(app->lightmapTex)) bgfx::destroy(app->lightmapTex);
	app->lightmapTex = bgfx::createTexture2D((uint16_t)w, (uint16_t)h, false, 1,
	    bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
	    bgfx::copy(pixels.data(), (uint32_t)pixels.size()));
	rebuildSceneMeshesLightmap(app, vertexUV);
	app->hasLightmap = true;
}

// Build the navmesh debug overlay: turn the walkable triangle soup into a line list (each
// triangle's 3 edges), lifted slightly off the floor so it reads over the surface.
void buildNavDebugMesh(App* app, const std::vector<float>& tris) {
	if (bgfx::isValid(app->navDbgVbh)) { bgfx::destroy(app->navDbgVbh); app->navDbgVbh = BGFX_INVALID_HANDLE; }
	app->navDbgVerts = 0;
	const size_t ntri = tris.size() / 9;
	if (ntri == 0) return;
	std::vector<BrushVertex> v;
	v.reserve(ntri * 6);
	auto pt = [&](size_t i) { return bx::Vec3(tris[i * 3 + 0], tris[i * 3 + 1], tris[i * 3 + 2] + 1.5f); };
	auto edge = [&](const bx::Vec3& a, const bx::Vec3& b) {
		v.push_back({a.x, a.y, a.z, 0, 0, 1, 0, 0});
		v.push_back({b.x, b.y, b.z, 0, 0, 1, 0, 0});
	};
	for (size_t t = 0; t < ntri; ++t) {
		const bx::Vec3 a = pt(t * 3 + 0), b = pt(t * 3 + 1), c = pt(t * 3 + 2);
		edge(a, b); edge(b, c); edge(c, a);
	}
	app->navDbgVbh = bgfx::createVertexBuffer(bgfx::copy(v.data(), uint32_t(v.size() * sizeof(BrushVertex))), app->layout);
	app->navDbgVerts = (uint32_t)v.size();
}

void rebuildGrid(App* app) {
	app->grid.destroy();
	std::vector<BrushVertex> verts;
	std::vector<uint16_t> idx;
	const float e = kGridExtent;
	const float step = app->gridStep;
	auto addLine = [&](bx::Vec3 a, bx::Vec3 b) {
		const uint16_t base = (uint16_t)verts.size();
		verts.push_back({a.x, a.y, a.z, 0, 0, 1});
		verts.push_back({b.x, b.y, b.z, 0, 0, 1});
		idx.push_back(base);
		idx.push_back(base + 1);
	};
	for (float c = -e; c <= e + 0.5f; c += step) {
		addLine({c, -e, 0.02f}, {c, e, 0.02f});  // lines parallel to Y
		addLine({-e, c, 0.02f}, {e, c, 0.02f});  // lines parallel to X
	}
	app->grid.vbh = bgfx::createVertexBuffer(
	    bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(BrushVertex))), app->layout);
	app->grid.ibh = bgfx::createIndexBuffer(
	    bgfx::copy(idx.data(), uint32_t(idx.size() * sizeof(uint16_t))));
	app->grid.numIndices = uint32_t(idx.size());
}

void resetPushPull(App* app) { app->ppId = 0; app->ppFace = -1; }

void sendMsg(App* app, const std::vector<uint8_t>& m) {
	netSend(app->net, m.data(), m.size(), true);
}

// Fire a hitscan shot if the cooldown has elapsed: send the shot to the server (which resolves
// damage authoritatively) and draw a brief local tracer to the nearest brush hit for feedback.
void tryFire(App* app) {
	const uint64_t now = SDL_GetTicksNS();
	if (now < app->nextFireNs) return;
	app->nextFireNs = now + 250'000'000ULL;   // 4 shots/sec
	const bx::Vec3 fwd = app->cam.forward();
	float tMax = 5000.0f;
	for (const Brush& b : app->brushes) {
		const RayHit h = rayBrushIntersect(app->cam.pos, fwd, b);
		if (h.hit && h.t > 0.0f && h.t < tMax) tMax = h.t;
	}
	// Offset the tracer start down-right from the eye (a "muzzle" position) so the line is
	// visible from a couple of pixels off-screen-center instead of a single point straight ahead.
	const bx::Vec3 right = bx::normalize(bx::cross(bx::Vec3(0, 0, 1), fwd));
	app->tracerStart = bx::add(bx::add(app->cam.pos, bx::mul(right, 4.0f)), bx::Vec3(0, 0, -4.0f));
	app->tracerEnd = bx::add(app->cam.pos, bx::mul(fwd, tMax));
	app->tracerExpireNs = now + 80'000'000ULL;  // ~80 ms streak
	sendMsg(app, msgPlayerFire(app->cam.pos, fwd));
}

void pushUndo(App* app) {
	app->undoStack.push_back(app->brushes);
	app->redoStack.clear();
	if (app->undoStack.size() > 128) app->undoStack.erase(app->undoStack.begin());
}

// Undo/redo are local-only (single-player). Collaborative undo isn't supported in Milestone 5.
void undo(App* app) {
	if (app->online || app->undoStack.empty()) return;
	app->redoStack.push_back(app->brushes);
	app->brushes = app->undoStack.back();
	app->undoStack.pop_back();
	app->selectedId = 0;
	app->selectedFace = -1;
	resetPushPull(app);
	rebuildSceneMeshes(app);
}

void redo(App* app) {
	if (app->online || app->redoStack.empty()) return;
	app->undoStack.push_back(app->brushes);
	app->brushes = app->redoStack.back();
	app->redoStack.pop_back();
	app->selectedId = 0;
	app->selectedFace = -1;
	resetPushPull(app);
	rebuildSceneMeshes(app);
}

// Sauerbraten-style: push (out) or pull (in) the SELECTED face along its normal by one grid
// step. No clamp — pulling past the brush's vertices cuts it, and pushing back restores it.
// Online: send the new plane distance to the server (applied on echo). Offline: mutate locally.
void pushPullFace(App* app, int dir) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	const int fi = app->selectedFace;
	if (bi < 0 || fi < 0 || fi >= (int)app->brushes[bi].planes.size()) return;
	const float newD = app->brushes[bi].planes[fi].d + dir * app->gridStep;
	if (app->online) {
		sendMsg(app, msgSetPlaneD(app->selectedId, (uint32_t)fi, newD));
		return;
	}
	if (app->selectedId != app->ppId || fi != app->ppFace) {  // coalesce a scroll session into one undo
		pushUndo(app);
		app->ppId = app->selectedId;
		app->ppFace = fi;
	}
	app->brushes[bi].planes[fi].d = newD;
	app->meshes[bi].destroy();
	app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;  // may be empty if collapsed
}

// Where the crosshair ray meets the z=0 plane (for brush placement).
bool aimGroundPoint(const App* app, const bx::Vec3& fwd, bx::Vec3& out) {
	if (bx::abs(fwd.z) < 1.0e-5f) return false;
	const float t = (0.0f - app->cam.pos.z) / fwd.z;
	if (t <= 0.0f) return false;
	out = bx::add(app->cam.pos, bx::mul(fwd, t));
	return true;
}

void createBrushAtAim(App* app) {
	bx::Vec3 g(0.0f, 0.0f, 0.0f);
	if (!aimGroundPoint(app, app->cam.forward(), g)) return;
	const float s = app->gridStep;
	const bx::Vec3 center{snapToGrid(g.x, s), snapToGrid(g.y, s), s * 0.5f};
	Brush b = makeBox(center, {s * 0.5f, s * 0.5f, s * 0.5f}, 0.70f, 0.55f, 0.40f);
	if (app->online) {
		app->selectOnNextCreate = true;  // server assigns the id; select it when it echoes back
		sendMsg(app, msgCreateBrush(b));
		return;
	}
	pushUndo(app);
	b.id = app->localNextId++;
	app->brushes.push_back(b);
	app->selectedId = b.id;
	app->selectedFace = -1;
	resetPushPull(app);
	rebuildSceneMeshes(app);
}

void deleteSelected(App* app) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	if (bi < 0) return;
	if (app->online) {
		sendMsg(app, msgDeleteBrush(app->selectedId));
		app->selectedId = 0;
		app->selectedFace = -1;
		resetPushPull(app);
		return;
	}
	pushUndo(app);
	app->brushes.erase(app->brushes.begin() + bi);
	app->selectedId = 0;
	app->selectedFace = -1;
	resetPushPull(app);
	rebuildSceneMeshes(app);
}

void nudgeSelected(App* app, const bx::Vec3& dir) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	if (bi < 0) return;
	const bx::Vec3 delta = bx::mul(dir, app->gridStep);
	if (app->online) {
		sendMsg(app, msgTranslateBrush(app->selectedId, delta));
		return;
	}
	pushUndo(app);
	resetPushPull(app);
	translateBrush(app->brushes[bi], delta);
	app->meshes[bi].destroy();
	app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;
}

// Cycle the SELECTED FACE to the next texture (synced when online).
void cycleTexture(App* app) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	if (bi < 0 || app->selectedFace < 0 || app->textures.empty()) return;
	if (app->selectedFace >= (int)app->brushes[bi].planes.size()) return;
	const uint32_t next =
	    (app->brushes[bi].planes[app->selectedFace].textureId + 1) % (uint32_t)app->textures.size();
	if (app->online) {
		sendMsg(app, msgSetFaceTexture(app->selectedId, (uint32_t)app->selectedFace, next));
		return;
	}
	pushUndo(app);
	resetPushPull(app);
	app->brushes[bi].planes[app->selectedFace].textureId = next;  // bound at draw — no rebuild
}

// The currently selected face's plane, or null.
Plane* selectedPlane(App* app) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	if (bi < 0 || app->selectedFace < 0 || app->selectedFace >= (int)app->brushes[bi].planes.size())
		return nullptr;
	return &app->brushes[bi].planes[app->selectedFace];
}

// Set the selected face's UV alignment to absolute values (synced when online; rebuilds the mesh).
void setFaceUV(App* app, float us, float vs, float uo, float vo, float rot) {
	const int bi = indexOfId(app->brushes, app->selectedId);
	if (bi < 0 || app->selectedFace < 0 || app->selectedFace >= (int)app->brushes[bi].planes.size())
		return;
	us = bx::clamp(us, 0.0625f, 64.0f);
	vs = bx::clamp(vs, 0.0625f, 64.0f);
	if (app->online) {
		sendMsg(app, msgSetFaceUV(app->selectedId, (uint32_t)app->selectedFace, us, vs, uo, vo, rot));
		return;
	}
	pushUndo(app);
	resetPushPull(app);
	Plane& pl = app->brushes[bi].planes[app->selectedFace];
	pl.uScale = us; pl.vScale = vs; pl.uOffset = uo; pl.vOffset = vo; pl.rotation = rot;
	app->meshes[bi].destroy();
	app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;  // UVs are baked in
}

// Place a point light above the crosshair's ground point (synced when online).
void createLight(App* app) {
	bx::Vec3 g(0.0f, 0.0f, 0.0f);
	if (!aimGroundPoint(app, app->cam.forward(), g)) return;
	Light l;
	l.pos = bx::add(g, bx::Vec3(0.0f, 0.0f, 128.0f));
	l.color[0] = kPlaceColors[app->placeColorIdx][0];
	l.color[1] = kPlaceColors[app->placeColorIdx][1];
	l.color[2] = kPlaceColors[app->placeColorIdx][2];
	l.radius = 384.0f;
	if (app->online) { sendMsg(app, msgCreateLight(l)); return; }
	l.id = app->localNextId++;
	app->lights.push_back(l);
}

// Delete the light nearest the crosshair ray (synced when online).
void deleteNearestLight(App* app) {
	const bx::Vec3 ro = app->cam.pos, rd = app->cam.forward();
	int best = -1;
	float bestPerp = 96.0f;  // must be within this distance of the aim ray
	for (int i = 0; i < (int)app->lights.size(); ++i) {
		const float t = bx::dot(bx::sub(app->lights[i].pos, ro), rd);
		if (t <= 0.0f) continue;
		const float perp = bx::length(bx::sub(app->lights[i].pos, bx::add(ro, bx::mul(rd, t))));
		if (perp < bestPerp) { bestPerp = perp; best = i; }
	}
	if (best < 0) return;
	const uint32_t id = app->lights[best].id;
	if (app->online) { sendMsg(app, msgDeleteLight(id)); return; }
	app->lights.erase(app->lights.begin() + best);
}

int lightIndexById(App* app, uint32_t id) {
	for (int i = 0; i < (int)app->lights.size(); ++i)
		if (app->lights[i].id == id) return i;
	return -1;
}

// Intersect a ray with a plane (normal n through point p). false if behind / parallel.
bool rayPlane(const bx::Vec3& ro, const bx::Vec3& rd, const bx::Vec3& n, const bx::Vec3& p,
              bx::Vec3& out) {
	const float denom = bx::dot(n, rd);
	if (bx::abs(denom) < 1.0e-5f) return false;
	const float t = bx::dot(n, bx::sub(p, ro)) / denom;
	if (t <= 0.0f) return false;
	out = bx::add(ro, bx::mul(rd, t));
	return true;
}

// Sauerbraten-style: grab the targeted light's cube face and drag it within that face's plane.
void startLightDrag(App* app) {
	if (app->targetedLight < 0) return;
	const Light& l = app->lights[app->targetedLight];
	app->dragLightId = l.id;
	app->dragPlaneN = app->targetedLightNormal;  // drag within the plane of the clicked face
	app->dragPlanePoint = l.pos;
	bx::Vec3 P(0, 0, 0);
	app->dragOffset = rayPlane(app->cam.pos, app->cam.forward(), app->dragPlaneN, app->dragPlanePoint, P)
	                      ? bx::sub(P, l.pos)
	                      : bx::Vec3(0, 0, 0);
}

void updateLightDrag(App* app) {
	if (app->dragLightId == 0) return;
	const int bi = lightIndexById(app, app->dragLightId);
	if (bi < 0) { app->dragLightId = 0; return; }
	bx::Vec3 P(0, 0, 0);
	if (!rayPlane(app->cam.pos, app->cam.forward(), app->dragPlaneN, app->dragPlanePoint, P)) return;
	bx::Vec3 np = bx::sub(P, app->dragOffset);
	const float g = app->gridStep;
	app->lights[bi].pos = bx::Vec3(snapToGrid(np.x, g), snapToGrid(np.y, g), snapToGrid(np.z, g));
}

void endLightDrag(App* app) {
	if (app->dragLightId == 0) return;
	const int bi = lightIndexById(app, app->dragLightId);
	if (bi >= 0 && app->online) sendMsg(app, msgSetLightPos(app->dragLightId, app->lights[bi].pos));
	app->dragLightId = 0;
}

// Light uniforms: real lighting for brushes, flat (ambient=1) for unlit overlays/markers.
void setBrushLighting(App* app) {
	const int n = (int)app->lights.size() > kMaxLights ? kMaxLights : (int)app->lights.size();
	const float lp[4] = {(float)n, 0.15f, app->hasLightmap ? 1.0f : 0.0f, 0.0f};  // z = use lightmap
	const bx::Vec3 sd = bx::normalize(bx::Vec3(0.35f, 0.25f, 0.9f));
	const float sun[4] = {sd.x, sd.y, sd.z, 0.6f};
	bgfx::setUniform(app->u_lightParams, lp);
	bgfx::setUniform(app->u_sunDir, sun);
	float posR[kMaxLights * 4] = {0};
	float col[kMaxLights * 4] = {0};
	for (int i = 0; i < n; ++i) {
		const Light& L = app->lights[i];
		posR[i * 4 + 0] = L.pos.x; posR[i * 4 + 1] = L.pos.y; posR[i * 4 + 2] = L.pos.z; posR[i * 4 + 3] = L.radius;
		col[i * 4 + 0] = L.color[0]; col[i * 4 + 1] = L.color[1]; col[i * 4 + 2] = L.color[2]; col[i * 4 + 3] = 1.0f;
	}
	bgfx::setUniform(app->u_lightPosRadius, posR, n > 0 ? n : 1);
	bgfx::setUniform(app->u_lightColor, col, n > 0 ? n : 1);
}

void setFlatLighting(App* app) {
	const float lp[4] = {0.0f, 1.0f, 0.0f, 0.0f};  // no point lights, full ambient -> flat albedo
	const float sun[4] = {0.0f, 0.0f, 1.0f, 0.0f};
	bgfx::setUniform(app->u_lightParams, lp);
	bgfx::setUniform(app->u_sunDir, sun);
}

// Apply a message received from the server to the local map.
void applyNetMessage(App* app, const uint8_t* data, size_t len) {
	ByteReader r(data, len);
	switch ((MsgType)r.u8()) {
		case MsgType::Snapshot: {
			const uint32_t n = r.u32();
			app->brushes.clear();
			for (uint32_t i = 0; i < n && r.ok; ++i) app->brushes.push_back(readBrush(r));
			const uint32_t ln = r.u32();
			app->lights.clear();
			for (uint32_t i = 0; i < ln && r.ok; ++i) app->lights.push_back(readLight(r));
			app->selectedId = 0;
			app->selectedFace = -1;
			rebuildSceneMeshes(app);
			break;
		}
		case MsgType::CreateLight: {
			Light l = readLight(r);
			if (r.ok) app->lights.push_back(l);
			break;
		}
		case MsgType::DeleteLight: {
			const uint32_t id = r.u32();
			if (r.ok)
				for (size_t i = 0; i < app->lights.size(); ++i)
					if (app->lights[i].id == id) { app->lights.erase(app->lights.begin() + i); break; }
			break;
		}
		case MsgType::SetLightPos: {
			const uint32_t id = r.u32();
			const bx::Vec3 pos = r.vec3();
			const int bi = lightIndexById(app, id);
			// Don't clobber the light we're actively dragging with our own echo.
			if (r.ok && bi >= 0 && id != app->dragLightId) app->lights[bi].pos = pos;
			break;
		}
		case MsgType::CreateBrush: {
			Brush b = readBrush(r);
			if (!r.ok) break;
			app->brushes.push_back(b);
			app->meshes.push_back(buildMeshFromBrush(b, app->layout));
			app->hasLightmap = false;  // new geometry without lightmap UVs
			if (app->selectOnNextCreate) {
				app->selectedId = b.id;
				app->selectedFace = -1;
				app->selectOnNextCreate = false;
			}
			break;
		}
		case MsgType::DeleteBrush: {
			const uint32_t id = r.u32();
			const int bi = indexOfId(app->brushes, id);
			if (bi >= 0) {
				app->meshes[bi].destroy();
				app->meshes.erase(app->meshes.begin() + bi);
				app->brushes.erase(app->brushes.begin() + bi);
			}
			if (app->selectedId == id) { app->selectedId = 0; app->selectedFace = -1; }
			break;
		}
		case MsgType::TranslateBrush: {
			const uint32_t id = r.u32();
			const bx::Vec3 d = r.vec3();
			const int bi = indexOfId(app->brushes, id);
			if (r.ok && bi >= 0) {
				translateBrush(app->brushes[bi], d);
				app->meshes[bi].destroy();
				app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;
			}
			break;
		}
		case MsgType::SetPlaneD: {
			const uint32_t id = r.u32();
			const uint32_t f = r.u32();
			const float d = r.f32();
			const int bi = indexOfId(app->brushes, id);
			if (r.ok && bi >= 0 && f < app->brushes[bi].planes.size()) {
				app->brushes[bi].planes[f].d = d;
				app->meshes[bi].destroy();
				app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;
			}
			break;
		}
		case MsgType::AssignId: {
			app->myPlayerId = r.u32();
			const uint32_t serverVersion = r.u32();
			app->versionMismatch = (!r.ok || serverVersion != kProtocolVersion);
			if (app->versionMismatch)
				SDL_Log("protocol mismatch: server v%u, client v%u", serverVersion, kProtocolVersion);
			break;
		}
		case MsgType::PlayerStates: {
			const uint32_t n = r.u32();
			app->remotePlayers.clear();
			for (uint32_t i = 0; i < n && r.ok; ++i) {
				const uint32_t id = r.u32();
				const bx::Vec3 p = r.vec3();
				const float yaw = r.f32(), pitch = r.f32();
				if (id != app->myPlayerId) app->remotePlayers[id] = RemotePlayer{p, yaw, pitch};
			}
			break;
		}
		case MsgType::SetFaceTexture: {
			const uint32_t id = r.u32();
			const uint32_t face = r.u32();
			const uint32_t tid = r.u32();
			const int bi = indexOfId(app->brushes, id);
			if (r.ok && bi >= 0 && face < app->brushes[bi].planes.size())
				app->brushes[bi].planes[face].textureId = tid;
			break;
		}
		case MsgType::SetFaceUV: {
			const uint32_t id = r.u32();
			const uint32_t face = r.u32();
			const float us = r.f32(), vs = r.f32(), uo = r.f32(), vo = r.f32(), rot = r.f32();
			const int bi = indexOfId(app->brushes, id);
			if (r.ok && bi >= 0 && face < app->brushes[bi].planes.size()) {
				Plane& pl = app->brushes[bi].planes[face];
				pl.uScale = us; pl.vScale = vs; pl.uOffset = uo; pl.vOffset = vo; pl.rotation = rot;
				app->meshes[bi].destroy();
				app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout); app->hasLightmap = false;
			}
			break;
		}
		case MsgType::Lightmap: {  // server shipped a baked lightmap (on join or after a peer's F6)
			const LightmapData lm = readLightmap(r);
			if (r.ok && lm.valid()) applyLightmapTexture(app, lm.w, lm.h, lm.pixels, lm.vertexUV);
			break;
		}
		case MsgType::AgentStates: {  // server-driven AI agent positions
			const uint32_t n = r.u32();
			app->agents.clear();
			for (uint32_t i = 0; i < n && r.ok; ++i) {
				const uint32_t id = r.u32();
				const bx::Vec3 p = r.vec3();
				const float yaw = r.f32();
				const uint32_t hp = r.u32();
				if (r.ok) app->agents[id] = AiAgent{p, yaw, hp};
			}
			break;
		}
		case MsgType::NavMesh: {  // walkable triangle soup for the debug overlay
			const uint32_t n = r.u32();
			const uint8_t* fp = r.take((size_t)n * sizeof(float));
			if (r.ok && fp) {
				std::vector<float> tris(n);
				if (n) memcpy(tris.data(), fp, (size_t)n * sizeof(float));
				buildNavDebugMesh(app, tris);
			}
			break;
		}
		case MsgType::PlayerState:  // client->server only
			break;
	}
}

void netPoll(App* app) {
	if (!app->net) return;
	std::vector<NetEvent> evs;
	netService(app->net, evs);
	for (const NetEvent& e : evs) {
		if (e.type == NetEvent::Connected) { app->online = true; SDL_Log("connected to server"); }
		else if (e.type == NetEvent::Disconnected) { app->online = false; SDL_Log("disconnected from server"); }
		else if (e.type == NetEvent::Data) applyNetMessage(app, e.data.data(), e.data.size());
	}
}

void updateTargeted(App* app) {
	app->targeted = -1;
	app->targetedFace = -1;
	app->targetedLight = -1;
	if (!app->editMode) return;
	const bx::Vec3 ro = app->cam.pos;
	const bx::Vec3 rd = app->cam.forward();

	float bestBrush = 1.0e30f;
	for (int i = 0; i < (int)app->brushes.size(); ++i) {
		const RayHit h = rayBrushIntersect(ro, rd, app->brushes[i]);
		if (h.hit && h.t < bestBrush) {
			bestBrush = h.t;
			app->targeted = i;
			app->targetedFace = h.face;
		}
	}
	// Light cubes: raycast a box centred on each light to pick a face to drag.
	float bestLight = 1.0e30f;
	for (int i = 0; i < (int)app->lights.size(); ++i) {
		const Brush box = makeBox(app->lights[i].pos, {kLightHalf, kLightHalf, kLightHalf}, 1, 1, 1);
		const RayHit h = rayBrushIntersect(ro, rd, box);
		if (h.hit && h.t < bestLight && h.face >= 0) {
			bestLight = h.t;
			app->targetedLight = i;
			app->targetedLightNormal = box.planes[h.face].n;
		}
	}
	// Whichever is closer wins (so a click drags the light only if it's in front of the brush).
	if (app->targetedLight >= 0 && bestLight < bestBrush) { app->targeted = -1; app->targetedFace = -1; }
	else { app->targetedLight = -1; }
}

void buildInitialScene(App* app) {
	auto add = [&](Brush b) { b.id = app->localNextId++; app->brushes.push_back(std::move(b)); };
	add(makeBox({0, 0, -8}, {512, 512, 8}, 0.45f, 0.47f, 0.50f));
	add(makeBox({0, 0, 32}, {32, 32, 32}, 0.80f, 0.30f, 0.25f));
	add(makeBox({128, 0, 24}, {24, 48, 24}, 0.30f, 0.65f, 0.35f));
	rebuildSceneMeshes(app);
}

bool fillPlatformData(SDL_Window* window, bgfx::PlatformData& pd) {
#if defined(__EMSCRIPTEN__)
	(void)window;
	pd.nwh = (void*)"#canvas";
	return true;
#else
	const SDL_PropertiesID props = SDL_GetWindowProperties(window);
	if (props == 0) return false;
#if defined(SDL_PLATFORM_WIN32)
	pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
	pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
	const char* driver = SDL_GetCurrentVideoDriver();
	if (driver && SDL_strcmp(driver, "wayland") == 0) {
		pd.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
		pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
		pd.type = bgfx::NativeWindowHandleType::Wayland;
	} else {
		pd.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
		pd.nwh = (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
	}
#else
	#error "Unsupported platform"
#endif
	return pd.nwh != nullptr;
#endif
}

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return SDL_APP_FAILURE;

	App* app = new App();
	*appstate = app;

	// Optional: `cooped <serverHost> [port]` connects to a dedicated server (native only).
	const char* serverHost = (argc > 1) ? argv[1] : nullptr;
	const uint16_t serverPort = (argc > 2) ? (uint16_t)atoi(argv[2]) : 27500;

	app->window = SDL_CreateWindow("cooped — milestone 5", (int)app->width, (int)app->height,
	                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!app->window) return SDL_APP_FAILURE;

	int pw = 0, ph = 0;
	SDL_GetWindowSizeInPixels(app->window, &pw, &ph);
	app->width = (uint32_t)pw;
	app->height = (uint32_t)ph;

	bgfx::renderFrame();
	bgfx::Init init;
	init.type = bgfx::RendererType::Count;
	init.resolution.width = app->width;
	init.resolution.height = app->height;
	init.resolution.reset = BGFX_RESET_VSYNC;
	if (!fillPlatformData(app->window, init.platformData)) return SDL_APP_FAILURE;
	if (!bgfx::init(init)) return SDL_APP_FAILURE;
	app->bgfxInitialized = true;

	bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x202830ff, 1.0f, 0);
	bgfx::setViewMode(0, bgfx::ViewMode::Sequential);  // draw in submit order: sky first, world over it
	bgfx::setDebug(BGFX_DEBUG_TEXT);

	app->layout.begin()
	    .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
	    .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
	    .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
	    .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
	    .end();
	app->program = createWorldProgram();
	app->u_albedo = bgfx::createUniform("u_albedo", bgfx::UniformType::Vec4);
	app->s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
	app->s_lightmap = bgfx::createUniform("s_lightmap", bgfx::UniformType::Sampler);
	app->u_lightParams = bgfx::createUniform("u_lightParams", bgfx::UniformType::Vec4);
	app->u_sunDir = bgfx::createUniform("u_sunDir", bgfx::UniformType::Vec4);
	app->u_lightPosRadius = bgfx::createUniform("u_lightPosRadius", bgfx::UniformType::Vec4, kMaxLights);
	app->u_lightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4, kMaxLights);
	// Wireframe cube (12 edges) drawn at each light, sized to the editable box.
	{
		std::vector<BrushVertex> wire;
		auto corner = [](int i) {
			return bx::Vec3((i & 1) ? kLightHalf : -kLightHalf, (i & 2) ? kLightHalf : -kLightHalf,
			                (i & 4) ? kLightHalf : -kLightHalf);
		};
		for (int i = 0; i < 8; ++i)
			for (int bit : {1, 2, 4})
				if (!(i & bit)) {
					const bx::Vec3 a = corner(i), b = corner(i | bit);
					wire.push_back({a.x, a.y, a.z, 0, 0, 1, 0, 0});
					wire.push_back({b.x, b.y, b.z, 0, 0, 1, 0, 0});
				}
		app->lightWire = bgfx::createVertexBuffer(
		    bgfx::copy(wire.data(), uint32_t(wire.size() * sizeof(BrushVertex))), app->layout);
		app->lightWireVerts = (uint32_t)wire.size();
	}

	const uint32_t whitePixel = 0xffffffff;
	app->whiteTex = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
	                                      bgfx::copy(&whitePixel, 4));
	const char* texFiles[] = {"data/brick.jpg", "data/floor.jpg", "data/tiles.jpg"};
	for (const char* tf : texFiles) {
		bgfx::TextureHandle t = loadTexture(tf);
		if (!bgfx::isValid(t)) { t = app->whiteTex; SDL_Log("texture load failed: %s", tf); }
		app->textures.push_back(t);
	}

	// HDR skybox: program, uniforms, the two panoramas, and a fullscreen triangle.
	app->skyProgram = createSkyProgram();
	app->s_sky = bgfx::createUniform("s_sky", bgfx::UniformType::Sampler);
	app->u_skyParams = bgfx::createUniform("u_skyParams", bgfx::UniformType::Vec4);
	app->skyTex[0] = loadHdrTexture("data/rosendal_plains_2_1k.hdr");
	app->skyTex[1] = loadHdrTexture("data/sunny_rose_garden_1k.hdr");
	if (!bgfx::isValid(app->skyTex[0])) SDL_Log("skybox HDR load failed");
	const BrushVertex fsTri[3] = {
	    {-1, -1, 0, 0, 0, 0, 0, 0}, {3, -1, 0, 0, 0, 0, 0, 0}, {-1, 3, 0, 0, 0, 0, 0, 0}};
	app->fullscreen = bgfx::createVertexBuffer(bgfx::copy(fsTri, sizeof(fsTri)), app->layout);

	// Avatar: a body-sized box centered at origin (translated per remote player when drawn).
	app->avatarMesh = buildMeshFromBrush(makeBox({0, 0, 0}, kPlayerHalf, 0.8f, 0.8f, 0.8f), app->layout);
	// AI agent: a smaller, brighter cube (orange) centered at origin — fallback if a model fails.
	app->agentMesh = buildMeshFromBrush(makeBox({0, 0, 0}, kAgentHalf, 0.95f, 0.55f, 0.15f), app->layout);
	// NPC characters (FBX). Each agent picks one by id % count. Meshy exports face +Y locally,
	// so a -90deg pre-rotation around Z aligns them with cooped's "yaw=0 means facing +X".
	const float kMeshyYaw = -bx::kPiHalf;
	app->npcModels.push_back(loadFbxModel("data/frog.fbx",  app->layout, "data/frog.png",  32.0f, kMeshyYaw));
	app->npcModels.push_back(loadFbxModel("data/sunny.fbx", app->layout, "data/sunny.png", 32.0f, kMeshyYaw));

	buildInitialScene(app);  // local sandbox; replaced by the server snapshot if we connect
	rebuildGrid(app);

#if defined(__EMSCRIPTEN__)
	(void)serverHost; (void)serverPort;
	if (netGlobalInit()) {  // browser always tries to connect; URL comes from ?server= or default
		app->net = netConnect(nullptr, 0);
		SDL_Log("connecting to server via WebRTC ...");
	}
#else
	if (serverHost) {
		if (netGlobalInit()) {
			app->net = netConnect(serverHost, serverPort);
			SDL_Log(app->net ? "connecting to %s:%u ..." : "netConnect to %s:%u failed",
			        serverHost, serverPort);
		} else {
			SDL_Log("net init failed");
		}
	}
#endif

	SDL_SetWindowRelativeMouseMode(app->window, true);
	app->lastTicksNs = SDL_GetTicksNS();
	SDL_Log("bgfx renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
	App* app = static_cast<App*>(appstate);
	const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
	const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;

	switch (event->type) {
		case SDL_EVENT_QUIT:
			return SDL_APP_SUCCESS;
		case SDL_EVENT_MOUSE_MOTION:
			app->mouseDx += event->motion.xrel;
			app->mouseDy += event->motion.yrel;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (app->editMode && event->button.button == SDL_BUTTON_LEFT) {
				if (app->targetedLight >= 0) {
					startLightDrag(app);  // grab a light cube face to drag it
				} else {
					app->selectedId = (app->targeted >= 0) ? app->brushes[app->targeted].id : 0;
					app->selectedFace = app->targetedFace;  // the face becomes the push/pull target
					resetPushPull(app);
				}
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (event->button.button == SDL_BUTTON_LEFT) endLightDrag(app);
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (app->editMode) {
				if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_G]) {  // G + wheel: change grid size
					if (event->wheel.y > 0) app->gridStep = bx::min(app->gridStep * 2.0f, 512.0f);
					else if (event->wheel.y < 0) app->gridStep = bx::max(app->gridStep * 0.5f, 8.0f);
					rebuildGrid(app);
				} else if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_H]) {  // H + wheel: lightmap density
					// Up = finer (more texels/unit). Capped at 1/8 now the bake is multithreaded.
					if (event->wheel.y > 0) app->lightmapTexelsPerUnit = bx::min(app->lightmapTexelsPerUnit * 2.0f, 1.0f / 8.0f);
					else if (event->wheel.y < 0) app->lightmapTexelsPerUnit = bx::max(app->lightmapTexelsPerUnit * 0.5f, 1.0f / 128.0f);
				} else {  // wheel: push/pull the selected face (up = push out, down = pull in)
					if (event->wheel.y > 0) pushPullFace(app, -1);
					else if (event->wheel.y < 0) pushPullFace(app, +1);
				}
			}
			break;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			app->width = (uint32_t)event->window.data1;
			app->height = (uint32_t)event->window.data2;
			bgfx::reset(app->width, app->height, BGFX_RESET_VSYNC);
			break;
		case SDL_EVENT_KEY_DOWN:
			switch (event->key.key) {
				case SDLK_ESCAPE: return SDL_APP_SUCCESS;
					case SDLK_B: app->skyIdx = (app->skyIdx + 1) % 2; break;  // cycle skybox
					case SDLK_N: app->showNavMesh = !app->showNavMesh; break;  // toggle navmesh overlay
				case SDLK_E:
					app->editMode = !app->editMode;
					app->selectedId = 0;
					app->selectedFace = -1;
					if (!app->editMode) {  // entering play: drop the body in at the fly-cam eye
						app->playerPos = bx::sub(app->cam.pos, bx::Vec3(0, 0, kEyeOffset));
						app->playerVel = bx::Vec3(0, 0, 0);
						app->onGround = false;
					}
					break;
				default: break;
			}
			if (app->editMode) {
				if (ctrl && event->key.key == SDLK_Z) { shift ? redo(app) : undo(app); }
				else if (ctrl && event->key.key == SDLK_Y) { redo(app); }
				else {
					Plane* p = selectedPlane(app);  // for UV-align keys
					switch (event->key.key) {
						case SDLK_RETURN: createBrushAtAim(app); break;
						case SDLK_T: cycleTexture(app); break;
#if !defined(__EMSCRIPTEN__)
						case SDLK_F6: {  // GI bake on a worker thread; result uploaded when done (no UI freeze)
							if (!app->bakeRunning.load()) {
								app->bakeRunning.store(true);
								app->bakeDone.store(false);
								// Snapshot the scene so the worker is unaffected by concurrent edits.
								std::vector<Brush> brushes = app->brushes;
								std::vector<Light> lights = app->lights;
								const float density = app->lightmapTexelsPerUnit;
								app->bakeThread = std::thread(
								    [app, brushes = std::move(brushes), lights = std::move(lights), density]() {
									    app->bakeResult = bakeLightmaps(brushes, lights, density);
									    app->bakeDone.store(true);
								    });
							}
							break;
						}
#endif
						case SDLK_L: createLight(app); break;
						case SDLK_K: deleteNearestLight(app); break;
						case SDLK_C: app->placeColorIdx = (app->placeColorIdx + 1) % 5; break;
						case SDLK_DELETE:
						case SDLK_X: deleteSelected(app); break;
						// Arrows: Shift = UV offset on the selected face, else move the brush.
						case SDLK_RIGHT:
							if (shift && p) setFaceUV(app, p->uScale, p->vScale, p->uOffset + 0.125f, p->vOffset, p->rotation);
							else nudgeSelected(app, {1, 0, 0});
							break;
						case SDLK_LEFT:
							if (shift && p) setFaceUV(app, p->uScale, p->vScale, p->uOffset - 0.125f, p->vOffset, p->rotation);
							else nudgeSelected(app, {-1, 0, 0});
							break;
						case SDLK_UP:
							if (shift && p) setFaceUV(app, p->uScale, p->vScale, p->uOffset, p->vOffset + 0.125f, p->rotation);
							else nudgeSelected(app, {0, 1, 0});
							break;
						case SDLK_DOWN:
							if (shift && p) setFaceUV(app, p->uScale, p->vScale, p->uOffset, p->vOffset - 0.125f, p->rotation);
							else nudgeSelected(app, {0, -1, 0});
							break;
						case SDLK_PAGEUP:   nudgeSelected(app, {0, 0, 1}); break;
						case SDLK_PAGEDOWN: nudgeSelected(app, {0, 0, -1}); break;
						// UV align on the selected face.
						case SDLK_LEFTBRACKET:  if (p) setFaceUV(app, p->uScale * 0.5f, p->vScale * 0.5f, p->uOffset, p->vOffset, p->rotation); break;
						case SDLK_RIGHTBRACKET: if (p) setFaceUV(app, p->uScale * 2.0f, p->vScale * 2.0f, p->uOffset, p->vOffset, p->rotation); break;
						case SDLK_COMMA:  if (p) setFaceUV(app, p->uScale, p->vScale, p->uOffset, p->vOffset, p->rotation - 15.0f); break;
						case SDLK_PERIOD: if (p) setFaceUV(app, p->uScale, p->vScale, p->uOffset, p->vOffset, p->rotation + 15.0f); break;
						case SDLK_BACKSLASH: if (p) setFaceUV(app, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f); break;
						default: break;
					}
				}
			}
			break;
		default:
			break;
	}
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
	App* app = static_cast<App*>(appstate);

	netPoll(app);  // apply any snapshot/edit ops from the server

#if !defined(__EMSCRIPTEN__)
	// A background bake finished: upload it (bgfx must run on this thread) and share it.
	if (app->bakeDone.load()) {
		if (app->bakeThread.joinable()) app->bakeThread.join();
		app->bakeDone.store(false);
		app->bakeRunning.store(false);
		const BakeResult& br = app->bakeResult;
		if (br.ok && !br.pixels.empty()) {
			applyLightmapTexture(app, br.atlasWidth, br.atlasHeight, br.pixels, br.vertexUV);
			if (app->online) {
				LightmapData lm;
				lm.w = br.atlasWidth; lm.h = br.atlasHeight;
				lm.pixels = br.pixels; lm.vertexUV = br.vertexUV;
				sendMsg(app, msgLightmap(lm));
			}
			SDL_Log("bake done: atlas %ux%u charts=%u", br.atlasWidth, br.atlasHeight, br.chartCount);
		}
	}
#endif

	const uint64_t now = SDL_GetTicksNS();
	float dt = float(double(now - app->lastTicksNs) / 1.0e9);
	app->lastTicksNs = now;
	if (dt > 0.1f) dt = 0.1f;

	const float sens = 0.0025f;
	app->cam.yaw += app->mouseDx * sens;
	app->cam.pitch = bx::clamp(app->cam.pitch - app->mouseDy * sens, -1.5f, 1.5f);
	app->mouseDx = app->mouseDy = 0.0f;

	const bool* keys = SDL_GetKeyboardState(nullptr);
	const bx::Vec3 fwd = app->cam.forward();
	const bx::Vec3 worldUp{0.0f, 0.0f, 1.0f};

	if (app->editMode) {
		// Free-fly camera.
		const bx::Vec3 right = bx::normalize(bx::cross(worldUp, fwd));
		const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 700.0f : 250.0f) * dt;
		bx::Vec3 move{0, 0, 0};
		if (keys[SDL_SCANCODE_W]) move = bx::add(move, fwd);
		if (keys[SDL_SCANCODE_S]) move = bx::sub(move, fwd);
		if (keys[SDL_SCANCODE_D]) move = bx::add(move, right);
		if (keys[SDL_SCANCODE_A]) move = bx::sub(move, right);
		if (keys[SDL_SCANCODE_SPACE]) move = bx::add(move, worldUp);
		if (keys[SDL_SCANCODE_LCTRL]) move = bx::sub(move, worldUp);
		if (bx::length(move) > 0.0f)
			app->cam.pos = bx::add(app->cam.pos, bx::mul(bx::normalize(move), speed));
	} else {
		// Walking player: gravity + WASD on the ground plane, collide against brushes.
		bx::Vec3 fwdH(fwd.x, fwd.y, 0.0f);
		if (bx::length(fwdH) > 1.0e-4f) fwdH = bx::normalize(fwdH);
		const bx::Vec3 rightH = bx::normalize(bx::cross(worldUp, fwdH));
		bx::Vec3 wish{0, 0, 0};
		if (keys[SDL_SCANCODE_W]) wish = bx::add(wish, fwdH);
		if (keys[SDL_SCANCODE_S]) wish = bx::sub(wish, fwdH);
		if (keys[SDL_SCANCODE_D]) wish = bx::add(wish, rightH);
		if (keys[SDL_SCANCODE_A]) wish = bx::sub(wish, rightH);
		const bx::Vec3 wishDir = (bx::length(wish) > 0.0f) ? bx::normalize(wish) : bx::Vec3(0, 0, 0);

		app->playerVel.z -= kGravity * dt;
		if (app->onGround) {
			const float sp = bx::sqrt(app->playerVel.x * app->playerVel.x +
			                          app->playerVel.y * app->playerVel.y);
			if (sp > 0.0f) {  // horizontal friction
				const float ns = bx::max(sp - sp * kFriction * dt, 0.0f) / sp;
				app->playerVel.x *= ns;
				app->playerVel.y *= ns;
			}
			accelerate(app->playerVel, wishDir, kMaxSpeed, kGroundAccel, dt);
			if (keys[SDL_SCANCODE_SPACE]) { app->playerVel.z = kJumpSpeed; app->onGround = false; }
		} else {
			accelerate(app->playerVel, wishDir, kMaxSpeed, kAirAccel, dt);
		}

		app->onGround = movePlayer(app->brushes, app->playerPos, app->playerVel, kPlayerHalf, dt);
		app->cam.pos = bx::add(app->playerPos, bx::Vec3(0, 0, kEyeOffset));

		// Hold LMB to auto-fire (tryFire enforces the cooldown).
		if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) tryFire(app);
	}

	updateTargeted(app);
	updateLightDrag(app);  // if dragging a light, follow the crosshair within its plane

	// Send my position/orientation to the server ~20 Hz so others can see me.
	if (app->online && now - app->lastStateSendNs > 50000000ULL) {
		app->lastStateSendNs = now;
		sendMsg(app, msgPlayerState(app->cam.pos, app->cam.yaw, app->cam.pitch));
	}

	float view[16], proj[16];
	bx::mtxLookAt(view, app->cam.pos, bx::add(app->cam.pos, fwd), worldUp);
	bx::mtxProj(proj, 60.0f, float(app->width) / float(app->height), 1.0f, 12000.0f,
	            bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewRect(0, 0, 0, (uint16_t)app->width, (uint16_t)app->height);
	bgfx::setViewTransform(0, view, proj);
	bgfx::touch(0);

	// HDR skybox: fullscreen triangle sampling the panorama by view ray (drawn first, no depth).
	if (bgfx::isValid(app->skyProgram) && bgfx::isValid(app->skyTex[app->skyIdx])) {
		const float skyp[4] = {1.0f, 0.0f, 0.0f, 0.0f};  // x = exposure
		bgfx::setUniform(app->u_skyParams, skyp);
		bgfx::setTexture(0, app->s_sky, app->skyTex[app->skyIdx]);
		bgfx::setVertexBuffer(0, app->fullscreen);
		bgfx::setState(BGFX_STATE_WRITE_RGB);  // no depth test/write -> background
		bgfx::submit(0, app->skyProgram);  // u_invViewProj / u_invView are bgfx built-ins
	}

	const int selIdx = indexOfId(app->brushes, app->selectedId);
	const uint64_t triState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
	                          BGFX_STATE_DEPTH_TEST_LESS;
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	setBrushLighting(app);  // ambient + sun + point lights, for the brush draws
	for (int i = 0; i < (int)app->meshes.size(); ++i) {
		const Mesh& m = app->meshes[i];
		if (!bgfx::isValid(m.vbh)) continue;
		// Draw each face with its own texture (selection/targeting shown by the outline below).
		for (const FaceRange& fr : m.faces) {
			const uint32_t tid = app->brushes[i].planes[fr.face].textureId;
			const bgfx::TextureHandle tex = (tid < app->textures.size()) ? app->textures[tid] : app->whiteTex;
			bgfx::setTexture(0, app->s_tex, tex);
			bgfx::setTexture(1, app->s_lightmap, app->hasLightmap ? app->lightmapTex : app->whiteTex);
			bgfx::setVertexBuffer(0, m.vbh);
			bgfx::setIndexBuffer(m.ibh, fr.firstIndex, fr.numIndices);
			bgfx::setUniform(app->u_albedo, white);
			bgfx::setState(triState);
			bgfx::submit(0, app->program);
		}
	}

	setFlatLighting(app);  // overlays/markers below render unlit (flat albedo)

	// Light markers: a wireframe cube at each light, in its color (white if targeted/dragged).
	if (app->editMode && bgfx::isValid(app->lightWire)) {
		const uint64_t lineState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
		                           BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES;
		for (int i = 0; i < (int)app->lights.size(); ++i) {
			const Light& l = app->lights[i];
			float mtx[16];
			bx::mtxTranslate(mtx, l.pos.x, l.pos.y, l.pos.z);
			float col[4] = {l.color[0], l.color[1], l.color[2], 1.0f};
			if (l.id == app->dragLightId || i == app->targetedLight) { col[0] = col[1] = col[2] = 1.0f; }
			bgfx::setTransform(mtx);
			bgfx::setTexture(0, app->s_tex, app->whiteTex);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
			bgfx::setVertexBuffer(0, app->lightWire);
			bgfx::setUniform(app->u_albedo, col);
			bgfx::setState(lineState);
			bgfx::submit(0, app->program);
		}
	}

	// Remote players: a body-sized box at each (eye pos minus eye offset = body centre).
	if (bgfx::isValid(app->avatarMesh.vbh)) {
		for (const auto& kv : app->remotePlayers) {
			const RemotePlayer& rp = kv.second;
			float mtx[16];
			bx::mtxTranslate(mtx, rp.pos.x, rp.pos.y, rp.pos.z - kEyeOffset);
			float col[4];
			colorFromId(kv.first, col);
			bgfx::setTransform(mtx);
			bgfx::setTexture(0, app->s_tex, app->whiteTex);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
			bgfx::setVertexBuffer(0, app->avatarMesh.vbh);
			bgfx::setIndexBuffer(app->avatarMesh.ibh);
			bgfx::setUniform(app->u_albedo, col);
			bgfx::setState(triState);
			bgfx::submit(0, app->program);
		}
	}

	// AI agents: each one renders as a character (FBX), with an HP bar billboarded above. NPCs
	// get sun+ambient (no lightmap — their UVs are the model's, not the lightmap atlas's).
	if (!app->agents.empty()) {
		const bx::Vec3 sd = bx::normalize(bx::Vec3(0.35f, 0.25f, 0.9f));
		const float lpNpc[4] = {0.0f, 0.20f, 0.0f, 0.0f};   // 0 point lights, ambient, no lightmap
		const float sunNpc[4] = {sd.x, sd.y, sd.z, 0.6f};
		bgfx::setUniform(app->u_lightParams, lpNpc);
		bgfx::setUniform(app->u_sunDir, sunNpc);
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		const float fallback[4] = {0.95f, 0.55f, 0.15f, 1.0f};  // orange cube if a model is missing
		for (const auto& kv : app->agents) {
			const AiAgent& ag = kv.second;
			const Model* mdl = !app->npcModels.empty() ? &app->npcModels[kv.first % app->npcModels.size()] : nullptr;
			float matR[16], matT[16], W[16];
			bx::mtxRotateZ(matR, ag.yaw);
			bx::mtxTranslate(matT, ag.pos.x, ag.pos.y, ag.pos.z);
			bx::mtxMul(W, matT, matR);  // model already pre-centred + scaled at load
			bgfx::setTransform(W);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
			if (mdl && mdl->ok()) {
				bgfx::setTexture(0, app->s_tex, bgfx::isValid(mdl->texture) ? mdl->texture : app->whiteTex);
				bgfx::setVertexBuffer(0, mdl->vbh);
				bgfx::setIndexBuffer(mdl->ibh, 0, mdl->numIndices);
				bgfx::setUniform(app->u_albedo, white);
				bgfx::setState(triState);
				bgfx::submit(0, app->program);
			} else if (bgfx::isValid(app->agentMesh.vbh)) {
				// Fallback: orange cube centred at feet + half-height. Use a vertical offset.
				float Tc[16]; bx::mtxTranslate(Tc, ag.pos.x, ag.pos.y, ag.pos.z + kAgentHalf.z);
				bgfx::setTransform(Tc);
				bgfx::setTexture(0, app->s_tex, app->whiteTex);
				bgfx::setVertexBuffer(0, app->agentMesh.vbh);
				bgfx::setIndexBuffer(app->agentMesh.ibh);
				bgfx::setUniform(app->u_albedo, fallback);
				bgfx::setState(triState);
				bgfx::submit(0, app->program);
			}
		}

		// HP bars are constant-colour overlays — switch back to flat shading first.
		setFlatLighting(app);
		const float redCol[4] = {0.9f, 0.15f, 0.15f, 1.0f};
		const float grnCol[4] = {0.2f, 0.95f, 0.2f, 1.0f};
		const bx::Vec3 camRight = bx::normalize(bx::cross(bx::Vec3(0, 0, 1), fwd));
		const uint64_t barState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
		                          BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES;
		for (const auto& kv : app->agents) {
			const AiAgent& ag = kv.second;
			const float halfW = 9.0f;
			const bx::Vec3 c(ag.pos.x, ag.pos.y, ag.pos.z + 2.0f * kAgentHalf.z + 6.0f);
			const bx::Vec3 l = bx::sub(c, bx::mul(camRight, halfW));
			const bx::Vec3 rEnd = bx::add(c, bx::mul(camRight, halfW));
			const float frac = bx::clamp((float)ag.hp / (float)kAgentMaxHp, 0.0f, 1.0f);
			const bx::Vec3 gEnd = bx::add(l, bx::mul(camRight, 2.0f * halfW * frac));
			auto drawSeg = [&](const bx::Vec3& a, const bx::Vec3& b, const float* color) {
				if (bgfx::getAvailTransientVertexBuffer(2, app->layout) < 2) return;
				BrushVertex v[2] = {{a.x, a.y, a.z, 0, 0, 1, 0, 0}, {b.x, b.y, b.z, 0, 0, 1, 0, 0}};
				bgfx::TransientVertexBuffer tvb;
				bgfx::allocTransientVertexBuffer(&tvb, 2, app->layout);
				memcpy(tvb.data, v, sizeof(v));
				bgfx::setTexture(0, app->s_tex, app->whiteTex);
				bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
				bgfx::setVertexBuffer(0, &tvb);
				bgfx::setUniform(app->u_albedo, color);
				bgfx::setState(barState);
				bgfx::submit(0, app->program);
			};
			drawSeg(l, rEnd, redCol);
			if (frac > 0.0f) drawSeg(l, gEnd, grnCol);
		}
	}

	// Hitscan tracer: a brief yellow line from the eye to the impact point (local feedback).
	if (SDL_GetTicksNS() < app->tracerExpireNs) {
		BrushVertex tv[2] = {
		    {app->tracerStart.x, app->tracerStart.y, app->tracerStart.z, 0, 0, 1, 0, 0},
		    {app->tracerEnd.x,   app->tracerEnd.y,   app->tracerEnd.z,   0, 0, 1, 0, 0},
		};
		if (bgfx::getAvailTransientVertexBuffer(2, app->layout) >= 2) {
			bgfx::TransientVertexBuffer tvb;
			bgfx::allocTransientVertexBuffer(&tvb, 2, app->layout);
			memcpy(tvb.data, tv, sizeof(tv));
			const float tcol[4] = {1.0f, 0.95f, 0.3f, 1.0f};
			bgfx::setTexture(0, app->s_tex, app->whiteTex);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
			bgfx::setVertexBuffer(0, &tvb);
			bgfx::setUniform(app->u_albedo, tcol);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
			bgfx::submit(0, app->program);
		}
	}

	// Navmesh debug overlay (N): the walkable surface as a cyan wireframe over the floor.
	if (app->showNavMesh && bgfx::isValid(app->navDbgVbh)) {
		const float navCol[4] = {0.2f, 0.8f, 1.0f, 1.0f};
		bgfx::setTexture(0, app->s_tex, app->whiteTex);
		bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
		bgfx::setVertexBuffer(0, app->navDbgVbh, 0, app->navDbgVerts);
		bgfx::setUniform(app->u_albedo, navCol);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
		bgfx::submit(0, app->program);
	}

	// Outline a face (selected = bright, else targeted = subtle) — the polygon edges as lines,
	// lifted slightly off the surface. Easier on the eyes than recoloring the whole face.
	const bool haveSel = selIdx >= 0 && app->selectedFace >= 0;
	const int hiBrush = haveSel ? selIdx : app->targeted;
	const int hiFace = haveSel ? app->selectedFace : app->targetedFace;
	if (app->editMode && hiBrush >= 0 && hiFace >= 0 && hiBrush < (int)app->brushes.size()) {
		const std::vector<bx::Vec3> poly = brushFacePolygon(app->brushes[hiBrush], (size_t)hiFace);
		if (poly.size() >= 3) {
			const bx::Vec3 n = app->brushes[hiBrush].planes[hiFace].n;
			const bx::Vec3 off = bx::mul(n, 0.4f);  // lift the line off the surface
			// Use the light direction as the line's normal so it shades to full brightness
			// regardless of which way the face points (outline reads crisply).
			const bx::Vec3 ln = bx::normalize(bx::Vec3(0.35f, 0.25f, 0.9f));
			std::vector<BrushVertex> lines;
			lines.reserve(poly.size() * 2);
			for (size_t i = 0; i < poly.size(); ++i) {  // edge i -> i+1, as a line-list pair
				const bx::Vec3 a = bx::add(poly[i], off);
				const bx::Vec3 b = bx::add(poly[(i + 1) % poly.size()], off);
				lines.push_back({a.x, a.y, a.z, ln.x, ln.y, ln.z});
				lines.push_back({b.x, b.y, b.z, ln.x, ln.y, ln.z});
			}
			const uint32_t cnt = (uint32_t)lines.size();
			if (bgfx::getAvailTransientVertexBuffer(cnt, app->layout) >= cnt) {
				bgfx::TransientVertexBuffer tvb;
				bgfx::allocTransientVertexBuffer(&tvb, cnt, app->layout);
				memcpy(tvb.data, lines.data(), cnt * sizeof(BrushVertex));
				const float sel[4] = {1.0f, 0.95f, 0.3f, 1.0f};  // selected: bright yellow
				const float tgt[4] = {0.7f, 0.85f, 1.0f, 1.0f};  // targeted: pale blue
				bgfx::setTexture(0, app->s_tex, app->whiteTex);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
				bgfx::setVertexBuffer(0, &tvb);
				bgfx::setUniform(app->u_albedo, haveSel ? sel : tgt);
				bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
				               BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
				bgfx::submit(0, app->program);
			}
		}
	}

	if (app->editMode && bgfx::isValid(app->grid.vbh)) {
		const float gridColor[4] = {0.30f, 0.34f, 0.40f, 1.0f};
		bgfx::setTexture(0, app->s_tex, app->whiteTex);
			bgfx::setTexture(1, app->s_lightmap, app->whiteTex);
		bgfx::setVertexBuffer(0, app->grid.vbh);
		bgfx::setIndexBuffer(app->grid.ibh);
		bgfx::setUniform(app->u_albedo, gridColor);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
		               BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES);
		bgfx::submit(0, app->program);
	}

	bgfx::dbgTextClear();
	if (app->versionMismatch)
		bgfx::dbgTextPrintf(1, 5, 0x4f, " SERVER PROTOCOL MISMATCH - rebuild & restart the server ");
#if !defined(__EMSCRIPTEN__)
	if (app->bakeRunning.load())
		bgfx::dbgTextPrintf(1, 7, 0x6f, " baking lightmap... (UI stays responsive) ");
#endif
	if (app->editMode) {
		const uint16_t cols = uint16_t(app->width / 8), rows = uint16_t(app->height / 16);
		bgfx::dbgTextPrintf(cols / 2, rows / 2, 0x0f, "+");  // crosshair
		bgfx::dbgTextPrintf(1, 1, 0x0e, "EDIT [%s]  grid:%d  lmap:%du/tx  brushes:%zu  sel:%u face:%d",
		                    app->net ? (app->online ? "online" : "connecting") : "local",
		                    (int)app->gridStep, (int)(1.0f / app->lightmapTexelsPerUnit),
		                    app->brushes.size(), app->selectedId, app->selectedFace);
		bgfx::dbgTextPrintf(1, 2, 0x0a, "L-click:select face   wheel:push(up)/pull(down)   Enter:new   X/Del:delete");
		bgfx::dbgTextPrintf(1, 3, 0x0a, "arrows/PgUp/PgDn:move  G+wheel:grid  H+wheel:lmap  T:texture  F6:bake  Ctrl+Z/Y:undo  E:play");
		bgfx::dbgTextPrintf(1, 4, 0x09, "face UV:  [ ]:scale  , .:rotate  shift+arrows:offset  \\:reset");
		bgfx::dbgTextPrintf(1, 6, 0x0d, "lights:  L:place  drag a cube face to move  K:delete  C:color (%d)  |  count:%zu",
		                    app->placeColorIdx, app->lights.size());
	} else {
		const uint16_t cols = uint16_t(app->width / 8), rows = uint16_t(app->height / 16);
		bgfx::dbgTextPrintf(cols / 2, rows / 2, 0x0f, "+");  // aim reticle
		bgfx::dbgTextPrintf(1, 1, 0x0f, "PLAY  WASD:walk  space:jump  LMB:fire  %s   B:sky   N:navmesh   E:edit   esc:quit",
		                    app->onGround ? "[grounded]" : "[airborne]");
	}
	if (app->net)
		bgfx::dbgTextPrintf(1, 0, 0x0b, "%s  players online: %zu  AI agents: %zu",
		                    app->online ? "online" : "connecting", app->remotePlayers.size() + 1,
		                    app->agents.size());

	bgfx::frame();
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
	App* app = static_cast<App*>(appstate);
	if (!app) return;
#if !defined(__EMSCRIPTEN__)
	if (app->bakeThread.joinable()) app->bakeThread.join();  // let any in-flight bake finish
#endif
	if (app->bgfxInitialized) {
		for (Mesh& m : app->meshes) m.destroy();
		app->grid.destroy();
		app->avatarMesh.destroy();
		app->agentMesh.destroy();
		for (Model& m : app->npcModels) m.destroy();
		if (bgfx::isValid(app->navDbgVbh)) bgfx::destroy(app->navDbgVbh);
		if (bgfx::isValid(app->lightWire)) bgfx::destroy(app->lightWire);
		for (bgfx::UniformHandle u : {app->u_lightParams, app->u_sunDir, app->u_lightPosRadius, app->u_lightColor})
			if (bgfx::isValid(u)) bgfx::destroy(u);
		for (bgfx::TextureHandle t : app->textures)
			if (bgfx::isValid(t) && t.idx != app->whiteTex.idx) bgfx::destroy(t);
		if (bgfx::isValid(app->whiteTex)) bgfx::destroy(app->whiteTex);
		for (bgfx::TextureHandle t : app->skyTex) if (bgfx::isValid(t)) bgfx::destroy(t);
		if (bgfx::isValid(app->fullscreen)) bgfx::destroy(app->fullscreen);
		if (bgfx::isValid(app->skyProgram)) bgfx::destroy(app->skyProgram);
		for (bgfx::UniformHandle u : {app->s_sky, app->u_skyParams})
			if (bgfx::isValid(u)) bgfx::destroy(u);
		if (bgfx::isValid(app->s_tex)) bgfx::destroy(app->s_tex);
		if (bgfx::isValid(app->lightmapTex)) bgfx::destroy(app->lightmapTex);
		if (bgfx::isValid(app->s_lightmap)) bgfx::destroy(app->s_lightmap);
		if (bgfx::isValid(app->u_albedo)) bgfx::destroy(app->u_albedo);
		if (bgfx::isValid(app->program)) bgfx::destroy(app->program);
		bgfx::shutdown();
	}
	if (app->net) netDisconnect(app->net);
	if (app->window) SDL_DestroyWindow(app->window);
	delete app;
	SDL_Quit();
}
