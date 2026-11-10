#include "audio.h"

#include "sampler.h"
#include "synth.h"
#include "vsthost.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Limitatore soft-knee: unitario sotto la soglia, comprime dolcemente sopra
// fino ad asintotare a 1.0 (evita il clipping "duro" con il boost di volume).
inline float softLimit(float x) {
    constexpr float t = 0.95f;
    const float a = std::fabs(x);
    if (a <= t) return x;
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    return sign * (t + (1.0f - t) * std::tanh((a - t) / (1.0f - t)));
}

} // namespace

struct AudioEngine::Impl {
    std::atomic<VstHost*> vst{nullptr};
    std::atomic<Sampler*> sampler{nullptr};
    std::atomic<Synth*>   synth{nullptr};

    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec     spec{};

    std::atomic<float> gain{1.0f};
    std::atomic<float> peak{0.0f};
    std::atomic<int>   clickPending{0};
    std::atomic<bool>  clickAccent{false};
    float clickPhase = 0.0f;
    float clickEnv   = 0.0f;
    float clickFreq  = 1320.0f;

    int requestedRate  = 48000;
    int requestedBlock = 512;

    // ---------------------------------------------------------- riverbero ---
    // Rete compatta stile Freeverb sul mix finale: incolla gli strumenti e da'
    // profondita' anche a chi non ha un effetto proprio (Rhodes, piano, sampler).
    static constexpr int kCombCount    = 4;
    static constexpr int kAllpassCount = 2;

    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float  damp = 0.0f;
        void init(int n) {
            buf.assign(static_cast<size_t>(std::max(8, n)), 0.0f);
            idx = 0;
            damp = 0.0f;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            idx = 0;
            damp = 0.0f;
        }
        float process(float in) {
            const float out = buf[idx];
            damp = out * 0.72f + damp * 0.28f;
            buf[idx] = in + damp * 0.84f;
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

    void initReverb(int rate) {
        const double k = static_cast<double>(rate) / 44100.0;
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

    bool reverbReady() const { return !combL[0].buf.empty(); }

    void process(float* out, int frames);
    static void cb(void* userdata, Uint8* stream, int len);
};

void AudioEngine::Impl::process(float* out, int frames) {
    const size_t samples = static_cast<size_t>(frames) * 2;
    std::memset(out, 0, samples * sizeof(float));

    if (VstHost* v = vst.load(std::memory_order_acquire))
        if (v->prepared()) v->renderAdd(out, frames);
    if (Sampler* s = sampler.load(std::memory_order_acquire))
        if (s->prepared()) s->renderAdd(out, frames);
    if (Synth* y = synth.load(std::memory_order_acquire))
        if (y->prepared()) y->renderAdd(out, frames);

    if (clickPending.exchange(0, std::memory_order_relaxed) > 0) {
        clickEnv  = 0.55f;
        clickPhase = 0.0f;
        clickFreq = clickAccent.exchange(false, std::memory_order_relaxed) ? 1760.0f
                                                                           : 1320.0f;
    }
    if (clickEnv > 1.0e-4f) {
        const float rate = spec.freq > 0 ? static_cast<float>(spec.freq) : 48000.0f;
        const float decay = std::exp(-1.0f / (0.018f * rate));
        const float step = clickFreq / rate;
        for (int i = 0; i < frames; ++i) {
            const float s = std::sin(clickPhase * 6.28318530718f) * clickEnv;
            out[static_cast<size_t>(i) * 2]     += s * 0.32f;
            out[static_cast<size_t>(i) * 2 + 1] += s * 0.32f;
            clickPhase += step;
            if (clickPhase >= 1.0f) clickPhase -= 1.0f;
            clickEnv *= decay;
        }
        if (clickEnv < 1.0e-4f) clickEnv = 0.0f;
    }

    // Riverbero di master: discreto, ma da' aria e coesione al mix.
    if (reverbReady()) {
        constexpr float wet = 0.085f;
        for (int i = 0; i < frames; ++i) {
            const size_t l = static_cast<size_t>(i) * 2;
            const float send = (out[l] + out[l + 1]) * 0.5f;
            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < kCombCount; ++k) {
                wl += combL[k].process(send);
                wr += combR[k].process(send);
            }
            for (int k = 0; k < kAllpassCount; ++k) {
                wl = allpassL[k].process(wl);
                wr = allpassR[k].process(wr);
            }
            out[l]     += wl * wet;
            out[l + 1] += wr * wet;
        }
    }

    const float g = gain.load(std::memory_order_relaxed);
    float localPeak = 0.0f;
    for (size_t i = 0; i < samples; ++i) {
        const float x = softLimit(out[i] * g);
        out[i] = x;
        const float ax = std::fabs(x);
        if (ax > localPeak) localPeak = ax;
    }
    const float prev = peak.load(std::memory_order_relaxed);
    peak.store(localPeak > prev ? localPeak : prev * 0.90f, std::memory_order_relaxed);
}

void AudioEngine::Impl::cb(void* userdata, Uint8* stream, int len) {
    auto* impl = static_cast<Impl*>(userdata);
    if (len <= 0) return;
    impl->process(reinterpret_cast<float*>(stream),
                  len / (2 * static_cast<int>(sizeof(float))));
}

AudioEngine::AudioEngine() : impl_(new Impl) {}
AudioEngine::~AudioEngine() {
    stop();
    delete impl_;
}

void AudioEngine::setVst(VstHost* vst) { impl_->vst.store(vst, std::memory_order_release); }
void AudioEngine::setSampler(Sampler* s) { impl_->sampler.store(s, std::memory_order_release); }
void AudioEngine::setSynth(Synth* s) { impl_->synth.store(s, std::memory_order_release); }

bool AudioEngine::start(int sampleRate, int blockSize) {
    if (impl_->dev) return true;
    impl_->requestedRate  = sampleRate;
    impl_->requestedBlock = blockSize;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;

    SDL_AudioSpec want{};
    want.freq     = sampleRate;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = static_cast<Uint16>(blockSize);
    want.callback = &Impl::cb;
    want.userdata = impl_;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &impl_->spec,
                                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (dev == 0) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    impl_->dev = dev;
    impl_->initReverb(impl_->spec.freq);

    // Prepara le sorgenti alla frequenza/blocco effettivi del dispositivo,
    // *prima* di avviare il callback.
    if (VstHost* v = impl_->vst.load(std::memory_order_acquire))
        if (v->loaded() && !v->prepare(impl_->spec.freq, impl_->spec.samples))
            std::fprintf(stderr, "Audio: preparazione plugin fallita: %s\n",
                         v->lastError().c_str());
    if (Sampler* s = impl_->sampler.load(std::memory_order_acquire))
        if (s->loaded() && !s->prepare(impl_->spec.freq, impl_->spec.samples))
            std::fprintf(stderr, "Audio: preparazione strumento fallita: %s\n",
                         s->lastError().c_str());
    if (Synth* y = impl_->synth.load(std::memory_order_acquire))
        if (!y->prepare(impl_->spec.freq, impl_->spec.samples))
            std::fprintf(stderr, "Audio: preparazione synth fallita\n");

    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void AudioEngine::stop() {
    if (!impl_->dev) return;
    SDL_PauseAudioDevice(impl_->dev, 1);
    // Chiudere il dispositivo garantisce che il callback non sia piu' in corso,
    // cosi' le sorgenti possono essere riconfigurate/distrutte in sicurezza.
    SDL_CloseAudioDevice(impl_->dev);
    impl_->dev = 0;
    impl_->clearReverb();

    if (VstHost* v = impl_->vst.load(std::memory_order_acquire)) v->releaseProcessing();
    if (Sampler* s = impl_->sampler.load(std::memory_order_acquire)) s->releaseProcessing();
    if (Synth* y = impl_->synth.load(std::memory_order_acquire)) y->releaseProcessing();

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioEngine::running() const { return impl_->dev != 0; }

int AudioEngine::sampleRate() const {
    return impl_->spec.freq ? impl_->spec.freq : impl_->requestedRate;
}
int AudioEngine::blockSize() const {
    return impl_->dev ? impl_->spec.samples : impl_->requestedBlock;
}
int AudioEngine::deviceSampleRate() const { return sampleRate(); }

void AudioEngine::setMasterGain(float g) {
    impl_->gain.store(std::max(0.0f, std::min(2.0f, g)), std::memory_order_relaxed);
}
float AudioEngine::masterGain() const { return impl_->gain.load(std::memory_order_relaxed); }

float AudioEngine::peakLevel() const { return impl_->peak.load(std::memory_order_relaxed); }

void AudioEngine::triggerClick(bool accent) {
    if (accent) impl_->clickAccent.store(true, std::memory_order_relaxed);
    impl_->clickPending.fetch_add(1, std::memory_order_relaxed);
}
