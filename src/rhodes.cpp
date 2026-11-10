#include "rhodes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kTableSize = 4096;

const float* sinTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(kTableSize + 1);
        for (int i = 0; i <= kTableSize; ++i)
            t[i] = static_cast<float>(std::sin(2.0 * M_PI * i / kTableSize));
        return t;
    }();
    return table.data();
}

inline float fastSin(double phase) { // phase in [0,1)
    const double p = phase - std::floor(phase);
    const double x = p * kTableSize;
    const int i = static_cast<int>(x);
    const float f = static_cast<float>(x - i);
    const float* t = sinTable();
    const int idx = i & (kTableSize - 1);
    return t[idx] + (t[idx + 1] - t[idx]) * f;
}

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

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

// Tangente iperbolica razionale: saturazione morbida, niente std::tanh nel
// percorso caldo.
inline float fastTanh(float x) {
    x = std::max(-3.0f, std::min(3.0f, x));
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Coefficiente di decadimento esponenziale per una costante di tempo in secondi.
inline float decayCoef(float seconds, int rate) {
    return std::exp(-1.0f / (std::max(0.0005f, seconds) * static_cast<float>(rate)));
}

} // namespace

// ============================================================================
struct RhodesPiano::Impl {
    static constexpr int kMaxVoices = 48;

    int rate = 48000;
    unsigned long long stampCounter = 0;

    struct Voice {
        bool  active   = false;
        bool  held     = false;
        bool  sustained = false;
        bool  released = false;
        int   pitch    = 60;
        float vel      = 0.5f;

        double carPhase = 0.0, carInc = 0.0;
        double modPhase = 0.0, modInc = 0.0;
        double tinePhase = 0.0, tineInc = 0.0;

        float modIdxBase = 0.0f;
        float modIdxPeak = 0.0f;
        float idxEnv     = 1.0f;
        float idxCoef    = 0.0f;

        float amp      = 0.0f;   // guadagno di velocita' (costante)
        float ampA     = 0.0f;   // decadimento iniziale, quello che "sboccia"
        float ampACoef = 0.0f;
        float ampB     = 0.0f;   // coda lunga del tine
        float ampBCoef = 0.0f;
        float env      = 0.0f;
        float envInc   = 0.0f;
        float damper   = 1.0f;
        float damperCoef = 0.0f;

        float tineAmp  = 0.0f;
        float tineCoef = 0.0f;

        // Parziali inarmonici del tone bar: il colore metallico del Rhodes.
        double bellPhase[2] = {};
        double bellInc[2]   = {};
        float  bellAmp[2]   = {};
        float  bellCoef[2]  = {};

        float clickEnv    = 0.0f;
        float clickCoef   = 0.0f;
        float clickLp     = 0.0f;
        float clickLpCoef = 0.5f;

        float panL = 0.70710678f;
        float panR = 0.70710678f;

        uint32_t rng = 1u;
        unsigned long long stamp = 0;
    };

    Voice voices[kMaxVoices];
    bool  sustain = false;
    float tremPhase = 0.0f;
    float lpL = 0.0f;   // filtro dolce sull'uscita: toglie l'asprezza digitale
    float lpR = 0.0f;

    // ------------------------------------------------------------- utility ---
    Voice* allocVoice() {
        for (int i = 0; i < kMaxVoices; ++i)
            if (!voices[i].active) return &voices[i];

        // Furto: prima le voci gia' smorzate, poi quelle con inviluppo minore.
        int best = 0;
        float bestScore = 1e9f;
        for (int i = 0; i < kMaxVoices; ++i) {
            const Voice& v = voices[i];
            const float score = (v.released ? 0.0f : 1.0f) +
                                v.amp * (v.ampA + v.ampB) * v.damper;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return &voices[best];
    }

    void beginRelease(Voice& v, float seconds) {
        if (!v.active || v.released) return;
        v.released = true;
        v.held = false;
        v.sustained = false;
        v.damperCoef = decayCoef(seconds, rate);
        // Il feltro dello smorzatore tocca il tine: piccolo rumore sordo.
        v.clickEnv = std::max(v.clickEnv, 0.010f);
    }

    // ---------------------------------------------------------------------- on
    void noteOn(int pitch, int velocity) {
        if (pitch < 0 || pitch > 127) return;
        if (velocity <= 0) { noteOff(pitch); return; }

        const float vel = clampf(velocity / 127.0f, 0.0f, 1.0f);

        // Ritrigger sullo stesso tasto: la voce precedente viene smorzata.
        for (Voice& old : voices) {
            if (!old.active || old.released) continue;
            if (old.pitch != pitch || !old.held) continue;
            old.held = false;
            if (sustain) old.sustained = true;
            else beginRelease(old, 0.040f);
        }

        Voice& v = *allocVoice();
        const unsigned long long stamp = ++stampCounter;
        v = Voice{};
        v.active = true;
        v.held   = true;
        v.pitch  = pitch;
        v.vel    = vel;
        v.stamp  = stamp;
        v.rng    = 0x9e3779b9u ^ (static_cast<uint32_t>(pitch) * 2654435761u) ^
                   static_cast<uint32_t>(stamp);

        const double f0  = 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
        const double nyq = 0.45 * static_cast<double>(rate);

        v.carInc  = f0 / static_cast<double>(rate);
        // Il modulatore e' appena calante: da' il movimento "vivo" del tine.
        v.modInc  = (f0 * 1.0016) / static_cast<double>(rate);
        v.tineInc = std::min(f0 * (6.0 + 2.5 * static_cast<double>(vel)), nyq) /
                    static_cast<double>(rate);

        // Indice FM: attacco deciso, corpo quasi sinusoidale. Limitato per non
        // generare alias sulle note acute.
        const float headroom = static_cast<float>(nyq / std::max(40.0, f0));
        const float baseMax  = std::max(0.0f, headroom - 1.6f);
        const float peakMax  = std::max(0.0f, headroom - 1.0f);
        v.modIdxBase = clampf(0.28f + 0.80f * vel, 0.0f, baseMax);
        v.modIdxPeak = clampf(2.20f + 6.50f * vel * vel, 0.0f, peakMax);
        v.idxEnv     = 1.0f;
        v.idxCoef    = decayCoef(0.045f + 0.060f * vel, rate);

        // Attacco rapidissimo, poi decadimento naturale per nota.
        v.env    = 0.0f;
        v.envInc = 1.0f / (0.0012f * static_cast<float>(rate));
        // Due stadi: il tine perde subito energia (attacco che "sboccia") e poi
        // lascia una coda lunga e calda. E' il respiro tipico del Rhodes.
        const float tauFast = clampf(0.26f * std::pow(2.0f, (60.0f - pitch) / 30.0f),
                                     0.10f, 0.85f);
        const float tauSlow = clampf(1.95f * std::pow(2.0f, (60.0f - pitch) / 24.0f),
                                     0.45f, 8.0f);
        v.ampACoef = decayCoef(tauFast, rate);
        v.ampBCoef = decayCoef(tauSlow, rate);
        v.ampA = 1.0f;
        v.ampB = 1.0f;
        v.amp  = 0.26f * std::pow(vel, 1.30f);

        // Ping metallico del tine: solo nell'attacco, sparisce in fretta.
        v.tineAmp  = 0.20f * vel * vel;
        v.tineCoef = decayCoef(0.030f + 0.020f * (1.0f - vel), rate);

        const double bellRatios[2] = {2.76, 5.40};
        const float  bellGains[2]  = {0.075f, 0.040f};
        for (int k = 0; k < 2; ++k) {
            v.bellInc[k]  = std::min(f0 * bellRatios[k], nyq) /
                            static_cast<double>(rate);
            v.bellPhase[k] = 0.0;
            v.bellAmp[k]   = bellGains[k] * vel * vel;
            v.bellCoef[k]  = decayCoef(0.28f + 0.20f * (1.0f - vel), rate);
        }

        // Martelletto + pickup.
        v.clickEnv    = 0.050f * vel * vel;
        v.clickCoef   = decayCoef(0.0045f, rate);
        v.clickLpCoef = 1.0f - std::exp(-2.0 * M_PI * 3200.0 /
                                        static_cast<double>(rate));
        v.clickLp     = 0.0f;

        // Bassi a sinistra, acuti a destra: come il pianoforte visto da chi suona.
        const float panPos = clampf((pitch - 60) / 70.0f, -0.55f, 0.55f);
        const float ang = (panPos + 1.0f) * static_cast<float>(M_PI / 4.0);
        v.panL = std::cos(ang);
        v.panR = std::sin(ang);
    }

    void noteOff(int pitch) {
        for (Voice& v : voices) {
            if (!v.active || v.released) continue;
            if (v.pitch != pitch || !v.held) continue;
            v.held = false;
            if (sustain) v.sustained = true;
            else beginRelease(v, 0.085f);
        }
    }

    void allOff() {
        sustain = false;
        for (Voice& v : voices)
            if (v.active) beginRelease(v, 0.060f);
    }

    void controlChange(int cc, int value) {
        if (cc == 64) {
            const bool on = value >= 64;
            if (sustain && !on) {
                for (Voice& v : voices)
                    if (v.active && v.sustained) beginRelease(v, 0.085f);
            }
            sustain = on;
        } else if (cc == 120 || cc == 123) {
            allOff();
        }
    }

    void reset() {
        for (Voice& v : voices) v = Voice{};
        sustain = false;
        tremPhase = 0.0f;
        lpL = 0.0f;
        lpR = 0.0f;
    }

    int activeVoices() const {
        int n = 0;
        for (const Voice& v : voices)
            if (v.active) ++n;
        return n;
    }

    // ----------------------------------------------------------------- render
    void renderAdd(float* out, int frames) {
        if (frames <= 0) return;

        const float tremDepth = 0.11f;
        const float tremInc   = 5.2f / static_cast<float>(rate);
        const float lpCoef    = 1.0f - std::exp(-2.0 * M_PI * 9000.0 /
                                               static_cast<double>(rate));
        float trem = tremPhase;

        for (int fr = 0; fr < frames; ++fr) {
            float l = 0.0f, r = 0.0f;

            for (Voice& v : voices) {
                if (!v.active) continue;

                if (v.env < 1.0f) {
                    v.env += v.envInc;
                    if (v.env > 1.0f) v.env = 1.0f;
                }

                // Portante modulata: indice massimo all'attacco, poi si placa.
                const float idx = v.modIdxBase + v.modIdxPeak * v.idxEnv;
                v.idxEnv *= v.idxCoef;

                float s = fastSin(v.carPhase + idx * fastSin(v.modPhase));

                if (v.tineAmp > 1.0e-5f) {
                    s += fastSin(v.tinePhase) * v.tineAmp;
                    v.tinePhase += v.tineInc;
                    if (v.tinePhase >= 1.0) v.tinePhase -= 1.0;
                    v.tineAmp *= v.tineCoef;
                }

                if (v.clickEnv > 1.0e-5f) {
                    const float w = noiseBipolar(v.rng);
                    v.clickLp += v.clickLpCoef * (w - v.clickLp);
                    s += v.clickLp * v.clickEnv;
                    v.clickEnv *= v.clickCoef;
                } else {
                    v.clickEnv = 0.0f;
                }

                v.carPhase += v.carInc;
                if (v.carPhase >= 1.0) v.carPhase -= 1.0;
                v.modPhase += v.modInc;
                if (v.modPhase >= 1.0) v.modPhase -= 1.0;

                for (int k = 0; k < 2; ++k) {
                    if (v.bellAmp[k] <= 1.0e-5f) continue;
                    s += fastSin(v.bellPhase[k]) * v.bellAmp[k];
                    v.bellPhase[k] += v.bellInc[k];
                    if (v.bellPhase[k] >= 1.0) v.bellPhase[k] -= 1.0;
                    v.bellAmp[k] *= v.bellCoef[k];
                }

                const float body = 0.62f * v.ampA + 0.38f * v.ampB;
                v.ampA *= v.ampACoef;
                v.ampB *= v.ampBCoef;

                float g = s * v.env * v.amp * body;
                if (v.released) {
                    g *= v.damper;
                    v.damper *= v.damperCoef;
                    if (v.damper < 1.0e-4f) { v.damper = 0.0f; v.active = false; }
                }
                if (body < 1.0e-4f) v.active = false;

                // Saturazione morbida del pickup: corpo e un filo di "growl".
                const float shaped = fastTanh(g * 1.35f) * 0.74f;
                l += shaped * v.panL;
                r += shaped * v.panR;
            }

            // Un filo di filtro sul mix delle voci: corpo senza asprezza.
            lpL += lpCoef * (l - lpL);
            lpR += lpCoef * (r - lpR);
            l = lpL;
            r = lpR;

            const float tm = fastSin(trem) * tremDepth;
            out[static_cast<size_t>(fr) * 2]     += l * (1.0f - tm);
            out[static_cast<size_t>(fr) * 2 + 1] += r * (1.0f + tm);

            trem += tremInc;
            if (trem >= 1.0f) trem -= 1.0f;
        }
        tremPhase = trem;
    }
};

// ============================================================================
RhodesPiano::RhodesPiano() : impl_(new Impl) {
    sinTable(); // prepara la tabella prima del thread audio
}

RhodesPiano::~RhodesPiano() { delete impl_; }

void RhodesPiano::prepare(int sampleRate) {
    if (sampleRate > 0) impl_->rate = sampleRate;
    impl_->reset();
}

void RhodesPiano::reset() { impl_->reset(); }

void RhodesPiano::noteOn(int pitch, int velocity) { impl_->noteOn(pitch, velocity); }
void RhodesPiano::noteOff(int pitch) { impl_->noteOff(pitch); }
void RhodesPiano::controlChange(int cc, int value) { impl_->controlChange(cc, value); }

void RhodesPiano::renderAdd(float* out, int frames) {
    if (out && frames > 0) impl_->renderAdd(out, frames);
}

int RhodesPiano::activeVoices() const { return impl_->activeVoices(); }
