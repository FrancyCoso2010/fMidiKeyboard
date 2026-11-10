#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Evento MIDI grezzo con timestamp (secondi monotoni).
struct RawMidi {
    double  ts = 0.0;
    uint8_t status = 0;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
};

// Ingresso MIDI basato su RtMidi.
// La callback di RtMidi gira su un thread separato: gli eventi vengono passati
// al thread principale tramite un ring buffer lock-free (SPSC).
class MidiInput {
public:
    MidiInput();
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    std::vector<std::string> listPorts();
    bool openPort(int index);
    void closePort();
    bool isOpen() const;
    std::string currentName() const;

    // Restituisce true se c'e' un evento disponibile.
    bool poll(RawMidi& out);

    // Handler opzionale invocato direttamente sul thread di RtMidi non appena
    // arriva un evento (oltre all'accodamento nel ring per il thread principale).
    // Serve a far suonare lo strumento anche se il thread grafico e' occupato.
    // Passare nullptr per rimuoverlo.
    using Handler = std::function<void(const RawMidi&)>;
    void setHandler(Handler h);

    // Orologio monotono condiviso (secondi dall'avvio).
    static double now();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
