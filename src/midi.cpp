#include "midi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>

#if __has_include(<rtmidi/RtMidi.h>)
#include <rtmidi/RtMidi.h>
#elif __has_include(<RtMidi.h>)
#include <RtMidi.h>
#else
#error "RtMidi.h non trovato"
#endif

namespace {

// Coda single-producer / single-consumer senza lock.
struct SpscRing {
    static constexpr size_t N = 8192; // potenza di due

    RawMidi buf[N];
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    bool push(const RawMidi& e) {
        const size_t h  = head.load(std::memory_order_relaxed);
        const size_t nh = (h + 1) & (N - 1);
        if (nh == tail.load(std::memory_order_acquire)) return false; // pieno: scarto
        buf[h] = e;
        head.store(nh, std::memory_order_release);
        return true;
    }

    bool pop(RawMidi& out) {
        const size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false; // vuoto
        out = buf[t];
        tail.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }
};

} // namespace

struct MidiInput::Impl {
    RtMidiIn*  in = nullptr;
    SpscRing   ring;
    std::string name;
    Handler    handler;

    static void callback(double /*dt*/, std::vector<unsigned char>* msg, void* ud) {
        auto* self = static_cast<Impl*>(ud);
        if (!msg || msg->empty()) return;
        RawMidi e;
        e.ts     = MidiInput::now();
        e.status = (*msg)[0];
        e.d1     = msg->size() > 1 ? (*msg)[1] : 0;
        e.d2     = msg->size() > 2 ? (*msg)[2] : 0;
        if (self->handler) self->handler(e); // immediato (thread RtMidi)
        self->ring.push(e);                  // per la grafica (thread principale)
    }
};

MidiInput::MidiInput() {
    impl_ = new Impl();
    try {
        impl_->in = new RtMidiIn(RtMidi::UNSPECIFIED, "fMidiKeyboard");
        impl_->in->ignoreTypes(true, true, true); // sysex, timing, active sensing
        impl_->in->setCallback(&Impl::callback, impl_);
    } catch (const std::exception& e) {
        delete impl_->in;
        impl_->in = nullptr;
        std::fprintf(stderr, "Init MIDI: %s\n", e.what());
    }
}

MidiInput::~MidiInput() {
    closePort();
    delete impl_->in;
    delete impl_;
}

std::vector<std::string> MidiInput::listPorts() {
    std::vector<std::string> out;
    if (!impl_->in) return out;
    try {
        const unsigned n = impl_->in->getPortCount();
        out.reserve(n);
        for (unsigned i = 0; i < n; ++i) out.push_back(impl_->in->getPortName(i));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "listPorts: %s\n", e.what());
    }
    return out;
}

bool MidiInput::openPort(int index) {
    if (!impl_->in || index < 0) return false;
    try {
        if (impl_->in->isPortOpen()) impl_->in->closePort();
        impl_->in->openPort(static_cast<unsigned>(index));
        impl_->name = impl_->in->getPortName(static_cast<unsigned>(index));
        return impl_->in->isPortOpen();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "openPort: %s\n", e.what());
        return false;
    }
}

void MidiInput::closePort() {
    if (!impl_ || !impl_->in) return;
    try {
        if (impl_->in->isPortOpen()) impl_->in->closePort();
    } catch (...) {
    }
}

bool MidiInput::isOpen() const {
    if (!impl_ || !impl_->in) return false;
    try {
        return impl_->in->isPortOpen();
    } catch (...) {
        return false;
    }
}

std::string MidiInput::currentName() const {
    return impl_ ? impl_->name : std::string();
}

bool MidiInput::poll(RawMidi& out) {
    return impl_->ring.pop(out);
}

void MidiInput::setHandler(Handler h) {
    if (impl_) impl_->handler = std::move(h);
}

double MidiInput::now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}
