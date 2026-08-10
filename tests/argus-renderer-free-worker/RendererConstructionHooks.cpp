#include "RendererConstructionHooks.h"

#undef SDL_CreateWindow
#undef SDL_CreateRenderer
#undef SDL_GL_CreateContext

#include <atomic>

namespace
{

std::atomic<int> windowCalls { 0 };
std::atomic<int> rendererCalls { 0 };
std::atomic<int> openGlContextCalls { 0 };

}

namespace ArgusRendererFreeOracle
{

void resetRendererConstructionCounts()
{
    windowCalls.store(0, std::memory_order_release);
    rendererCalls.store(0, std::memory_order_release);
    openGlContextCalls.store(0, std::memory_order_release);
}

RendererConstructionCounts rendererConstructionCounts()
{
    return {
        windowCalls.load(std::memory_order_acquire),
        rendererCalls.load(std::memory_order_acquire),
        openGlContextCalls.load(std::memory_order_acquire),
    };
}

}

extern "C" SDL_Window* SDLCALL ArgusOracleCreateWindow(
    const char* title,
    int x,
    int y,
    int width,
    int height,
    Uint32 flags)
{
    windowCalls.fetch_add(1, std::memory_order_acq_rel);
    return SDL_CreateWindow(title, x, y, width, height, flags);
}

extern "C" SDL_Renderer* SDLCALL ArgusOracleCreateRenderer(
    SDL_Window* window,
    int index,
    Uint32 flags)
{
    rendererCalls.fetch_add(1, std::memory_order_acq_rel);
    return SDL_CreateRenderer(window, index, flags);
}

extern "C" SDL_GLContext SDLCALL ArgusOracleCreateOpenGlContext(
    SDL_Window* window)
{
    openGlContextCalls.fetch_add(1, std::memory_order_acq_rel);
    return SDL_GL_CreateContext(window);
}
