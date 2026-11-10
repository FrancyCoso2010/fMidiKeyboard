#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"
#include "chord.h"
#include "gfx.h"
#include "midi.h"
#include "notes.h"
#include "sampler.h"
#include "synth.h"
#include "text.h"
#include "vsthost.h"

namespace fs = std::filesystem;

static const int   PIANO_MIN  = 21;   // A0
static const int   PIANO_MAX  = 108;  // C8
static const float KEYBOARD_H = 128.0f;

// Due colori (ambra nei gravi, azzurro negli acuti) fusi lungo la tastiera:
// il registro si legge a colpo d'occhio, ma la transizione resta morbida.
static const SDL_Color NOTE_LOW  = {255, 148, 74, 255};  // ambra
static const SDL_Color NOTE_HIGH = {86, 198, 255, 255};  // azzurro

static SDL_Color mixColor(SDL_Color a, SDL_Color b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return SDL_Color{
        static_cast<Uint8>(a.r + (b.r - a.r) * t),
        static_cast<Uint8>(a.g + (b.g - a.g) * t),
        static_cast<Uint8>(a.b + (b.b - a.b) * t),
        static_cast<Uint8>(a.a + (b.a - a.a) * t)};
}

static SDL_Color noteColor(int pitch) {
    const float t = static_cast<float>(pitch - PIANO_MIN) /
                    static_cast<float>(PIANO_MAX - PIANO_MIN);
    return mixColor(NOTE_LOW, NOTE_HIGH, t);
}

// ------------------------------------------------------------------ Layout ----

static bool isBlackKey(int p) {
    switch (p % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

struct Layout {
    float x[128] = {};
    float w[128] = {};
    bool  black[128] = {};
    float whiteW = 1.0f;
};

static void computeLayout(int winW, Layout& L) {
    int whiteCount = 0;
    for (int p = PIANO_MIN; p <= PIANO_MAX; ++p)
        if (!isBlackKey(p)) ++whiteCount;
    if (whiteCount <= 0) whiteCount = 1;

    L.whiteW = static_cast<float>(winW) / static_cast<float>(whiteCount);

    int wi = 0;
    for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
        if (!isBlackKey(p)) {
            L.x[p]     = static_cast<float>(wi) * L.whiteW;
            L.w[p]     = L.whiteW;
            L.black[p] = false;
            ++wi;
        } else {
            L.w[p]     = L.whiteW * 0.62f;
            L.x[p]     = static_cast<float>(wi) * L.whiteW - L.w[p] * 0.5f;
            L.black[p] = true;
        }
    }
}

static int pitchAtScreen(const Layout& L, float mx, float my, float keyboardY) {
    if (my < keyboardY || my > keyboardY + KEYBOARD_H) return -1;
    if (my < keyboardY + KEYBOARD_H * 0.62f) {
        for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
            if (!L.black[p]) continue;
            if (mx >= L.x[p] && mx <= L.x[p] + L.w[p]) return p;
        }
    }
    for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
        if (L.black[p]) continue;
        if (mx >= L.x[p] && mx <= L.x[p] + L.w[p]) return p;
    }
    return -1;
}

// --------------------------------------------------------------- Impostazioni -

// Fonte audio come maschera di bit: 1=VST3, 2=interno, 4=sintetizzatore.
// I valori storici 0..3 restano validi; 7 = tutto insieme.
enum { SRC_OFF = 0, SRC_VST = 1, SRC_INT = 2, SRC_BOTH = 3, SRC_SYNTH = 4 };
static bool srcHasVst(int s)     { return (s & 1) != 0; }
static bool srcHasSampler(int s) { return (s & 2) != 0; }
static bool srcHasSynth(int s)   { return (s & 4) != 0; }

struct Settings {
    bool        rising        = true;
    double      speed         = 420.0;
    bool        showNoteNames = true;
    bool        showChord     = true;
    int         notation      = NOTATION_IT;
    int         audioSource   = SRC_INT;
    float       volume        = 0.8f;
    SynthSettings synth;
    std::string pluginPath;
    std::string instrumentPath;
    std::string midiPort;

    // Geometria finestra (persistita tra le sessioni).
    int  winW = 1280, winH = 720;
    int  winX = SDL_WINDOWPOS_CENTERED, winY = SDL_WINDOWPOS_CENTERED;
    bool maximized  = false;
    bool fullscreen = false;
};

enum PanelItem { P_DIR = 0, P_SPEED, P_NAMES, P_CHORD, P_NOTATION,
                 P_SOURCE, P_VOLUME, P_SYNTH,
                 P_INSTRUMENT, P_LOAD_INSTR,
                 P_PLUGIN, P_EDITOR, P_BROWSE, P_COUNT };

// Modalità del file browser.
enum { BROWSE_PLUGIN = 0, BROWSE_INSTRUMENT = 1 };

// ------------------------------------------------------------- file browser ----

struct BrowseEntry {
    std::string name;
    std::string path;
    bool        isDir = false;
};

struct FileBrowser {
    bool                     open = false;
    int                      mode = BROWSE_PLUGIN;
    std::string              dir;
    std::vector<BrowseEntry> entries;
    int                      sel = 0;

    static bool accept(int mode, const std::string& ext) {
        if (mode == BROWSE_INSTRUMENT)
            return ext == ".sfz" || ext == ".dspreset" || ext == ".dslibrary";
        return ext == ".vst3" || ext == ".so" || ext == ".dll";
    }

    void refresh() {
        namespace fs = std::filesystem;
        entries.clear();
        std::error_code ec;
        const fs::path d(dir);
        if (!fs::is_directory(d, ec)) return;

        if (d.has_parent_path() && d.parent_path() != d)
            entries.push_back({"..", d.parent_path().string(), true});

        // Strumenti integrati sempre disponibili in cima all'elenco.
        if (mode == BROWSE_INSTRUMENT) {
            entries.push_back({"* Rhodes (piano elettrico)", ":rhodes", false});
            entries.push_back({"* Pianoforte (sintetico)", ":piano", false});
        }

        std::vector<BrowseEntry> dirs, files;
        for (auto& e : fs::directory_iterator(d, ec)) {
            const std::string nm = e.path().filename().string();
            if (!nm.empty() && nm[0] == '.') continue;
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(c));
            if (accept(mode, ext)) {
                files.push_back({nm, e.path().string(), false});
            } else {
                std::error_code e2;
                if (e.is_directory(e2)) dirs.push_back({nm, e.path().string(), true});
            }
        }
        auto cmp = [](const BrowseEntry& a, const BrowseEntry& b) { return a.name < b.name; };
        std::sort(dirs.begin(), dirs.end(), cmp);
        std::sort(files.begin(), files.end(), cmp);
        for (auto& e : dirs) entries.push_back(e);
        for (auto& e : files) entries.push_back(e);

        if (sel >= static_cast<int>(entries.size())) sel = static_cast<int>(entries.size()) - 1;
        if (sel < 0) sel = 0;
    }
};

static void loadSettings(Settings& s, const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        int    iv = 0;
        double dv = 0.0;
        if (std::sscanf(line.c_str(), "rising=%d", &iv) == 1)         s.rising = iv != 0;
        else if (std::sscanf(line.c_str(), "speed=%lf", &dv) == 1)    s.speed = std::min(2000.0, std::max(80.0, dv));
        else if (std::sscanf(line.c_str(), "noteNames=%d", &iv) == 1) s.showNoteNames = iv != 0;
        else if (std::sscanf(line.c_str(), "chord=%d", &iv) == 1)     s.showChord = iv != 0;
        else if (std::sscanf(line.c_str(), "notation=%d", &iv) == 1)  s.notation = iv ? 1 : 0;
        else if (std::sscanf(line.c_str(), "audio=%d", &iv) == 1)     s.audioSource = iv ? SRC_VST : SRC_OFF;
        else if (std::sscanf(line.c_str(), "source=%d", &iv) == 1)    s.audioSource = std::min<int>(7, std::max<int>(0, iv));
        else if (std::sscanf(line.c_str(), "volume=%d", &iv) == 1)
            s.volume = std::min(125, std::max(0, iv)) / 100.0f;
        else if (std::sscanf(line.c_str(), "winW=%d", &iv) == 1)     s.winW = std::min(16384, std::max(320, iv));
        else if (std::sscanf(line.c_str(), "winH=%d", &iv) == 1)     s.winH = std::min(16384, std::max(240, iv));
        else if (std::sscanf(line.c_str(), "winX=%d", &iv) == 1)     s.winX = iv;
        else if (std::sscanf(line.c_str(), "winY=%d", &iv) == 1)     s.winY = iv;
        else if (std::sscanf(line.c_str(), "maximized=%d", &iv) == 1) s.maximized = iv != 0;
        else if (std::sscanf(line.c_str(), "fullscreen=%d", &iv) == 1) s.fullscreen = iv != 0;
        else if (std::sscanf(line.c_str(), "synthPreset=%d", &iv) == 1) s.synth.preset = iv;
        else if (std::sscanf(line.c_str(), "synthWave=%d", &iv) == 1)
            s.synth.wave = std::min(SYNTH_WAVE_COUNT - 1, std::max(0, iv));
        else if (std::sscanf(line.c_str(), "synthOctave=%d", &iv) == 1)
            s.synth.octave = std::min(2, std::max(-2, iv));
        else if (std::sscanf(line.c_str(), "synthUnison=%d", &iv) == 1)
            s.synth.unison = std::min(4, std::max(1, iv));
        else if (std::sscanf(line.c_str(), "synthDetune=%lf", &dv) == 1)
            s.synth.detune = std::max(0.0f, std::min(50.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthSub=%lf", &dv) == 1)
            s.synth.sub = std::max(0.0f, std::min(1.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthCutoff=%lf", &dv) == 1)
            s.synth.cutoff = std::max(30.0f, std::min(18000.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthResonance=%lf", &dv) == 1)
            s.synth.resonance = std::max(0.0f, std::min(0.95f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthAttack=%lf", &dv) == 1)
            s.synth.attack = std::max(0.001f, std::min(4.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthDecay=%lf", &dv) == 1)
            s.synth.decay = std::max(0.005f, std::min(4.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthSustain=%lf", &dv) == 1)
            s.synth.sustain = std::max(0.0f, std::min(1.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthRelease=%lf", &dv) == 1)
            s.synth.release = std::max(0.01f, std::min(8.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthLfoRate=%lf", &dv) == 1)
            s.synth.lfoRate = std::max(0.02f, std::min(20.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthLfoDepth=%lf", &dv) == 1)
            s.synth.lfoDepth = std::max(0.0f, std::min(100.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthDrive=%lf", &dv) == 1)
            s.synth.drive = std::max(1.0f, std::min(4.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthVolume=%lf", &dv) == 1)
            s.synth.volume = std::max(0.0f, std::min(1.5f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthFilterEnv=%lf", &dv) == 1)
            s.synth.filterEnv = std::max(0.0f, std::min(1.0f, static_cast<float>(dv)));
        else if (std::sscanf(line.c_str(), "synthReverb=%lf", &dv) == 1)
            s.synth.reverb = std::max(0.0f, std::min(1.0f, static_cast<float>(dv)));
        else if (line.rfind("plugin=", 0) == 0)     s.pluginPath = line.substr(7);
        else if (line.rfind("instrument=", 0) == 0) s.instrumentPath = line.substr(11);
        else if (line.rfind("midiPort=", 0) == 0)   s.midiPort = line.substr(9);
    }
}

static void saveSettings(const Settings& s, const std::string& path) {
    std::ofstream f(path);
    if (!f) return;
    f << "rising=" << (s.rising ? 1 : 0) << "\n";
    f << "speed=" << static_cast<int>(s.speed) << "\n";
    f << "noteNames=" << (s.showNoteNames ? 1 : 0) << "\n";
    f << "chord=" << (s.showChord ? 1 : 0) << "\n";
    f << "notation=" << s.notation << "\n";
    f << "source=" << s.audioSource << "\n";
    f << "volume=" << static_cast<int>(s.volume * 100.0f + 0.5f) << "\n";
    f << "winW=" << s.winW << "\n";
    f << "winH=" << s.winH << "\n";
    f << "winX=" << s.winX << "\n";
    f << "winY=" << s.winY << "\n";
    f << "maximized=" << (s.maximized ? 1 : 0) << "\n";
    f << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    f << "synthPreset=" << s.synth.preset << "\n";
    f << "synthWave=" << s.synth.wave << "\n";
    f << "synthOctave=" << s.synth.octave << "\n";
    f << "synthUnison=" << s.synth.unison << "\n";
    f << "synthDetune=" << s.synth.detune << "\n";
    f << "synthSub=" << s.synth.sub << "\n";
    f << "synthCutoff=" << s.synth.cutoff << "\n";
    f << "synthResonance=" << s.synth.resonance << "\n";
    f << "synthAttack=" << s.synth.attack << "\n";
    f << "synthDecay=" << s.synth.decay << "\n";
    f << "synthSustain=" << s.synth.sustain << "\n";
    f << "synthRelease=" << s.synth.release << "\n";
    f << "synthLfoRate=" << s.synth.lfoRate << "\n";
    f << "synthLfoDepth=" << s.synth.lfoDepth << "\n";
    f << "synthDrive=" << s.synth.drive << "\n";
    f << "synthVolume=" << s.synth.volume << "\n";
    f << "synthFilterEnv=" << s.synth.filterEnv << "\n";
    f << "synthReverb=" << s.synth.reverb << "\n";
    f << "plugin=" << s.pluginPath << "\n";
    f << "instrument=" << s.instrumentPath << "\n";
    f << "midiPort=" << s.midiPort << "\n";
}

// ---------------------------------------------------------------- UI helpers --

static const char* onOff(bool v) { return v ? "Si" : "No"; }

static std::string ellipsize(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 2) + "..";
}

// --------------------------------------------------------------- tema --------
namespace th {
const SDL_Color bgTop    = {30, 35, 62, 255};
const SDL_Color bgBot    = {5, 6, 13, 255};
const SDL_Color card     = {17, 20, 33, 250};
const SDL_Color cardEdge = {74, 116, 198, 255};
const SDL_Color header1  = {52, 76, 138, 255};
const SDL_Color header2  = {20, 24, 42, 255};
const SDL_Color title    = {240, 244, 255, 255};
const SDL_Color subtle   = {154, 196, 248, 255};
const SDL_Color section  = {120, 190, 255, 255};
const SDL_Color label    = {202, 210, 230, 255};
const SDL_Color value    = {140, 214, 255, 255};
const SDL_Color dim      = {124, 132, 158, 255};
const SDL_Color hint     = {146, 156, 182, 255};
const SDL_Color sel      = {48, 92, 168, 235};
const SDL_Color selEdge  = {116, 180, 255, 255};
const SDL_Color rowBg    = {28, 33, 50, 255};
const SDL_Color good     = {122, 226, 165, 255};
const SDL_Color warn     = {250, 186, 96, 255};
} // namespace th

// Alone morbido dietro una forma: strati arrotondati con alpha decrescente.
static void addGlow(GfxBatch& b, float x, float y, float w, float h, float rad,
                    SDL_Color c, Uint8 peak, int layers = 4) {
    for (int i = layers; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(layers);
        const float e = 2.0f + 10.0f * t;
        const float a = static_cast<float>(peak) * (1.0f - t);
        b.addRoundRect(x - e, y - e, w + 2.0f * e, h + 2.0f * e, rad + e,
                       withAlpha(c, static_cast<Uint8>(a + 1.0f)));
    }
}

static bool panelSectionStart(int i) {
    return i == P_DIR || i == P_SOURCE || i == P_SYNTH ||
           i == P_INSTRUMENT || i == P_PLUGIN;
}
static const char* panelSectionName(int i) {
    switch (i) {
        case P_DIR:        return "RIPRODUZIONE";
        case P_SOURCE:     return "AUDIO";
        case P_SYNTH:      return "SINTETIZZATORE";
        case P_INSTRUMENT: return "STRUMENTO INTERNO  (SFZ / Decent Sampler)";
        case P_PLUGIN:     return "PLUGIN VST3";
        default:           return "";
    }
}

static void drawPanel(SDL_Renderer* r, GfxBatch& batch, int ww, int wh,
                      const Settings& s, int selected, const std::string& device,
                      const std::string& pluginLabel, bool vstAvailable,
                      bool pluginLoaded, bool editorIsOpen,
                      const std::string& instrumentLabel, bool instrumentLoaded,
                      float audioPeak) {
    const int secH    = 26;
    const int headerH = 60;
    const int pad     = 22;
    const int boxW    = std::min(600, ww - 40);

    const int nSections = 5;
    const int devH = 36;
    // Altezza righe adattiva: il pannello resta interamente visibile anche
    // su finestre basse (prima poteva uscire dallo schermo).
    const int rowH = std::min(32, std::max(22,
                        (wh - (headerH + nSections * secH + devH + 52)) / P_COUNT));
    const int boxH = headerH + P_COUNT * rowH + nSections * secH + devH + 40;
    const int boxX = (ww - boxW) / 2;
    const int boxY = (wh - boxH) / 2;

    // ombra + bordo + corpo
    batch.addRoundRect(boxX - 6, boxY - 4, boxW + 12, boxH + 12, 22,
                       SDL_Color{0, 0, 0, 110});
    batch.addRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 18, th::cardEdge);
    batch.addRoundRect(boxX, boxY, boxW, boxH, 16, th::card);
    // intestazione con gradiente
    batch.addRoundRectGrad(boxX + 3, boxY + 3, boxW - 6, headerH - 3, 14,
                           th::header1, th::header2);
    batch.addRect(boxX + 3, boxY + headerH - 2, boxW - 6, 1.5f,
                  withAlpha(th::subtle, 90));

    // posizioni righe/section
    int rowY[P_COUNT];
    int secY[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) { rowY[i] = -1; secY[i] = -1; }
    int y = boxY + headerH + 8;
    for (int i = 0; i < P_COUNT; ++i) {
        if (panelSectionStart(i)) { secY[i] = y + 4; y += secH; }
        rowY[i] = y;
        y += rowH;
    }

    // evidenzia riga selezionata
    if (selected >= 0 && selected < P_COUNT && rowY[selected] >= 0) {
        const int ry = rowY[selected];
        batch.addRoundRect(boxX + 12, ry - 4, boxW - 24, rowH - 6, 9, th::sel);
        batch.addRoundRect(boxX + 12, ry - 4, 4, rowH - 6, 2, th::selEdge);
    }

    // riga dispositivo MIDI
    const int devY = y + 6;
    batch.addRoundRect(boxX + 12, devY, boxW - 24, devH - 8, 9, th::rowBg);
    // VU meter nella intestazione
    {
        const int mx = boxX + boxW - 24 - 150;
        const int my = boxY + 22;
        batch.addRoundRect(mx, my, 150, 14, 7, SDL_Color{12, 14, 22, 220});
        float lvl = audioPeak;
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        SDL_Color lc = lvl > 0.9f ? SDL_Color{240, 110, 90, 255}
                     : lvl > 0.6f ? th::warn
                                  : th::good;
        if (lvl > 0.005f)
            batch.addRoundRectGrad(mx + 2, my + 2, (150 - 4) * lvl, 10, 5, lc, shade(lc, 0.7f));
    }
    batch.flush(r);

    // testo intestazione
    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 14),
             "Impostazioni", th::title, 2.1f);
    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 38),
             "fMidiKeyboard", withAlpha(th::subtle, 170), 1.3f);
    drawText(r, static_cast<float>(boxX + boxW - 24 - 150), static_cast<float>(boxY + 38),
             "USCITA", withAlpha(th::subtle, 170), 1.1f);

    const char* labels[P_COUNT] = {"Direzione", "Velocità", "Nomi note",
                                   "Vista accordo", "Notazione",
                                   "Fonte audio", "Volume",
                                   "Sintetizzatore",
                                   "Strumento", "Apri strumento...",
                                   "Plugin VST3", "Editor plugin",
                                   "Apri plugin .vst3..."};
    char speedBuf[32];
    std::snprintf(speedBuf, sizeof(speedBuf), "%d px/s", static_cast<int>(s.speed));
    char volBuf[16];
    std::snprintf(volBuf, sizeof(volBuf), "%d%%", static_cast<int>(s.volume * 100.0f + 0.5f));
    const char* srcNames[8] = {"Spento", "VST3", "Interno", "VST3 + interno",
                               "Synth", "Synth + VST3", "Synth + interno", "Tutto"};
    const char* srcVal = srcNames[(s.audioSource < 0 || s.audioSource > 7)
                                      ? 0 : s.audioSource];
    const std::string pluginVal   = ellipsize(pluginLabel, 24);
    const std::string instrVal    = ellipsize(instrumentLabel, 24);
    const char* editorVal = !vstAvailable ? "n/d"
                            : (!pluginLoaded ? "-" : (editorIsOpen ? "aperto" : "chiuso"));
    const char* values[P_COUNT] = {
        s.rising ? "Su (cascata)" : "Giù (caduta)",
        speedBuf,
        onOff(s.showNoteNames),
        onOff(s.showChord),
        s.notation == NOTATION_IT ? "Italiano" : "Inglese",
        srcVal,
        volBuf,
        "Apri (Y)",
        instrVal.c_str(),
        "Invio",
        pluginVal.c_str(),
        editorVal,
        "Invio",
    };
    const bool badValues[P_COUNT] = {
        false, false, false, false, false,
        s.audioSource == SRC_OFF, false, false,
        !instrumentLoaded, false,
        !pluginLoaded, !pluginLoaded, false,
    };

    for (int i = 0; i < P_COUNT; ++i) {
        if (panelSectionStart(i) && secY[i] >= 0) {
            drawText(r, static_cast<float>(boxX + pad), static_cast<float>(secY[i]),
                     panelSectionName(i), th::section, 1.25f);
            const float sw = textWidth(panelSectionName(i), 1.25f);
            batch.addRect(boxX + pad + sw + 10, secY[i] + 8,
                          boxW - pad * 2 - sw - 10, 1.0f,
                          withAlpha(th::subtle, 60));
        }
        const bool sel = (i == selected);
        const float ty = static_cast<float>(rowY[i]);
        drawText(r, static_cast<float>(boxX + pad + 6), ty, labels[i],
                 sel ? th::title : th::label, 1.6f);

        SDL_Color vc = badValues[i] ? th::dim : th::value;
        const float vw = textWidth(values[i], 1.6f);
        float vx = static_cast<float>(boxX + boxW - pad) - vw;
        if (sel) {
            drawText(r, static_cast<float>(boxX + pad + boxW - 2 * pad) - vw - 26, ty,
                     "<", th::selEdge, 1.6f);
            drawText(r, static_cast<float>(boxX + boxW - pad) + 12, ty, ">",
                     th::selEdge, 1.6f);
            batch.addRoundRect(vx - 8, ty - 4, vw + 16, rowH - 8, 8,
                               SDL_Color{14, 20, 34, 200});
            batch.flush(r);
        }
        drawText(r, vx, ty, values[i], vc, 1.6f);
    }

    drawText(r, static_cast<float>(boxX + pad + 6), static_cast<float>(devY + 7),
             "MIDI (TAB)", th::label, 1.5f);
    const std::string dev = ellipsize(device, 30);
    const float dw = textWidth(dev.c_str(), 1.5f);
    drawText(r, static_cast<float>(boxX + boxW - pad) - dw,
             static_cast<float>(devY + 7), dev.c_str(), th::value, 1.5f);

    drawText(r, static_cast<float>(boxX + pad),
             static_cast<float>(boxY + boxH - 26),
             "S/Esc chiudi    Su/Giù seleziona    Sinistra/Destra modifica",
             th::hint, 1.3f);
}

static void drawChordHud(SDL_Renderer* r, GfxBatch& batch, int ww, const ChordInfo& ci) {
    if (ci.name.empty()) return;

    const float scale  = 3.1f;
    const float tw     = textWidth(ci.name.c_str(), scale);
    const int   panelW = static_cast<int>(tw) + 64;
    const int   panelH = 72;
    const int   px     = (ww - panelW) / 2;
    const int   py     = 14;

    const SDL_Color accent = (ci.root >= 0) ? noteColor(ci.root)
                                            : SDL_Color{130, 140, 170, 255};

    // alone colorato + scheda in vetro
    addGlow(batch, static_cast<float>(px), static_cast<float>(py),
            static_cast<float>(panelW), static_cast<float>(panelH), 18.0f,
            accent, 90, 5);
    batch.addRoundRect(px - 1.5f, py - 1.5f, panelW + 3.0f, panelH + 3.0f, 19,
                       withAlpha(accent, 190));
    batch.addRoundRectGrad(px, py, panelW, panelH, 18,
                           SDL_Color{20, 23, 36, 245}, SDL_Color{10, 11, 19, 245});
    batch.addRoundRect(px + 1.0f, py + 1.0f, panelW - 2.0f, 1.5f, 0.75f,
                       SDL_Color{255, 255, 255, 26});
    batch.addRoundRect(px + 6.0f, py + 11.0f, 5.0f, panelH - 22.0f, 2.5f, accent);
    batch.flush(r);

    drawText(r, px + (panelW - tw) * 0.5f + 3.0f, py + 13.0f, ci.name.c_str(),
             SDL_Color{248, 250, 255, 255}, scale);

    if (ci.count < 2 || ci.noteList.empty()) return;

    // "chip" delle note
    std::vector<std::string> names;
    {
        std::string cur;
        for (char c : ci.noteList) {
            if (c == ' ') {
                if (!cur.empty()) names.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) names.push_back(cur);
    }
    if (names.empty()) return;

    const float cs    = 1.7f;
    const int   chipH = 30;
    const int   gap   = 8;
    int total = 0;
    std::vector<int> ws;
    for (auto& n : names) {
        const int w = static_cast<int>(textWidth(n.c_str(), cs)) + 24;
        ws.push_back(w);
        total += w;
    }
    total += gap * (static_cast<int>(names.size()) - 1);

    if (total > ww - 40) return; // troppo lungo: niente chip

    int x = (ww - total) / 2;
    const int y = py + panelH + 9;
    for (size_t i = 0; i < names.size(); ++i) {
        batch.addRoundRect(static_cast<float>(x) - 1.0f, static_cast<float>(y) - 1.0f,
                           static_cast<float>(ws[i]) + 2.0f, chipH + 2.0f, 10,
                           withAlpha(accent, 55));
        batch.addRoundRectGrad(static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(ws[i]), static_cast<float>(chipH), 10,
                               SDL_Color{36, 41, 60, 245}, SDL_Color{22, 26, 40, 245});
        batch.flush(r);
        const float nw = textWidth(names[i].c_str(), cs);
        drawText(r, x + (ws[i] - nw) * 0.5f, static_cast<float>(y + 6),
                 names[i].c_str(), SDL_Color{214, 232, 255, 255}, cs);
        x += ws[i] + gap;
    }
}

static void drawHelp(SDL_Renderer* r, GfxBatch& batch, int ww, int wh) {
    const int boxW = std::min(900, ww - 28);
    const int boxH = std::min(530, wh - 28);
    const int boxX = (ww - boxW) / 2;
    const int boxY = (wh - boxH) / 2;

    batch.addRoundRect(boxX - 6, boxY - 4, boxW + 12, boxH + 12, 22,
                       SDL_Color{0, 0, 0, 120});
    batch.addRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 18, th::cardEdge);
    batch.addRoundRect(boxX, boxY, boxW, boxH, 16, th::card);
    batch.addRoundRectGrad(boxX + 3, boxY + 3, boxW - 6, 58, 14,
                           th::header1, th::header2);
    batch.flush(r);

    drawText(r, boxX + 24, boxY + 14, "Comandi e funzioni", th::title, 2.0f);
    drawText(r, boxX + 24, boxY + 39,
             "fMidiKeyboard  -  tutto quello che serve, senza menu nascosti",
             withAlpha(th::subtle, 190), 1.1f);

    const int top = boxY + 78;
    const int colW = (boxW - 72) / 2;
    const int lineH = std::max(17, std::min(23, (boxH - 108) / 18));
    const float fs = lineH <= 20 ? 1.05f : 1.15f;

    const char* left[][2] = {
        {"SUONO", ""},
        {"MIDI", "collega il piano; TAB cambia porta"},
        {"Mouse", "clicca i tasti per suonare"},
        {"P", "panic / ferma tutte le note"},
        {"S", "impostazioni audio e strumenti"},
        {"Y", "editor sintetizzatore"},
        {"O / I", "apri plugin VST3 / strumento"},
        {"E", "editor del plugin"},
        {"VISTA", ""},
        {"V / N / A", "direzione, nomi note, accordo"},
        {"F", "schermo intero"},
        {"H", "chiudi questo aiuto"},
        {"Esc", "esci dall'app"},
    };
    const char* right[][2] = {
        {"STRUMENTI", ""},
        {"I", "scegli Rhodes, pianoforte o SFZ"},
        {"--rhodes", "avvia col piano elettrico"},
        {"PLUGIN", ""},
        {"TAB", "porta MIDI successiva"},
        {"Mouse", "suona anche senza pianoforte MIDI"},
    };

    auto drawColumn = [&](int cx, const char* const items[][2], int count) {
        int y = top;
        for (int i = 0; i < count; ++i) {
            const char* key = items[i][0];
            const char* desc = items[i][1];
            if (!*desc) {
                drawText(r, static_cast<float>(cx), static_cast<float>(y), key,
                         th::section, 1.2f);
                y += lineH + 2;
            } else {
                drawText(r, static_cast<float>(cx), static_cast<float>(y), key,
                         th::title, fs);
                drawText(r, static_cast<float>(cx + 92), static_cast<float>(y), desc,
                         th::label, fs);
                y += lineH;
            }
        }
    };
    drawColumn(boxX + 24, left, static_cast<int>(sizeof(left) / sizeof(left[0])));
    drawColumn(boxX + 36 + colW, right, static_cast<int>(sizeof(right) / sizeof(right[0])));

    drawText(r, boxX + 24, boxY + boxH - 25,
             "H / Esc chiudi aiuto", th::hint, 1.05f);
}

static void drawBrowser(SDL_Renderer* r, GfxBatch& batch, int ww, int wh,
                        const FileBrowser& fb) {
    const int rowH = 30;
    const int pad  = 20;
    const int boxW = std::min(760, ww - 40);
    const int rows = 12;
    const int headerH = 84;
    const int boxH = headerH + rows * rowH + 38;
    const int boxX = (ww - boxW) / 2;
    const int boxY = (wh - boxH) / 2;

    batch.addRoundRect(boxX - 6, boxY - 4, boxW + 12, boxH + 12, 22,
                       SDL_Color{0, 0, 0, 110});
    batch.addRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 18, th::cardEdge);
    batch.addRoundRect(boxX, boxY, boxW, boxH, 16, th::card);
    batch.addRoundRectGrad(boxX + 3, boxY + 3, boxW - 6, headerH - 3, 14,
                           th::header1, th::header2);
    batch.flush(r);

    const bool instr = (fb.mode == BROWSE_INSTRUMENT);
    const char* title = instr ? "Scegli uno strumento" : "Scegli un plugin VST3";
    const char* kind  = instr ? "SFZ / Decent Sampler" : "VST3";

    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 14),
             title, th::title, 2.0f);
    {
        const float tw = textWidth(title, 2.0f);
        const float bw = textWidth(kind, 1.15f) + 16.0f;
        batch.addRoundRect(boxX + pad + tw + 14, boxY + 16, bw, 20, 10,
                           withAlpha(th::subtle, 55));
        batch.flush(r);
        drawText(r, boxX + pad + tw + 22, boxY + 19, kind, th::subtle, 1.15f);
    }

    const std::string path = ellipsize(fb.dir, 52);
    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 52),
             path.c_str(), th::value, 1.4f);

    const int listY = boxY + headerH;
    int first = 0;
    if (fb.sel >= rows) first = fb.sel - rows + 1;
    const int last = std::min(static_cast<int>(fb.entries.size()), first + rows);

    if (fb.entries.empty())
        drawText(r, static_cast<float>(boxX + pad), static_cast<float>(listY + 8),
                 "(nessun file compatibile)", th::dim, 1.5f);

    for (int i = first; i < last; ++i) {
        const int y = listY + (i - first) * rowH;
        if (i == fb.sel) {
            batch.addRoundRect(static_cast<float>(boxX + 10), static_cast<float>(y - 3),
                               static_cast<float>(boxW - 20), static_cast<float>(rowH - 4),
                               8, th::sel);
            batch.addRoundRect(static_cast<float>(boxX + 10), static_cast<float>(y - 3),
                               4.0f, static_cast<float>(rowH - 4), 2, th::selEdge);
            batch.flush(r);
        }
        const BrowseEntry& e = fb.entries[i];
        std::string name = ellipsize(e.name, 46);
        if (e.isDir) name += "/";
        SDL_Color c = e.isDir ? SDL_Color{214, 219, 238, 255} : th::good;
        if (i == fb.sel) c = th::title;
        drawText(r, static_cast<float>(boxX + pad + 6), static_cast<float>(y),
                 name.c_str(), c, 1.5f);
    }

    // scrollbar
    if (static_cast<int>(fb.entries.size()) > rows) {
        const float trackX = static_cast<float>(boxX + boxW - 12);
        const float trackY = static_cast<float>(listY);
        const float trackH = static_cast<float>(rows * rowH);
        batch.addRoundRect(trackX, trackY, 5, trackH, 2.5f, withAlpha(th::subtle, 40));
        const float total = static_cast<float>(fb.entries.size());
        const float thumbH = std::max(24.0f, trackH * rows / total);
        const float thumbY = trackY + (trackH - thumbH) * (static_cast<float>(first) /
                                                           std::max(1.0f, total - rows));
        batch.addRoundRect(trackX, thumbY, 5, thumbH, 2.5f, withAlpha(th::selEdge, 200));
        batch.flush(r);
    }

    drawText(r, static_cast<float>(boxX + pad),
             static_cast<float>(boxY + boxH - 26),
             "Invio apri    Backspace su    Esc annulla",
             th::hint, 1.35f);
}

// ------------------------------------------------- editor sintetizzatore ----

enum SynthParam {
    SY_PRESET = 0, SY_WAVE, SY_OCTAVE, SY_UNISON, SY_DETUNE, SY_SUB,
    SY_CUTOFF, SY_RESONANCE, SY_ATTACK, SY_DECAY, SY_SUSTAIN, SY_RELEASE,
    SY_LFO_RATE, SY_LFO_DEPTH, SY_DRIVE, SY_VOLUME, SY_FILTER_ENV, SY_REVERB,
    SY_COUNT
};

static const char* synthParamLabel(int i) {
    switch (i) {
        case SY_PRESET:     return "Preset";
        case SY_WAVE:       return "Onda";
        case SY_OCTAVE:     return "Ottava";
        case SY_UNISON:     return "Unisono";
        case SY_DETUNE:     return "Detune";
        case SY_SUB:        return "Sub-oscillatore";
        case SY_CUTOFF:     return "Cutoff";
        case SY_RESONANCE:  return "Risonanza";
        case SY_ATTACK:     return "Attacco";
        case SY_DECAY:      return "Decadimento";
        case SY_SUSTAIN:    return "Sostenuto";
        case SY_RELEASE:    return "Rilascio";
        case SY_LFO_RATE:   return "LFO velocità";
        case SY_LFO_DEPTH:  return "LFO profondità";
        case SY_DRIVE:      return "Drive";
        case SY_VOLUME:     return "Volume synth";
        case SY_FILTER_ENV: return "Filtro ADSR";
        case SY_REVERB:     return "Riverbero";
        default:            return "";
    }
}

static std::string timeString(float seconds) {
    char b[32];
    if (seconds < 0.999f)
        std::snprintf(b, sizeof(b), "%.0f ms", seconds * 1000.0f);
    else
        std::snprintf(b, sizeof(b), "%.2f s", seconds);
    return b;
}

static std::string synthParamValue(const SynthSettings& s, int i) {
    char b[64];
    switch (i) {
        case SY_PRESET:
            return s.preset < 0 ? "Personalizzato" : synthPresetName(s.preset);
        case SY_WAVE:
            return synthWaveName(s.wave);
        case SY_OCTAVE:
            std::snprintf(b, sizeof(b), "%+d", s.octave);
            return b;
        case SY_UNISON:
            std::snprintf(b, sizeof(b), "%d x", s.unison);
            return b;
        case SY_DETUNE:
            std::snprintf(b, sizeof(b), "%.0f ct", s.detune);
            return b;
        case SY_SUB:
            std::snprintf(b, sizeof(b), "%d%%", static_cast<int>(s.sub * 100.0f + 0.5f));
            return b;
        case SY_CUTOFF:
            if (s.cutoff >= 1000.0f)
                std::snprintf(b, sizeof(b), "%.2f kHz", s.cutoff / 1000.0f);
            else
                std::snprintf(b, sizeof(b), "%.0f Hz", s.cutoff);
            return b;
        case SY_RESONANCE:
            std::snprintf(b, sizeof(b), "%d%%",
                          static_cast<int>(s.resonance * 100.0f + 0.5f));
            return b;
        case SY_ATTACK:  return timeString(s.attack);
        case SY_DECAY:   return timeString(s.decay);
        case SY_SUSTAIN:
            std::snprintf(b, sizeof(b), "%d%%",
                          static_cast<int>(s.sustain * 100.0f + 0.5f));
            return b;
        case SY_RELEASE: return timeString(s.release);
        case SY_LFO_RATE:
            std::snprintf(b, sizeof(b), "%.1f Hz", s.lfoRate);
            return b;
        case SY_LFO_DEPTH:
            std::snprintf(b, sizeof(b), "%.0f ct", s.lfoDepth);
            return b;
        case SY_DRIVE:
            std::snprintf(b, sizeof(b), "%.2f", s.drive);
            return b;
        case SY_VOLUME:
            std::snprintf(b, sizeof(b), "%d%%",
                          static_cast<int>(s.volume * 100.0f + 0.5f));
            return b;
        case SY_FILTER_ENV:
            std::snprintf(b, sizeof(b), "%d%%",
                          static_cast<int>(s.filterEnv * 100.0f + 0.5f));
            return b;
        case SY_REVERB:
            std::snprintf(b, sizeof(b), "%d%%",
                          static_cast<int>(s.reverb * 100.0f + 0.5f));
            return b;
        default:
            return "";
    }
}

static float previewWave(int wave, float t) {
    switch (wave) {
        case SYNTH_WAVE_SINE:     return std::sin(6.28318530718f * t);
        case SYNTH_WAVE_SAW:      return 2.0f * t - 1.0f;
        case SYNTH_WAVE_SQUARE:   return t < 0.5f ? 1.0f : -1.0f;
        case SYNTH_WAVE_TRIANGLE: return 1.0f - 4.0f * std::fabs(t - 0.5f);
        case SYNTH_WAVE_PULSE:    return t < 0.25f ? 1.0f : -1.0f;
        case SYNTH_WAVE_NOISE:
            return std::sin(6.2831853f * t * 17.0f) * 0.62f +
                   std::sin(6.2831853f * t * 41.0f) * 0.38f;
        default:                  return 0.0f;
    }
}

static void plotCurve(GfxBatch& batch, const float* v, int n, float x, float y,
                      float w, float h, float gain, SDL_Color c) {
    float py = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float cx = x + (w * static_cast<float>(i)) /
                             static_cast<float>(std::max(1, n - 1));
        float cy = y + h * 0.5f - v[i] * gain * h * 0.5f;
        cy = std::max(y, std::min(y + h, cy));
        if (i > 0) {
            const float top = std::min(py, cy);
            const float bot = std::max(py, cy);
            batch.addRect(cx - 1.0f, top, 2.0f, std::max(1.2f, bot - top), c);
        }
        py = cy;
    }
}

static void drawSynthEditor(SDL_Renderer* r, GfxBatch& batch, int ww, int wh,
                            const SynthSettings& s, int selected,
                            const Synth& synth) {
    const int pad     = 24;
    const int headerH = 70;
    const int footerH = 34;
    const int boxW    = std::min(1040, ww - 40);
    const int rowH    = std::min(32, std::max(18,
                            (wh - (headerH + footerH + 28)) / SY_COUNT));
    const int boxH    = headerH + SY_COUNT * rowH + footerH;
    const int boxX    = (ww - boxW) / 2;
    const int boxY    = (wh - boxH) / 2;
    const int colW    = std::min(440, boxW * 45 / 100);
    const int rx      = boxX + colW + 8;
    const int rw      = boxW - colW - pad - 4;

    batch.addRoundRect(boxX - 6, boxY - 4, boxW + 12, boxH + 12, 22,
                       SDL_Color{0, 0, 0, 110});
    batch.addRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 18, th::cardEdge);
    batch.addRoundRect(boxX, boxY, boxW, boxH, 16, th::card);
    batch.addRoundRectGrad(boxX + 3, boxY + 3, boxW - 6, headerH - 3, 14,
                           th::header1, th::header2);
    batch.flush(r);

    const int scopeY = boxY + headerH + 8;
    const int scopeH = std::max(90, std::min(160, (boxH - headerH - footerH) * 34 / 100));
    const int waveY  = scopeY + scopeH + 10;
    const int waveH  = std::max(70, std::min(100, (boxH - headerH - footerH) * 22 / 100));
    const int envY   = waveY + waveH + 10;
    const int envH   = boxY + boxH - footerH - 12 - envY;

    batch.addRoundRect(rx, scopeY, rw, scopeH, 10, SDL_Color{10, 12, 20, 235});
    batch.addRoundRect(rx, waveY, rw, waveH, 10, SDL_Color{10, 12, 20, 235});
    if (envH >= 54)
        batch.addRoundRect(rx, envY, rw, envH, 10, SDL_Color{10, 12, 20, 235});

    const SDL_Color grid = withAlpha(th::subtle, 26);
    for (int i = 1; i < 4; ++i) {
        const float gy = scopeY + scopeH * i / 4.0f;
        batch.addRect(rx + 10, gy, rw - 20, 1.0f, grid);
        const float gx = rx + rw * i / 4.0f;
        batch.addRect(gx, scopeY + 8, 1.0f, scopeH - 16, grid);
    }
    for (int i = 1; i < 3; ++i)
        batch.addRect(rx + 10, waveY + waveH * i / 3.0f, rw - 20, 1.0f, grid);

    {
        float sc[256];
        synth.copyScope(sc, 256);
        float peak = 0.0f;
        for (float v : sc) peak = std::max(peak, std::fabs(v));
        const float gain = peak > 1e-4f ? std::min(4.0f, 0.46f / peak) : 0.0f;
        plotCurve(batch, sc, 256, rx + 10, scopeY + 10, rw - 20, scopeH - 20,
                  gain, SDL_Color{96, 218, 255, 235});
    }

    {
        float pts[240];
        for (int i = 0; i < 240; ++i) {
            const float t = static_cast<float>(i) / 239.0f * 2.0f;
            pts[i] = previewWave(s.wave, t - std::floor(t));
        }
        plotCurve(batch, pts, 240, rx + 12, waveY + 8, rw - 24, waveH - 16,
                  0.72f, withAlpha(noteColor(72), 235));
    }

    if (envH >= 54) {
        const float ax = rx + 18;
        const float aw = rw - 36;
        const float base = envY + envH - 18;
        const float top = envY + 26;
        const float span = base - top;
        const float a = std::max(0.001f, s.attack);
        const float d = std::max(0.005f, s.decay);
        const float r = std::max(0.01f, s.release);
        const float sum = a + d + r;
        const float attackW = aw * (0.12f + 0.30f * a / sum);
        const float decayW = aw * (0.12f + 0.30f * d / sum);
        const float releaseW = aw * (0.20f + 0.26f * r / sum);
        const float sustainW = std::max(8.0f, aw - attackW - decayW - releaseW);
        const float sy = base - span * std::max(0.0f, std::min(1.0f, s.sustain));

        struct Pt { float x, y; };
        const Pt pts[] = {
            {ax, base},
            {ax + attackW, top},
            {ax + attackW + decayW, sy},
            {ax + attackW + decayW + sustainW, sy},
            {std::min(ax + aw, ax + attackW + decayW + sustainW + releaseW), base}
        };
        for (int i = 0; i < 4; ++i) {
            const float dx = pts[i + 1].x - pts[i].x;
            const int steps = std::max(1, static_cast<int>(dx / 3.0f));
            for (int k = 0; k <= steps; ++k) {
                const float f = static_cast<float>(k) / static_cast<float>(steps);
                const float x = pts[i].x + dx * f;
                const float y = pts[i].y + (pts[i + 1].y - pts[i].y) * f;
                batch.addRect(x - 1.0f, y - 1.0f, 2.0f, 2.0f,
                              withAlpha(th::good, 235));
            }
        }
    }

    const int rowX = boxX + pad;
    const int rowW = colW - pad;
    const int listY = boxY + headerH + 8;
    for (int i = 0; i < SY_COUNT; ++i) {
        if (i != selected) continue;
        const int ry = listY + i * rowH;
        batch.addRoundRect(rowX - 8, ry - 3, rowW + 8, rowH - 4, 9, th::sel);
        batch.addRoundRect(rowX - 8, ry - 3, 4, rowH - 4, 2, th::selEdge);
    }
    batch.flush(r);

    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 16),
             "Sintetizzatore", th::title, 2.1f);
    drawText(r, static_cast<float>(boxX + pad), static_cast<float>(boxY + 42),
             "Generatore di onde PolyBLEP  -  filtro risonante  -  ADSR",
             withAlpha(th::subtle, 180), 1.25f);
    {
        const char* hint = "Y / Esc chiudi";
        const float hw = textWidth(hint, 1.2f) + 18.0f;
        drawText(r, static_cast<float>(boxX + boxW - pad) - hw,
                 static_cast<float>(boxY + 20), hint, th::hint, 1.2f);
    }

    char vbuf[64];
    std::snprintf(vbuf, sizeof(vbuf), "VOCI %d / 32", synth.activeVoices());
    drawText(r, static_cast<float>(rx + 12), static_cast<float>(scopeY + 8),
             vbuf, withAlpha(th::good, 220), 1.1f);
    drawText(r, static_cast<float>(rx + 12), static_cast<float>(scopeY + 22),
             "OSCILLOSCOPIO", withAlpha(th::subtle, 150), 1.0f);
    drawText(r, static_cast<float>(rx + 12), static_cast<float>(waveY + 6),
             "FORMA D'ONDA", withAlpha(th::subtle, 150), 1.0f);
    if (envH >= 54)
        drawText(r, static_cast<float>(rx + 12), static_cast<float>(envY + 6),
                 "INVILUPPO ADSR", withAlpha(th::subtle, 150), 1.0f);

    const float rowScale = rowH >= 28 ? 1.50f : rowH >= 23 ? 1.32f : 1.12f;
    for (int i = 0; i < SY_COUNT; ++i) {
        const int ry = listY + i * rowH;
        const bool sel = (i == selected);
        drawText(r, static_cast<float>(rowX + 4), static_cast<float>(ry),
                 synthParamLabel(i), sel ? th::title : th::label, rowScale);

        const std::string val = synthParamValue(s, i);
        const float vw = textWidth(val.c_str(), rowScale);
        const float vx = static_cast<float>(rowX + rowW - 16) - vw;
        if (sel) {
            drawText(r, vx - 18.0f, static_cast<float>(ry), "<", th::selEdge, rowScale);
            drawText(r, vx + vw + 5.0f, static_cast<float>(ry), ">", th::selEdge, rowScale);
        }
        drawText(r, vx, static_cast<float>(ry), val.c_str(), th::value, rowScale);
    }

    drawText(r, static_cast<float>(boxX + pad),
             static_cast<float>(boxY + boxH - 24),
             "Su/Giù seleziona    Sinistra/Destra regola    Invio aumenta    Y/Esc chiudi",
             th::hint, 1.25f);
}

// ------------------------------------------------------------------- Main -----

#ifndef FMK_VERSION
#define FMK_VERSION "1.0"
#endif
static const char* kVersion = "fMidiKeyboard " FMK_VERSION;

static void printHelp(const char* exe) {
    std::printf(
        "%s - Visualizzatore MIDI per pianoforte con synth integrato (stile Synthesia)\n\n"
        "Uso: %s [opzioni]\n\n"
        "Opzioni:\n"
        "  --plugin PERCORSO     carica all'avvio un plugin/bundle VST3\n"
        "  --instrument PERCORSO carica all'avvio uno strumento .sfz/.dspreset\n"
        "  --rhodes              usa il piano elettrico Rhodes integrato\n"
        "  --piano               usa il pianoforte sintetizzato integrato\n"
        "  --synth               parte con il sintetizzatore (fonte audio=4)\n"
        "  --source N            fonte: 0=spento 1=VST3 2=interno 3=VST3+interno\n"
        "                        4=synth 5=synth+VST3 6=synth+interno 7=tutto\n"
        "  --vst-scan            elenca i plugin VST3 trovati\n"
        "  --vst-info PERCORSO   verifica il caricamento di un plugin VST3\n"
        "  -h, --help            mostra questo aiuto\n"
        "  -v, --version         mostra la versione\n\n"
        "Scorciatoie in finestra:\n"
        "  S=pannello  Y=synth  O=plugin  I=strumento  E=editor  TAB=MIDI\n"
        "  V=direzione  N=nomi  A=accordo  Su/Giu'=velocita'  P=panic\n"
        "  H=aiuto  F=schermo intero  Mouse=suona i tasti  Esc=esci\n",
        kVersion, exe);
}

struct Args {
    std::string instrument;
    std::string plugin;
    int         source = -1;
};

// Restituisce 0 per continuare con la GUI, altrimenti il processo termina.
static int cliMode(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--vst-scan") == 0) {
            const auto list = VstHost::scanPlugins();
            std::printf("Plugin VST3 trovati: %zu\n", list.size());
            for (const auto& p : list) std::printf("  %s\n", p.c_str());
            return 1;
        }
        if (std::strcmp(a, "--vst-info") == 0 && i + 1 < argc) {
            VstHost h;
            if (!h.available()) {
                std::printf("Questa build non include l'hosting VST3.\n");
                return 1;
            }
            if (h.load(argv[i + 1]))
                std::printf("OK: '%s'  (%s)\n", h.pluginName().c_str(), argv[i + 1]);
            else
                std::printf("ERRORE: %s\n", h.lastError().c_str());
            return 1;
        }
        if (std::strcmp(a, "--plugin") == 0 && i + 1 < argc)     { args.plugin = argv[++i]; continue; }
        if (std::strcmp(a, "--instrument") == 0 && i + 1 < argc) { args.instrument = argv[++i]; continue; }
        if (std::strcmp(a, "--piano") == 0)                      { args.instrument = ":piano"; continue; }
        if (std::strcmp(a, "--rhodes") == 0)                     { args.instrument = ":rhodes"; continue; }
        if (std::strcmp(a, "--synth") == 0)                      { args.source = SRC_SYNTH; continue; }
        if (std::strcmp(a, "--source") == 0 && i + 1 < argc)     { args.source = std::atoi(argv[++i]); continue; }
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) { printHelp(argv[0]); return 1; }
        if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--version") == 0) {
            std::printf("%s\n", kVersion);
            return 1;
        }
        std::fprintf(stderr, "Opzione sconosciuta: %s\n", a);
        printHelp(argv[0]);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    Args args;
    if (cliMode(argc, argv, args)) return 0;

#if defined(__linux__)
    // I VST3 su Linux usano X11 per l'editor: sotto Wayland forza XWayland,
    // altrimenti l'interfaccia dei plugin (sfizz, Decent Sampler, ...) non si
    // aggancia e non e' possibile caricare gli strumenti.
    if (!std::getenv("SDL_VIDEODRIVER")) {
        const int nd = SDL_GetNumVideoDrivers();
        for (int i = 0; i < nd; ++i) {
            if (std::strcmp(SDL_GetVideoDriver(i), "x11") == 0) {
                setenv("SDL_VIDEODRIVER", "x11", 0);
                break;
            }
        }
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Carica le impostazioni (incl. geometria finestra) prima di crearla.
    Settings    settings;
    std::string settingsPath;
    if (char* pref = SDL_GetPrefPath("fMidiKeyboard", "fMidiKeyboard")) {
        settingsPath = std::string(pref) + "settings.txt";
        SDL_free(pref);
        loadSettings(settings, settingsPath);
    }
    // Opzioni da riga di comando: hanno la precedenza sul file. Sono
    // temporanee: se l'utente non le cambia durante la sessione vengono rimosse
    // al salvataggio, cosi' non sovrascrivono il file di impostazioni.
    const std::string fileInstrument = settings.instrumentPath;
    const std::string filePlugin     = settings.pluginPath;
    const int         fileSource     = settings.audioSource;

    const std::string cliInstrument = args.instrument;
    const std::string cliPlugin     = args.plugin;
    int               cliSource     = -1;

    if (!args.instrument.empty()) settings.instrumentPath = args.instrument;
    if (!args.plugin.empty())     settings.pluginPath     = args.plugin;
    if (args.source >= 0) {
        cliSource = std::min<int>(7, std::max<int>(0, args.source));
        settings.audioSource = cliSource;
    }

    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (settings.fullscreen) winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    SDL_Window* win = SDL_CreateWindow(
        "fMidiKeyboard - MIDI Piano + Synth",
        settings.winX, settings.winY, settings.winW, settings.winH, winFlags);
    if (!win) {
        std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (settings.maximized && !settings.fullscreen) SDL_MaximizeWindow(win);
    SDL_SetWindowMinimumSize(win, 720, 480);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (!textInit(ren))
        std::fprintf(stderr, "Testo: uso il font di fallback.\n");

    MidiInput   midi;
    NoteTracker tracker;
    VstHost     vst;
    Sampler     sampler;
    Synth       synth;
    synth.applySettings(settings.synth);
    AudioEngine audio;                              // unico dispositivo + mix
    std::recursive_mutex audioMutex;                // protegge vst/sampler vs. thread MIDI
    std::atomic<int> audioSourceAtomic{SRC_OFF};    // fonte audio letta dal thread MIDI
    FileBrowser fb;

    // Messaggio di stato transitorio mostrato a fondo schermo.
    std::string statusMsg;
    Uint32      statusUntil = 0;
    auto setStatus = [&](std::string s, Uint32 ms = 3500) {
        statusMsg   = std::move(s);
        statusUntil = SDL_GetTicks() + ms;
    };

    // (Ri)configura il motore audio in base alla fonte scelta (VST3 e/o interno).
    auto applyAudioSource = [&]() {
        audioSourceAtomic.store(settings.audioSource, std::memory_order_relaxed);
        std::lock_guard<std::recursive_mutex> lk(audioMutex);
        const int  src = settings.audioSource;
        const bool onV = srcHasVst(src)     && vst.loaded();
        const bool onS = srcHasSampler(src) && sampler.loaded();
        const bool onY = srcHasSynth(src);

        audio.stop();
        audio.setVst(onV ? &vst : nullptr);
        audio.setSampler(onS ? &sampler : nullptr);
        audio.setSynth(onY ? &synth : nullptr);
        audio.setMasterGain(settings.volume);
        if ((onV || onS || onY) && !audio.start(48000, 256)) {
            std::fprintf(stderr, "Audio: impossibile aprire il dispositivo (%s)\n",
                         SDL_GetError());
            setStatus("Audio: dispositivo non disponibile");
        } else if (!onV && !onS && !onY) {
            if (srcHasVst(src) && !vst.loaded())
                setStatus("Nessun plugin VST3 caricato (tasto O)");
            else if (srcHasSampler(src) && !sampler.loaded())
                setStatus("Nessuno strumento interno caricato (tasto I)");
        }
    };

    // Stato del plugin (ricorda lo strumento caricato nei sampler).
    auto vstStateFile = [&](const std::string& p) -> std::string {
        if (settingsPath.empty() || p.empty()) return {};
        return settingsPath + ".vst3state." +
               std::to_string(std::hash<std::string>{}(p));
    };
    auto savePluginState = [&]() {
        if (!vst.loaded()) return;
        std::string sf = vstStateFile(vst.pluginPath());
        if (!sf.empty()) vst.saveState(sf);
    };
    auto restorePluginState = [&]() {
        if (!vst.loaded()) return;
        std::string sf = vstStateFile(vst.pluginPath());
        if (!sf.empty() && fs::exists(sf)) vst.loadState(sf);
    };

    // Silenzia subito tutto (note bloccate). Usato dal tasto P e all'uscita.
    auto panic = [&]() {
        std::lock_guard<std::recursive_mutex> lk(audioMutex);
        vst.allNotesOff();
        sampler.allNotesOff();
        synth.allNotesOff();
        tracker.panic(MidiInput::now());
    };

    // --- plugin VST3 ---
    std::vector<std::string> plugins = vst.available() ? VstHost::scanPlugins()
                                                       : std::vector<std::string>{};
    int curPlugin = -1;
    std::string pluginLabel = vst.available() ? "nessuno" : "n/d";
    if (!settings.pluginPath.empty()) {
        auto it = std::find(plugins.begin(), plugins.end(), settings.pluginPath);
        if (it != plugins.end()) curPlugin = static_cast<int>(it - plugins.begin());
        if (vst.load(settings.pluginPath)) {
            pluginLabel = vst.pluginName();
            restorePluginState();
        } else {
            std::fprintf(stderr, "VST3: %s\n", vst.lastError().c_str());
            setStatus("Errore VST3: " + vst.lastError());
        }
    }

    // --- strumento interno (SFZ / Decent Sampler) ---
    std::string instrumentLabel = "nessuno";
    if (!settings.instrumentPath.empty()) {
        if (sampler.load(settings.instrumentPath)) {
            instrumentLabel = sampler.instrumentName();
            if (sampler.isBuiltin())
                std::printf("Strumento interno: %s\n", instrumentLabel.c_str());
            else
                std::printf("Strumento interno: %s (%d regioni, %d campioni)\n",
                            instrumentLabel.c_str(), sampler.regionCount(),
                            sampler.sampleCount());
        } else {
            std::fprintf(stderr, "Strumento: %s\n", sampler.lastError().c_str());
            setStatus("Errore strumento: " + sampler.lastError());
            instrumentLabel = "errore";
        }
    } else {
        // Nessuno strumento configurato: parti col Rhodes, cosi' l'app suona
        // subito senza dover scaricare campioni.
        sampler.loadBuiltinRhodes();
        instrumentLabel = sampler.instrumentName();
        std::printf("Strumento interno: %s (predefinito)\n", instrumentLabel.c_str());
    }
    applyAudioSource();
    if (vst.available())
        std::printf("Trovati %zu plugin VST3. Usa il pannello (S) per sceglierne uno.\n",
                    plugins.size());

    // Instrada un evento verso tutte le sorgenti attive. Usato sia dal thread
    // MIDI sia dalla tastiera virtuale.
    auto routeToAudio = [&](const RawMidi& m) {
        const int type = m.status & 0xF0;
        const int src  = audioSourceAtomic.load(std::memory_order_relaxed);
        const bool toVst = srcHasVst(src);
        const bool toSmp = srcHasSampler(src);
        const bool toSyn = srcHasSynth(src);
        if (!toVst && !toSmp && !toSyn) return;
        std::lock_guard<std::recursive_mutex> lk(audioMutex);
        if (type == 0x90 && m.d2 > 0) {
            if (toVst) vst.noteOn(m.d1, m.d2);
            if (toSmp) sampler.noteOn(m.d1, m.d2);
            if (toSyn) synth.noteOn(m.d1, m.d2);
        } else if (type == 0x80 || (type == 0x90 && m.d2 == 0)) {
            if (toVst) vst.noteOff(m.d1);
            if (toSmp) sampler.noteOff(m.d1);
            if (toSyn) synth.noteOff(m.d1);
        } else if (type == 0xB0) {
            if (toVst) vst.controlChange(m.d1, m.d2);
            if (toSmp) sampler.controlChange(m.d1, m.d2);
            if (toSyn) synth.controlChange(m.d1, m.d2);
        }
    };

    // Le note live arrivano dal thread MIDI: le instradiamo subito, per una
    // latenza minima, verso le sorgenti attive.
    midi.setHandler([&](const RawMidi& m) {
        routeToAudio(m);
    });

    auto rescanPlugins = [&]() {
        if (!vst.available()) return;
        plugins = VstHost::scanPlugins();
        curPlugin = -1;
        auto it = std::find(plugins.begin(), plugins.end(), settings.pluginPath);
        if (it != plugins.end()) curPlugin = static_cast<int>(it - plugins.begin());
    };

    auto openBrowser = [&](int mode) {
        fb.open = true;
        fb.mode = mode;
        fb.sel  = 0;
        std::error_code ec;
        const std::string& cur =
            (mode == BROWSE_INSTRUMENT) ? settings.instrumentPath : settings.pluginPath;
        if (!cur.empty()) {
            fs::path p(cur);
            fs::path d = fs::is_directory(p, ec) ? p : p.parent_path();
            if (!d.empty() && fs::is_directory(d, ec)) fb.dir = d.string();
        }
        if (fb.dir.empty()) {
            const char* home = std::getenv("HOME");
            fb.dir = home ? home : ".";
        }
        fb.refresh();
    };

    std::vector<std::string> ports = midi.listPorts();
    int curPort = -1;

    auto selectPreferredPort = [&]() {
        ports = midi.listPorts();
        if (ports.empty()) return;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        int preferred = -1;
        if (!settings.midiPort.empty()) {
            for (size_t i = 0; i < ports.size(); ++i)
                if (ports[i] == settings.midiPort) { preferred = static_cast<int>(i); break; }
        }
        if (preferred < 0) {
            for (size_t i = 0; i < ports.size(); ++i) {
                const std::string nm = lower(ports[i]);
                if (nm.find("through") != std::string::npos) continue;
                if (nm.find("loopback") != std::string::npos) continue;
                preferred = static_cast<int>(i);
                break;
            }
        }
        if (preferred < 0) preferred = 0;
        if (midi.openPort(preferred)) {
            curPort = preferred;
            settings.midiPort = ports[preferred];
        }
    };

    std::printf("=== fMidiKeyboard ===\n");
    if (ports.empty()) {
        std::printf("Nessun dispositivo MIDI trovato. Collega il piano e premi TAB.\n");
    } else {
        std::printf("Porte MIDI disponibili:\n");
        for (size_t i = 0; i < ports.size(); ++i)
            std::printf("  [%zu] %s\n", i, ports[i].c_str());
        selectPreferredPort();
        if (curPort >= 0) std::printf("Aperto: %s\n", ports[curPort].c_str());
    }
    std::printf("Comandi: S=impostazioni  Y=sintetizzatore  O=plugin  "
                "I=strumento (Rhodes / pianoforte / SFZ)  E=editor  TAB=MIDI  H=aiuto  "
                "V=direzione  N=nomi  A=accordo  P=panic  F=schermo intero  Esc=esci\n");

    GfxBatch batch;
    bool     panelOpen = false;
    int      panelSel  = 0;
    bool     synthOpen = false;
    int      synthSel  = 0;
    bool     helpOpen  = false;
    int      mouseNote = -1;
    bool     running   = true;

    auto mouseNoteOff = [&]() {
        if (mouseNote < 0) return;
        RawMidi m;
        m.ts = MidiInput::now();
        m.status = 0x80;
        m.d1 = static_cast<uint8_t>(mouseNote);
        m.d2 = 0;
        tracker.noteOff(m.ts, mouseNote);
        routeToAudio(m);
        mouseNote = -1;
    };

    auto mouseNoteOn = [&](int pitch, int velocity) {
        if (pitch < 0 || pitch > 127) return;
        if (mouseNote == pitch) return;
        mouseNoteOff();
        RawMidi m;
        m.ts = MidiInput::now();
        m.status = 0x90;
        m.d1 = static_cast<uint8_t>(pitch);
        m.d2 = static_cast<uint8_t>(std::max(1, std::min(127, velocity)));
        tracker.noteOn(m.ts, pitch, m.d2, 0);
        routeToAudio(m);
        mouseNote = pitch;
    };

    // Apre l'editor del synth e, se necessario, instrada subito l'audio su di
    // esso: cosi' i tasti producono il suono che si sta editando.
    auto openSynthEditor = [&]() {
        synthOpen = true;
        panelOpen = false;
        helpOpen = false;
        fb.open = false;
        if (!srcHasSynth(settings.audioSource)) {
            settings.audioSource = SRC_SYNTH;
            applyAudioSource();
            setStatus("Fonte audio: Synth");
        }
    };

    SDL_Event ev;

    Uint32 lastTicks = SDL_GetTicks();
    double fps       = 0.0;

    auto adjustSpeed = [&](double d) {
        settings.speed = std::min(2000.0, std::max(80.0, settings.speed + d));
    };
    auto activateItem = [&](int item, bool right) {
        switch (item) {
            case P_DIR:      settings.rising = !settings.rising; break;
            case P_SPEED:    adjustSpeed(right ? 40.0 : -40.0); break;
            case P_NAMES:    settings.showNoteNames = !settings.showNoteNames; break;
            case P_CHORD:    settings.showChord = !settings.showChord; break;
            case P_NOTATION: settings.notation = 1 - settings.notation; break;
            case P_SOURCE: {
                const int step = right ? 1 : -1;
                settings.audioSource = (settings.audioSource + step + 8) % 8;
                applyAudioSource();
                break;
            }
            case P_VOLUME:
                settings.volume = std::min(1.25f, std::max(0.0f,
                                      settings.volume + (right ? 0.05f : -0.05f)));
                audio.setMasterGain(settings.volume);
                break;
            case P_PLUGIN:
                if (vst.available() && !plugins.empty()) {
                    std::lock_guard<std::recursive_mutex> lk(audioMutex);
                    const int n = static_cast<int>(plugins.size());
                    savePluginState();
                    audio.stop();
                    curPlugin = ((curPlugin + (right ? 1 : -1)) % n + n) % n;
                    if (vst.load(plugins[curPlugin])) {
                        pluginLabel    = vst.pluginName();
                        settings.pluginPath = plugins[curPlugin];
                        restorePluginState();
                        applyAudioSource();
                        setStatus("Plugin: " + pluginLabel);
                    } else {
                        pluginLabel = "errore";
                        std::fprintf(stderr, "VST3: %s\n", vst.lastError().c_str());
                        setStatus("Errore VST3: " + vst.lastError());
                    }
                }
                break;
            case P_EDITOR:
                if (vst.loaded()) {
                    if (vst.editorOpen()) vst.closeEditor();
                    else if (!vst.openEditor()) {
                        std::fprintf(stderr, "Editor: %s\n", vst.lastError().c_str());
                        setStatus("Errore editor: " + vst.lastError());
                    }
                }
                break;
            case P_LOAD_INSTR:
                openBrowser(BROWSE_INSTRUMENT);
                break;
            case P_SYNTH:
                openSynthEditor();
                break;
            case P_BROWSE:
                openBrowser(BROWSE_PLUGIN);
                break;
        }
    };

    // Regola un parametro del synth (un passo a sinistra o a destra).
    auto adjustSynthParam = [&](int item, int dir) {
        SynthSettings ss = settings.synth;
        const float d = static_cast<float>(dir);
        switch (item) {
            case SY_PRESET: {
                const int n = synthPresetCount();
                int idx;
                if (ss.preset < 0)
                    idx = (dir > 0) ? 0 : n - 1;
                else
                    idx = ((ss.preset + dir) % n + n) % n;
                settings.synth = synthPresetSettings(idx);
                synth.applySettings(settings.synth);
                setStatus(std::string("Synth: ") + synthPresetName(idx));
                return;
            }
            case SY_WAVE:
                ss.wave = (ss.wave + dir + SYNTH_WAVE_COUNT) % SYNTH_WAVE_COUNT;
                break;
            case SY_OCTAVE:
                ss.octave = std::max(-2, std::min(2, ss.octave + dir));
                break;
            case SY_UNISON:
                ss.unison = std::max(1, std::min(4, ss.unison + dir));
                break;
            case SY_DETUNE:
                ss.detune = std::max(0.0f, std::min(50.0f, ss.detune + d * 2.0f));
                break;
            case SY_SUB:
                ss.sub = std::max(0.0f, std::min(1.0f, ss.sub + d * 0.05f));
                break;
            case SY_CUTOFF:
                ss.cutoff = std::max(30.0f, std::min(18000.0f,
                                     ss.cutoff * std::pow(1.15f, d)));
                break;
            case SY_RESONANCE:
                ss.resonance = std::max(0.0f, std::min(0.95f,
                                        ss.resonance + d * 0.05f));
                break;
            case SY_ATTACK:
                ss.attack = std::max(0.001f, std::min(4.0f,
                                     ss.attack * std::pow(1.25f, d)));
                break;
            case SY_DECAY:
                ss.decay = std::max(0.005f, std::min(4.0f,
                                    ss.decay * std::pow(1.25f, d)));
                break;
            case SY_SUSTAIN:
                ss.sustain = std::max(0.0f, std::min(1.0f,
                                       ss.sustain + d * 0.05f));
                break;
            case SY_RELEASE:
                ss.release = std::max(0.01f, std::min(8.0f,
                                      ss.release * std::pow(1.25f, d)));
                break;
            case SY_LFO_RATE:
                ss.lfoRate = std::max(0.02f, std::min(20.0f,
                                     ss.lfoRate + d * 0.5f));
                break;
            case SY_LFO_DEPTH:
                ss.lfoDepth = std::max(0.0f, std::min(100.0f,
                                       ss.lfoDepth + d * 2.0f));
                break;
            case SY_DRIVE:
                ss.drive = std::max(1.0f, std::min(4.0f, ss.drive + d * 0.1f));
                break;
            case SY_VOLUME:
                ss.volume = std::max(0.0f, std::min(1.5f,
                                      ss.volume + d * 0.05f));
                break;
            case SY_FILTER_ENV:
                ss.filterEnv = std::max(0.0f, std::min(1.0f,
                                         ss.filterEnv + d * 0.05f));
                break;
            case SY_REVERB:
                ss.reverb = std::max(0.0f, std::min(1.0f,
                                      ss.reverb + d * 0.05f));
                break;
            default:
                break;
        }
        ss.preset = -1; // parametro modificato a mano
        settings.synth = ss;
        synth.applySettings(settings.synth);
    };

    const Uint32 mainWinId   = SDL_GetWindowID(win);
    bool         minimized   = false;

    while (running) {
        const Uint32 frameStart = SDL_GetTicks();
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT &&
                       ev.window.windowID == mainWinId) {
                switch (ev.window.event) {
                    case SDL_WINDOWEVENT_MINIMIZED:
                    case SDL_WINDOWEVENT_HIDDEN:
                        minimized = true;
                        break;
                    case SDL_WINDOWEVENT_RESTORED:
                    case SDL_WINDOWEVENT_SHOWN:
                        minimized = false;
                        break;
                    default: break;
                }
            } else if (ev.type == SDL_WINDOWEVENT && vst.editorOpen() &&
                       ev.window.windowID == vst.editorWindowId()) {
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    vst.closeEditor();
                } else if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                           ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    vst.onEditorResized();
                }
            } else if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
                const SDL_Keycode k = ev.key.keysym.sym;

                if (helpOpen) {
                    if (k == SDLK_ESCAPE || k == SDLK_h) helpOpen = false;
                } else if (fb.open) {
                    switch (k) {
                        case SDLK_ESCAPE: fb.open = false; break;
                        case SDLK_UP:
                            if (fb.sel > 0) --fb.sel;
                            break;
                        case SDLK_DOWN:
                            if (fb.sel + 1 < static_cast<int>(fb.entries.size())) ++fb.sel;
                            break;
                        case SDLK_PAGEUP:
                            fb.sel = std::max(0, fb.sel - 12);
                            break;
                        case SDLK_PAGEDOWN:
                            fb.sel = std::min(static_cast<int>(fb.entries.size()) - 1, fb.sel + 12);
                            break;
                        case SDLK_BACKSPACE: {
                            fs::path p(fb.dir);
                            fs::path parent = p.parent_path();
                            if (!parent.empty() && parent != p) {
                                fb.dir = parent.string();
                                fb.sel = 0;
                                fb.refresh();
                            }
                            break;
                        }
                        case SDLK_RETURN:
                        case SDLK_SPACE: {
                            if (fb.sel >= 0 &&
                                fb.sel < static_cast<int>(fb.entries.size())) {
                                const BrowseEntry e = fb.entries[fb.sel];
                                if (e.isDir) {
                                    fb.dir = e.path;
                                    fb.sel = 0;
                                    fb.refresh();
                                } else if (fb.mode == BROWSE_INSTRUMENT) {
                                    std::lock_guard<std::recursive_mutex> lk(audioMutex);
                                    audio.stop();
                                    if (sampler.load(e.path)) {
                                        instrumentLabel = sampler.instrumentName();
                                        settings.instrumentPath = e.path;
                                        applyAudioSource();
                                        if (sampler.isBuiltin())
                                            std::printf("Strumento caricato: %s\n",
                                                        instrumentLabel.c_str());
                                        else
                                            std::printf("Strumento caricato: %s (%d regioni, %d campioni)\n",
                                                        instrumentLabel.c_str(),
                                                        sampler.regionCount(),
                                                        sampler.sampleCount());
                                        setStatus("Strumento: " + instrumentLabel);
                                    } else {
                                        instrumentLabel = "errore";
                                        std::fprintf(stderr, "Strumento: %s\n",
                                                     sampler.lastError().c_str());
                                        setStatus("Errore strumento: " + sampler.lastError());
                                    }
                                    fb.open = false;
                                } else {
                                    std::lock_guard<std::recursive_mutex> lk(audioMutex);
                                    savePluginState();
                                    audio.stop();
                                    if (vst.load(e.path)) {
                                        pluginLabel = vst.pluginName();
                                        settings.pluginPath = e.path;
                                        restorePluginState();
                                        applyAudioSource();
                                        std::printf("Plugin caricato: %s\n",
                                                    pluginLabel.c_str());
                                        setStatus("Plugin: " + pluginLabel);
                                        if (!vst.openEditor())
                                            std::fprintf(stderr, "Editor: %s\n",
                                                         vst.lastError().c_str());
                                    } else {
                                        pluginLabel = "errore";
                                        std::fprintf(stderr, "VST3: %s\n",
                                                     vst.lastError().c_str());
                                        setStatus("Errore VST3: " + vst.lastError());
                                    }
                                    fb.open = false;
                                }
                            }
                            break;
                        }
                        default: break;
                    }
                } else if (synthOpen) {
                    switch (k) {
                        case SDLK_ESCAPE:
                        case SDLK_y: synthOpen = false; break;
                        case SDLK_UP:
                            synthSel = (synthSel - 1 + SY_COUNT) % SY_COUNT;
                            break;
                        case SDLK_DOWN:
                            synthSel = (synthSel + 1) % SY_COUNT;
                            break;
                        case SDLK_LEFT:
                            adjustSynthParam(synthSel, -1);
                            break;
                        case SDLK_RIGHT:
                        case SDLK_RETURN:
                        case SDLK_SPACE:
                            adjustSynthParam(synthSel, 1);
                            break;
                        default: break;
                    }
                } else if (panelOpen) {
                    switch (k) {
                        case SDLK_ESCAPE:
                        case SDLK_s: panelOpen = false; break;
                        case SDLK_y: openSynthEditor(); break;
                        case SDLK_UP:   panelSel = (panelSel - 1 + P_COUNT) % P_COUNT; break;
                        case SDLK_DOWN: panelSel = (panelSel + 1) % P_COUNT; break;
                        case SDLK_LEFT: activateItem(panelSel, false); break;
                        case SDLK_RIGHT:
                        case SDLK_RETURN:
                        case SDLK_SPACE: activateItem(panelSel, true); break;
                        case SDLK_f: {
                            const bool fs =
                                (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                            break;
                        }
                        default: break;
                    }
                } else {
                    switch (k) {
                        case SDLK_ESCAPE: running = false; break;
                        case SDLK_s:      synthOpen = false; rescanPlugins(); panelOpen = true; break;
                        case SDLK_y:      openSynthEditor(); break;
                        case SDLK_v:      settings.rising = !settings.rising; break;
                        case SDLK_n:      settings.showNoteNames = !settings.showNoteNames; break;
                        case SDLK_a:      settings.showChord = !settings.showChord; break;
                        case SDLK_p:      panic(); break;
                        case SDLK_h:      helpOpen = true; break;
                        case SDLK_o:      openBrowser(BROWSE_PLUGIN); break;
                        case SDLK_i:      openBrowser(BROWSE_INSTRUMENT); break;
                        case SDLK_e:
                            if (vst.loaded()) {
                                if (vst.editorOpen()) vst.closeEditor();
                                else if (!vst.openEditor())
                                    std::fprintf(stderr, "Editor: %s\n", vst.lastError().c_str());
                            }
                            break;
                        case SDLK_UP:     adjustSpeed(+40.0); break;
                        case SDLK_DOWN:   adjustSpeed(-40.0); break;
                        case SDLK_f: {
                            const bool fs =
                                (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                            break;
                        }
                        case SDLK_TAB: {
                            ports = midi.listPorts();
                            if (!ports.empty()) {
                                curPort = (curPort + 1) % static_cast<int>(ports.size());
                                midi.openPort(curPort);
                                settings.midiPort = ports[curPort];
                                setStatus("MIDI: " + ports[curPort]);
                                std::printf("Aperto: %s\n", ports[curPort].c_str());
                            }
                            break;
                        }
                        default: break;
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN &&
                       ev.button.button == SDL_BUTTON_LEFT &&
                       !fb.open && !synthOpen && !panelOpen && !helpOpen) {
                int mw = 0, mh = 0;
                SDL_GetWindowSize(win, &mw, &mh);
                Layout ml;
                computeLayout(mw, ml);
                const int p = pitchAtScreen(ml, static_cast<float>(ev.button.x),
                                            static_cast<float>(ev.button.y),
                                            static_cast<float>(mh) - KEYBOARD_H);
                if (p >= 0) {
                    const float rel = (static_cast<float>(ev.button.y) -
                                       (static_cast<float>(mh) - KEYBOARD_H)) / KEYBOARD_H;
                    const int vel = std::max(30, std::min(127,
                                        static_cast<int>(127.0f * (1.0f - 0.65f * rel))));
                    mouseNoteOn(p, vel);
                }
            } else if (ev.type == SDL_MOUSEMOTION && mouseNote >= 0 &&
                       (ev.motion.state & SDL_BUTTON_LMASK) &&
                       !fb.open && !synthOpen && !panelOpen && !helpOpen) {
                int mw = 0, mh = 0;
                SDL_GetWindowSize(win, &mw, &mh);
                Layout ml;
                computeLayout(mw, ml);
                const int p = pitchAtScreen(ml, static_cast<float>(ev.motion.x),
                                            static_cast<float>(ev.motion.y),
                                            static_cast<float>(mh) - KEYBOARD_H);
                if (p < 0) {
                    mouseNoteOff();
                } else if (p != mouseNote) {
                    const float rel = (static_cast<float>(ev.motion.y) -
                                       (static_cast<float>(mh) - KEYBOARD_H)) / KEYBOARD_H;
                    const int vel = std::max(30, std::min(127,
                                        static_cast<int>(127.0f * (1.0f - 0.65f * rel))));
                    mouseNoteOn(p, vel);
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                mouseNoteOff();
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.windowID == mainWinId &&
                       ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                mouseNoteOff();
            }
        }

        // FPS
        const Uint32 nowTicks = SDL_GetTicks();
        const double dt = (nowTicks - lastTicks) / 1000.0;
        lastTicks = nowTicks;
        if (dt > 0.0) fps = fps * 0.9 + (1.0 / dt) * 0.1;

        // Eventi MIDI
        const double t = MidiInput::now();
        RawMidi m;
        while (midi.poll(m)) {
            // L'audio e' gia' stato instradato dal handler sul thread MIDI;
            // qui aggiorniamo solo la grafica (NoteTracker).
            const int type    = m.status & 0xF0;
            const int channel = m.status & 0x0F;
            if (type == 0x90 && m.d2 > 0) {
                tracker.noteOn(m.ts, m.d1, m.d2, channel);
            } else if (type == 0x80 || (type == 0x90 && m.d2 == 0)) {
                tracker.noteOff(m.ts, m.d1);
            } else if (type == 0xB0) {
                tracker.controlChange(m.ts, m.d1, m.d2);
            }
        }

        // Finestra minimizzata/nascosta: non disegniamo (evita di bloccarci su
        // present/vsync), ma restiamo vivi per eventi e MIDI. L'audio e'
        // comunque indipendente (thread MIDI -> coda -> thread audio).
        if (minimized) {
            SDL_Delay(10);
            continue;
        }

        // Lavora in coordinate "punti" (logiche) e lascia che SDL le scali ai
        // pixel fisici: cosi' la UI resta corretta anche su schermi HiDPI.
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        if (ww <= 0 || wh <= 0) { SDL_Delay(10); continue; }
        int lw = 0, lh = 0;
        SDL_RenderGetLogicalSize(ren, &lw, &lh);
        if (lw != ww || lh != wh) SDL_RenderSetLogicalSize(ren, ww, wh);
        const float keyboardY = static_cast<float>(wh) - KEYBOARD_H;

        Layout L;
        computeLayout(ww, L);

        tracker.prune(t - (keyboardY / settings.speed + 3.0));

        const float pk     = audio.running() ? audio.peakLevel() : 0.0f;
        const float pkGlow = std::min(1.0f, pk * 1.5f);

        // ----------------------------------------------------- sfondo
        const float nowSec = static_cast<float>(SDL_GetTicks()) * 0.001f;

        // gradiente profondo
        batch.addRectGrad(0, 0, static_cast<float>(ww), static_cast<float>(wh),
                          th::bgTop, th::bgBot);

        // due aloni che respirano lentamente: ambra a sinistra, azzurro a destra
        {
            const float warmX = static_cast<float>(ww) *
                                (0.26f + 0.03f * std::sin(nowSec * 0.21f));
            const float warmY = keyboardY *
                                (0.58f + 0.05f * std::sin(nowSec * 0.17f + 1.3f));
            const float coolX = static_cast<float>(ww) *
                                (0.76f + 0.03f * std::sin(nowSec * 0.19f + 2.4f));
            const float coolY = keyboardY *
                                (0.40f + 0.05f * std::sin(nowSec * 0.15f + 0.6f));
            const float bw = static_cast<float>(ww) * 0.62f;
            const float bh = keyboardY * 0.75f;
            for (int i = 7; i >= 1; --i) {
                const float lt = static_cast<float>(i) / 7.0f;
                const float gw = bw * (0.35f + 0.65f * lt);
                const float gh = bh * (0.35f + 0.65f * lt);
                const Uint8 aw = static_cast<Uint8>(6.0f * (1.0f - lt) * (1.0f - lt) + 1.0f);
                const Uint8 ac = static_cast<Uint8>(7.0f * (1.0f - lt) * (1.0f - lt) + 1.0f);
                batch.addRoundRect(warmX - gw * 0.5f, warmY - gh * 0.5f, gw, gh,
                                   gw * 0.5f, withAlpha(NOTE_LOW, aw));
                batch.addRoundRect(coolX - gw * 0.5f, coolY - gh * 0.5f, gw, gh,
                                   gw * 0.5f, withAlpha(NOTE_HIGH, ac));
            }
        }

        // vignettatura: concentra lo sguardo verso il centro
        batch.addRectGrad(0, 0, static_cast<float>(ww), static_cast<float>(wh) * 0.42f,
                          SDL_Color{0, 0, 0, 70}, SDL_Color{0, 0, 0, 0});
        batch.addRectGrad(0, static_cast<float>(wh) * 0.55f, static_cast<float>(ww),
                          static_cast<float>(wh) * 0.45f,
                          SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 120});
        batch.addRectGrad(0, 0, static_cast<float>(ww) * 0.22f, static_cast<float>(wh),
                          SDL_Color{0, 0, 0, 80}, SDL_Color{0, 0, 0, 0});
        batch.addRectGrad(static_cast<float>(ww) * 0.78f, 0,
                          static_cast<float>(ww) * 0.22f, static_cast<float>(wh),
                          SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 80});

        // guide verticali, quasi invisibili
        for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
            if (isBlackKey(p)) continue;
            const float x = L.x[p] + L.w[p];
            batch.addRect(x, 0, 1.0f, keyboardY, SDL_Color{255, 255, 255, 9});
        }

        // ----------------------------------------------------- note
        for (const NoteBlock& b : tracker.blocks()) {
            if (b.pitch < PIANO_MIN || b.pitch > PIANO_MAX) continue;

            const double t1 = (b.end < 0.0) ? t : b.end;
            float yTop, yBot;

            if (settings.rising) {
                yTop = static_cast<float>(keyboardY - (t - b.start) * settings.speed);
                yBot = static_cast<float>(keyboardY - (t - t1) * settings.speed);
                if (yBot < 0.0f || yTop > keyboardY) continue;
            } else {
                const float h = 12.0f + b.velocity * 30.0f;
                yBot = static_cast<float>((t - b.start) * settings.speed);
                yTop = yBot - h;
                if (yBot < 0.0f || yTop > keyboardY) continue;
            }

            const float top = std::max(0.0f, std::min(yTop, yBot));
            const float bot = std::min(keyboardY, std::max(yTop, yBot));
            if (bot - top < 1.0f) continue;

            Uint8 alpha = 240;
            if (b.end >= 0.0) {
                const float age = static_cast<float>(t - b.end);
                const float f   = std::max(0.0f, 1.0f - age / 1.6f);
                alpha = static_cast<Uint8>(30.0f + 210.0f * f * f);
            }

            const float bx  = L.x[b.pitch];
            const float bw  = std::max(3.0f, L.w[b.pitch] - 1.5f);
            const float rad = std::min(4.0f, bw * 0.5f);
            const float vel = std::max(0.0f, std::min(1.0f, b.velocity));
            const float hgt = bot - top;

            SDL_Color base = noteColor(b.pitch);
            base.a = alpha;
            const SDL_Color bright  = withAlpha(shade(base, 1.30f + 0.45f * vel), alpha);
            const SDL_Color deep    = withAlpha(shade(base, 0.60f), alpha);
            const SDL_Color outline = withAlpha(shade(base, 0.50f), alpha);

            // alone: la nota illumina lo sfondo
            addGlow(batch, bx, top, bw, hgt, rad, base,
                    static_cast<Uint8>(26.0f + 30.0f * vel), 3);

            // corpo con doppio gradiente (piu' chiaro dove arriva il colpo)
            batch.addRoundRect(bx - 1.0f, top - 1.0f, bw + 2.0f, hgt + 2.0f,
                               rad + 1.0f, outline);
            batch.addRoundRectGrad(bx, top, bw, hgt, rad,
                                   settings.rising ? bright : base,
                                   settings.rising ? base : bright);
            if (hgt > 10.0f) {
                const float tailH = std::min(hgt * 0.55f, 90.0f);
                const float tailY = settings.rising ? bot - tailH : top;
                batch.addRoundRectGrad(bx, tailY, bw, tailH, rad,
                                       withAlpha(deep, 0),
                                       withAlpha(shade(base, 0.55f),
                                                 static_cast<Uint8>(alpha * 0.85f)));
            }

            // fronte luminoso + riflesso interno
            const float capY = settings.rising ? top : (bot - 3.5f);
            batch.addRoundRect(bx + 1.0f, capY, std::max(1.0f, bw - 2.0f), 3.5f, 1.75f,
                               withAlpha(shade(base, 1.8f), alpha));
            if (bw > 6.0f)
                batch.addRect(bx + 1.5f, top + 2.0f, 1.2f, std::max(0.0f, hgt - 4.0f),
                              withAlpha(shade(base, 2.0f),
                                        static_cast<Uint8>(alpha * 0.35f)));
        }

        // ----------------------------------------------------- tastiera
        // ombra ampia che stacca la tastiera dal fondo
        batch.addRectGrad(0, keyboardY - 64.0f, static_cast<float>(ww), 64.0f,
                          SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 170});

        // fondo della tastiera: sempre un filo illuminato
        batch.addRectGrad(0, keyboardY, static_cast<float>(ww), KEYBOARD_H,
                          SDL_Color{24, 27, 40, 255}, SDL_Color{8, 9, 14, 255});

        // tasti bianchi
        for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
            if (L.black[p]) continue;
            const float w = std::max(2.0f, L.w[p] - 2.0f);
            const float x = L.x[p] + 1.0f;
            if (tracker.sounding[p]) {
                const SDL_Color c = noteColor(p);
                addGlow(batch, x, keyboardY, w, KEYBOARD_H, 3.0f, c, 40, 2);
                batch.addRoundRectGrad(x, keyboardY, w, KEYBOARD_H, 3.0f,
                                       shade(c, 1.45f), shade(c, 0.80f));
                batch.addRoundRectGrad(x + 1.0f, keyboardY + 1.0f,
                                       std::max(1.0f, w - 2.0f), 26.0f, 2.0f,
                                       SDL_Color{255, 255, 255, 70},
                                       SDL_Color{255, 255, 255, 0});
            } else {
                batch.addRoundRectGrad(x, keyboardY, w, KEYBOARD_H, 3.0f,
                                       SDL_Color{246, 248, 253, 255},
                                       SDL_Color{191, 195, 208, 255});
                batch.addRect(x + 1.0f, keyboardY + 1.0f,
                              std::max(1.0f, w - 2.0f), 2.0f,
                              SDL_Color{255, 255, 255, 235});
                batch.addRectGrad(x + 1.0f, keyboardY + KEYBOARD_H - 18.0f,
                                  std::max(1.0f, w - 2.0f), 17.0f,
                                  SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 34});
                batch.addRect(x + w - 1.0f, keyboardY + 2.0f, 1.0f,
                              KEYBOARD_H - 4.0f, SDL_Color{0, 0, 0, 40});
            }
        }

        // tasti neri
        for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
            if (!L.black[p]) continue;
            const float bh = KEYBOARD_H * 0.62f;
            const float w  = std::max(2.0f, L.w[p]);
            if (tracker.sounding[p]) {
                const SDL_Color c = noteColor(p);
                addGlow(batch, L.x[p], keyboardY, w, bh, 3.0f, c, 50, 2);
                batch.addRoundRectGrad(L.x[p], keyboardY, w, bh, 3.0f,
                                       shade(c, 1.40f), shade(c, 0.65f));
            } else {
                // ombra proiettata sul tasto bianco
                batch.addRect(L.x[p] + 2.0f, keyboardY + bh - 2.0f, w, 8.0f,
                              SDL_Color{0, 0, 0, 34});
                batch.addRoundRectGrad(L.x[p], keyboardY, w, bh, 3.0f,
                                       SDL_Color{46, 50, 64, 255},
                                       SDL_Color{10, 11, 17, 255});
                batch.addRoundRectGrad(L.x[p] + 1.0f, keyboardY + 1.0f,
                                       std::max(1.0f, w - 2.0f),
                                       std::min(26.0f, bh * 0.45f), 2.0f,
                                       SDL_Color{96, 103, 124, 110},
                                       SDL_Color{60, 65, 82, 0});
            }
        }

        // luce di impatto sui tasti premuti
        for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
            if (!tracker.sounding[p]) continue;
            const float w = L.black[p] ? std::max(2.0f, L.w[p])
                                       : std::max(2.0f, L.w[p] - 2.0f);
            const float x = L.black[p] ? L.x[p] : L.x[p] + 1.0f;
            const SDL_Color c = noteColor(p);
            batch.addRoundRect(x, keyboardY, w, 7.0f, 3.0f,
                               withAlpha(shade(c, 1.6f), 230));
            batch.addRoundRectGrad(x, keyboardY - 14.0f, w, 14.0f, 3.0f,
                                   withAlpha(c, 0), withAlpha(c, 90));
        }

        // fascia audio-reattiva sopra la tastiera
        if (pkGlow > 0.01f) {
            batch.addRectGrad(0, keyboardY - 120.0f, static_cast<float>(ww), 120.0f,
                              withAlpha(th::section, 0),
                              withAlpha(th::section,
                                        static_cast<Uint8>(26.0f * pkGlow)));
        }

        // linea di battuta
        batch.addRect(0, keyboardY, static_cast<float>(ww), 1.5f,
                      SDL_Color{255, 255, 255, 60});

        batch.flush(ren);

        // nomi note sui tasti: tutte le note bianche se c'e' spazio, altrimenti
        // solo le ottave (Do). Il Do e' sempre evidenziato.
        if (settings.showNoteNames && L.whiteW > 18.0f) {
            const bool allNames = L.whiteW > 34.0f;
            for (int p = PIANO_MIN; p <= PIANO_MAX; ++p) {
                if (isBlackKey(p)) continue;
                const bool isC = (p % 12 == 0);
                if (!isC && !allNames) continue;
                const std::string nm = noteName(p, settings.notation);
                if (textWidth(nm.c_str(), 1.3f) > L.w[p] - 4.0f) continue;
                const float tw = textWidth(nm.c_str(), 1.3f);
                drawText(ren, L.x[p] + (L.w[p] - tw) * 0.5f,
                         keyboardY + KEYBOARD_H - 22.0f, nm.c_str(),
                         isC ? SDL_Color{150, 90, 30, 255}
                             : SDL_Color{110, 112, 124, 255},
                         1.3f);
            }
        }

        // ----------------------------------------------------- HUD accordo
        if (settings.showChord) {
            ChordInfo ci = analyzeChord(tracker.sounding, settings.notation);
            drawChordHud(ren, batch, ww, ci);
        }
        batch.flush(ren);

        // ----------------------------------------------------- barra info
        const std::string dev = (curPort >= 0 && curPort < static_cast<int>(ports.size()))
                                    ? ports[curPort]
                                    : std::string("nessun MIDI");
        int activeNotes = 0;
        for (int p = 0; p < 128; ++p)
            if (tracker.sounding[p]) ++activeNotes;

        // fascia superiore: sfuma nel fondo
        batch.addRectGrad(0, 0, static_cast<float>(ww), 46.0f,
                          withAlpha(th::bgTop, 235), withAlpha(th::bgTop, 0));

        // chip del dispositivo MIDI
        const SDL_Color devColor = (curPort >= 0) ? SDL_Color{120, 220, 160, 255}
                                                  : SDL_Color{230, 130, 115, 255};
        {
            const float twd = textWidth(dev.c_str(), 1.35f);
            const float cw  = twd + 36.0f;
            batch.addRoundRect(12.0f, 8.0f, cw, 23.0f, 11.5f,
                               SDL_Color{12, 15, 25, 205});
            batch.addRoundRect(12.0f, 8.0f, cw, 23.0f, 11.5f, withAlpha(devColor, 42));
            batch.addRoundRect(21.0f, 16.5f, 6.0f, 6.0f, 3.0f, devColor);
            batch.flush(ren);
            drawText(ren, 33.0f, 12.0f, dev.c_str(),
                     SDL_Color{200, 216, 238, 255}, 1.35f);
        }

        // cluster a destra: FPS | VU | note | HOLD
        float rx = static_cast<float>(ww) - 14.0f;
        char fpsBuf[32];
        std::snprintf(fpsBuf, sizeof(fpsBuf), "%d fps", static_cast<int>(fps + 0.5));
        rx -= textWidth(fpsBuf, 1.2f);
        drawText(ren, rx, 13.0f, fpsBuf, withAlpha(th::hint, 200), 1.2f);
        rx -= 18.0f;

        // VU meter a segmenti
        {
            const int   segs = 22;
            const float sw = 4.0f, gap = 2.0f;
            const float mw = segs * (sw + gap) - gap;
            const float my = 11.0f;
            rx -= mw;
            const float lvl = std::max(0.0f, std::min(1.0f, pk));
            for (int i = 0; i < segs; ++i) {
                const float f = static_cast<float>(i) / (segs - 1);
                SDL_Color c = f > 0.88f ? SDL_Color{245, 110, 95, 255}
                            : f > 0.66f ? th::warn : th::good;
                if (f <= lvl) c = shade(c, 0.85f + 0.35f * f);
                else          c = withAlpha(th::subtle, 30);
                batch.addRoundRect(rx + i * (sw + gap), my, sw, 13.0f, 2.0f, c);
            }
            batch.flush(ren);
            rx -= 18.0f;
        }

        char noteBuf[40];
        std::snprintf(noteBuf, sizeof(noteBuf), "note  %d", activeNotes);
        rx -= textWidth(noteBuf, 1.25f);
        drawText(ren, rx, 13.0f, noteBuf, withAlpha(th::label, 215), 1.25f);
        rx -= 16.0f;

        if (tracker.sustain) {
            const char* hold = "HOLD";
            const float hw = textWidth(hold, 1.2f) + 18.0f;
            rx -= hw;
            batch.addRoundRect(rx, 8.0f, hw, 23.0f, 11.5f, withAlpha(th::warn, 45));
            batch.addRoundRect(rx, 8.0f, hw, 23.0f, 11.5f, withAlpha(th::warn, 85));
            batch.flush(ren);
            drawText(ren, rx + 9.0f, 12.0f, hold, th::warn, 1.2f);
        }
        batch.flush(ren);

        // ----------------------------------------------------- pannello
        if (panelOpen) {
            drawPanel(ren, batch, ww, wh, settings, panelSel, dev, pluginLabel,
                      vst.available(), vst.loaded(), vst.editorOpen(),
                      instrumentLabel, sampler.loaded(), pk);
            batch.flush(ren);
        }

        // --------------------------------------------- editor sintetizzatore
        if (synthOpen) {
            drawSynthEditor(ren, batch, ww, wh, settings.synth, synthSel, synth);
            batch.flush(ren);
        }

        // ----------------------------------------------------- file browser
        if (fb.open) {
            drawBrowser(ren, batch, ww, wh, fb);
            batch.flush(ren);
        }

        // ----------------------------------------------------- aiuto
        if (helpOpen) {
            drawHelp(ren, batch, ww, wh);
            batch.flush(ren);
        }

        // ----------------------------------------------------- toast di stato
        if (!statusMsg.empty() && SDL_GetTicks() < statusUntil) {
            const float tw = textWidth(statusMsg.c_str(), 1.4f);
            const float bw = tw + 40.0f;
            const float bx = (ww - bw) * 0.5f;
            const float by = keyboardY - 122.0f;
            addGlow(batch, bx, by, bw, 30.0f, 9.0f, th::selEdge, 55, 3);
            batch.addRoundRect(bx, by, bw, 30.0f, 9.0f, SDL_Color{18, 22, 34, 240});
            batch.addRoundRect(bx, by, 4.0f, 30.0f, 2.0f, th::selEdge);
            batch.flush(ren);
            drawText(ren, bx + 18.0f, by + 9.0f, statusMsg.c_str(),
                     SDL_Color{230, 236, 248, 255}, 1.4f);
        }

        SDL_RenderPresent(ren);

        // Limita a ~60 fps quando il vsync non sta gia' frenando il loop
        // (es. renderer software), per non consumare CPU inutilmente.
        const int elapsed = static_cast<int>(SDL_GetTicks() - frameStart);
        if (elapsed < 16) SDL_Delay(static_cast<Uint32>(16 - elapsed));
    }

    midi.closePort();        // ferma i callback RtMidi
    midi.setHandler(nullptr);
    panic();
    audio.stop();
    savePluginState();

    // Ripristina le scelte iniettate da riga di comando se l'utente non le ha
    // modificate a sessione in corso: vanno solo per questa esecuzione.
    if (!cliInstrument.empty() && settings.instrumentPath == cliInstrument)
        settings.instrumentPath = fileInstrument;
    if (!cliPlugin.empty() && settings.pluginPath == cliPlugin)
        settings.pluginPath = filePlugin;
    if (cliSource >= 0 && settings.audioSource == cliSource)
        settings.audioSource = fileSource;

    if (!settingsPath.empty()) {
        const Uint32 wf = SDL_GetWindowFlags(win);
        settings.maximized  = (wf & SDL_WINDOW_MAXIMIZED) != 0;
        settings.fullscreen = (wf & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        if (!settings.maximized && !settings.fullscreen) {
            int x = 0, y = 0, w = 0, h = 0;
            SDL_GetWindowPosition(win, &x, &y);
            SDL_GetWindowSize(win, &w, &h);
            settings.winX = x;
            settings.winY = y;
            settings.winW = w;
            settings.winH = h;
        }
        saveSettings(settings, settingsPath);
    }

    vst.unload();

    textShutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
