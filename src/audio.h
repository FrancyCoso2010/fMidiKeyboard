#pragma once

// Motore audio unico: apre *un solo* dispositivo SDL e mixa insieme le varie
// sorgenti (plugin VST3 e motore interno). Sul mix finale applica un riverbero
// stereo discreto (da' spazio agli strumenti che non ne hanno uno proprio),
// il volume master, un limitatore soft-knee e misura il picco per il VU meter.
//
// Avere un unico dispositivo evita la deriva di clock tra due uscite separate
// (fase/eco quando VST3 e sampler suonano assieme) e dimezza l'uso di risorse.
class VstHost;
class Sampler;
class Synth;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Sorgenti mixate (nullptr per disattivarle). Impostarle mentre il motore
    // e' in esecuzione ha effetto al prossimo start().
    void setVst(VstHost* vst);
    void setSampler(Sampler* sampler);
    void setSynth(Synth* synth);

    bool start(int sampleRate = 48000, int blockSize = 512);
    void stop();
    bool running() const;

    int sampleRate() const;
    int blockSize() const;
    // Frequenza effettiva del dispositivo (puo' differire da quella richiesta).
    int deviceSampleRate() const;

    // Volume master applicato al mix finale (0.0 .. 2.0).
    void  setMasterGain(float gain);
    float masterGain() const;

    // Picco recente del mix finale (0.0 .. 1.0+), per il VU meter.
    float peakLevel() const;

    // Click del metronomo (accento sul primo battito della battuta).
    void triggerClick(bool accent = false);

private:
    struct Impl;
    Impl* impl_;
};
