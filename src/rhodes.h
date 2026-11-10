#pragma once

// Pianoforte elettrico Rhodes, sintetizzato senza campioni.
//
// Catena per voce: modulazione di frequenza (portante + modulatore) con indice
// che decade rapidamente: nasce il "bark" metallico dell'attacco e resta un
// corpo quasi sinusoidale, come sul tine vero. In piu' un parziale acuto
// (tine/pickup) con decadimento fulmineo, il rumore del martelletto, lo
// smorzatore a rilascio del tasto e un tremolo stereo leggerissimo in stile
// "suitcase". Il decadimento e' per nota: lungo nei bassi, corto negli acuti.
class RhodesPiano {
public:
    RhodesPiano();
    ~RhodesPiano();

    RhodesPiano(const RhodesPiano&) = delete;
    RhodesPiano& operator=(const RhodesPiano&) = delete;

    void prepare(int sampleRate);
    void reset();

    void noteOn(int pitch, int velocity);
    void noteOff(int pitch);
    void controlChange(int cc, int value); // 64 sustain, 120/123 all off
    void renderAdd(float* out, int frames);

    int activeVoices() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
