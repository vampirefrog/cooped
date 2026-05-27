# cooped — Design Document

A multiplayer, collaboratively-editable FPS with Quake-style convex brush geometry,
in-game editing (Sauerbraten "press E"), and baked HDR global illumination.
Runs on desktop and in the browser.

---

## 1. The core idea, distilled

- **Geometry model: convex brushes** (Quake/TrenchBroom), *not* Cube 2's octree voxels.
  A brush is a convex polyhedron defined as the intersection of half-spaces (planes).
- **Editing model: Sauerbraten's "press E"** — flip between *play* and *edit* in the same
  running client/session. No separate editor app.
- **Multiplayer is first-class for both play and edit.** The server holds the authoritative
  map; new clients get a full snapshot on join, then live edit ops.
- **Lighting target: Source/VRAD quality** — baked HDR lightmaps with radiosity bounce/GI,
  plus a real-time *preview* mode so editing isn't blind.

This is essentially "Tesseract/Cube 2 editing UX + Quake brush geometry + VRAD lighting,
in a browser-capable engine."

---

## 2. Technology stack (recommended)

| Concern | Choice | Why |
|---|---|---|
| Language / core | **C++20** | enet, Recast, Embree, Jolt, bgfx are all C++ (bx/bgfx require C++20). Native + wasm from one codebase. |
| Browser target | **Emscripten → WebAssembly** | Compile the same C++ to the web. Proven by Tesseract/Cube 2 web ports. |
| Build targets | **client** (native + wasm), **server/coordinator**, **headless worker** | One codebase; headless worker drops renderer/input/audio (§13). |
| Rendering abstraction | **bgfx** | One API over WebGL2/WebGPU, GL, Vulkan, D3D, Metal. Solves browser+desktop in one layer. Supports MRT for deferred shading. |
| Windowing/input/audio | **SDL3** (native + web) | Current stable (3.2.x, 2025); SDL2 is maintenance-only. Its **`SDL_AppIterate` callback main loop** maps cleanly to *both* native and the browser's non-blocking rAF loop — no `emscripten_set_main_loop` hack. Supports Emscripten as a first-class platform. **Keep bgfx for rendering** — SDL3's own `SDL_gpu` targets Vulkan/Metal/D3D12 but has **no WebGL/WebGPU backend**, so it can't render in the browser. SDL3 = window/input/audio/loop only. |
| Networking | **ENet** (native UDP) + **libdatachannel** WebRTC (browser), both in the server process | Single process speaks both, no gateway; WSS signaling embedded. See §7. |
| Navmesh / AI | **Recast & Detour** | Industry standard; what you named. |
| Physics | **Custom Quake-style swept traces** (+ optional **Jolt** for props) | See §8. |
| Lightmap UVs | **xatlas** | Best-in-class open-source atlas/unwrap. |
| GI bake | **Patch-to-patch radiosity**, **Embree** for form-factor visibility/BVH | See §6. |
| Models/animation | **IQM** (Inter-Quake Model) | Skeletal animation, open, born from the Cube community. Use for players, monsters, mapmodels. |
| Static / generated meshes | **glTF/GLB** via cgltf + meshoptimizer + VHACD | Format AI providers return; decimation + collision-hull conditioning for mapmodels (§15). |
| AI model gen | Pluggable **`ModelGenProvider`** (Meshy/Tripo/Rodin/… + self-hosted Hunyuan3D/TRELLIS) | Text/image → mapmodel, keys server-side (§15). |
| Scripting/config | **Lua** (embedded) | Map vars, console binds, entity logic. |
| Chat bridge | **External relay (Matterbridge)** over HTTP+WebSocket; in-game chat over the reliable channel | Keeps IRC/Discord protocol code out of the C++ server (§16). |

**Language is C++** (with Emscripten for the web). enet, Recast, Embree, Jolt, and bgfx are all
native C++, so one codebase compiles to desktop and wasm with no binding layer.

---

## 3. Map data model

```
Map
├── worldspawn (global key/values: skybox, sun angle, ambient, fog, gravity…)
├── brushes[]        // each owns a server-assigned uint id
│   └── faces[]      // plane (n, d) + texture + UV axes/offset/scale/rotation
├── patches[]        // Quake3 Bezier patches: control-point grid + texture (§5)
├── entities[]       // key/value dicts (Quake .map style)
│   ├── info_player_start, light_*, mapmodel, trigger, mover, jumppad, teleport…
├── mapmodels[]      // referenced .iqm/.obj props with transform + collision hull
└── lightmaps[]      // baked HDR atlas pages (RGBA16F or RGBM-encoded)
```

- **On-disk format is Quake `.map`-compatible** (plane-based brushes + entity dicts).
  Big payoff: **TrenchBroom can open/export your maps** as an external editor, and you get a
  proven, compact, text format for free. Lightmaps/navmesh ship in a sidecar binary (`.bsp`-like
  compiled blob) keyed by a map content hash.
- Brushes store **planes**, not vertices. The renderable polygon for a face = its plane
  clipped by every other face's half-space. Recompute on edit.

---

## 4. Geometry pipeline (brush → renderable + collidable)

1. **CSG / hull build:** for each brush face, clip the plane polygon against all sibling
   half-spaces → convex face polygon → triangulate.
2. **Spatial index (editing/MVP):** insert brushes into a loose octree / spatial hash for
   frustum culling (render) and collision broadphase. This is what's live *while editing* —
   cheap to keep current as brushes change.
3. **BSP + PVS (compiled, planned):** a full **BSP tree + VIS/PVS** is compiled as a later
   addition for runtime culling/occlusion and faster collision queries on big maps. It's
   expensive to rebuild, so it's **not maintained live during edits** — it's a **distributed
   compute job** (§13), baked alongside lighting, and the map falls back to the spatial index
   (step 2) until a fresh BSP/PVS is available. Compiled blob ships in the map sidecar.
4. **Lightmap atlas:** xatlas unwraps world faces **and tessellated Bezier patches** into atlas
   UVs (separate from texture UVs) — patches are treated as ordinary world geometry here.
   Default texel density **1 luxel / 16 world units** (per-surface override), packed into
   **4096² atlas pages**.
5. **Mesh batching:** group faces by material into draw batches; rebuild only dirty batches
   when a brush changes (incremental, so editing stays responsive).

Curved surfaces are handled separately as **Bezier patches** (§5).

---

## 5. Bezier patches (Quake 3 curves)

Curved geometry via **biquadratic Bezier patches** (Quake 3 style), alongside brushes.

- **Definition:** a control-point grid of size `(2n+1) × (2m+1)`, composed of `3×3`
  biquadratic patch cells. Stored in the `.map` as **`patchDef2`** (TrenchBroom and Radiant both
  edit these, keeping external-editor compatibility). The editor offers **patch primitives** —
  simple curve, bevel, end-cap, cylinder, cone, sphere.
- **Tessellation:** subdivide each `3×3` cell via the Bezier basis with **adaptive LOD**
  (curvature / screen-space error). Cache the mesh; rebuild only the edited patch (incremental,
  like dirty brush batches). A **coarser fixed tessellation** is used for collision and lighting.
  Collapsed control rows (cone tips, capped cylinders) are handled gracefully — normals at a
  collapsed pole are reconstructed from neighbors rather than from zero-area triangles.
- **Texture operations (GtkRadiant-style):** texcoords live at the control points `(s,t)`. The
  editor provides the same patch texturing commands as GtkRadiant:
  - **Naturalize** — recompute `(s,t)` across the control grid so the texture flows evenly over
    the curve with no stretching/distortion (the main "fix the texture coords" command).
  - **Project onto an axis-aligned plane** — derive `(s,t)` by projecting the patch from a
    chosen/dominant axial plane (XY/XZ/YZ), exactly like axial projection on a brush face.
  - **Cap**, **Fit**, **Set S/T**, and tile/scale/rotate/shift offsets.
  Lightmap UVs are kept separate (parametric, see below).
- **Collision (§8):** generate a fixed-LOD collision mesh and convert it to **convex facets**
  (triangle + bevel planes), à la Quake 3 `CM_GeneratePatchCollide`, since a curved patch is
  non-convex. The swept-AABB trace from §8 runs against these facets — same routine as brushes.
- **Baked lighting (§6):** patches are lightmap-unwrapped with **xatlas on their tessellated
  mesh, exactly like world brush faces** — no special parametric path; a patch is just more
  world geometry to the baker. They participate in the radiosity bake as receivers and emitters.
- **Networking:** patches sync as edit ops just like brushes (see §7).

---

## 6. Lighting pipeline (the VRAD-style part)

Two modes that coexist:

**A. Real-time preview (while editing)** — deferred/clustered shading (Tesseract-style):
point, spot, sun, area lights rendered live so you can place lights and see *roughly* what
you'll get. No bounce, cheaper. This keeps editing interactive.

**B. Baked HDR (final quality)** — triggered by a `bake` command:
1. Gather emitters: point/sphere lights, **area lights** (emissive quads), spotlights,
   directional **sun**, and **emissive textures** (treated as area emitters).
2. Lightmap-unwrap world via xatlas → texel positions + normals.
3. **Direct lighting:** trace shadow rays from each texel to each light (Embree BVH).
4. **Bounce/GI:** **patch-to-patch radiosity** — subdivide surfaces into patches, compute
   form factors between mutually-visible patches (Embree for the visibility/BVH), and solve the
   radiosity equation, iterating N bounces. Closest to VRAD/HL2. (No hemicube/path-tracing path.)
5. **Seam fixup:** run a **post-bake seam-fixup pass** over atlas/patch boundaries (and tile
   boundaries from distributed bakes, §13) — average/blend matching edge texels so cross-machine
   float differences don't show as visible seams.
6. **Encoding:** store the master lightmap as **`RGBA16F` on disk / in the map sidecar** (full
   HDR, lossless). When delivering to a **WebGL2** client, **convert to RGBM** at upload (encodes
   HDR into RGBA8, no float-texture dependency). Desktop/WebGPU can sample the `RGBA16F` directly.
7. Runtime: sample baked lightmap, add real-time dynamic lights for moving objects, then
   **tonemap + bloom** on output (HDR → LDR).

**Who bakes in multiplayer:** a client runs the bake locally and **uploads the lightmap blob**
to the server, which redistributes it (keyed by map hash). Avoids a heavy server CPU bill and
lets bakes happen on a beefy machine. Long bakes can be incremental (only dirty atlas regions).

---

## 7. Networking (ENet + browser)

**Two protocol layers over one transport:**

1. **Gameplay netcode** — authoritative server, client-side prediction + reconciliation,
   delta-compressed snapshots (Quake 3 / Source model). Movement, shooting, monster state.
   Defaults: **20 Hz server snapshots**, **60 Hz client command rate**, **~100 ms interpolation
   delay** (≈2 snapshots) — all configurable.
2. **Edit netcode** — operation-based. Server assigns brush/entity IDs, sequences ops, and
   rebroadcasts. **Last-writer-wins per object** with server authority is enough (don't need
   full CRDT/OT for a map editor). On join: server streams the **full map snapshot**, then the
   client tails the live op log.

   Edit ops (compact, plane-based brushes are tiny on the wire):
   `CreateBrush, DeleteBrush, DragFace, MoveBrush, SetFaceTexture, SetUV,
    CreatePatch, EditPatchCP, SetPatchTexture, SetPatchTess, DeletePatch,
    CreateEntity, EditEntity, DeleteEntity, PlaceMapmodel, UploadLightmap`.

**The browser problem:** browsers cannot open raw UDP sockets, so native ENet doesn't run in a
tab. Clients therefore split by platform:
- **Desktop client:** ENet over UDP.
- **Browser client:** the browser's native `RTCPeerConnection` (via Emscripten JS interop — the
  browser supplies WebRTC, so the wasm client links no WebRTC lib).

**The server speaks both transports in one process — no gateway.** It runs an ENet host *and*
an embedded WebRTC endpoint side by side:
- **WebRTC via [libdatachannel]** (lightweight C++ DataChannel stack — *not* Google's libwebrtc).
  It bundles ICE/DTLS/SCTP, so the server is the WebRTC peer directly.
- **Embedded WebSocket (WSS) signaling listener** in the same process for the SDP/ICE handshake
  (needs a TLS cert). A STUN server handles NAT discovery (the server is publicly reachable), and
  a **self-hosted TURN relay (e.g. coturn) is a fallback** for clients whose direct/STUN path
  fails (strict NATs, locked-down networks) — traffic relays through TURN rather than dropping
  the connection. **TURN endpoint + credentials are server config settings** (off by default;
  point it at your own relay to enable).
- **Unified `Connection` abstraction:** an interface (`sendReliable`, `sendUnreliable`, `recv`,
  `disconnect`, capability flags) with two implementations — `EnetPeer` and `RtcPeer`. All game
  and edit code talks only to `Connection` and never knows which transport a client uses.
- **Channel mapping** (symmetric across both transports): each browser client opens **two data
  channels** — one *reliable+ordered* (edits, asset streaming, control) and one *unreliable+
  unordered, maxRetransmits=0* (gameplay snapshots/movement). These mirror ENet's reliable and
  unreliable channels.
- **Event loop:** ENet is polled in the server tick (`enet_host_service`); libdatachannel
  delivers via callbacks on its own threads, so those callbacks **enqueue into a thread-safe
  queue drained by the authoritative game loop**. Outgoing sends are issued from the game thread.

Trade-off (accepted): one process is simpler to deploy and lower-latency (no extra hop), at the
cost of linking libdatachannel + a WebSocket/TLS stack (OpenSSL) into the server and managing a
signaling cert. Native headless workers/compute nodes (§13) connect over ENet like any desktop
client.

[libdatachannel]: https://github.com/paullouisageneau/libdatachannel

---

## 8. Physics (keep it simple)

You don't need a rigid-body engine for the core game. Recommended split:

- **Player & world: custom Quake-style collision.** Player is an AABB (or capsule) swept
  against brush half-spaces (clip-move / "trace" against planes), with classic Quake
  acceleration, friction, air-control, step-up, and crouch. This is light, deterministic,
  netcode-friendly, and idiomatic for brush worlds. ~No dependency.
- **Mapmodel collision:** simplified convex hulls or AABBs per prop, same trace routine.
- **Dynamic props: Jolt Physics, scoped to barrels, ragdolls, and debris only.** Modern, fast,
  has a wasm build. Jolt is **not** in the movement/world/mapmodel collision path (those stay on
  the custom traces above) — it only drives those few pushable/throwable/floppy objects, so the
  core netcode stays simple. Added as a later milestone.

Recommendation: ship with **custom traces only**; bolt on Jolt later for flavor.

---

## 9. AI (monsters + friendlies)

- **Recast** bakes the navmesh from the compiled world geometry (rebake on significant edits,
  or on map load/bake). **Detour** does runtime pathfinding + path following; **DetourCrowd**
  for multiple agents with local avoidance.
- Behavior: simple **state machines / behavior trees** (idle, patrol, chase, attack, flee).
- Friendlies: follow/escort, share navmesh, faction flag determines targeting.
- Server-authoritative AI (clients just render interpolated state).

---

## 10. Sauerbraten-inspired editing & features

In-game editor (toggle with **E**):
- Free-fly **edit camera**; grid with adjustable size; snap-to-grid.
- **Face dragging / extrude**, brush create/clone/delete, vertex/edge manipulation.
- **Texture browser**, per-face texture + **TrenchBroom-style UV editing** (lock, fit, align,
  rotate, scale, shift).
- **Entity placement:** spawns, lights (all types), mapmodels, triggers, jumppads, teleporters,
  particle emitters, ambient sounds.
- **Heightmap edit mode** for terrain-like sculpting of brush tops.
- **Copy/paste**, **prefabs**, **undo/redo** (per-client, integrated with the op log).
- **Collaborative cursors** — see where other editors are looking/working; edit chat.
- Instant save/reload; `newmap` to start blank.

Sauerbraten gameplay carry-overs:
- Multiple modes: **deathmatch, instagib, CTF, capture, coop-edit**.
- **Master/auth server** + server browser.
- **Demo recording/playback** (record the packet stream).
- **Lua-backed console** for binds, aliases, map vars, and entity scripting (Sauerbraten's
  CubeScript role, played by embedded Lua).

---

## 11. Confirmed extras (all in scope)

- **Skybox** — static. **No day/night / no sun-angle changing** (fixed sun keeps the baked GI
  valid; a moving sun would force constant relights).
- **Decals** (bullet holes, scorch) and **GPU particle systems** (sparks, smoke, blood).
- **Movers/doors/platforms/elevators** as kinematic brush groups on triggers.
- **Jumppads, teleporters, ladders, water volumes** (swim physics + screen tint).
- **Pickups:** weapons, ammo, health, armor with respawn timers.
- **Footstep/material sounds** keyed to surface texture; positional audio (OpenAL / Web Audio).
- **Minimap** — **precomputed** (rendered top-down at bake/save time, shipped with the map).
- **Scoreboard, killfeed, spectator mode.**
- **Bots** for offline play (reuses the monster AI + navmesh).
- **Screenshot + map thumbnail** generation for the server browser.
- **Content hashing** so clients verify they have the right map/assets before joining.

---

## 12. Suggested build order (milestones)

1. **Skeleton:** C++ + bgfx + SDL3 window; native + Emscripten builds both producing a triangle.
2. **Brush core:** brush data model, CSG hull build, render unlit world; fly camera.
3. **In-game editor:** E-toggle, grid, create/drag/delete brushes, face texturing, undo/redo.
   *(Bezier patches: primitives, tessellation, texture ops (naturalize/project), facet collision —
   land alongside or just after the brush editor; patch lightmaps fold into the bake at step 10.)*
4. **Player physics:** Quake-style swept-AABB movement & collision; play/edit toggle.
5. **Networking I:** ENet server holding authoritative map; full-snapshot join; live edit ops
   (single transport, native first).
6. **Progressive loading:** manifest + content-addressed cache; staged brushes→entities→spawn
   gate (no gravity until brushes built, no spawn until entities ready); background asset stream
   with placeholders.
7. **Networking II:** gameplay snapshots + prediction; shooting; then WebRTC backend for browser.
8. **Lighting preview:** deferred real-time lights (point/spot/sun/area).
9. **Distributed compute:** coordinator + job/task model + headless worker build + client
   native background-thread workers (browser clients excluded) + fault tolerance + progress HUD.
10. **Lighting bake:** xatlas unwrap + Embree direct + **radiosity bounce distributed over the
    pool**; HDR lightmaps; tonemap/bloom; assemble-and-hot-swap flow.
11. **AI:** Recast navmesh + Detour monsters/friendlies (navmesh bake runs on the pool).
12. **AI model gen:** glTF/GLB loader + `ModelGenProvider` abstraction; server-routed Meshy
    (first provider) → prompt-to-mapmodel with placeholder/hot-swap; then self-hosted backend.
13. **Chat:** in-game text chat (scopes + moderation) over the reliable channel; `ChatBus` +
    server-side relay connector to Matterbridge (IRC/Discord/…).
14. **Content & polish:** IQM models, mapmodels, entities, modes, audio, server browser, extras.
15. **BSP + PVS (perf, when maps get big):** compile BSP tree + VIS as a distributed job for
    occlusion culling and faster collision; spatial index remains the live edit-time fallback.

---

## 13. Distributed background compute

Long-running map computations — **radiosity lighting bake, BSP build, VIS/PVS, Recast navmesh,
xatlas unwrap** — never block gameplay and are spread across a worker pool.

### Worker pool (native only)
Heavy bakes use **Embree**, which is x86 SIMD and **does not compile to WebAssembly** — so
compute is **native-only**. **Browser clients just play; they never run bake jobs.** Workers are:
- **Native game clients** that have opted in (`compute_contribute 1`, default on, toggleable).
- **The server**, optionally configured as a worker as well as the coordinator.
- **Headless compute nodes (native-only):** a separate **native** build target of the same C++
  engine with no renderer/input/audio (no wasm/Node variant). Connects over ENet, registers as a
  worker, and idles waiting for jobs. Uses all cores; ideal for a dedicated bake box or cloud
  instances.

The **server is the coordinator/broker** (scheduling + assembly), and may also be a worker.

### Job → task model
- **Job** = a high-level request bound to a **map content hash**, e.g. *"radiosity bake,
  full"* or *"rebake dirty atlas tiles {…}"*, *"compute PVS"*, *"bake navmesh"*.
- **Task** = an independent unit: a lightmap tile, a PVS cluster, a navmesh region. Has an id,
  an input reference, and params.
- Coordinator state machine per job:
  `PENDING → SPLIT → DISTRIBUTING → GATHERING → ASSEMBLING → COMPLETE | FAILED | SUPERSEDED`.

### Scheduling & fairness
- Workers advertise **capabilities**: cores, SIMD, memory, headless-vs-client, opted-in,
  supported job types. Headless nodes get large chunks; **game clients get small chunks** so
  framerate stays smooth.
- On a native game client the worker runs on a **low-priority background thread**, time-sliced so
  it never starves the render/game loop. *"Run in the background so the player can still play"* is
  a hard requirement here.
- **Gameplay packets get QoS priority** over bulk task/result traffic on the wire.

### Parallelism notes per job type
- **Radiosity** is parallel *within* a bounce but has a **sync barrier between bounces**:
  Phase 0 atlas prep (once, shared) → Phase 1 direct lighting per tile (parallel) → Phase 2..N
  bounce iterations, each gathering from the previous full lightmap. Workers cache static
  geometry/BVH; only the evolving lightmap is exchanged each bounce.
- **VIS/PVS** is parallel per cluster/leaf (embarrassingly parallel).
- **BSP build** is largely sequential — run it whole on one strong worker; feeds VIS.
- **Navmesh (Recast)** parallel per tile.

### Correctness & lifecycle
- **Staleness:** every job is keyed to a map hash. An edit supersedes in-flight jobs for the
  changed regions (incremental dirty-region rebakes preferred over full rebakes).
- **Fault tolerance:** tasks are idempotent and at-least-once. If a worker disconnects, opts
  out, or times out, the coordinator **reassigns** the task. Results deduped by task id.
- **Float determinism / tile seams:** heterogeneous machines can produce slightly different
  results at tile boundaries. Rather than chase bit-exact determinism, rely on the
  **post-bake seam-fixup pass** (§6, step 5) to blend matching edge texels — good enough for now.
- **Data delivery:** workers need the current map; headless nodes fetch it from the server like
  any client (§14). Large task payloads/results are chunked over the reliable channel.
- **On completion:** the assembled artifact (e.g. lightmap blob, PVS, navmesh) is distributed
  to all clients and **hot-swapped at runtime** — no map reload. Replaces the
  "client bakes and uploads" flow in §6: now *the pool bakes, the coordinator assembles*.
- **UX:** server reports aggregate progress to all players — *"Baking: 47% · 6 workers"* — like
  Sauerbraten's `calclight` feedback.

## 14. Progressive / async loading on join

A joining player starts loading immediately and is **gated into play by collision readiness**,
not made to wait for the whole map.

### Connection lifecycle
1. **Connect** → handshake; receive **map manifest** (asset list + content hashes + sizes) and
   worldspawn metadata (gravity, fog, sun…).
2. **Inert state:** the player exists as a **frozen, non-colliding free-look camera** with
   **gravity OFF** and **no spawn**. Shows a loading HUD with progress. Cannot fall through the
   not-yet-loaded world.
3. **Prioritized streaming** begins:
   - **P1 — brush geometry** (the collision world). The client receives planes, runs the CSG
     hull build locally, and **ACKs `brushesReady`**. *Gravity stays off until this fires.*
   - **P2 — entities** (spawn points, triggers, colliders, light/mapmodel metadata). Client
     ACKs `entitiesReady`. Spawn points are needed to know *where* to put the player.
4. **Spawn gate:** the server spawns the player **only when both `brushesReady` AND
   `entitiesReady`** are true — now there is something to collide with and somewhere to stand.
   Gravity, collision, and control switch on.
5. **Keep streaming in the background** after spawn, with placeholders so play isn't blocked:
   - **Textures** → checkerboard/dev texture until loaded, then pop in.
   - **Models (IQM/mapmodels)** → bbox/low-poly proxy until loaded.
   - **Lightmaps** → render with the real-time preview lighting (§6) or fullbright until the
     baked HDR lightmap arrives from the compute pool (§13), then hot-swap.
   - Audio, particles, distant/LOD assets last.

### Mechanics
- **Two server-tracked readiness flags per client** (`brushesReady`, `entitiesReady`), driven
  by **client ACKs** (because the client must finish *building* collision structures after
  receiving data, not just receive bytes).
- **Content-addressed cache** (filesystem native / IndexedDB browser): returning players skip
  re-downloading anything whose hash they already hold → near-instant joins.
- **Proximity prioritization** after spawn: stream assets nearest the player first.
- **Bandwidth QoS:** bulk asset streaming yields to gameplay packets (shares the priority rule
  from §13).
- **Edits restream deltas only:** an edit pushes just the changed brushes/entities and triggers
  an incremental local rebuild — not a full reload.
- **Browser:** run CSG/atlas builds on a Web Worker to keep the render thread responsive.

## 15. AI-generated 3D models (text/image → mapmodel)

In edit mode, type a prompt (or drop a reference image) and a generated mesh is imported and
placed as a **mapmodel**. Backed by a pluggable provider layer so any API works.

### Provider abstraction
A common `ModelGenProvider` interface — `submit(prompt|image, params) → jobId`,
`poll(jobId) → status/progress`, `fetch(jobId) → asset` — with interchangeable backends:
- **Cloud APIs:** Meshy, Tripo, Rodin (Hyper3D/Deemos), Luma Genie, Stability (Stable Fast 3D),
  CSM, Sloyd. Aggregators **Replicate / fal.ai** expose many models behind one API.
- **Self-hosted:** a GPU service running open models — **Hunyuan3D, TRELLIS, TripoSR/TripoSG,
  InstantMesh, Stable Fast 3D** — exposed over HTTP; the server treats it as just another
  provider endpoint (and it can register like a specialized GPU worker, §13).

### Secrets & routing (important)
**API keys live on the server, never on clients.** A client sends the prompt → the **server
calls the provider** with its credentials → the result becomes a normal **content-addressed
asset** distributed via the streaming/cache path (§14). This centralizes secrets, enables
**per-user quotas, rate limiting, and prompt moderation** (paid APIs cost money), and dedups:
identical *prompt+params+provider* hash reuses a prior result instead of regenerating.

### Async + multiplayer flow
1. Editor prompt box → submit job (reuses the job lifecycle + progress HUD from §13; these run
   on the provider/GPU, not the CPU compute pool).
2. A **placeholder proxy** (bbox/low-poly) is placed immediately and **hot-swapped** when the
   mesh arrives — same mechanism as §14.
3. Completion is broadcast as a `PlaceMapmodel` **edit op** so all editors get the asset and
   placement; content-hash caching means no one regenerates it.

### Import / post-processing pipeline
Providers return **glTF/GLB** (also OBJ/FBX/USDZ) → engine gains a **glTF/GLB loader** (cgltf)
for static mapmodels (PBR materials). Generated meshes are then conditioned:
- **Decimate + LOD** (meshoptimizer) — generated meshes are often dense.
- **Collision hull** — convex hull, or **VHACD** for concave props (feeds §8 mapmodel collision).
- **Normalize** scale/orientation/pivot. **No baked GI** — generated mapmodels are **dynamically
  lit only** (no lightmap unwrap), so generation stays instant and never triggers a relight.
- Store **license/attribution + source prompt** metadata alongside the asset.

## 16. Chat & external integrations

### In-game text chat
Server-relayed text over the reliable channel (§7). Scopes: **global, team, coop-edit**, plus
whispers. Each message carries sender + display name + timestamp; the server validates, rate-
limits, and rebroadcasts. Works with zero external config.

### Bridging to IRC / Discord / … — via an external relay (no protocol code in C++)
Embedding an IRC client *and* the Discord gateway (WebSocket, heartbeats, TLS, per-platform rate
limits, reconnection) into the game server is exactly the complexity to avoid. Instead the server
connects to **one external relay** that speaks every platform and exposes a single simple API.

- **Recommended: [Matterbridge]** — a single Go service that bridges IRC, Discord, Slack,
  Telegram, Matrix, XMPP, Mattermost, and more, and exposes an **HTTP + WebSocket "API" gateway**.
  The server POSTs outgoing messages and streams incoming over WebSocket; Matterbridge fans out
  to every platform. This is the "external chat relay" idea, off the shelf.
- **Alternatives:** a Matrix hub + `matrix-appservice-irc` / `-discord` bridges (server speaks the
  Matrix client-server HTTP API to one room); or a thin custom relay (Node/Go with discord.js +
  an IRC lib) exposing WebSocket/gRPC if you want full control.
- **Engine side:** a small **`ChatBus`** with pluggable sources/sinks. In-game chat is one
  source/sink; the **relay connector (server-only)** is another, translating both directions.
  External messages render with their origin, e.g. `[Discord] alice: …`.
- **Secrets stay off clients** (same rule as AI keys, §15): platform tokens live on the relay or
  in server config; clients only ever see normalized `ChatBus` messages. Relay endpoint +
  channel↔gateway mapping are config, **off by default**.
- **Moderation / flood control** lives at the `ChatBus` on the server, applied to every source.

*(Voice chat is out of scope for now — text chat + bridging only.)*

[Matterbridge]: https://github.com/42wim/matterbridge

## 17. Locked decisions

- **Language:** **C++20** (required by bx/bgfx), one codebase to desktop + wasm via Emscripten. No Rust.
- **Render backend:** bgfx (single layer over WebGL2/WebGPU/GL/Vulkan).
- **Browser transport:** **WebRTC DataChannels** — unreliable channel for gameplay, reliable
  for edits. The **server speaks ENet *and* WebRTC in one process (no gateway)** via
  libdatachannel + an embedded WSS signaling listener, behind a unified `Connection` interface.
  STUN for NAT discovery + **self-hosted TURN relay fallback** (coturn), configured via server
  settings, for clients that can't connect directly.
- **GI bounce:** **Radiosity** (patch-to-patch form factors, most VRAD/HL2-like).
- **Dynamic physics:** custom Quake traces for movement/world/mapmodels; **Jolt (later
  milestone) only for barrels, ragdolls, and debris** — kept off the movement critical path.
- **Scripting:** **embed Lua** for config, console binds, map vars, and entity logic.
- **Heavy compute is distributed** across opted-in **native** clients + server + headless nodes,
  server as coordinator, on low-priority background threads. **Native-only (Embree doesn't
  target wasm); browser clients just play** (§13).
- **Loading is staged and collision-gated:** no gravity until brushes are built, no spawn until
  brushes *and* entities are ready, then background-stream the rest (§14).
- **AI model generation** is a pluggable provider layer (cloud + self-hosted), **routed through
  the server** which holds API keys and enforces quotas/moderation; results become
  content-addressed mapmodels that are **dynamically lit (no baked GI)** (§15).
- **Geometry is brushes + Quake3 Bezier patches** — patches with GtkRadiant-style texture ops
  (naturalize / project onto axis-aligned plane), facet-based collision, and **xatlas baked
  lightmaps treated exactly like world geometry** (§5).
- **Chat & integrations:** in-game text chat over the reliable channel; IRC/Discord/etc. via an
  **external relay (Matterbridge), no protocol code in the C++ server**; tokens off clients.
  Voice chat is out of scope for now (§16).
- **Confirmed extras (all in scope):** static skybox (**fixed sun, no day/night**), decals, GPU
  particles, movers/doors/platforms/elevators, jumppads/teleporters/ladders/water, pickups,
  material footstep sounds, **precomputed minimap**, scoreboard, killfeed, spectator, bots,
  screenshot/thumbnail generator, content hashing (§11).
- **BSP + PVS:** planned (not MVP). Compiled as a **distributed job** alongside lighting, not
  maintained live during edits; spatial index is the live fallback (§4).
- **Lightmap encoding:** master is **`RGBA16F`** on disk; **convert to RGBM** at upload for
  WebGL2 clients; WebGPU/desktop sample `RGBA16F` directly (§6).
- **Seam handling:** **post-bake seam-fixup pass** instead of chasing cross-machine float
  determinism (§6 step 5, §13).
- **Headless worker:** **native-only** — no wasm/Node variant (§13).

### Implementation conventions (locked before coding)
- **Coordinates:** right-handed, **Z-up, floating-point world units at Quake scale** (matches
  `.map`/TrenchBroom and the 16-unit lightmap default); convert to bgfx/glTF Y-up at the seams.
- **Simulation:** **fixed 60 Hz tick**, decoupled from render framerate and the 20 Hz snapshot
  rate; render interpolates between ticks (prediction/reconciliation depend on this).
- **`.map` flavor:** **Valve 220** (explicit texture axes), not legacy integer planes.
- **Dependencies:** **CMake FetchContent**, pinned by tag/commit; one tree builds native + wasm.
- **Shaders:** authored in bgfx `.sc`, compiled by **`shaderc`** to per-backend bytecode at build
  time (part of the milestone-1 toolchain).
- **Version control:** git from the start, with a `.gitignore` for build/deps.

## 18. Default tunables (all configurable)

All architecture questions are decided (§17). Sensible defaults, overridable in config:

- **Lightmap density:** 1 luxel / 16 world units, per-surface override; 4096² atlas pages (§4).
- **Movement:** gravity ~800 u/s² (Quake-scale); player ~32 u wide / ~56 u tall (tune to feel).
- **Netcode:** 60 Hz sim tick, 20 Hz server snapshots, 60 Hz client command rate, ~100 ms
  interpolation delay (≈2 snapshots) (§7).
- **TURN relay:** off by default; self-hosted (coturn) endpoint + credentials set in server
  config to enable (§7).
- **Compute contribution:** clients opt in by default (`compute_contribute 1`); toggleable (§13).

These get tuned against real content and latency during the relevant milestones; none affect the
architecture.
