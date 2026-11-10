#include "sampler.h"

#include "piano.h"
#include "rhodes.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

using Ops = std::unordered_map<std::string, std::string>;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool isAbsolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/') return true;
#if defined(_WIN32)
    if (p.size() > 1 && p[1] == ':') return true;
#endif
    return false;
}

std::string resolvePath(const std::string& baseDir, std::string rel) {
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (isAbsolute(rel)) return rel;
    return (fs::path(baseDir) / rel).lexically_normal().string();
}

double toDouble(const std::string& s, double def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return (end && end != s.c_str()) ? v : def;
}
long toLong(const std::string& s, long def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return (end && end != s.c_str()) ? v : def;
}

std::string getOp(const Ops& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = o.find(k);
        if (it != o.end()) return it->second;
    }
    return {};
}

// Carica un WAV come float mono alla sua frequenza nativa.
bool loadWavMono(const std::string& path, std::vector<float>& out, int& rate,
                 std::string& err) {
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) {
        err = SDL_GetError();
        return false;
    }
    bool ok = false;
    if (SDL_AudioStream* st = SDL_NewAudioStream(spec.format, spec.channels, spec.freq,
                                                 AUDIO_F32SYS, 1, spec.freq)) {
        if (SDL_AudioStreamPut(st, buf, static_cast<int>(len)) == 0 &&
            SDL_AudioStreamFlush(st) == 0) {
            const int avail = SDL_AudioStreamAvailable(st);
            std::vector<float> tmp(static_cast<size_t>(avail) / sizeof(float) + 1);
            const int got = SDL_AudioStreamGet(
                st, tmp.data(), static_cast<int>(tmp.size() * sizeof(float)));
            if (got > 0) {
                tmp.resize(static_cast<size_t>(got) / sizeof(float));
                out = std::move(tmp);
                rate = spec.freq;
                ok = true;
            }
        }
        SDL_FreeAudioStream(st);
    }
    SDL_FreeWAV(buf);
    if (!ok) err = "conversione audio non riuscita";
    return ok;
}

// Estrae gli attributi name="value" da un tag XML.
Ops parseXmlAttrs(const std::string& tag) {
    Ops o;
    size_t i = 0;
    while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i])) &&
           tag[i] != '>' && tag[i] != '/')
        ++i;
    while (i < tag.size()) {
        while (i < tag.size() && (std::isspace(static_cast<unsigned char>(tag[i])) ||
                                  tag[i] == '/'))
            ++i;
        if (i >= tag.size() || tag[i] == '>') break;
        size_t s = i;
        while (i < tag.size() && tag[i] != '=' && !std::isspace(static_cast<unsigned char>(tag[i])) &&
               tag[i] != '>' && tag[i] != '/')
            ++i;
        const std::string name = lower(trim(tag.substr(s, i - s)));
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || tag[i] != '=') { if (name.empty()) ++i; continue; }
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) continue;
        const char q = tag[i++];
        s = i;
        while (i < tag.size() && tag[i] != q) ++i;
        const std::string val = tag.substr(s, i - s);
        if (i < tag.size()) ++i;
        if (!name.empty()) o[name] = val;
    }
    return o;
}

// Coda SPSC lock-free per gli eventi MIDI (UI -> audio).
struct MidiCmd { uint8_t type, d1, d2; };
class CmdQueue {
public:
    void push(const MidiCmd& e) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t n = (h + 1) % kCap;
        if (n == tail_.load(std::memory_order_acquire)) return;
        buf_[h] = e;
        head_.store(n, std::memory_order_release);
    }
    bool pop(MidiCmd& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t];
        tail_.store((t + 1) % kCap, std::memory_order_release);
        return true;
    }
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }
private:
    static const size_t kCap = 8192;
    MidiCmd buf_[kCap];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

} // namespace

// ============================================================================
struct Sampler::Impl {
    struct Sample {
        std::vector<float> data;
        int rate = 48000;
    };
    struct Region {
        int    sample = -1;
        int    loKey = 0, hiKey = 127, root = 60;
        int    loVel = 0, hiVel = 127;
        float  gain = 1.0f;
        float  pan = 0.0f;
        double tuneSemi = 0.0;
        float  attack = 0.001f, decay = 0.0f, sustain = 1.0f, release = 0.30f;
        int    offset = 0;
        int    length = -1;
        int    loopStart = -1, loopEnd = -1;
        bool   loop = false;
    };
    struct Voice {
        const Region* region = nullptr;
        int    pitch = 60;
        double pos = 0.0;
        double end = 0.0;
        double ratio = 1.0;
        int    loopStart = 0, loopEnd = 0;
        bool   loop = false;
        float  env = 0.0f, aInc = 0.0f, dInc = 0.0f, sus = 1.0f, rInc = 0.0f;
        int    stage = 0;   // 0 spento, 1 attacco, 2 decadimento, 3 sostegno, 4 rilascio
        float  panL = 0.7071f, panR = 0.7071f;
        unsigned long long stamp = 0;
    };

    std::vector<Sample> samples;
    std::vector<Region> regions;
    std::unordered_map<std::string, int> sampleByPath;

    std::vector<Voice> voices;
    static const int kMaxVoices = 96;
    unsigned long long stampCounter = 0;

    bool sustainPedal = false;
    bool pendingOff[128] = {};

    CmdQueue queue;

    int   deviceRate = 48000;
    bool  active     = false;   // prepare() chiamato, pronto a renderizzare

    std::string name, path, error;
    bool dspresetFormat = false;
    bool pianoMode = false;   // suona il pianoforte sintetizzato invece dei campioni
    bool rhodesMode = false;  // suona il piano elettrico Rhodes al posto dei campioni

    PianoSynth  piano;
    RhodesPiano rhodes;

    // ------------------------------------------------------------- samples --
    int findOrLoadSample(const std::string& resolved) {
        auto it = sampleByPath.find(resolved);
        if (it != sampleByPath.end()) return it->second;
        Sample s;
        std::string err;
        if (!loadWavMono(resolved, s.data, s.rate, err)) return -1;
        if (s.data.empty()) return -1;
        samples.push_back(std::move(s));
        const int idx = static_cast<int>(samples.size()) - 1;
        sampleByPath[resolved] = idx;
        return idx;
    }

    // ------------------------------------------------------------- regions --
    void addRegion(const Ops& ops, const std::string& baseDir) {
        std::string sp = getOp(ops, {"sample", "path"});
        if (sp.empty()) return;
        const std::string resolved = resolvePath(baseDir, sp);
        const int sidx = findOrLoadSample(resolved);
        if (sidx < 0) return;
        const Sample& smp = samples[sidx];

        Region r;
        r.sample = sidx;

        // altezza / root
        const std::string keyOp = getOp(ops, {"key"});
        std::string pkc = getOp(ops, {"pitch_keycenter"});
        if (pkc.empty()) pkc = getOp(ops, {"rootnote", "root"});
        if (pkc.empty() && !keyOp.empty()) pkc = keyOp;
        r.root = static_cast<int>(toLong(pkc, 60));

        std::string lo = getOp(ops, {"lokey", "lonote"});
        std::string hi = getOp(ops, {"hikey", "hinote"});
        if (lo.empty() && !keyOp.empty()) lo = keyOp;
        if (hi.empty() && !keyOp.empty()) hi = keyOp;
        r.loKey = static_cast<int>(toLong(lo, 0));
        r.hiKey = static_cast<int>(toLong(hi, 127));

        r.loVel = static_cast<int>(toLong(getOp(ops, {"lovel", "lovelocity"}), 0));
        r.hiVel = static_cast<int>(toLong(getOp(ops, {"hivel", "hivelocity"}), 127));

        // accordatura: transpose (semitoni) + tune (centesimi) + tuning (semitoni)
        const double trans = toDouble(getOp(ops, {"transpose"}), 0.0);
        const double tune  = toDouble(getOp(ops, {"tune"}), 0.0);
        const double tuning = toDouble(getOp(ops, {"tuning"}), 0.0);
        r.tuneSemi = trans + tuning + tune / 100.0;

        // volume (dB) / gain (lineare) / pan (-100..100 oppure -1..1)
        const std::string vol = getOp(ops, {"volume"});
        if (!vol.empty()) r.gain *= static_cast<float>(std::pow(10.0, toDouble(vol, 0.0) / 20.0));
        const std::string g = getOp(ops, {"gain"});
        if (!g.empty()) r.gain *= static_cast<float>(toDouble(g, 1.0));
        double pv = toDouble(getOp(ops, {"pan"}), 0.0);
        if (std::fabs(pv) > 1.5) pv /= 100.0;
        r.pan = static_cast<float>(std::max(-1.0, std::min(1.0, pv)));

        // inviluppo ampeg_*
        r.attack  = static_cast<float>(toDouble(
            getOp(ops, {"ampeg_attack", "amp_attack"}), 0.001));
        r.decay   = static_cast<float>(toDouble(
            getOp(ops, {"ampeg_decay", "amp_decay"}), 0.0));
        r.release = static_cast<float>(toDouble(
            getOp(ops, {"ampeg_release", "amp_release"}), 0.30));
        const std::string sus = getOp(ops, {"ampeg_sustain", "amp_sustain"});
        r.sustain = sus.empty() ? 1.0f
                                : static_cast<float>(std::max(0.0, std::min(100.0,
                                      toDouble(sus, 100.0))) / 100.0);

        r.offset = static_cast<int>(toLong(getOp(ops, {"offset", "start"}), 0));
        r.length = static_cast<int>(toLong(getOp(ops, {"length"}), -1));

        // loop: sfz usa loop_start/loop_end, Decent Sampler loopStart/loopEnd
        const std::string loopMode =
            lower(getOp(ops, {"loop_mode", "loopmode"}));
        const int ls = static_cast<int>(toLong(
            getOp(ops, {"loop_start", "loopstart"}), -1));
        const int le = static_cast<int>(toLong(
            getOp(ops, {"loop_end", "loopend"}), -1));
        const bool oneShot = (loopMode == "one_shot" || loopMode == "no_loop" ||
                              loopMode == "none");
        if (!oneShot && ls >= 0 && le > ls) {
            r.loop = true;
            r.loopStart = ls;
            r.loopEnd = std::min(le, static_cast<int>(smp.data.size()));
        }

        regions.push_back(r);
    }

    // ------------------------------------------------------------ parsing ---
    void parseSfzFile(const fs::path& file, Ops& global, int depth) {
        if (depth > 8) return;
        std::ifstream f(file);
        if (!f) return;
        const std::string fileDir = file.parent_path().string();
        std::string defaultPath = fileDir;

        enum { GLOBAL, GROUP, REGION, OTHER } mode = OTHER;
        Ops group, region;
        bool regionHasSample = false;

        auto finalize = [&]() {
            if (regionHasSample) {
                addRegion(region, defaultPath);
                region = group;
                regionHasSample = false;
            }
        };

        std::string line;
        while (std::getline(f, line)) {
            const size_t c = line.find("//");
            if (c != std::string::npos) line = line.substr(0, c);
            line = trim(line);
            if (line.empty()) continue;

            if (line.rfind("#include", 0) == 0) {
                const size_t q1 = line.find('"');
                const size_t q2 = line.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    const std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
                    const fs::path ip = resolvePath(fileDir, inc);
                    Ops childGlobal = global;
                    parseSfzFile(ip, childGlobal, depth + 1);
                }
                continue;
            }

            std::istringstream ss(line);
            std::string tok;
            while (ss >> tok) {
                if (!tok.empty() && tok[0] == '<') {
                    finalize();
                    const std::string sec = lower(trim(tok));
                    if (sec == "<global>") { global.clear(); mode = GLOBAL; }
                    else if (sec == "<group>") { group = global; mode = GROUP; }
                    else if (sec == "<region>") { region = group; regionHasSample = false; mode = REGION; }
                    else mode = OTHER;
                    continue;
                }
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = lower(tok.substr(0, eq));
                const std::string val = tok.substr(eq + 1);
                if (key == "default_path") {
                    defaultPath = resolvePath(fileDir, val);
                    continue;
                }
                if (mode == GLOBAL) global[key] = val;
                else if (mode == GROUP) group[key] = val;
                else if (mode == REGION) {
                    if (key == "sample" && regionHasSample) {
                        addRegion(region, defaultPath);
                        region = group;
                        regionHasSample = false;
                    }
                    region[key] = val;
                    if (key == "sample") regionHasSample = true;
                }
            }
        }
        finalize();
    }

    void parseDspreset(const fs::path& file) {
        std::ifstream f(file);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string xml = ss.str();
        const std::string fileDir = file.parent_path().string();

        Ops group;
        size_t i = 0;
        while (i < xml.size()) {
            const size_t lt = xml.find('<', i);
            if (lt == std::string::npos) break;
            const size_t gt = xml.find('>', lt);
            if (gt == std::string::npos) break;
            const std::string tag = xml.substr(lt + 1, gt - lt - 1);
            i = gt + 1;

            const std::string ltag = lower(tag);
            if (ltag.rfind("group", 0) == 0 && ltag.size() > 5 &&
                !std::isalnum(static_cast<unsigned char>(ltag[5]))) {
                group = parseXmlAttrs(tag);
                // un <group .../> vuoto azzera
                if (tag.find('/') != std::string::npos &&
                    tag.find("sample") == std::string::npos)
                    group.clear();
            } else if (ltag.rfind("sample", 0) == 0 &&
                       (ltag.size() == 6 || !std::isalnum(static_cast<unsigned char>(ltag[6])))) {
                Ops ops = group;
                for (auto& kv : parseXmlAttrs(tag)) ops[kv.first] = kv.second;
                if (ops.count("volume")) {
                    // Decent Sampler usa i dB per il volume
                }
                addRegion(ops, fileDir);
            }
        }
    }

    // ---------------------------------------------------------- playback ----
    void startVoice(int pitch, int vel, const Region& r) {
        int slot = -1;
        for (int i = 0; i < kMaxVoices; ++i)
            if (voices[i].stage == 0) { slot = i; break; }
        if (slot < 0) {
            // furto: preferisci una voce in rilascio con inviluppo minimo
            float best = 1e9f;
            for (int i = 0; i < kMaxVoices; ++i) {
                const Voice& v = voices[i];
                const float score = (v.stage == 4 ? 0.0f : 1.0f) + v.env;
                if (score < best) { best = score; slot = i; }
            }
        }
        const Sample& s = samples[r.sample];
        Voice& v = voices[slot];
        v = Voice{};
        v.region = &r;
        v.pitch = pitch;
        v.stamp = ++stampCounter;
        const double semis = (pitch - r.root) + r.tuneSemi;
        v.ratio = (static_cast<double>(s.rate) / deviceRate) * std::pow(2.0, semis / 12.0);
        v.pos = r.offset;
        v.end = (r.length > 0) ? static_cast<double>(r.offset + r.length)
                               : static_cast<double>(s.data.size());
        v.loop = r.loop;
        v.loopStart = r.loopStart >= 0 ? r.loopStart : 0;
        v.loopEnd = (r.loopEnd > r.loopStart) ? r.loopEnd : static_cast<int>(v.end);
        const float velGain = std::max(0.0f, std::min(1.0f, vel / 127.0f));
        const float g = r.gain * velGain;
        const double a = (r.pan + 1.0) * (M_PI / 4.0);
        v.panL = static_cast<float>(std::cos(a)) * g;
        v.panR = static_cast<float>(std::sin(a)) * g;

        v.sus = std::max(0.0f, std::min(1.0f, r.sustain));
        v.env = 0.0f;
        v.stage = 1;
        v.aInc = (r.attack > 0.0005f)
                     ? 1.0f / (r.attack * static_cast<float>(deviceRate))
                     : 1e9f;
        v.dInc = (r.decay > 0.0005f)
                     ? (1.0f - v.sus) / (r.decay * static_cast<float>(deviceRate))
                     : 1e9f;
    }

    void triggerOff(int pitch) {
        for (Voice& v : voices)
            if (v.pitch == pitch && v.stage != 0 && v.stage != 4) {
                const float rel = v.region->release > 0.01f ? v.region->release : 0.01f;
                v.env = std::max(v.env, 1e-4f);
                v.rInc = v.env / (rel * static_cast<float>(deviceRate));
                v.stage = 4;
            }
    }

    void handleCmd(const MidiCmd& e) {
        const uint8_t type = e.type & 0xF0;
        if (type == 0x90 && e.d2 > 0) {
            const int pitch = e.d1, vel = e.d2;
            pendingOff[pitch] = false;
            // sceglie una regione per ogni intervallo (round-robin automatico)
            std::vector<long long> done;
            for (const Region& r : regions) {
                if (pitch < r.loKey || pitch > r.hiKey || vel < r.loVel || vel > r.hiVel)
                    continue;
                const long long sig = (static_cast<long long>(r.loKey) << 40) |
                                      (static_cast<long long>(r.hiKey) << 24) |
                                      (static_cast<long long>(r.loVel) << 8) |
                                      static_cast<long long>(r.hiVel);
                if (std::find(done.begin(), done.end(), sig) != done.end()) continue;
                done.push_back(sig);
                startVoice(pitch, vel, r);
            }
        } else if (type == 0x80 || (type == 0x90 && e.d2 == 0)) {
            if (sustainPedal) pendingOff[e.d1] = true;
            else triggerOff(e.d1);
        } else if (type == 0xB0 && e.d1 == 64) {
            sustainPedal = e.d2 >= 64;
            if (!sustainPedal)
                for (int p = 0; p < 128; ++p)
                    if (pendingOff[p]) { pendingOff[p] = false; triggerOff(p); }
        } else if (type == 0xB0 && (e.d1 == 120 || e.d1 == 123)) {
            sustainPedal = false;
            for (int p = 0; p < 128; ++p) pendingOff[p] = false;
            for (Voice& v : voices)
                if (v.stage != 0 && v.stage != 4) {
                    v.env = std::max(v.env, 1e-4f);
                    v.rInc = v.env / (0.08f * static_cast<float>(deviceRate));
                    v.stage = 4;
                }
        }
    }

    void routePiano(const MidiCmd& e) {
        const uint8_t type = e.type & 0xF0;
        if (type == 0x90 && e.d2 > 0)        piano.noteOn(e.d1, e.d2);
        else if (type == 0x80 || (type == 0x90 && e.d2 == 0)) piano.noteOff(e.d1);
        else if (type == 0xB0)               piano.controlChange(e.d1, e.d2);
    }

    void routeRhodes(const MidiCmd& e) {
        const uint8_t type = e.type & 0xF0;
        if (type == 0x90 && e.d2 > 0)        rhodes.noteOn(e.d1, e.d2);
        else if (type == 0x80 || (type == 0x90 && e.d2 == 0)) rhodes.noteOff(e.d1);
        else if (type == 0xB0)               rhodes.controlChange(e.d1, e.d2);
    }

    void renderAdd(float* out, int frames) {
        if (!active || frames <= 0) return;

        MidiCmd cmd;
        while (queue.pop(cmd)) {
            if (pianoMode)       routePiano(cmd);
            else if (rhodesMode) routeRhodes(cmd);
            else                 handleCmd(cmd);
        }
        if (pianoMode) { piano.renderAdd(out, frames); return; }
        if (rhodesMode) { rhodes.renderAdd(out, frames); return; }

        for (Voice& v : voices) {
            if (v.stage == 0) continue;
            const Sample& s = samples[v.region->sample];
            const float* data = s.data.data();
            const int n = static_cast<int>(s.data.size());
            for (int fr = 0; fr < frames; ++fr) {
                switch (v.stage) {
                    case 1: v.env += v.aInc; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 2; } break;
                    case 2: v.env -= v.dInc; if (v.env <= v.sus) { v.env = v.sus; v.stage = 3; } break;
                    case 3: v.env = v.sus; break;
                    case 4: v.env -= v.rInc; if (v.env <= 0.0f) { v.env = 0.0f; v.stage = 0; } break;
                    default: break;
                }
                if (v.stage == 0) break;
                if (v.pos >= v.end) {
                    if (v.loop && v.loopEnd > v.loopStart) v.pos -= (v.loopEnd - v.loopStart);
                    else { v.stage = 0; break; }
                }
                const int i0 = static_cast<int>(v.pos);
                if (i0 < 0 || i0 >= n) { v.stage = 0; break; }
                const float frac = static_cast<float>(v.pos - i0);
                // interpolazione cubica (Catmull-Rom) per una riproduzione piu'
                // fedele in fase di trasposizione.
                const float xm1 = (i0 > 0) ? data[i0 - 1] : data[i0];
                const float x0  = data[i0];
                const float x1  = (i0 + 1 < n) ? data[i0 + 1] : x0;
                const float x2  = (i0 + 2 < n) ? data[i0 + 2] : x1;
                const float c1 = 0.5f * (x1 - xm1);
                const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                const float smp = (((c3 * frac + c2) * frac + c1) * frac + x0) * v.env;
                out[fr * 2]     += smp * v.panL;
                out[fr * 2 + 1] += smp * v.panR;
                v.pos += v.ratio;
            }
        }
    }

    void clearState() {
        voices.assign(kMaxVoices, Voice{});
        sustainPedal = false;
        for (bool& b : pendingOff) b = false;
    }
};

// ============================================================================
Sampler::Sampler() : impl_(new Impl) { impl_->clearState(); }
Sampler::~Sampler() {
    delete impl_;
}

bool Sampler::load(const std::string& path) {
    if (path == ":piano")  return loadBuiltinPiano();
    if (path == ":rhodes") return loadBuiltinRhodes();

    impl_->active = false;   // il chiamante deve aver fermato l'AudioEngine
    impl_->pianoMode = false;
    impl_->rhodesMode = false;
    impl_->piano.reset();
    impl_->rhodes.reset();
    impl_->samples.clear();
    impl_->regions.clear();
    impl_->sampleByPath.clear();
    impl_->clearState();
    impl_->error.clear();
    impl_->dspresetFormat = false;

    std::error_code ec;
    fs::path p(path);
    if (!fs::exists(p, ec)) {
        impl_->error = "File non trovato: " + path;
        return false;
    }
    const std::string ext = lower(p.extension().string());
    impl_->name = p.filename().string();
    impl_->path = path;

    if (ext == ".sfz") {
        Ops global;
        impl_->parseSfzFile(p, global, 0);
    } else if (ext == ".dspreset" || ext == ".dslibrary") {
        impl_->dspresetFormat = true;
        impl_->parseDspreset(p);
    } else {
        impl_->error = "Formato non supportato (usa .sfz o .dspreset): " + ext;
        return false;
    }

    if (impl_->regions.empty()) {
        impl_->error = "Nessuna regione/campione valido in " + impl_->name;
        return false;
    }
    return true;
}

bool Sampler::loadBuiltinPiano() {
    impl_->active = false;
    impl_->samples.clear();
    impl_->regions.clear();
    impl_->sampleByPath.clear();
    impl_->clearState();
    impl_->error.clear();
    impl_->dspresetFormat = false;
    impl_->pianoMode = true;
    impl_->rhodesMode = false;
    impl_->name = "Pianoforte (sintetico)";
    impl_->path = ":piano";
    impl_->piano.reset();
    return true;
}

bool Sampler::loadBuiltinRhodes() {
    impl_->active = false;
    impl_->samples.clear();
    impl_->regions.clear();
    impl_->sampleByPath.clear();
    impl_->clearState();
    impl_->error.clear();
    impl_->dspresetFormat = false;
    impl_->pianoMode = false;
    impl_->rhodesMode = true;
    impl_->name = "Rhodes (piano elettrico)";
    impl_->path = ":rhodes";
    impl_->rhodes.reset();
    return true;
}

bool Sampler::isBuiltinPiano() const { return impl_->pianoMode; }
bool Sampler::isBuiltinRhodes() const { return impl_->rhodesMode; }
bool Sampler::isBuiltin() const { return impl_->pianoMode || impl_->rhodesMode; }

void Sampler::unload() {
    impl_->active = false;
    impl_->pianoMode = false;
    impl_->rhodesMode = false;
    impl_->piano.reset();
    impl_->rhodes.reset();
    impl_->samples.clear();
    impl_->regions.clear();
    impl_->sampleByPath.clear();
    impl_->clearState();
    impl_->name.clear();
    impl_->path.clear();
}

bool        Sampler::loaded() const {
    return impl_->pianoMode || impl_->rhodesMode || !impl_->regions.empty();
}
std::string Sampler::instrumentName() const { return impl_->name; }
std::string Sampler::instrumentPath() const { return impl_->path; }
int         Sampler::regionCount() const { return static_cast<int>(impl_->regions.size()); }
int         Sampler::sampleCount() const { return static_cast<int>(impl_->samples.size()); }
std::string Sampler::lastError() const { return impl_->error; }

bool Sampler::prepare(int sampleRate, int blockSize) {
    (void)blockSize;
    if (sampleRate > 0) impl_->deviceRate = sampleRate;
    impl_->queue.clear();   // niente eventi in sospeso dall'ultimo stop
    impl_->clearState();
    impl_->piano.prepare(impl_->deviceRate);
    impl_->rhodes.prepare(impl_->deviceRate);
    impl_->active = true;
    return true;
}

void Sampler::releaseProcessing() {
    impl_->clearState();
    impl_->piano.reset();
    impl_->rhodes.reset();
    impl_->active = false;
}

bool Sampler::prepared() const { return impl_->active; }

void Sampler::renderAdd(float* out, int frames) { impl_->renderAdd(out, frames); }

void Sampler::noteOn(int pitch, int velocity) {
    impl_->queue.push(MidiCmd{0x90, static_cast<uint8_t>(pitch),
                              static_cast<uint8_t>(velocity)});
}
void Sampler::noteOff(int pitch) {
    impl_->queue.push(MidiCmd{0x80, static_cast<uint8_t>(pitch), 0});
}
void Sampler::controlChange(int cc, int value) {
    impl_->queue.push(MidiCmd{0xB0, static_cast<uint8_t>(cc),
                              static_cast<uint8_t>(value)});
}
void Sampler::allNotesOff() {
    impl_->queue.push(MidiCmd{0xB0, 123, 0});
}
