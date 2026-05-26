# cooped

A multiplayer, collaboratively-editable FPS with Quake-style convex brushes, in-game editing
(Sauerbraten "press E"), and baked HDR radiosity. Runs on desktop and in the browser.

See [`DESIGN.md`](DESIGN.md) for the full architecture (render `DESIGN.html` with the command at
the bottom for a nicer read).

## Status: milestone 1 — dual-target skeleton

Stands up the engine shell and proves the load-bearing risk (one C++ codebase running on both
desktop and the browser): SDL3 owns the window/input/loop, bgfx owns the GPU bound to SDL3's
native window handle, single-threaded for the web. It clears the screen and draws debug text.
No shaders/geometry yet — that's milestone 2.

## Prerequisites

- CMake ≥ 3.20, a C++17 compiler, and Ninja (or Make).
- Git (dependencies are fetched at configure time via CMake `FetchContent`).
- Network access on the first configure (to clone SDL3 + bgfx).
- For the web build: the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
  (`emcc` / `emcmake` on your `PATH`).

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
# from an activated emsdk environment (emsdk_env.sh)
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
emrun build-web/cooped.html        # or serve the dir over http and open cooped.html
```

The same scene renders via WebGL2 in the tab.

## Dependency versions

Pinned in `CMakeLists.txt`:

- **SDL3** — `release-3.2.0` (bump to the latest 3.2.x as desired).
- **bgfx** — via [`bgfx.cmake`](https://github.com/bkaradzic/bgfx.cmake) `master`
  (pin to a specific commit for fully reproducible builds).

Later milestones add ENet, Recast/Detour, Embree, Jolt, xatlas, etc. (also via FetchContent).

## Conventions

Right-handed, **Z-up**, floating-point world units at Quake scale. See `DESIGN.md` §17.

---

To regenerate the styled design doc preview:

```sh
pandoc DESIGN.md -s --embed-resources --metadata title="cooped — Design Document" \
  -H /tmp/cooped-style.html -o DESIGN.html
```
