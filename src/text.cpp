#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "stb_easy_font.h"

namespace {

constexpr int   kFirstChar = 32;
constexpr int   kCharCount = 224;   // ASCII + Latin-1 (accenti italiani inclusi)
constexpr int   kAtlasW    = 1024;
constexpr int   kAtlasH    = 1024;
constexpr float kBakePx    = 64.0f;

SDL_Renderer*       gRenderer = nullptr;
SDL_Texture*        gAtlasTex = nullptr;
stbtt_packedchar    gGlyphs[kCharCount] = {};
float               gAscent = 0.0f;
float               gDescent = 0.0f;
float               gLineGap = 0.0f;
bool                gReady = false;

bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > 32 * 1024 * 1024) return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f);
}

std::string fontPath() {
    if (const char* env = std::getenv("FMK_FONT")) {
        if (*env) return env;
    }

    std::vector<std::string> candidates;
    if (char* base = SDL_GetBasePath()) {
        candidates.push_back(std::string(base) + "assets/LiberationSans-Regular.ttf");
        SDL_free(base);
    }
    candidates.push_back("assets/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/TTF/LiberationSans-Regular.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");

    std::ifstream test;
    for (const std::string& p : candidates) {
        test.open(p, std::ios::binary);
        if (test.good()) {
            test.close();
            return p;
        }
        test.close();
    }
    return {};
}

uint32_t nextCodepoint(const char*& p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x80) { ++p; return c; }
    if ((c & 0xE0) == 0xC0 && (static_cast<unsigned char>(p[1]) & 0xC0) == 0x80) {
        const uint32_t v = ((c & 0x1Fu) << 6) |
                           (static_cast<unsigned char>(p[1]) & 0x3Fu);
        p += 2;
        return v;
    }
    if ((c & 0xF0) == 0xE0 && (static_cast<unsigned char>(p[1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(p[2]) & 0xC0) == 0x80) {
        const uint32_t v = ((c & 0x0Fu) << 12) |
                           ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 6) |
                           (static_cast<unsigned char>(p[2]) & 0x3Fu);
        p += 3;
        return v;
    }
    if ((c & 0xF8) == 0xF0 && (static_cast<unsigned char>(p[1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(p[2]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(p[3]) & 0xC0) == 0x80) {
        const uint32_t v = ((c & 0x07u) << 18) |
                           ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 12) |
                           ((static_cast<unsigned char>(p[2]) & 0x3Fu) << 6) |
                           (static_cast<unsigned char>(p[3]) & 0x3Fu);
        p += 4;
        return v;
    }
    ++p;
    return '?';
}

const stbtt_packedchar* glyphFor(uint32_t cp) {
    if (cp < static_cast<uint32_t>(kFirstChar) ||
        cp >= static_cast<uint32_t>(kFirstChar + kCharCount))
        cp = '?';
    return &gGlyphs[cp - kFirstChar];
}

float pixelSize(float scale) {
    return std::max(7.0f, 12.0f * scale);
}

// ------------------------------------------------------------------ fallback
void drawTextLegacy(SDL_Renderer* r, float x, float y, const char* text,
                    SDL_Color color, float scale) {
    unsigned char col[4] = {color.r, color.g, color.b, color.a};
    const float s = std::max(0.5f, scale * 1.55f);

    const size_t need = std::strlen(text) * 300 + 512;
    static std::vector<char> buf;
    if (buf.size() < need) buf.resize(need);

    const int quads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(text), col,
                                          buf.data(), static_cast<int>(buf.size()));
    if (quads <= 0) return;

    static std::vector<SDL_Vertex> verts;
    static std::vector<int> idx;
    verts.clear();
    idx.clear();
    verts.reserve(static_cast<size_t>(quads) * 4);
    idx.reserve(static_cast<size_t>(quads) * 6);

    for (int i = 0; i < quads * 4; ++i) {
        const float* p = reinterpret_cast<const float*>(
            buf.data() + static_cast<size_t>(i) * 16);
        SDL_Vertex v;
        v.position = {x + p[0] * s, y + p[1] * s};
        v.color = color;
        v.tex_coord = {0.0f, 0.0f};
        verts.push_back(v);
    }
    for (int i = 0; i < quads; ++i) {
        const int b = i * 4;
        idx.push_back(b + 0);
        idx.push_back(b + 1);
        idx.push_back(b + 2);
        idx.push_back(b + 0);
        idx.push_back(b + 2);
        idx.push_back(b + 3);
    }
    SDL_RenderGeometry(r, nullptr, verts.data(), static_cast<int>(verts.size()),
                       idx.data(), static_cast<int>(idx.size()));
}

} // namespace

bool textInit(SDL_Renderer* r) {
    textShutdown();
    if (!r) return false;
    gRenderer = r;

    const std::string path = fontPath();
    if (path.empty()) {
        std::fprintf(stderr, "Testo: font non trovato, uso il fallback.\n");
        return false;
    }

    std::vector<unsigned char> fontData;
    if (!readFile(path, fontData)) {
        std::fprintf(stderr, "Testo: impossibile leggere il font %s\n", path.c_str());
        return false;
    }

    stbtt_fontinfo font{};
    const int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font, fontData.data(), offset)) {
        std::fprintf(stderr, "Testo: font non valido %s\n", path.c_str());
        return false;
    }

    std::vector<unsigned char> atlas(static_cast<size_t>(kAtlasW) * kAtlasH, 0);
    stbtt_pack_context pack{};
    if (!stbtt_PackBegin(&pack, atlas.data(), kAtlasW, kAtlasH, 0, 2, nullptr)) {
        std::fprintf(stderr, "Testo: atlas non inizializzato\n");
        return false;
    }
    const int packed = stbtt_PackFontRange(&pack, fontData.data(), 0, kBakePx,
                                           kFirstChar, kCharCount, gGlyphs);
    stbtt_PackEnd(&pack);
    if (!packed) {
        std::fprintf(stderr, "Testo: impossibile rasterizzare il font\n");
        return false;
    }

    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(&font, &a, &d, &lg);
    const float fscale = stbtt_ScaleForPixelHeight(&font, kBakePx);
    gAscent  = static_cast<float>(a) * fscale;
    gDescent = static_cast<float>(d) * fscale;
    gLineGap = static_cast<float>(lg) * fscale;

    std::vector<unsigned char> rgba(static_cast<size_t>(kAtlasW) * kAtlasH * 4, 255);
    for (size_t i = 0; i < atlas.size(); ++i)
        rgba[i * 4 + 3] = atlas[i];

    gAtlasTex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                  kAtlasW, kAtlasH);
    if (!gAtlasTex) {
        std::fprintf(stderr, "Testo: texture non creata: %s\n", SDL_GetError());
        return false;
    }
    SDL_UpdateTexture(gAtlasTex, nullptr, rgba.data(), kAtlasW * 4);
    SDL_SetTextureBlendMode(gAtlasTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(gAtlasTex, SDL_ScaleModeLinear);

    gReady = true;
    std::printf("Testo: font %s (atlas %dx%d)\n", path.c_str(), kAtlasW, kAtlasH);
    return true;
}

void textShutdown() {
    if (gAtlasTex) {
        SDL_DestroyTexture(gAtlasTex);
        gAtlasTex = nullptr;
    }
    gReady = false;
    gRenderer = nullptr;
}

void drawText(SDL_Renderer* r, float x, float y, const char* text, SDL_Color color,
              float scale) {
    if (!text || !*text) return;
    if (!gReady || !gAtlasTex) {
        drawTextLegacy(r, x, y, text, color, scale);
        return;
    }

    const float px = pixelSize(scale);
    const float s  = px / kBakePx;
    const float baseline = y + gAscent * s;

    static std::vector<SDL_Vertex> verts;
    static std::vector<int> idx;
    verts.clear();
    idx.clear();
    verts.reserve(std::strlen(text) * 4);
    idx.reserve(std::strlen(text) * 6);

    float pen = x;
    const char* p = text;
    while (*p) {
        const uint32_t cp = nextCodepoint(p);
        if (cp == '\n') {
            pen = x;
            y += textHeight("Ag", scale) * 1.15f;
            continue;
        }
        if (cp == '\t') {
            pen += 4.0f * px * 0.5f;
            continue;
        }
        const stbtt_packedchar* g = glyphFor(cp);
        if (g->x1 > g->x0 && g->y1 > g->y0) {
            const float x0 = pen + g->xoff * s;
            const float y0 = baseline + g->yoff * s;
            const float x1 = pen + g->xoff2 * s;
            const float y1 = baseline + g->yoff2 * s;
            const float u0 = static_cast<float>(g->x0) / kAtlasW;
            const float v0 = static_cast<float>(g->y0) / kAtlasH;
            const float u1 = static_cast<float>(g->x1) / kAtlasW;
            const float v1 = static_cast<float>(g->y1) / kAtlasH;
            const int base = static_cast<int>(verts.size());
            const SDL_FPoint pos[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            const SDL_FPoint uv[4]  = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            for (int i = 0; i < 4; ++i) {
                SDL_Vertex v;
                v.position = pos[i];
                v.color = color;
                v.tex_coord = uv[i];
                verts.push_back(v);
            }
            idx.push_back(base + 0);
            idx.push_back(base + 1);
            idx.push_back(base + 2);
            idx.push_back(base + 0);
            idx.push_back(base + 2);
            idx.push_back(base + 3);
        }
        pen += g->xadvance * s;
    }

    if (!idx.empty()) {
        SDL_RenderGeometry(r ? r : gRenderer, gAtlasTex, verts.data(),
                           static_cast<int>(verts.size()), idx.data(),
                           static_cast<int>(idx.size()));
    }
}

float textWidth(const char* text, float scale) {
    if (!text || !*text) return 0.0f;
    if (!gReady) {
        return static_cast<float>(stb_easy_font_width(const_cast<char*>(text))) *
               std::max(0.5f, scale * 1.55f);
    }

    const float s = pixelSize(scale) / kBakePx;
    float w = 0.0f;
    const char* p = text;
    while (*p) {
        const uint32_t cp = nextCodepoint(p);
        if (cp == '\n' || cp == '\t') continue;
        w += glyphFor(cp)->xadvance * s;
    }
    return w;
}

float textHeight(const char* text, float scale) {
    (void)text;
    if (!gReady) {
        return 10.0f * std::max(0.5f, scale * 1.55f);
    }
    return (gAscent - gDescent + gLineGap) *
           (pixelSize(scale) / kBakePx);
}
