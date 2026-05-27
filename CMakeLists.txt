cmake_minimum_required(VERSION 3.20)
project(cooped CXX)

set(CMAKE_CXX_STANDARD 20)  # bx/bgfx headers require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# bgfx/bx apply SSE flags (e.g. -msse4.2) to their sources. Under Emscripten, em++ requires
# -msimd128 whenever an SSE flag is present, so enable wasm SIMD globally for the web build.
# Must be set before FetchContent_MakeAvailable so it applies to the dependency targets too.
if(EMSCRIPTEN)
  add_compile_options(-msimd128)
endif()

# ---------------------------------------------------------------------------
# Dependencies (pinned; bump tags as desired — see README).
# Milestone 1 only needs SDL3 (window/input/loop) and bgfx (rendering).
# enet / Recast / Embree / Jolt / xatlas come in later milestones.
# ---------------------------------------------------------------------------

# SDL3 -----------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.30     # latest 3.2.x; fixes PipeWire header skew vs 3.2.0
  GIT_SHALLOW    TRUE
)

# bgfx (via the community CMake wrapper, which vendors bx/bimg/bgfx as submodules)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# shaderc is a host tool: build it for native compiles only. The web build can't run a wasm
# shaderc, so it consumes the pre-generated, committed embedded shader headers instead.
if(EMSCRIPTEN)
  set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
else()
  set(BGFX_BUILD_TOOLS ON  CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
  bgfx
  GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
  GIT_TAG        master             # pin to a commit for full reproducibility
)

FetchContent_MakeAvailable(SDL3 bgfx)

# ENet — native UDP transport for the client and the dedicated server. Browsers can't do raw
# UDP, so the web build skips it (net.cpp stubs out) until the WebRTC transport (Milestone 7).
if(NOT EMSCRIPTEN)
  FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(enet)

  # libdatachannel: WebRTC DataChannels + WebSocket signaling, for browser clients (server side).
  # Data channels only (no media); the server speaks ENet *and* WebRTC in one process (no gateway).
  set(NO_MEDIA    ON CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS    ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.24.3
  )
  FetchContent_MakeAvailable(libdatachannel)
endif()

# ---------------------------------------------------------------------------
# Shaders → embedded headers (compiled natively, committed, reused by the web build)
# ---------------------------------------------------------------------------
set(GEN_SHADER_DIR ${CMAKE_SOURCE_DIR}/src/shaders/generated)
if(NOT EMSCRIPTEN)
  include(${bgfx_SOURCE_DIR}/cmake/bgfxToolUtils.cmake)
  set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/src/shaders)
  # 120 -> _glsl (desktop GL), 300_es -> _essl (WebGL2), spirv -> _spv (Vulkan)
  bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS ${SHADER_SRC_DIR}/vs_world.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying.def.sc
    OUTPUT_DIR ${GEN_SHADER_DIR}
    INCLUDE_DIRS ${bgfx_SOURCE_DIR}/bgfx/src
    PROFILES 430 300_es spirv
    AS_HEADERS
    OUT_FILES_VAR VS_SHADER_HEADERS
  )
  bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS ${SHADER_SRC_DIR}/fs_world.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying.def.sc
    OUTPUT_DIR ${GEN_SHADER_DIR}
    INCLUDE_DIRS ${bgfx_SOURCE_DIR}/bgfx/src
    PROFILES 430 300_es spirv
    AS_HEADERS
    OUT_FILES_VAR FS_SHADER_HEADERS
  )
  bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS ${SHADER_SRC_DIR}/vs_sky.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying_sky.def.sc
    OUTPUT_DIR ${GEN_SHADER_DIR}
    INCLUDE_DIRS ${bgfx_SOURCE_DIR}/bgfx/src
    PROFILES 430 300_es spirv
    AS_HEADERS
    OUT_FILES_VAR VS_SKY_HEADERS
  )
  bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS ${SHADER_SRC_DIR}/fs_sky.sc
    VARYING_DEF ${SHADER_SRC_DIR}/varying_sky.def.sc
    OUTPUT_DIR ${GEN_SHADER_DIR}
    INCLUDE_DIRS ${bgfx_SOURCE_DIR}/bgfx/src
    PROFILES 430 300_es spirv
    AS_HEADERS
    OUT_FILES_VAR FS_SKY_HEADERS
  )
  add_custom_target(cooped_shaders DEPENDS ${VS_SHADER_HEADERS} ${FS_SHADER_HEADERS}
                                           ${VS_SKY_HEADERS} ${FS_SKY_HEADERS})
endif()

# ---------------------------------------------------------------------------
# The client executable
# ---------------------------------------------------------------------------
add_executable(cooped
  src/main.cpp
  src/brush.cpp
  src/physics.cpp
  src/net.cpp
)
target_include_directories(cooped PRIVATE src)
# Ensure the embedded shader headers are (re)generated before main.cpp compiles (native only).
if(NOT EMSCRIPTEN)
  add_dependencies(cooped cooped_shaders)
  target_link_libraries(cooped PRIVATE enet)
  target_include_directories(cooped PRIVATE ${enet_SOURCE_DIR}/include)  # enet doesn't export it
endif()

# ---------------------------------------------------------------------------
# Dedicated server (native only): authoritative map + edit-op relay. No renderer.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
  add_executable(cooped_server src/server_main.cpp src/brush.cpp)
  target_include_directories(cooped_server PRIVATE src ${enet_SOURCE_DIR}/include)
  target_link_libraries(cooped_server PRIVATE bx enet LibDataChannel::LibDataChannel)
endif()

# SDL3 exposes SDL3::SDL3-static when built static; SDL3::SDL3 otherwise.
if(TARGET SDL3::SDL3-static)
  target_link_libraries(cooped PRIVATE SDL3::SDL3-static)
else()
  target_link_libraries(cooped PRIVATE SDL3::SDL3)
endif()
target_link_libraries(cooped PRIVATE bgfx bx bimg bimg_decode)

# ---------------------------------------------------------------------------
# Emscripten (browser) target
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
  set_target_properties(cooped PROPERTIES SUFFIX ".html")
  target_link_options(cooped PRIVATE
    "-sUSE_WEBGL2=1"
    "-sMIN_WEBGL_VERSION=2"
    "-sMAX_WEBGL_VERSION=2"
    "-sFULL_ES3=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sGL_ENABLE_GET_PROC_ADDRESS=1"
    # Package the texture set into the virtual filesystem at /data (read at runtime).
    "--preload-file=${CMAKE_SOURCE_DIR}/data/brick.jpg@/data/brick.jpg"
    "--preload-file=${CMAKE_SOURCE_DIR}/data/floor.jpg@/data/floor.jpg"
    "--preload-file=${CMAKE_SOURCE_DIR}/data/tiles.jpg@/data/tiles.jpg"
    "--preload-file=${CMAKE_SOURCE_DIR}/data/rosendal_plains_2_1k.hdr@/data/rosendal_plains_2_1k.hdr"
    "--preload-file=${CMAKE_SOURCE_DIR}/data/sunny_rose_garden_1k.hdr@/data/sunny_rose_garden_1k.hdr"
  )
endif()
