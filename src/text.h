#pragma once

#include <SDL.h>

// Testo 2D con font vettoriale (Liberation Sans) rasterizzato in un atlas.
// Se il font non e' disponibile usa ancora stb_easy_font come fallback.
//
// textInit() va chiamata dopo la creazione del renderer e prima del primo
// disegno; textShutdown() prima di distruggere il renderer.
bool  textInit(SDL_Renderer* r);
void  textShutdown();

void  drawText(SDL_Renderer* r, float x, float y, const char* text, SDL_Color color,
               float scale = 1.0f);
float textWidth(const char* text, float scale = 1.0f);
float textHeight(const char* text, float scale = 1.0f);
