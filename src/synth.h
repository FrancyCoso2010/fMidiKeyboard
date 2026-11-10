#pragma once

#include <string>

// Sintetizzatore polifonico sottrattivo.
//
// Catena per voce: oscillatori (unison, con anti-aliasing PolyBLEP) ->
// sub-oscillatore -> filtro passa-basso risonante (SVF TPT) -> inviluppo ADSR
// -> saturazione morbida. LFO globale per il vibrato, mod wheel sul canale 1.
//
// Tutti i parametri sono modificabili in tempo reale dal thread grafico: il
// motore li legge da variabili atomiche una volta per blocco, mentre gli eventi
// MIDI passano da una coda lock-free (thread MIDI -> thread audio).

enum SynthWave {
    SYNTH_WAVE_SINE = 0,
    SYNTH_WAVE_SAW,
    SYNTH_WAVE_SQUARE,
    SYNTH_WAVE_TRIANGLE,
    SYNTH_WAVE_PULSE,
    SYNTH_WAVE_NOISE,
    SYNTH_WAVE_COUNT
};

struct SynthSettings {
    int   preset     = 0;
    int   wave       = SYNTH_WAVE_SAW;
    int   octave     = 0;       // -2 .. +2
    int   unison     = 1;       // 1 .. 4 oscillatori per voce
    float detune     = 0.0f;    // centesimi di scarto fra gli unisoni
    float sub        = 0.0f;    // livello del sub-oscillatore (0..1)
    float cutoff     = 6000.0f; // Hz
    float resonance  = 0.15f;   // 0..1
    float attack     = 0.005f;  // secondi
    float decay      = 0.30f;   // secondi
    float sustain    = 0.80f;   // 0..1
    float release    = 0.30f;   // secondi
    float lfoRate    = 5.0f;    // Hz
    float lfoDepth   = 0.0f;    // centesimi di vibrato
    float drive      = 1.0f;    // 1..4 saturazione
    float volume     = 0.65f;   // 0..1
    float filterEnv  = 0.20f;   // 0..1 apertura del filtro guidata dall'ADSR
    float reverb     = 0.10f;   // 0..1 riverbero stereo
};

int                  synthPresetCount();
const char*          synthPresetName(int index);
SynthSettings        synthPresetSettings(int index);
const char*          synthWaveName(int wave);

class Synth {
public:
    Synth();
    ~Synth();

    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    // Chiamate dal thread audio mentre il dispositivo e' fermo.
    bool prepare(int sampleRate, int blockSize);
    void releaseProcessing();
    bool prepared() const;

    // Somma l'uscita stereo interleaved del synth nel buffer di mix.
    void renderAdd(float* out, int frames);

    // Eventi MIDI (thread-safe: accodati e consumati dal thread audio).
    void noteOn(int pitch, int velocity);
    void noteOff(int pitch);
    void controlChange(int cc, int value);
    void allNotesOff();

    // Parametri (thread-safe, effetto dal prossimo blocco audio).
    void          applySettings(const SynthSettings& s);
    SynthSettings settings() const;

    // Ultimi campioni prodotti dal synth (per l'oscilloscopio). n <= 512.
    void copyScope(float* dst, int n) const;
    int  activeVoices() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
