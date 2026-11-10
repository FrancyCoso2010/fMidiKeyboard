#include "synth.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kTableSize  = 4096;
constexpr int kMaxVoices  = 32;
constexpr int kMaxUnison  = 4;
constexpr int kScopeSize  = 512;

// Tabella seno condivisa, costruita una sola volta (thread-safe in C++11).
const float* sinTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(kTableSize + 1);
        for (int i = 0; i <= kTableSize; ++i)
            t[i] = static_cast<float>(std::sin(2.0 * M_PI * i / kTableSize));
        return t;
    }();
    return table.data();
}

inline float fastSin(double phase) {
    double p = phase - std::floor(phase);
    const double x = p * kTableSize;
    const int i = static_cast<int>(x);
    const float f = static_cast<float>(x - i);
    const float* t = sinTable();
    const int idx = i & (kTableSize - 1);
    return t[idx] + (t[idx + 1] - t[idx]) * f;
}

// Tangente iperbolica razionale: niente std::tanh nel percorso caldo.
inline uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline float noiseBipolar(uint32_t& s) {
    return static_cast<float>(static_cast<int32_t>(xorshift(s))) *
           (1.0f / 2147483648.0f);
}

inline float fastTanh(float x) {
    x = std::max(-3.0f, std::min(3.0f, x));
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Correzione PolyBLEP per ridurre l'aliasing di sega/quadra/impulso.
inline double polyBlep(double t, double dt) {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

inline float oscillator(int wave, double t, double dt) {
    switch (wave) {
        case SYNTH_WAVE_SINE:
            return fastSin(t);
        case SYNTH_WAVE_SAW:
            return static_cast<float>(2.0 * t - 1.0 - polyBlep(t, dt));
        case SYNTH_WAVE_SQUARE: {
            float v = t < 0.5 ? 1.0f : -1.0f;
            v += static_cast<float>(polyBlep(t, dt));
            double t2 = t + 0.5;
            if (t2 >= 1.0) t2 -= 1.0;
            v -= static_cast<float>(polyBlep(t2, dt));
            return v;
        }
        case SYNTH_WAVE_TRIANGLE:
            return static_cast<float>(1.0 - 4.0 * std::fabs(t - 0.5));
        case SYNTH_WAVE_PULSE: {
            float v = t < 0.25 ? 1.0f : -1.0f;
            v += static_cast<float>(polyBlep(t, dt));
            double t2 = t + 0.75;
            if (t2 >= 1.0) t2 -= 1.0;
            v -= static_cast<float>(polyBlep(t2, dt));
            return v;
        }
        default:
            return fastSin(t);
    }
}

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
    static const size_t kCap = 4096;
    MidiCmd buf_[kCap];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

} // namespace

// ============================================================================

struct Synth::Impl {
    // ---------------------------------------------------------- parametri ----
    std::atomic<int>   wave{SYNTH_WAVE_SAW};
    std::atomic<int>   octave{0};
    std::atomic<int>   unison{1};
    std::atomic<float> detune{0.0f};
    std::atomic<float> sub{0.0f};
    std::atomic<float> cutoff{6000.0f};
    std::atomic<float> resonance{0.15f};
    std::atomic<float> attack{0.005f};
    std::atomic<float> decay{0.30f};
    std::atomic<float> sustain{0.80f};
    std::atomic<float> release{0.30f};
    std::atomic<float> lfoRate{5.0f};
    std::atomic<float> lfoDepth{0.0f};
    std::atomic<float> drive{1.0f};
    std::atomic<float> volume{0.65f};
    std::atomic<float> filterEnv{0.20f};
    std::atomic<float> reverb{0.10f};
    std::atomic<float> modWheel{0.0f};

    // ------------------------------------------------------------- stato -----
    int  rate      = 48000;
    bool active    = false;
    int  chunkCap  = 512;

    std::vector<float> scratch;

    CmdQueue queue;
    bool     sustainPedal = false;
    bool     pendingOff[128] = {};
    unsigned long long stampCounter = 0;

    float lfoPhase = 0.0f;

    float scope[kScopeSize] = {};
    std::atomic<int> scopeWrite{0};
    std::atomic<int> voiceCount{0};

    // ------------------------------------------------------------- riverbero --
    // Rete compatta stile Freeverb: 4 comb filtrati + 2 allpass per canale.
    // Il send e' mono, l'uscita e' stereo con lunghezze leggermente diverse.
    static constexpr int kCombCount = 4;
    static constexpr int kAllpassCount = 2;

    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float dampStore = 0.0f;
        void init(int n) {
            buf.assign(static_cast<size_t>(std::max(8, n)), 0.0f);
            idx = 0;
            dampStore = 0.0f;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            idx = 0;
            dampStore = 0.0f;
        }
        float process(float in) {
            const float out = buf[idx];
            dampStore = out * 0.76f + dampStore * 0.24f;
            buf[idx] = in + dampStore * 0.84f;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        void init(int n) {
            buf.assign(static_cast<size_t>(std::max(8, n)), 0.0f);
            idx = 0;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            idx = 0;
        }
        float process(float in) {
            const float b = buf[idx];
            buf[idx] = in + b * 0.5f;
            if (++idx >= buf.size()) idx = 0;
            return b - in;
        }
    };

    Comb    combL[kCombCount], combR[kCombCount];
    Allpass allpassL[kAllpassCount], allpassR[kAllpassCount];

    bool reverbReady() const { return !combL[0].buf.empty(); }

    void initReverb(int sampleRate) {
        const double k = static_cast<double>(sampleRate) / 44100.0;
        auto scaled = [k](int n) { return static_cast<int>(std::round(n * k)); };
        const int combTune[kCombCount] = {1116, 1188, 1277, 1356};
        const int apTune[kAllpassCount] = {556, 441};
        for (int i = 0; i < kCombCount; ++i) {
            combL[i].init(scaled(combTune[i]));
            combR[i].init(scaled(combTune[i] + 23));
        }
        for (int i = 0; i < kAllpassCount; ++i) {
            allpassL[i].init(scaled(apTune[i]));
            allpassR[i].init(scaled(apTune[i] + 23));
        }
    }

    void clearReverb() {
        for (int i = 0; i < kCombCount; ++i) { combL[i].clear(); combR[i].clear(); }
        for (int i = 0; i < kAllpassCount; ++i) { allpassL[i].clear(); allpassR[i].clear(); }
    }

    struct Voice {
        int   stage = 0;      // 0 libera, 1 attacco, 2 decadimento, 3 sostegno, 4 rilascio
        bool  held = false;
        bool  sustained = false;
        int   pitch = 60;
        float velocity = 0.7f;

        int    unison = 1;
        double phase[kMaxUnison] = {};
        double inc[kMaxUnison]   = {};
        float  panL[kMaxUnison]  = {};
        float  panR[kMaxUnison]  = {};

        double subPhase = 0.0;
        double subInc   = 0.0;
        uint32_t noiseState[kMaxUnison] = {};

        float env        = 0.0f;
        float attackInc  = 0.0f;
        float decayCoef  = 0.0f;
        float sustainLvl = 0.8f;
        float releaseCoef = 0.0f;

        float keyCutoff = 1.0f;

        // stato del filtro SVF (canale sinistro/destro)
        float ic1L = 0.0f, ic2L = 0.0f;
        float ic1R = 0.0f, ic2R = 0.0f;

        unsigned long long stamp = 0;
    };

    Voice voices[kMaxVoices];

    // ------------------------------------------------------------- utility ---
    Voice* allocVoice() {
        for (int i = 0; i < kMaxVoices; ++i)
            if (voices[i].stage == 0) return &voices[i];

        // Furto: prima le voci in rilascio, poi quella con inviluppo piu' basso.
        int best = 0;
        float bestScore = 1e9f;
        for (int i = 0; i < kMaxVoices; ++i) {
            const Voice& v = voices[i];
            const float score = (v.stage == 4 ? 0.0f : 1.0f) + v.env;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return &voices[best];
    }

    static float attackIncFor(float seconds, int rate) {
        return 1.0f / (std::max(0.001f, seconds) * static_cast<float>(rate));
    }
    static float decayCoefFor(float seconds, int rate) {
        return 1.0f - std::exp(-4.6f / (std::max(0.005f, seconds) * static_cast<float>(rate)));
    }
    static float releaseCoefFor(float seconds, int rate) {
        return std::exp(-4.6f / (std::max(0.010f, seconds) * static_cast<float>(rate)));
    }

    void beginRelease(Voice& v) {
        if (v.stage == 0) return;
        v.stage = 4;
        v.held = false;
        v.sustained = false;
        v.releaseCoef = releaseCoefFor(release.load(std::memory_order_relaxed), rate);
    }

    // -------------------------------------------------------------- eventi ---
    void noteOn(int pitch, int velocity) {
        if (pitch < 0 || pitch > 127) return;
        if (velocity <= 0) { noteOff(pitch); return; }

        const float velN = clampf(velocity / 127.0f, 0.0f, 1.0f);

        // Ritrigger della stessa nota: la voce precedente viene rilasciata (o
        // tenuta dal pedale) invece di restare appesa per sempre.
        for (Voice& old : voices) {
            if (old.stage == 0 || old.stage == 4) continue;
            if (old.pitch != pitch || !old.held) continue;
            old.held = false;
            if (sustainPedal) old.sustained = true;
            else beginRelease(old);
        }

        Voice* vp = allocVoice();
        Voice& v = *vp;
        v = Voice{};
        v.stage = 1;
        v.held = true;
        v.pitch = pitch;
        v.velocity = velN;
        v.stamp = ++stampCounter;

        const int   oct     = std::max(-2, std::min(2, octave.load(std::memory_order_relaxed)));
        const int   oscs    = std::max(1, std::min(kMaxUnison, unison.load(std::memory_order_relaxed)));
        const float spread  = clampf(detune.load(std::memory_order_relaxed), 0.0f, 50.0f);
        const double f0     = 440.0 * std::pow(2.0, (pitch + oct * 12 - 69) / 12.0);

        v.unison = oscs;
        const float amp = 0.16f * velN / std::sqrt(static_cast<float>(oscs));

        for (int i = 0; i < oscs; ++i) {
            const double pos = (oscs == 1)
                                   ? 0.0
                                   : (2.0 * static_cast<double>(i) /
                                          static_cast<double>(oscs - 1) - 1.0);
            const double cents = pos * static_cast<double>(spread);
            const double f = f0 * std::pow(2.0, cents / 1200.0);
            v.inc[i] = f / static_cast<double>(rate);
            v.phase[i] = 0.0;
            const double panPos = pos * 0.75;
            const double ang = (panPos + 1.0) * (M_PI / 4.0);
            v.panL[i] = static_cast<float>(std::cos(ang)) * amp;
            v.panR[i] = static_cast<float>(std::sin(ang)) * amp;
            v.noiseState[i] = 0x9e3779b9u ^
                              (static_cast<uint32_t>(pitch) * 2654435761u) ^
                              (static_cast<uint32_t>(v.stamp) * 40503u) ^
                              (static_cast<uint32_t>(i) * 2246822519u);
            if (v.noiseState[i] == 0) v.noiseState[i] = 1;
        }

        v.subInc = f0 * 0.5 / static_cast<double>(rate);
        v.subPhase = 0.0;

        // 50% di key-tracking: le note acute restano brillanti, le basse corpose.
        v.keyCutoff = static_cast<float>(std::pow(2.0, (pitch - 60) / 24.0));

        v.env = 0.0f;
        v.attackInc = attackIncFor(attack.load(std::memory_order_relaxed), rate);
        v.decayCoef = decayCoefFor(decay.load(std::memory_order_relaxed), rate);
        v.sustainLvl = clampf(sustain.load(std::memory_order_relaxed), 0.0f, 1.0f);
        v.releaseCoef = releaseCoefFor(release.load(std::memory_order_relaxed), rate);
    }

    void noteOff(int pitch) {
        for (Voice& v : voices) {
            if (v.stage == 0 || v.stage == 4) continue;
            if (v.pitch != pitch || !v.held) continue;
            v.held = false;
            if (sustainPedal) v.sustained = true;
            else beginRelease(v);
        }
    }

    void allOff() {
        sustainPedal = false;
        for (bool& b : pendingOff) b = false;
        for (Voice& v : voices)
            if (v.stage != 0) beginRelease(v);
    }

    void controlChange(int cc, int value) {
        if (cc == 64) {
            const bool on = value >= 64;
            if (sustainPedal && !on) {
                for (Voice& v : voices)
                    if (v.stage != 0 && v.sustained) beginRelease(v);
            }
            sustainPedal = on;
        } else if (cc == 1) {
            modWheel.store(clampf(value / 127.0f, 0.0f, 1.0f), std::memory_order_relaxed);
        } else if (cc == 120 || cc == 123) {
            allOff();
        }
    }

    void handleCmd(const MidiCmd& e) {
        const int type = e.type & 0xF0;
        if (type == 0x90 && e.d2 > 0) {
            pendingOff[e.d1] = false;
            noteOn(e.d1, e.d2);
        } else if (type == 0x80 || (type == 0x90 && e.d2 == 0)) {
            if (sustainPedal) pendingOff[e.d1] = true;
            else noteOff(e.d1);
        } else if (type == 0xB0 && e.d1 == 64) {
            const bool on = e.d2 >= 64;
            controlChange(64, e.d2); // aggiorna il pedale *prima* dei rilasci
            if (!on)
                for (int p = 0; p < 128; ++p)
                    if (pendingOff[p]) { pendingOff[p] = false; noteOff(p); }
        } else if (type == 0xB0) {
            controlChange(e.d1, e.d2);
        }
    }

    // ------------------------------------------------------------- render ----
    void renderChunk(float* out, int frames) {
        std::memset(scratch.data(), 0, static_cast<size_t>(frames) * 2 * sizeof(float));

        const int   wv       = std::max(0, std::min(SYNTH_WAVE_COUNT - 1,
                                      wave.load(std::memory_order_relaxed)));
        const float cut      = clampf(cutoff.load(std::memory_order_relaxed), 30.0f, 18000.0f);
        const float res      = clampf(resonance.load(std::memory_order_relaxed), 0.0f, 0.95f);
        const float subLvl   = clampf(sub.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float atk      = attack.load(std::memory_order_relaxed);
        const float dec      = decay.load(std::memory_order_relaxed);
        const float sus      = clampf(sustain.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float rel      = release.load(std::memory_order_relaxed);
        const float lfoHz    = clampf(lfoRate.load(std::memory_order_relaxed), 0.02f, 20.0f);
        const float depth    = clampf(lfoDepth.load(std::memory_order_relaxed), 0.0f, 100.0f) +
                               modWheel.load(std::memory_order_relaxed) * 25.0f;
        const float drv      = clampf(drive.load(std::memory_order_relaxed), 1.0f, 4.0f);
        const float vol      = clampf(volume.load(std::memory_order_relaxed), 0.0f, 1.5f);
        const float fenv     = clampf(filterEnv.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float rev      = clampf(reverb.load(std::memory_order_relaxed), 0.0f, 1.0f);

        const float vibAmt = depth * 0.00057762265f; // ln(2)/1200: cent -> fattore
        const float lfoInc = lfoHz / static_cast<float>(rate);
        const float nyqLimit = 0.45f * static_cast<float>(rate);

        const float atkInc  = attackIncFor(atk, rate);
        const float decCoef = decayCoefFor(dec, rate);
        const float relCoef = releaseCoefFor(rel, rate);

        for (Voice& v : voices) {
            if (v.stage == 0) continue;

            // I parametri d'inviluppo si aggiornano anche sulle note gia' attive.
            v.attackInc   = atkInc;
            v.decayCoef   = decCoef;
            v.sustainLvl  = sus;
            v.releaseCoef = relCoef;

            // Filtro: inviluppo dedicato + key tracking + velocita'. Le note
            // suonate piu' forte aprono di piu' il filtro, come su un vero synth.
            const float envForCut = 0.28f + 0.72f * v.env;
            const float envOpen   = 1.0f + fenv * v.env * v.env * 5.5f;
            const float velOpen   = 0.62f + 0.55f * v.velocity;
            const float fc = std::max(30.0f, std::min(nyqLimit,
                                  cut * v.keyCutoff * envForCut * envOpen * velOpen));
            const float g  = std::tan(static_cast<float>(M_PI) * fc / static_cast<float>(rate));
            const float q  = 0.5f + res * 9.5f;
            const float k  = 1.0f / q;
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;

            float lfo = lfoPhase;
            for (int fr = 0; fr < frames; ++fr) {
                switch (v.stage) {
                    case 1:
                        v.env += v.attackInc;
                        if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 2; }
                        break;
                    case 2:
                        v.env += (v.sustainLvl - v.env) * v.decayCoef;
                        if (std::fabs(v.env - v.sustainLvl) < 0.0008f) {
                            v.env = v.sustainLvl;
                            v.stage = 3;
                        }
                        break;
                    case 3:
                        v.env = v.sustainLvl;
                        break;
                    case 4:
                        v.env *= v.releaseCoef;
                        if (v.env < 1.0e-4f) { v.env = 0.0f; v.stage = 0; }
                        break;
                    default:
                        break;
                }
                if (v.stage == 0) break;

                const float lfoVal = fastSin(lfo);
                lfo += lfoInc;
                if (lfo >= 1.0f) lfo -= 1.0f;
                const float pitchMod = 1.0f + vibAmt * lfoVal;

                float sl = 0.0f, sr = 0.0f;
                for (int i = 0; i < v.unison; ++i) {
                    const double dt = v.inc[i] * pitchMod;
                    const float s = (wv == SYNTH_WAVE_NOISE)
                                        ? noiseBipolar(v.noiseState[i])
                                        : oscillator(wv, v.phase[i], dt);
                    sl += s * v.panL[i];
                    sr += s * v.panR[i];
                    double ph = v.phase[i] + dt;
                    if (ph >= 1.0) ph -= 1.0;
                    v.phase[i] = ph;
                }

                if (subLvl > 0.0001f) {
                    const float s = fastSin(v.subPhase) * subLvl * 0.30f * v.velocity;
                    sl += s;
                    sr += s;
                    v.subPhase += v.subInc * pitchMod;
                    if (v.subPhase >= 1.0) v.subPhase -= 1.0;
                }

                // Filtro passa-basso risonante (TPT SVF): stabile fino a Nyquist.
                float v3 = sl - v.ic2L;
                float v1 = a1 * v.ic1L + a2 * v3;
                float v2 = v.ic2L + a2 * v.ic1L + a3 * v3;
                v.ic1L = 2.0f * v1 - v.ic1L;
                v.ic2L = 2.0f * v2 - v.ic2L;
                sl = v2;

                v3 = sr - v.ic2R;
                v1 = a1 * v.ic1R + a2 * v3;
                v2 = v.ic2R + a2 * v.ic1R + a3 * v3;
                v.ic1R = 2.0f * v1 - v.ic1R;
                v.ic2R = 2.0f * v2 - v.ic2R;
                sr = v2;

                scratch[static_cast<size_t>(fr) * 2]     += sl * v.env;
                scratch[static_cast<size_t>(fr) * 2 + 1] += sr * v.env;
            }

            // Inviluppo esaurito: libera la voce.
            if (v.stage == 4 && v.env <= 0.0f) v = Voice{};
            else if (v.stage == 2 && v.sustainLvl < 1.0e-4f && v.env < 1.0e-4f) v = Voice{};
        }

        lfoPhase += lfoInc * static_cast<float>(frames);
        lfoPhase -= std::floor(lfoPhase);

        if (rev > 0.0005f && reverbReady()) {
            for (int fr = 0; fr < frames; ++fr) {
                const float send = (scratch[static_cast<size_t>(fr) * 2] +
                                    scratch[static_cast<size_t>(fr) * 2 + 1]) * 0.5f;
                float wl = 0.0f, wr = 0.0f;
                for (int i = 0; i < kCombCount; ++i) {
                    wl += combL[i].process(send);
                    wr += combR[i].process(send);
                }
                for (int i = 0; i < kAllpassCount; ++i) {
                    wl = allpassL[i].process(wl);
                    wr = allpassR[i].process(wr);
                }
                scratch[static_cast<size_t>(fr) * 2]     += wl * rev * 0.36f;
                scratch[static_cast<size_t>(fr) * 2 + 1] += wr * rev * 0.36f;
            }
        }

        const float norm = 1.0f / drv;
        for (int i = 0; i < frames * 2; ++i)
            scratch[static_cast<size_t>(i)] = fastTanh(scratch[static_cast<size_t>(i)] * drv) *
                                              norm * vol;

        // Copia nell'uscita + alimenta l'oscilloscopio.
        int w = scopeWrite.load(std::memory_order_relaxed);
        for (int fr = 0; fr < frames; ++fr) {
            const float l = scratch[static_cast<size_t>(fr) * 2];
            const float r = scratch[static_cast<size_t>(fr) * 2 + 1];
            out[static_cast<size_t>(fr) * 2]     += l;
            out[static_cast<size_t>(fr) * 2 + 1] += r;
            scope[w] = (l + r) * 0.5f;
            w = (w + 1) & (kScopeSize - 1);
        }
        scopeWrite.store(w, std::memory_order_release);
    }

    void renderAdd(float* out, int frames) {
        if (!active || frames <= 0) return;

        MidiCmd e;
        while (queue.pop(e)) handleCmd(e);

        int activeCount = 0;
        for (const Voice& v : voices)
            if (v.stage != 0) ++activeCount;
        voiceCount.store(activeCount, std::memory_order_relaxed);
        if (activeCount == 0 && reverb.load(std::memory_order_relaxed) <= 0.0005f)
            return;

        if (chunkCap < 1) chunkCap = 1;
        while (frames > 0) {
            const int n = std::min(frames, chunkCap);
            renderChunk(out, n);
            out += static_cast<size_t>(n) * 2;
            frames -= n;
        }
    }

    void reset() {
        for (Voice& v : voices) v = Voice{};
        sustainPedal = false;
        for (bool& b : pendingOff) b = false;
        lfoPhase = 0.0f;
        queue.clear();
        clearReverb();
        voiceCount.store(0, std::memory_order_relaxed);
    }
};

// ============================================================================

Synth::Synth() : impl_(new Impl) {}
Synth::~Synth() { delete impl_; }

bool Synth::prepare(int sampleRate, int blockSize) {
    if (sampleRate > 0) impl_->rate = sampleRate;
    impl_->chunkCap = std::max(1, std::min(blockSize > 0 ? blockSize : 512, 4096));
    impl_->scratch.assign(static_cast<size_t>(impl_->chunkCap) * 2, 0.0f);
    impl_->initReverb(impl_->rate);
    impl_->reset();
    impl_->active = true;
    return true;
}

void Synth::releaseProcessing() {
    impl_->reset();
    impl_->active = false;
}

bool Synth::prepared() const { return impl_->active; }

void Synth::renderAdd(float* out, int frames) { impl_->renderAdd(out, frames); }

void Synth::noteOn(int pitch, int velocity) {
    impl_->queue.push(MidiCmd{0x90, static_cast<uint8_t>(pitch),
                              static_cast<uint8_t>(velocity)});
}
void Synth::noteOff(int pitch) {
    impl_->queue.push(MidiCmd{0x80, static_cast<uint8_t>(pitch), 0});
}
void Synth::controlChange(int cc, int value) {
    impl_->queue.push(MidiCmd{0xB0, static_cast<uint8_t>(cc),
                              static_cast<uint8_t>(value)});
}
void Synth::allNotesOff() {
    impl_->queue.push(MidiCmd{0xB0, 123, 0});
}

void Synth::applySettings(const SynthSettings& s) {
    impl_->wave.store(std::max(0, std::min(SYNTH_WAVE_COUNT - 1, s.wave)),
                      std::memory_order_relaxed);
    impl_->octave.store(std::max(-2, std::min(2, s.octave)), std::memory_order_relaxed);
    impl_->unison.store(std::max(1, std::min(kMaxUnison, s.unison)),
                        std::memory_order_relaxed);
    impl_->detune.store(clampf(s.detune, 0.0f, 50.0f), std::memory_order_relaxed);
    impl_->sub.store(clampf(s.sub, 0.0f, 1.0f), std::memory_order_relaxed);
    impl_->cutoff.store(clampf(s.cutoff, 30.0f, 18000.0f), std::memory_order_relaxed);
    impl_->resonance.store(clampf(s.resonance, 0.0f, 0.95f), std::memory_order_relaxed);
    impl_->attack.store(clampf(s.attack, 0.001f, 4.0f), std::memory_order_relaxed);
    impl_->decay.store(clampf(s.decay, 0.005f, 4.0f), std::memory_order_relaxed);
    impl_->sustain.store(clampf(s.sustain, 0.0f, 1.0f), std::memory_order_relaxed);
    impl_->release.store(clampf(s.release, 0.010f, 8.0f), std::memory_order_relaxed);
    impl_->lfoRate.store(clampf(s.lfoRate, 0.02f, 20.0f), std::memory_order_relaxed);
    impl_->lfoDepth.store(clampf(s.lfoDepth, 0.0f, 100.0f), std::memory_order_relaxed);
    impl_->drive.store(clampf(s.drive, 1.0f, 4.0f), std::memory_order_relaxed);
    impl_->volume.store(clampf(s.volume, 0.0f, 1.5f), std::memory_order_relaxed);
    impl_->filterEnv.store(clampf(s.filterEnv, 0.0f, 1.0f), std::memory_order_relaxed);
    impl_->reverb.store(clampf(s.reverb, 0.0f, 1.0f), std::memory_order_relaxed);
}

SynthSettings Synth::settings() const {
    SynthSettings s;
    s.wave      = impl_->wave.load(std::memory_order_relaxed);
    s.octave    = impl_->octave.load(std::memory_order_relaxed);
    s.unison    = impl_->unison.load(std::memory_order_relaxed);
    s.detune    = impl_->detune.load(std::memory_order_relaxed);
    s.sub       = impl_->sub.load(std::memory_order_relaxed);
    s.cutoff    = impl_->cutoff.load(std::memory_order_relaxed);
    s.resonance = impl_->resonance.load(std::memory_order_relaxed);
    s.attack    = impl_->attack.load(std::memory_order_relaxed);
    s.decay     = impl_->decay.load(std::memory_order_relaxed);
    s.sustain   = impl_->sustain.load(std::memory_order_relaxed);
    s.release   = impl_->release.load(std::memory_order_relaxed);
    s.lfoRate   = impl_->lfoRate.load(std::memory_order_relaxed);
    s.lfoDepth  = impl_->lfoDepth.load(std::memory_order_relaxed);
    s.drive     = impl_->drive.load(std::memory_order_relaxed);
    s.volume    = impl_->volume.load(std::memory_order_relaxed);
    s.filterEnv = impl_->filterEnv.load(std::memory_order_relaxed);
    s.reverb    = impl_->reverb.load(std::memory_order_relaxed);
    return s;
}

void Synth::copyScope(float* dst, int n) const {
    if (!dst || n <= 0) return;
    n = std::min(n, kScopeSize);
    const int w = impl_->scopeWrite.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        int idx = (w - n + i) % kScopeSize;
        if (idx < 0) idx += kScopeSize;
        dst[i] = impl_->scope[idx];
    }
}

int Synth::activeVoices() const {
    return impl_->voiceCount.load(std::memory_order_relaxed);
}

// ============================================================================
// Preset di fabbrica: suoni pronti, tutti modificabili dai parametri.

const char* synthWaveName(int wave) {
    switch (wave) {
        case SYNTH_WAVE_SINE:     return "Seno";
        case SYNTH_WAVE_SAW:      return "Dente di sega";
        case SYNTH_WAVE_SQUARE:   return "Onda quadra";
        case SYNTH_WAVE_TRIANGLE: return "Triangolo";
        case SYNTH_WAVE_PULSE:    return "Impulso 25%";
        case SYNTH_WAVE_NOISE:    return "Rumore";
        default:                  return "?";
    }
}

namespace {

SynthSettings makePreset(int id, int wave, int octave, int unison, float detune,
                         float sub, float cutoff, float res, float a, float d,
                         float s, float r, float lfoHz, float lfoDepth, float drive,
                         float vol, float filterEnv, float reverb) {
    SynthSettings p;
    p.preset = id;
    p.wave = wave;
    p.octave = octave;
    p.unison = unison;
    p.detune = detune;
    p.sub = sub;
    p.cutoff = cutoff;
    p.resonance = res;
    p.attack = a;
    p.decay = d;
    p.sustain = s;
    p.release = r;
    p.lfoRate = lfoHz;
    p.lfoDepth = lfoDepth;
    p.drive = drive;
    p.volume = vol;
    p.filterEnv = filterEnv;
    p.reverb = reverb;
    return p;
}

const SynthSettings kPresets[] = {
    makePreset(0,  SYNTH_WAVE_SAW,      0, 1,  0.0f, 0.00f,  6000.0f, 0.15f,
               0.005f, 0.30f, 0.80f, 0.30f, 5.0f,  0.0f, 1.0f, 0.62f, 0.20f, 0.08f), // Iniziale
    makePreset(1,  SYNTH_WAVE_SAW,      0, 3,  9.0f, 0.15f,  7000.0f, 0.25f,
               0.005f, 0.18f, 0.80f, 0.22f, 5.5f,  8.0f, 1.3f, 0.58f, 0.35f, 0.15f), // Lead
    makePreset(2,  SYNTH_WAVE_SQUARE,  -1, 2,  5.0f, 0.45f,   900.0f, 0.35f,
               0.003f, 0.22f, 0.55f, 0.12f, 5.0f,  0.0f, 2.0f, 0.72f, 0.45f, 0.03f), // Basso
    makePreset(3,  SYNTH_WAVE_SAW,      0, 4, 22.0f, 0.20f,  3000.0f, 0.12f,
               0.800f, 1.20f, 0.85f, 1.60f, 4.2f, 10.0f, 1.0f, 0.55f, 0.12f, 0.38f), // Pad
    makePreset(4,  SYNTH_WAVE_TRIANGLE, 0, 2,  8.0f, 0.00f,  4500.0f, 0.45f,
               0.001f, 0.15f, 0.00f, 0.18f, 5.0f,  0.0f, 1.2f, 0.72f, 0.55f, 0.10f), // Pluck
    makePreset(5,  SYNTH_WAVE_SQUARE,   0, 2,  3.0f, 0.50f,  5000.0f, 0.10f,
               0.020f, 0.10f, 1.00f, 0.15f, 6.0f,  0.0f, 1.0f, 0.58f, 0.05f, 0.12f), // Organo
    makePreset(6,  SYNTH_WAVE_SAW,      0, 4, 35.0f, 0.10f,  8000.0f, 0.20f,
               0.020f, 0.40f, 0.90f, 0.50f, 5.0f, 12.0f, 1.1f, 0.52f, 0.30f, 0.25f), // Supersaw
    makePreset(7,  SYNTH_WAVE_TRIANGLE, 0, 2,  3.0f, 0.00f,  5200.0f, 0.18f,
               0.002f, 0.45f, 0.25f, 0.28f, 4.5f,  0.0f, 1.1f, 0.65f, 0.18f, 0.16f), // Piano elettrico
    makePreset(8,  SYNTH_WAVE_SAW,      0, 3, 12.0f, 0.05f,  3800.0f, 0.18f,
               0.550f, 0.90f, 0.88f, 1.10f, 4.8f,  7.0f, 0.9f, 0.55f, 0.10f, 0.40f), // Archi
    makePreset(9,  SYNTH_WAVE_SINE,     1, 1,  0.0f, 0.00f, 12000.0f, 0.05f,
               0.002f, 1.60f, 0.00f, 2.20f, 3.2f,  0.0f, 0.9f, 0.55f, 0.05f, 0.35f), // Campane
    makePreset(10, SYNTH_WAVE_SAW,      0, 2,  6.0f, 0.15f,  2400.0f, 0.28f,
               0.090f, 0.35f, 0.78f, 0.25f, 5.2f,  3.0f, 1.4f, 0.62f, 0.50f, 0.14f), // Ottoni
    makePreset(11, SYNTH_WAVE_SINE,     0, 1,  0.0f, 0.00f,  7000.0f, 0.08f,
               0.070f, 0.25f, 0.92f, 0.18f, 5.0f,  6.0f, 0.9f, 0.55f, 0.05f, 0.22f), // Flauto
    makePreset(12, SYNTH_WAVE_TRIANGLE, 0, 4, 15.0f, 0.05f,  5000.0f, 0.12f,
               0.650f, 1.00f, 0.90f, 1.40f, 4.5f,  9.0f, 0.85f, 0.52f, 0.05f, 0.45f), // Coro
    makePreset(13, SYNTH_WAVE_SAW,     -1, 2, 30.0f, 0.35f,   700.0f, 0.30f,
               0.005f, 0.30f, 0.75f, 0.18f, 5.0f,  3.0f, 1.8f, 0.62f, 0.35f, 0.04f), // Reese
    makePreset(14, SYNTH_WAVE_PULSE,    0, 1,  0.0f, 0.00f, 16000.0f, 0.05f,
               0.001f, 0.08f, 0.00f, 0.03f, 6.0f,  0.0f, 1.1f, 0.50f, 0.00f, 0.05f), // Chip
    makePreset(15, SYNTH_WAVE_NOISE,    0, 2, 20.0f, 0.00f,  2200.0f, 0.15f,
               0.400f, 1.20f, 0.75f, 1.10f, 4.0f, 12.0f, 0.9f, 0.45f, 0.12f, 0.55f), // Nebbia
};

const char* kPresetNames[] = {
    "Iniziale", "Lead", "Basso", "Pad", "Pluck", "Organo", "Supersaw",
    "Piano elettrico", "Archi", "Campane", "Ottoni", "Flauto", "Coro",
    "Reese", "Chip", "Nebbia"
};

} // namespace

int synthPresetCount() { return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); }

const char* synthPresetName(int index) {
    const int n = synthPresetCount();
    if (index < 0 || index >= n) index = 0;
    return kPresetNames[index];
}

SynthSettings synthPresetSettings(int index) {
    const int n = synthPresetCount();
    if (index < 0 || index >= n) index = 0;
    return kPresets[index];
}
