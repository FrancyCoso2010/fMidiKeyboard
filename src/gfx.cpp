#include "gfx.h"

#include <algorithm>
#include <cmath>

static const float D2R = 3.14159265358979f / 180.0f;

SDL_Color shade(SDL_Color c, float f) {
    return SDL_Color{
        static_cast<Uint8>(std::min(255.0f, c.r * f)),
        static_cast<Uint8>(std::min(255.0f, c.g * f)),
        static_cast<Uint8>(std::min(255.0f, c.b * f)),
        c.a};
}

SDL_Color withAlpha(SDL_Color c, Uint8 a) {
    c.a = a;
    return c;
}

static SDL_Color lerpColor(SDL_Color a, SDL_Color b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return SDL_Color{
        static_cast<Uint8>(a.r + (b.r - a.r) * t),
        static_cast<Uint8>(a.g + (b.g - a.g) * t),
        static_cast<Uint8>(a.b + (b.b - a.b) * t),
        static_cast<Uint8>(a.a + (b.a - a.a) * t)};
}

void GfxBatch::clear() {
    v.clear();
    idx.clear();
}

void GfxBatch::addRectGrad(float x, float y, float w, float h, SDL_Color top,
                           SDL_Color bot) {
    if (w <= 0.0f || h <= 0.0f) return;
    const int base = static_cast<int>(v.size());
    const SDL_FPoint pos[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    const SDL_Color cols[4] = {top, top, bot, bot};
    for (int i = 0; i < 4; ++i) {
        SDL_Vertex vv;
        vv.position  = pos[i];
        vv.color     = cols[i];
        vv.tex_coord = {0.0f, 0.0f};
        v.push_back(vv);
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) idx.push_back(base + order[i]);
}

void GfxBatch::addRect(float x, float y, float w, float h, SDL_Color c) {
    addRectGrad(x, y, w, h, c, c);
}

void GfxBatch::addRoundRectGrad(float x, float y, float w, float h, float rad,
                                SDL_Color top, SDL_Color bot) {
    if (w <= 0.0f || h <= 0.0f) return;
    rad = std::min(rad, std::min(w, h) * 0.5f);
    if (rad < 0.5f) {
        addRectGrad(x, y, w, h, top, bot);
        return;
    }

    const int base = static_cast<int>(v.size());

    auto push = [&](float px, float py) {
        SDL_Vertex vv;
        vv.position  = {px, py};
        vv.color     = lerpColor(top, bot, (py - y) / h);
        vv.tex_coord = {0.0f, 0.0f};
        v.push_back(vv);
    };

    // centro (per il triangle-fan)
    push(x + w * 0.5f, y + h * 0.5f);

    const int SEG = 4;
    push(x + rad, y);
    push(x + w - rad, y);
    for (int i = 1; i <= SEG; ++i) { // arco alto-destra -90..0
        const float a = (-90.0f + 90.0f * i / SEG) * D2R;
        push(x + w - rad + rad * std::cos(a), y + rad + rad * std::sin(a));
    }
    push(x + w, y + h - rad);
    for (int i = 1; i <= SEG; ++i) { // arco basso-destra 0..90
        const float a = (90.0f * i / SEG) * D2R;
        push(x + w - rad + rad * std::cos(a), y + h - rad + rad * std::sin(a));
    }
    push(x + rad, y + h);
    for (int i = 1; i <= SEG; ++i) { // arco basso-sinistra 90..180
        const float a = (90.0f + 90.0f * i / SEG) * D2R;
        push(x + rad + rad * std::cos(a), y + h - rad + rad * std::sin(a));
    }
    push(x, y + rad);
    for (int i = 1; i <= SEG; ++i) { // arco alto-sinistra 180..270
        const float a = (180.0f + 90.0f * i / SEG) * D2R;
        push(x + rad + rad * std::cos(a), y + rad + rad * std::sin(a));
    }

    const int n = static_cast<int>(v.size()) - base - 1; // punti sul perimetro
    for (int i = 0; i < n; ++i) {
        idx.push_back(base);
        idx.push_back(base + 1 + i);
        idx.push_back(base + 1 + ((i + 1) % n));
    }
}

void GfxBatch::addRoundRect(float x, float y, float w, float h, float rad,
                            SDL_Color c) {
    addRoundRectGrad(x, y, w, h, rad, c, c);
}

void GfxBatch::flush(SDL_Renderer* r) {
    if (idx.empty()) return;
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), idx.data(),
                       static_cast<int>(idx.size()));
    clear();
}
