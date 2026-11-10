#pragma once

#include <SDL.h>

#include <vector>

// Accumula forme colorate (rettangoli, rettangoli arrotondati, gradienti) in
// un unico buffer e le disegna con una sola chiamata a SDL_RenderGeometry.
struct GfxBatch {
    std::vector<SDL_Vertex> v;
    std::vector<int>        idx;

    void clear();
    void addRect(float x, float y, float w, float h, SDL_Color c);
    void addRectGrad(float x, float y, float w, float h, SDL_Color top, SDL_Color bot);
    void addRoundRect(float x, float y, float w, float h, float rad, SDL_Color c);
    void addRoundRectGrad(float x, float y, float w, float h, float rad, SDL_Color top,
                          SDL_Color bot);
    void flush(SDL_Renderer* r);
};

SDL_Color shade(SDL_Color c, float f);   // f<1 piu' scuro, f>1 piu' chiaro
SDL_Color withAlpha(SDL_Color c, Uint8 a);
