#pragma once

#include <string>

enum Notation { NOTATION_EN = 0, NOTATION_IT = 1 };

// Nome della nota nella notazione scelta. 60 -> "C4" (EN) / "Do4" (IT).
std::string noteName(int pitch, int notation);

// Scrive una classe di altezza (0..11) come nome senza ottava.
std::string spellPitchClass(int pc, int notation);

struct ChordInfo {
    bool        valid  = false; // accordo riconosciuto
    int         root   = -1;    // classe di altezza della fondamentale
    int         bass   = -1;    // classe di altezza della nota piu' grave
    std::string suffix;         // es. "m7"
    std::string name;           // nome completo (con /basso)
    std::string noteList;       // es. "Do4 Mib4 Sol4 Sib4"
    int         count  = 0;     // numero di note distinte
};

// Analizza le note suonanti e produce il nome dell'accordo.
ChordInfo analyzeChord(const bool sounding[128], int notation);
