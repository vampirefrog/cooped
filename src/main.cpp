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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "brush.h"
#include "net.h"
#include "physics.h"
#include "protocol.h"

#include "shaders/generated/glsl/vs_world.sc.bin.h"
#include "shaders/generated/essl/vs_world.sc.bin.h"
#include "shaders/generated/spirv/vs_world.sc.bin.h"
#include "shaders/generated/glsl/fs_world.sc.bin.h"
#include "shaders/generated/essl/fs_world.sc.bin.h"
#include "shaders/generated/spirv/fs_world.sc.bin.h"

namespace {

constexpr float kGridExtent = 2048.0f;  // half-size of the rendered grid

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
	app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout);  // may be empty if collapsed
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
	app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout);
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

// Apply a message received from the server to the local map.
void applyNetMessage(App* app, const uint8_t* data, size_t len) {
	ByteReader r(data, len);
	switch ((MsgType)r.u8()) {
		case MsgType::Snapshot: {
			const uint32_t n = r.u32();
			app->brushes.clear();
			for (uint32_t i = 0; i < n && r.ok; ++i) app->brushes.push_back(readBrush(r));
			app->selectedId = 0;
			app->selectedFace = -1;
			rebuildSceneMeshes(app);
			break;
		}
		case MsgType::CreateBrush: {
			Brush b = readBrush(r);
			if (!r.ok) break;
			app->brushes.push_back(b);
			app->meshes.push_back(buildMeshFromBrush(b, app->layout));
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
				app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout);
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
				app->meshes[bi] = buildMeshFromBrush(app->brushes[bi], app->layout);
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
	if (!app->editMode) return;
	const bx::Vec3 ro = app->cam.pos;
	const bx::Vec3 rd = app->cam.forward();
	float best = 1.0e30f;
	for (int i = 0; i < (int)app->brushes.size(); ++i) {
		const RayHit h = rayBrushIntersect(ro, rd, app->brushes[i]);
		if (h.hit && h.t < best) {
			best = h.t;
			app->targeted = i;
			app->targetedFace = h.face;
		}
	}
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
	bgfx::setDebug(BGFX_DEBUG_TEXT);

	app->layout.begin()
	    .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
	    .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
	    .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
	    .end();
	app->program = createWorldProgram();
	app->u_albedo = bgfx::createUniform("u_albedo", bgfx::UniformType::Vec4);
	app->s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

	const uint32_t whitePixel = 0xffffffff;
	app->whiteTex = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
	                                      bgfx::copy(&whitePixel, 4));
	const char* texFiles[] = {"data/brick.jpg", "data/floor.jpg", "data/tiles.jpg"};
	for (const char* tf : texFiles) {
		bgfx::TextureHandle t = loadTexture(tf);
		if (!bgfx::isValid(t)) { t = app->whiteTex; SDL_Log("texture load failed: %s", tf); }
		app->textures.push_back(t);
	}

	// Avatar: a body-sized box centered at origin (translated per remote player when drawn).
	app->avatarMesh = buildMeshFromBrush(makeBox({0, 0, 0}, kPlayerHalf, 0.8f, 0.8f, 0.8f), app->layout);

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
				app->selectedId = (app->targeted >= 0) ? app->brushes[app->targeted].id : 0;
				app->selectedFace = app->targetedFace;  // the face becomes the push/pull target
				resetPushPull(app);
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (app->editMode) {
				if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_G]) {  // G + wheel: change grid size
					if (event->wheel.y > 0) app->gridStep = bx::min(app->gridStep * 2.0f, 512.0f);
					else if (event->wheel.y < 0) app->gridStep = bx::max(app->gridStep * 0.5f, 8.0f);
					rebuildGrid(app);
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
				else switch (event->key.key) {
					case SDLK_RETURN: createBrushAtAim(app); break;
					case SDLK_T: cycleTexture(app); break;
					case SDLK_DELETE:
					case SDLK_X: deleteSelected(app); break;
					case SDLK_RIGHT: nudgeSelected(app, {1, 0, 0}); break;
					case SDLK_LEFT:  nudgeSelected(app, {-1, 0, 0}); break;
					case SDLK_UP:    nudgeSelected(app, {0, 1, 0}); break;
					case SDLK_DOWN:  nudgeSelected(app, {0, -1, 0}); break;
					case SDLK_PAGEUP:   nudgeSelected(app, {0, 0, 1}); break;
					case SDLK_PAGEDOWN: nudgeSelected(app, {0, 0, -1}); break;
					case SDLK_LEFTBRACKET:
						app->gridStep = bx::max(app->gridStep * 0.5f, 8.0f);
						rebuildGrid(app);
						break;
					case SDLK_RIGHTBRACKET:
						app->gridStep = bx::min(app->gridStep * 2.0f, 512.0f);
						rebuildGrid(app);
						break;
					default: break;
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
	}

	updateTargeted(app);

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

	const int selIdx = indexOfId(app->brushes, app->selectedId);
	const uint64_t triState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
	                          BGFX_STATE_DEPTH_TEST_LESS;
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	for (int i = 0; i < (int)app->meshes.size(); ++i) {
		const Mesh& m = app->meshes[i];
		if (!bgfx::isValid(m.vbh)) continue;
		// Draw each face with its own texture (selection/targeting shown by the outline below).
		for (const FaceRange& fr : m.faces) {
			const uint32_t tid = app->brushes[i].planes[fr.face].textureId;
			const bgfx::TextureHandle tex = (tid < app->textures.size()) ? app->textures[tid] : app->whiteTex;
			bgfx::setTexture(0, app->s_tex, tex);
			bgfx::setVertexBuffer(0, m.vbh);
			bgfx::setIndexBuffer(m.ibh, fr.firstIndex, fr.numIndices);
			bgfx::setUniform(app->u_albedo, white);
			bgfx::setState(triState);
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
			bgfx::setVertexBuffer(0, app->avatarMesh.vbh);
			bgfx::setIndexBuffer(app->avatarMesh.ibh);
			bgfx::setUniform(app->u_albedo, col);
			bgfx::setState(triState);
			bgfx::submit(0, app->program);
		}
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
	if (app->editMode) {
		const uint16_t cols = uint16_t(app->width / 8), rows = uint16_t(app->height / 16);
		bgfx::dbgTextPrintf(cols / 2, rows / 2, 0x0f, "+");  // crosshair
		bgfx::dbgTextPrintf(1, 1, 0x0e, "EDIT [%s]  grid:%d  brushes:%zu  sel:%u face:%d",
		                    app->net ? (app->online ? "online" : "connecting") : "local",
		                    (int)app->gridStep, app->brushes.size(), app->selectedId, app->selectedFace);
		bgfx::dbgTextPrintf(1, 2, 0x0a, "L-click:select face   wheel:push(up)/pull(down)   Enter:new   X/Del:delete");
		bgfx::dbgTextPrintf(1, 3, 0x0a, "arrows/PgUp/PgDn:move  G+wheel:grid  T:texture  Ctrl+Z/Y:undo  E:play");
	} else {
		bgfx::dbgTextPrintf(1, 1, 0x0f, "PLAY  WASD:walk  space:jump  %s   E:edit   esc:quit",
		                    app->onGround ? "[grounded]" : "[airborne]");
	}
	if (app->net)
		bgfx::dbgTextPrintf(1, 0, 0x0b, "%s  players online: %zu",
		                    app->online ? "online" : "connecting", app->remotePlayers.size() + 1);

	bgfx::frame();
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
	App* app = static_cast<App*>(appstate);
	if (!app) return;
	if (app->bgfxInitialized) {
		for (Mesh& m : app->meshes) m.destroy();
		app->grid.destroy();
		app->avatarMesh.destroy();
		for (bgfx::TextureHandle t : app->textures)
			if (bgfx::isValid(t) && t.idx != app->whiteTex.idx) bgfx::destroy(t);
		if (bgfx::isValid(app->whiteTex)) bgfx::destroy(app->whiteTex);
		if (bgfx::isValid(app->s_tex)) bgfx::destroy(app->s_tex);
		if (bgfx::isValid(app->u_albedo)) bgfx::destroy(app->u_albedo);
		if (bgfx::isValid(app->program)) bgfx::destroy(app->program);
		bgfx::shutdown();
	}
	if (app->net) netDisconnect(app->net);
	if (app->window) SDL_DestroyWindow(app->window);
	delete app;
	SDL_Quit();
}
