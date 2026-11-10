#include "piano.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kTableSize = 4096;

// Tabella seno condivisa (inizializzata una volta all'avvio, prima del thread
// audio). Accesso diretto senza guardie nel percorso piu' caldo.
float gSin[kTableSize + 1];

void initSinTable() {
    for (int i = 0; i <= kTableSize; ++i)
        gSin[i] = static_cast<float>(std::sin(2.0 * M_PI * i / kTableSize));
}

inline float fastSin(double phase) { // phase in [0,1)
    const double x = phase * kTableSize;
    const int i = static_cast<int>(x);
    const float frac = static_cast<float>(x - i);
    const int idx = i & (kTableSize - 1);
    const float a = gSin[idx];
    return a + (gSin[idx + 1] - a) * frac;
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

} // namespace

// ============================================================================
struct PianoSynth::Impl {
    static constexpr int kMaxVoices   = 64;
    static constexpr int kMaxPartials = 24;
    static constexpr int kStrings     = 2;

    int rate = 48000;

    struct Voice {
        bool active = false;
        bool held = false;       // tasto fisicamente premuto
        bool sustained = false;  // rilasciato ma tenuto dal pedale
        bool released = false;   // in fase di smorzamento (damper)

        int    pitch = 60;
        int    nPart = 0;
        int    nStr  = 1;

        double phase[kMaxPartials][kStrings] = {};
        double inc[kMaxPartials][kStrings]   = {};
        float  amp[kMaxPartials][kStrings]   = {};
        float  decay[kMaxPartials]           = {};

        float  att = 0.0f, attInc = 0.0f;
        float  relEnv = 1.0f, relInc = 0.0f; // smorzamento lineare dopo il rilascio

        float  noiseAmp = 0.0f, noiseDecay = 0.0f, noiseLp = 0.0f, noiseLpCoef = 0.5f;
        float  noiseSeedAmp = 0.0f; // livello di rumore da (ri)innescare

        float  panL = 0.70710678f, panR = 0.70710678f;
        unsigned int rng = 0x9e3779b9u;
        unsigned long long stamp = 0;
    };

    Voice voices[kMaxVoices];

    bool sustain = false;
    unsigned long long stampCounter = 0;

    // ---------------------------------------------------------------- utility
    Voice* allocVoice() {
        for (int i = 0; i < kMaxVoices; ++i)
            if (!voices[i].active) return &voices[i];
        // furto: la voce piu' debole (privilegia quelle gia' in rilascio)
        int best = 0;
        float bestScore = 1e9f;
        for (int i = 0; i < kMaxVoices; ++i) {
            const Voice& v = voices[i];
            const float score = (v.released ? 0.0f : 1.0f) + v.amp[0][0];
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return &voices[best];
    }

    void beginRelease(Voice& v) {
        if (v.released) return;
        const float relT = std::max(
            0.05f, std::min(0.35f, 0.12f * std::pow(2.0f, (60.0f - v.pitch) / 24.0f)));
        v.relInc = 1.0f / (relT * static_cast<float>(rate));
        v.released = true;
        v.held = false;
        v.sustained = false;
        // piccolo rumore di smorzatore
        v.noiseSeedAmp = std::max(v.noiseSeedAmp, 0.05f);
    }

    // ---------------------------------------------------------------------- on
    void noteOn(int pitch, int velocity) {
        if (pitch < 0 || pitch > 127) return;
        const float velN = std::max(0.0f, std::min(1.0f, velocity / 127.0f));

        Voice* vp = allocVoice();
        Voice& v = *vp;
        v = Voice{};
        v.active = true;
        v.held = true;
        v.pitch = pitch;
        v.stamp = ++stampCounter;
        v.rng = 0x9e3779b9u ^ (static_cast<unsigned int>(pitch) * 2654435761u) ^
                static_cast<unsigned int>(v.stamp);

        const double f0 = 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
        const double B = std::max(4e-5, std::min(4e-3,
                                0.0004 * std::pow(2.0, (60.0 - pitch) / 15.0)));
        const double nyq = 0.45 * rate;
        const double pExp = 1.6 - 1.05 * velN; // piu' veloce -> piu' brillante

        // Decadimento di base: piu' lunga per le note basse.
        double T0 = 3.2 * std::pow(2.0, (60.0 - pitch) / 13.0);
        T0 = std::max(0.5, std::min(18.0, T0));

        // Numero di corde (i bassi con corda singola).
        v.nStr = (pitch < 28) ? 1 : kStrings;
        const double detCents = 0.7;

        double amps[kMaxPartials];
        int    nPart = 0;
        for (int n = 1; n <= kMaxPartials; ++n) {
            const double nd = static_cast<double>(n);
            const double f = nd * f0 * std::sqrt(1.0 + B * nd * nd);
            if (f > nyq) break;
            double a = std::pow(nd, -pExp);
            // posizione del martelletto (~1/8 della corda): leggera comb
            a *= 0.55 + 0.45 * std::fabs(std::sin(M_PI * nd / 8.0));
            amps[nPart] = a;
            const double tau = T0 / std::pow(nd, 0.5);
            v.decay[nPart] = static_cast<float>(std::exp(-1.0 / (tau * rate)));
            for (int st = 0; st < v.nStr; ++st) {
                const double det = (st == 0) ? +detCents : -detCents;
                const double fst = (v.nStr == 1) ? f : f * std::pow(2.0, det / 1200.0);
                v.inc[nPart][st] = fst / rate;
                // sfasamento iniziale fra le corde -> battimento naturale
                v.phase[nPart][st] = (st == 0) ? 0.0
                                               : static_cast<double>(noiseBipolar(v.rng) * 0.5 + 0.5);
            }
            ++nPart;
        }
        v.nPart = std::max(1, nPart);

        // normalizza sul picco d'innesco (somma delle ampiezze), poi guadagno
        double sumA = 0.0;
        for (int n = 0; n < v.nPart; ++n) sumA += amps[n] * v.nStr;
        if (sumA < 1e-9) sumA = 1.0;
        const float g = 0.34f * std::pow(velN, 1.4f) / static_cast<float>(sumA);
        for (int n = 0; n < v.nPart; ++n)
            for (int st = 0; st < v.nStr; ++st)
                v.amp[n][st] = static_cast<float>(amps[n]) * g;

        // attacco (morbido, niente click)
        v.att = 0.0f;
        v.attInc = 1.0f / (0.0025f * static_cast<float>(rate));

        // rumore del martelletto
        const double cut = std::max(1200.0, std::min(8000.0, f0 * 8.0));
        v.noiseLpCoef = static_cast<float>(1.0 - std::exp(-2.0 * M_PI * cut / rate));
        v.noiseLp = 0.0f;
        v.noiseAmp = 0.45f * std::pow(velN, 1.4f);
        v.noiseDecay = static_cast<float>(std::exp(-1.0 / (0.008 * rate)));
        v.noiseSeedAmp = 0.0f;

        // posizione stereo (pianoforte visto dall'esecutore)
        const double panPos = std::max(-0.6, std::min(0.6, (pitch - 60) / 60.0));
        const double ang = (panPos + 1.0) * (M_PI / 4.0);
        v.panL = static_cast<float>(std::cos(ang));
        v.panR = static_cast<float>(std::sin(ang));
    }

    void noteOff(int pitch) {
        for (Voice& v : voices)
            if (v.active && v.held && v.pitch == pitch) {
                v.held = false;
                if (sustain) v.sustained = true;
                else beginRelease(v);
            }
    }

    void allOff() {
        sustain = false;
        for (Voice& v : voices)
            if (v.active) beginRelease(v);
    }

    void controlChange(int cc, int value) {
        if (cc == 64) {
            const bool on = value >= 64;
            if (sustain && !on) {
                for (Voice& v : voices)
                    if (v.active && v.sustained) beginRelease(v);
            }
            sustain = on;
        } else if (cc == 120 || cc == 123) {
            allOff();
        }
    }

    void reset() {
        for (Voice& v : voices) v = Voice{};
        sustain = false;
    }

    // ----------------------------------------------------------------- render
    void renderAdd(float* out, int frames) {
        for (Voice& v : voices) {
            if (!v.active) continue;
            for (int fr = 0; fr < frames; ++fr) {
                float s = 0.0f;
                for (int n = 0; n < v.nPart; ++n) {
                    for (int st = 0; st < v.nStr; ++st) {
                        s += v.amp[n][st] * fastSin(v.phase[n][st]);
                        double ph = v.phase[n][st] + v.inc[n][st];
                        if (ph >= 1.0) ph -= 1.0;
                        v.phase[n][st] = ph;
                    }
                }
                s *= v.att;
                if (v.att < 1.0f) { v.att += v.attInc; if (v.att > 1.0f) v.att = 1.0f; }
                if (v.released) {
                    s *= v.relEnv;
                    v.relEnv -= v.relInc;
                    if (v.relEnv < 0.0f) v.relEnv = 0.0f;
                }

                // rumore martelletto / smorzatore
                if (v.noiseAmp > 1e-5f || v.noiseSeedAmp > 1e-5f) {
                    if (v.noiseSeedAmp > 0.0f) {
                        v.noiseAmp += v.noiseSeedAmp * 0.35f;
                        v.noiseSeedAmp = 0.0f;
                    }
                    const float w = noiseBipolar(v.rng);
                    v.noiseLp += v.noiseLpCoef * (w - v.noiseLp);
                    s += v.noiseLp * v.noiseAmp;
                    v.noiseAmp *= v.noiseDecay;
                    if (v.noiseAmp < 1e-5f) v.noiseAmp = 0.0f;
                }

                const float L = s * v.panL;
                const float R = s * v.panR;
                out[fr * 2]     += L;
                out[fr * 2 + 1] += R;

                // decadimento delle armoniche
                for (int n = 0; n < v.nPart; ++n) {
                    const float d = v.decay[n];
                    for (int st = 0; st < v.nStr; ++st) v.amp[n][st] *= d;
                }
            }
            // fine voce: smorzamento completato oppure fondamentale ormai muta
            if (v.released && v.relEnv <= 0.0f) v = Voice{};
            else if (v.att >= 1.0f && v.amp[0][0] < 1.0e-5f) v = Voice{};
        }
    }

    int activeVoices() const {
        int c = 0;
        for (const Voice& v : voices) if (v.active) ++c;
        return c;
    }
};

// ============================================================================
PianoSynth::PianoSynth() : impl_(new Impl) { initSinTable(); }
PianoSynth::~PianoSynth() { delete impl_; }

void PianoSynth::prepare(int sampleRate) {
    if (sampleRate > 0) impl_->rate = sampleRate;
    impl_->reset();
}

void PianoSynth::reset() { impl_->reset(); }
void PianoSynth::noteOn(int p, int v) { impl_->noteOn(p, v); }
void PianoSynth::noteOff(int p) { impl_->noteOff(p); }
void PianoSynth::controlChange(int cc, int val) { impl_->controlChange(cc, val); }
void PianoSynth::renderAdd(float* out, int frames) {
    if (frames > 0) impl_->renderAdd(out, frames);
}
int PianoSynth::activeVoices() const { return impl_->activeVoices(); }
