#pragma once

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

namespace ArgusRendererFreeOracle
{

struct RendererConstructionCounts
{
    int windowCalls = 0;
    int rendererCalls = 0;
    int openGlContextCalls = 0;
};

void resetRendererConstructionCounts();
RendererConstructionCounts rendererConstructionCounts();

}

extern "C" SDL_Window* SDLCALL ArgusOracleCreateWindow(
    const char* title,
    int x,
    int y,
    int width,
    int height,
    Uint32 flags);
extern "C" SDL_Renderer* SDLCALL ArgusOracleCreateRenderer(
    SDL_Window* window,
    int index,
    Uint32 flags);
extern "C" SDL_GLContext SDLCALL ArgusOracleCreateOpenGlContext(
    SDL_Window* window);

#define SDL_CreateWindow ArgusOracleCreateWindow
#define SDL_CreateRenderer ArgusOracleCreateRenderer
#define SDL_GL_CreateContext ArgusOracleCreateOpenGlContext
