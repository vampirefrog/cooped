cmake_minimum_required(VERSION 3.20)
project(cooped CXX)

set(CMAKE_CXX_STANDARD 17)
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
set(BGFX_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)  # flip ON when we add shaderc (milestone 2)
FetchContent_Declare(
  bgfx
  GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
  GIT_TAG        master             # pin to a commit for full reproducibility
)

FetchContent_MakeAvailable(SDL3 bgfx)

# ---------------------------------------------------------------------------
# The client executable
# ---------------------------------------------------------------------------
add_executable(cooped src/main.cpp)

# SDL3 exposes SDL3::SDL3-static when built static; SDL3::SDL3 otherwise.
if(TARGET SDL3::SDL3-static)
  target_link_libraries(cooped PRIVATE SDL3::SDL3-static)
else()
  target_link_libraries(cooped PRIVATE SDL3::SDL3)
endif()
target_link_libraries(cooped PRIVATE bgfx bx bimg)

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
  )
endif()
