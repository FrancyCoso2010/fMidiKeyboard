#include "chord.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

static const char* kEn[12] = {"C", "Db", "D", "Eb", "E", "F",
                              "Gb", "G", "Ab", "A", "Bb", "B"};
static const char* kIt[12] = {"Do", "Reb", "Re", "Mib", "Mi", "Fa",
                              "Solb", "Sol", "Lab", "La", "Sib", "Si"};

static const char* pcName(int pc, int notation) {
    return notation == NOTATION_IT ? kIt[pc] : kEn[pc];
}

std::string spellPitchClass(int pc, int notation) {
    return pcName(((pc % 12) + 12) % 12, notation);
}

std::string noteName(int pitch, int notation) {
    if (pitch < 0 || pitch > 127) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s%d", pcName(pitch % 12, notation),
                  pitch / 12 - 1);
    return buf;
}

namespace {

struct ChordTemplate {
    const char*      suffix;
    std::vector<int> intervals;
};

// Tabella il piu' completa possibile. La quinta (7) puo' essere omessa.
const std::vector<ChordTemplate>& templates() {
    static const std::vector<ChordTemplate> t = {
        // --- triadi ---
        {"",         {0, 4, 7}},
        {"m",        {0, 3, 7}},
        {"dim",      {0, 3, 6}},
        {"aug",      {0, 4, 8}},
        {"5",        {0, 7}},
        {"sus2",     {0, 2, 7}},
        {"sus4",     {0, 5, 7}},
        // --- seste ---
        {"6",        {0, 4, 7, 9}},
        {"m6",       {0, 3, 7, 9}},
        {"6/9",      {0, 2, 4, 7, 9}},
        // --- settime ---
        {"7",        {0, 4, 7, 10}},
        {"maj7",     {0, 4, 7, 11}},
        {"m7",       {0, 3, 7, 10}},
        {"dim7",     {0, 3, 6, 9}},
        {"m7b5",     {0, 3, 6, 10}},
        {"mMaj7",    {0, 3, 7, 11}},
        {"aug7",     {0, 4, 8, 10}},
        {"7b5",      {0, 4, 6, 10}},
        {"maj7b5",   {0, 4, 6, 11}},
        {"maj7#5",   {0, 4, 8, 11}},
        {"dimMaj7",  {0, 3, 6, 11}},
        {"7sus4",    {0, 5, 7, 10}},
        {"7sus2",    {0, 2, 7, 10}},
        // --- note aggiunte ---
        {"add9",     {0, 2, 4, 7}},
        {"add11",    {0, 4, 5, 7}},
        {"add13",    {0, 4, 7, 9}},
        {"madd9",    {0, 2, 3, 7}},
        {"madd11",   {0, 3, 5, 7}},
        // --- none ---
        {"9",        {0, 2, 4, 7, 10}},
        {"maj9",     {0, 2, 4, 7, 11}},
        {"m9",       {0, 2, 3, 7, 10}},
        {"9sus4",    {0, 2, 5, 7, 10}},
        {"7b9",      {0, 1, 4, 7, 10}},
        {"7#9",      {0, 3, 4, 7, 10}},
        {"9#11",     {0, 2, 4, 6, 7, 10}},
        {"maj9#11",  {0, 2, 4, 6, 7, 11}},
        // --- undicesime ---
        {"11",       {0, 2, 4, 5, 7, 10}},
        {"maj11",    {0, 2, 4, 5, 7, 11}},
        {"m11",      {0, 2, 3, 5, 7, 10}},
        {"7#11",     {0, 2, 4, 6, 7, 10}},
        {"maj7#11",  {0, 2, 4, 6, 7, 11}},
        // --- tredicesime ---
        {"13",       {0, 2, 4, 7, 9, 10}},
        {"maj13",    {0, 2, 4, 7, 9, 11}},
        {"m13",      {0, 2, 3, 7, 9, 10}},
        {"13sus4",   {0, 2, 5, 7, 9, 10}},
        {"7b13",     {0, 4, 7, 8, 10}},
        {"7#11b13",  {0, 4, 6, 8, 10}},
        // --- alterati (dominanti) ---
        {"7b9b5",    {0, 1, 4, 6, 10}},
        {"7#5b9",    {0, 1, 4, 8, 10}},
        {"7b9#11",   {0, 1, 4, 6, 7, 10}},
        {"7#9#11",   {0, 3, 4, 6, 7, 10}},
        // --- quartali / altro ---
        {"sus4/add9",{0, 2, 5, 7}},
        {"m(maj9)",  {0, 2, 3, 7, 11}},
    };
    return t;
}

// 0 = non riconosciuto; altrimenti punteggio. "essenziale" = tutto tranne la 5a.
struct Match {
    bool  ok      = false;
    int   score   = -1000000;
    int   size    = 0;
    const ChordTemplate* tpl = nullptr;
};

Match bestMatch(const std::vector<int>& played, const std::array<bool, 12>& isPlayed) {
    Match best;
    for (const auto& tpl : templates()) {
        int matched = 0;
        int missing = 0;
        bool ok = true;

        // tutte le note suonate devono appartenere all'accordo
        for (int iv : played) {
            if (std::find(tpl.intervals.begin(), tpl.intervals.end(), iv) ==
                tpl.intervals.end()) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        // conta le note presenti/mancanti dell'accordo
        for (int iv : tpl.intervals) {
            if (isPlayed[iv]) {
                ++matched;
            } else {
                // si puo' omettere solo la quinta (7)
                if (iv != 7) {
                    ok = false;
                    break;
                }
                ++missing;
            }
        }
        if (!ok) continue;

        const int score = matched * 100 - missing * 10 +
                          static_cast<int>(tpl.intervals.size());
        if (score > best.score) {
            best.ok    = true;
            best.score = score;
            best.size  = static_cast<int>(tpl.intervals.size());
            best.tpl   = &tpl;
        }
    }
    return best;
}

} // namespace

ChordInfo analyzeChord(const bool sounding[128], int notation) {
    ChordInfo info;

    std::array<bool, 12> pc = {};
    int bass = -1;
    for (int p = 0; p < 128; ++p) {
        if (!sounding[p]) continue;
        if (bass < 0) bass = p;
        if (!pc[p % 12]) {
            pc[p % 12] = true;
            ++info.count;
        }
    }
    if (info.count == 0) return info;

    info.bass = bass % 12;

    std::string list;
    for (int p = 0; p < 128; ++p) {
        if (!sounding[p]) continue;
        if (!list.empty()) list += " ";
        list += noteName(p, notation);
    }
    info.noteList = list;

    if (info.count == 1) {
        info.name = noteName(bass, notation);
        return info;
    }

    std::vector<int> roots;
    roots.push_back(bass % 12);
    for (int i = 0; i < 12; ++i)
        if (pc[i] && i != bass % 12) roots.push_back(i);

    for (int root : roots) {
        std::vector<int> played;
        for (int i = 0; i < 12; ++i)
            if (pc[i]) played.push_back(((i - root) % 12 + 12) % 12);
        std::sort(played.begin(), played.end());

        std::array<bool, 12> isPlayed = {};
        for (int iv : played) isPlayed[iv] = true;

        const Match m = bestMatch(played, isPlayed);
        if (!m.ok) continue;

        info.valid  = true;
        info.root   = root;
        info.suffix = m.tpl->suffix;
        info.name   = spellPitchClass(root, notation) + m.tpl->suffix;
        if (info.bass != root)
            info.name += "/" + spellPitchClass(info.bass, notation);
        return info;
    }

    // non riconosciuto: elenca le note
    info.name = info.noteList;
    return info;
}
