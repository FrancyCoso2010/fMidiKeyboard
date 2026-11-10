#pragma once

#include <list>

// Un blocco-notes da disegnare a cascata.
struct NoteBlock {
    int    pitch = 0;      // 0..127
    int    channel = 0;
    float  velocity = 0.f; // 0..1
    double start = 0.0;    // secondi
    double end = -1.0;     // secondi, < 0 finche' il tasto e' premuto
};

// Tiene traccia dello stato di tasti e pedale, e produce i blocchi da animare.
class NoteTracker {
public:
    bool  pressed[128]     = {};
    bool  sounding[128]    = {};
    float keyVelocity[128] = {};
    bool  sustain = false;

    void noteOn(double t, int pitch, int velocity, int channel);
    void noteOff(double t, int pitch);
    void controlChange(double t, int cc, int value);
    void panic(double t);

    // Elimina i blocchi conclusi piu' vecchi del tempo di taglio.
    void prune(double cutoff);

    const std::list<NoteBlock>& blocks() const { return blocks_; }

private:
    std::list<NoteBlock> blocks_;
    NoteBlock* open_[128] = {};

    void endNote(double t, int pitch);
};
