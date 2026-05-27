# cooped

A multiplayer, collaboratively-editable FPS with Quake-style convex brushes, in-game editing
(Sauerbraten "press E"), and baked HDR radiosity. Runs on desktop and in the browser.

See [`DESIGN.md`](DESIGN.md) for the full architecture (render `DESIGN.html` with the command at
the bottom for a nicer read).

## Status

Milestones 1–5 + 7 done: dual-target engine (desktop + browser) · convex-brush CSG + lit
rendering · in-game editor (E-toggle, grid, select, Sauerbraten scroll push/pull, undo) ·
Quake-style player physics (walk/jump on brushes) · **collaborative editing over the network**
with a dedicated server speaking **ENet (native) and WebRTC (browser) in one process**.

## Prerequisites

- CMake ≥ 3.20, a C++20 compiler, and Ninja (or Make).
- Git (dependencies are fetched at configure time via CMake `FetchContent`).
- Network access on the first configure (to clone SDL3 + bgfx).
- For the web build: the Emscripten SDK. A local checkout is vendored at `./emsdk/`
  (gitignored); activate it with `source ./emsdk/emsdk_env.sh`.

## Build & run — desktop

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/cooped
```

You should see a dark window titled "cooped — milestone 1" with the active renderer name
(Vulkan/GL/Metal/D3D depending on platform) printed in the corner. Esc or closing the window
quits.

## Build & run — browser (WebAssembly)

```sh
source ./emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web --target cooped     # build only our target (see note below)
emrun build-web/cooped.html                 # or serve the dir over http and open cooped.html
```

The same scene renders via WebGL2 in the tab.

## Multiplayer (collaborative editing)

The server is authoritative and relays edit ops; native clients use ENet, browser clients use
WebRTC (with WebSocket signaling on `port + 1`).

```sh
# 1) server (ENet udp:27500, signaling ws:27501)
./build/cooped_server

# 2) native client(s)
./build/cooped 127.0.0.1            # or  ./build/cooped <host> [port]

# 3) browser client — serve the web build over plain HTTP so ws:// is allowed
python3 -m http.server 8000 -d build-web
#   open http://localhost:8000/cooped.html  (auto-connects to ws://localhost:27501)
```

Edits on any client (create/delete/move/scroll push-pull) sync to all the others. Running a
client with **no argument** (or the public HTTPS site with no `?server=`) stays single-player.

### Public server over TLS (`wss://`)

An **https** page (e.g. GitHub Pages) can only open a **`wss://`** signaling socket. Give the
server a TLS cert + key and it serves `wss` on `port + 1`:

```sh
./build/cooped_server 27500 /path/fullchain.pem /path/privkey.pem   # e.g. Let's Encrypt
```

Then point the deployed client at it: `https://vampirefrog.github.io/cooped/?server=wss://YOURHOST:27501`
(the host must present a browser-trusted cert; self-signed won't work for `wss`).

> **Why `--target cooped` for the web build:** bgfx.cmake also defines `bimg_encode` (the offline
> texture-compression encoder), which uses x86 CRC32 intrinsics that don't exist on wasm. We don't
> link it, so building only the `cooped` target skips it. The web build also enables `-msimd128`
> (in `CMakeLists.txt`) because bx/bgfx pass SSE flags that Emscripten requires wasm SIMD alongside.

## Dependency versions

Pinned in `CMakeLists.txt`:

- **SDL3** — `release-3.2.30`
- **bgfx** — via [`bgfx.cmake`](https://github.com/bkaradzic/bgfx.cmake) `master`
  (pin to a specific commit for fully reproducible builds)
- **ENet** `v1.3.18` and **libdatachannel** `v0.24.3` (native only; the server's transports)

Later milestones add Recast/Detour, Embree, Jolt, xatlas, etc. (also via FetchContent).

## Conventions

Right-handed, **Z-up**, floating-point world units at Quake scale. See `DESIGN.md` §17.

---

To regenerate the styled design doc preview:

```sh
pandoc DESIGN.md -s --embed-resources --metadata title="cooped — Design Document" \
  -H /tmp/cooped-style.html -o DESIGN.html
```
