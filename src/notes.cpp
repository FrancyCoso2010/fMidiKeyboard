#include "notes.h"

void NoteTracker::endNote(double t, int pitch) {
    // Chiude il blocco: la lunghezza dipende SOLO dal tempo di pressione,
    // indipendentemente dal pedale sustain.
    if (open_[pitch]) {
        open_[pitch]->end = t;
        open_[pitch] = nullptr;
    }
}

void NoteTracker::noteOn(double t, int pitch, int velocity, int channel) {
    if (pitch < 0 || pitch > 127) return;
    if (pressed[pitch]) endNote(t, pitch); // retrigger

    pressed[pitch]     = true;
    sounding[pitch]    = true;
    keyVelocity[pitch] = velocity / 127.0f;

    blocks_.push_back(NoteBlock{pitch, channel, velocity / 127.0f, t, -1.0});
    open_[pitch] = &blocks_.back();
}

void NoteTracker::noteOff(double t, int pitch) {
    if (pitch < 0 || pitch > 127) return;
    pressed[pitch]  = false;

    // Il blocco termina sempre al rilascio del tasto.
    endNote(t, pitch);

    // Il tasto NON resta evidenziato quando il pedale e' premuto:
    // il sustain viene inoltrato al sintetizzatore ma non ha effetto visivo.
    sounding[pitch] = false;
}

void NoteTracker::controlChange(double t, int cc, int value) {
    (void)t;
    if (cc == 64) { // sustain pedal: memorizzato solo per l'audio
        sustain = value >= 64;
    } else if (cc == 120 || cc == 123) { // all sound off / all notes off
        panic(t);
    }
}

void NoteTracker::panic(double t) {
    for (int p = 0; p < 128; ++p) {
        if (open_[p]) {
            open_[p]->end = t;
            open_[p] = nullptr;
        }
        pressed[p]  = false;
        sounding[p] = false;
    }
    sustain = false;
}

void NoteTracker::prune(double cutoff) {
    // std::list: annullare nodi non invalida i puntatori aperti in open_[].
    blocks_.remove_if([cutoff](const NoteBlock& b) {
        return b.end >= 0.0 && b.end < cutoff;
    });
}
