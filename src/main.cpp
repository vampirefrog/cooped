// cooped — milestone 1
// Proves the SDL3 <-> bgfx integration on both desktop and the browser:
//   * SDL3 owns the window/input/loop (SDL_AppIterate callback model)
//   * bgfx owns the GPU, bound to SDL3's native window handle
//   * single-threaded bgfx (renderFrame() before init) — required for the web
//   * clears the screen and draws debug text; no shaders yet (that's milestone 2)

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <cstdint>

namespace {

struct App {
    SDL_Window* window = nullptr;
    uint32_t    width  = 1280;
    uint32_t    height = 720;
    bool        bgfxInitialized = false;
};

// Pull the OS-native window/display handles out of SDL3 (properties API; SDL2's
// SDL_SysWMinfo is gone) and hand them to bgfx.
bool fillPlatformData(SDL_Window* window, bgfx::PlatformData& pd) {
#if defined(__EMSCRIPTEN__)
    (void)window;
    pd.nwh = (void*)"#canvas"; // bgfx targets the HTML canvas by CSS selector
    return true;
#else
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (props == 0) {
        SDL_Log("SDL_GetWindowProperties failed: %s", SDL_GetError());
        return false;
    }

#if defined(SDL_PLATFORM_WIN32)
    pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && SDL_strcmp(driver, "wayland") == 0) {
        pd.ndt  = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        pd.nwh  = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        pd.type = bgfx::NativeWindowHandleType::Wayland; // nwh is a wl_surface, not a window
    } else { // x11
        pd.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        pd.nwh = (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    }
#else
    #error "Unsupported platform for native window handle extraction"
#endif

    if (pd.nwh == nullptr) {
        SDL_Log("Could not obtain a native window handle from SDL3");
        return false;
    }
    return true;
#endif
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int /*argc*/, char** /*argv*/) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App* app = new App();
    *appstate = app;

    app->window = SDL_CreateWindow("cooped — milestone 1",
                                   (int)app->width, (int)app->height,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Track the real framebuffer size in pixels (may differ from logical size on HiDPI).
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(app->window, &pw, &ph);
    app->width  = (uint32_t)pw;
    app->height = (uint32_t)ph;

    // Single-threaded bgfx: call renderFrame() before init(). Mandatory on the web,
    // and simpler everywhere for now.
    bgfx::renderFrame();

    bgfx::Init init;
    init.type = bgfx::RendererType::Count; // auto-pick (Vulkan/Metal/D3D/GL/WebGL2)
    init.resolution.width  = app->width;
    init.resolution.height = app->height;
    init.resolution.reset  = BGFX_RESET_VSYNC;
    if (!fillPlatformData(app->window, init.platformData)) {
        return SDL_APP_FAILURE;
    }

    if (!bgfx::init(init)) {
        SDL_Log("bgfx::init failed");
        return SDL_APP_FAILURE;
    }
    app->bgfxInitialized = true;

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303040ff, 1.0f, 0);
    bgfx::setDebug(BGFX_DEBUG_TEXT);

    SDL_Log("bgfx renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    App* app = static_cast<App*>(appstate);

    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;
        case SDL_EVENT_KEY_DOWN:
            if (event->key.key == SDLK_ESCAPE) return SDL_APP_SUCCESS;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            app->width  = (uint32_t)event->window.data1;
            app->height = (uint32_t)event->window.data2;
            bgfx::reset(app->width, app->height, BGFX_RESET_VSYNC);
            break;
        }
        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    App* app = static_cast<App*>(appstate);

    bgfx::setViewRect(0, 0, 0, (uint16_t)app->width, (uint16_t)app->height);
    bgfx::touch(0); // make sure view 0 is submitted even with no draw calls

    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(1, 1, 0x0f, "cooped - milestone 1");
    bgfx::dbgTextPrintf(1, 2, 0x0a, "renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
    bgfx::dbgTextPrintf(1, 3, 0x0e, "backbuffer: %u x %u", app->width, app->height);
    bgfx::dbgTextPrintf(1, 5, 0x07, "esc / window close to quit");

    bgfx::frame(); // GPU work + buffer swap (NOT SDL — bgfx owns presentation)
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/) {
    App* app = static_cast<App*>(appstate);
    if (!app) return;
    if (app->bgfxInitialized) bgfx::shutdown();
    if (app->window) SDL_DestroyWindow(app->window);
    delete app;
    SDL_Quit();
}
