#pragma once

#include <string>

// Motore di campionamento interno: suona strumenti SFZ (sfizz) e Decent Sampler
// (.dspreset) senza bisogno di plugin VST3. Carica i campioni WAV in memoria e
// li riproduce con pitch-shifting (resampling lineare) e inviluppo ADSR.
// Include anche due strumenti sintetizzati: il pianoforte acustico e il piano
// elettrico Rhodes, pronti senza alcun campione.
class Sampler {
public:
    Sampler();
    ~Sampler();

    // Carica uno strumento: riconosce .sfz e .dspreset in base all'estensione.
    // Il percorso speciale ":piano" carica il pianoforte sintetizzato integrato.
    bool load(const std::string& path);
    // Carica il pianoforte sintetizzato integrato (nessun campione richiesto).
    bool loadBuiltinPiano();
    // Carica il piano elettrico Rhodes sintetizzato (nessun campione richiesto).
    bool loadBuiltinRhodes();
    bool isBuiltinPiano() const;
    bool isBuiltinRhodes() const;
    bool isBuiltin() const;
    void unload();
    bool loaded() const;

    std::string instrumentName() const;
    std::string instrumentPath() const;
    int  regionCount() const;
    int  sampleCount() const;
    std::string lastError() const;

    // Rendering: il dispositivo e' gestito da AudioEngine. prepare() prepara il
    // motore alla frequenza di campionamento effettiva; renderAdd() somma
    // l'uscita stereo interleaved nel buffer di mix.
    bool prepare(int sampleRate, int blockSize);
    void releaseProcessing();
    bool prepared() const;
    void renderAdd(float* out, int frames);

    void noteOn(int pitch, int velocity);
    void noteOff(int pitch);
    void controlChange(int cc, int value);
    void allNotesOff();

private:
    struct Impl;
    Impl* impl_;
};
