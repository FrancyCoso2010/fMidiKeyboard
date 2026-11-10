#pragma once

// Pianoforte sintetizzato (sintesi additiva modale) senza campioni: nessun file
// da scaricare, suono immediato. Modella l'inharmonicita' delle corde, il
// decadimento per armonica, l'accordo leggermente "battente" delle corde
// multiple, il rumore del martelletto e lo smorzatore (pedale di risonanza).
class PianoSynth {
public:
    PianoSynth();
    ~PianoSynth();

    void prepare(int sampleRate);
    void reset();

    void noteOn(int pitch, int velocity);
    void noteOff(int pitch);
    void controlChange(int cc, int value); // 64 sustain, 120/123 all off
    void renderAdd(float* out, int frames);

    int activeVoices() const;

private:
    struct Impl;
    Impl* impl_;
};
