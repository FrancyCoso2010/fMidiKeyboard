#include "vsthost.h"

#if !defined(FMK_HAVE_VST3)

// --------------------------------------------------------------- stub ---------
VstHost::VstHost() {}
VstHost::~VstHost() {}
std::vector<std::string> VstHost::scanPlugins() { return {}; }
std::string VstHost::resolveModule(const std::string&) { return {}; }
bool VstHost::available() const { return false; }
bool VstHost::load(const std::string&) { return false; }
void VstHost::unload() {}
bool VstHost::loaded() const { return false; }
std::string VstHost::pluginName() const { return {}; }
std::string VstHost::pluginPath() const { return {}; }
std::string VstHost::lastError() const { return "VST3 non abilitato in questa build."; }
bool VstHost::prepare(int, int) { return false; }
void VstHost::releaseProcessing() {}
bool VstHost::prepared() const { return false; }
void VstHost::renderAdd(float*, int) {}
bool VstHost::saveState(const std::string&) const { return false; }
bool VstHost::loadState(const std::string&) { return false; }
bool VstHost::hasEditor() const { return false; }
bool VstHost::openEditor() { return false; }
void VstHost::closeEditor() {}
bool VstHost::editorOpen() const { return false; }
void VstHost::onEditorResized() {}
unsigned int VstHost::editorWindowId() const { return 0; }
void VstHost::noteOn(int, int) {}
void VstHost::noteOff(int) {}
void VstHost::controlChange(int, int) {}
void VstHost::allNotesOff() {}

#else // ---------------------------------------------------------- reale -------

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32) || defined(__APPLE__) || defined(SDL_VIDEO_DRIVER_X11)
#include <SDL_syswm.h>
#define FMK_HAVE_SYSWM 1
#endif

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

namespace fs = std::filesystem;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ------------------------------------------------------------- eventi MIDI ----
struct MidiEv {
    uint8_t type = 0;
    uint8_t d1   = 0;
    uint8_t d2   = 0;
};

// Coda SPSC lock-free: producer = thread UI, consumer = thread audio.
class MidiQueue {
public:
    void push(const MidiEv& e) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t n = (h + 1) % kCap;
        if (n == tail_.load(std::memory_order_acquire)) return; // piena
        buf_[h] = e;
        head_.store(n, std::memory_order_release);
    }
    bool pop(MidiEv& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t];
        tail_.store((t + 1) % kCap, std::memory_order_release);
        return true;
    }
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }
private:
    static const size_t kCap = 4096;    MidiEv        buf_[kCap];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// --------------------------------------------------------------- host glue ----
class HostApp : public IHostApplication {
public:
    virtual ~HostApp() = default;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IHostApplication::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    tresult PLUGIN_API getName(String128 name) override {
        UString(name, 128).fromAscii("fMidiKeyboard");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
        *obj = nullptr;
        return kNoInterface;
    }
private:
    std::atomic<uint32> ref_{1};
};

class CompHandler : public IComponentHandler {
public:
    virtual ~CompHandler() = default;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    // Le modifiche fatte nell'editor del plugin vanno inoltrate al processore
    // come parameter change: senza questo molte UI non hanno effetto sul suono.
    void setTarget(ParameterChangeTransfer* t) { target_ = t; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        if (target_) target_->addChange(id, v, 0);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
private:
    ParameterChangeTransfer* target_ = nullptr;
    std::atomic<uint32> ref_{1};
};

// --------------------------------------------------------------- scanning -----
const char* currentArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#else
    return "";
#endif
}

// Raccoglie i moduli caricabili dentro una cartella (ricorsivo, profondita' limitata).
void collectModules(const fs::path& dir, int depth, std::vector<std::string>& out) {
    if (depth < 0) return;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            collectModules(p, depth - 1, out);
        } else if (it->is_regular_file(ec)) {
#if defined(__APPLE__)
            out.push_back(p.string());   // modulo macOS senza estensione
#else
            std::string ext = p.extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(c));
#if defined(_WIN32)
            const bool ok = (ext == ".vst3" || ext == ".dll");
#else
            const bool ok = (ext == ".so");   // Linux: solo shared object
#endif
            if (ok) out.push_back(p.string());
#endif
        }
    }
}

std::string pickByArch(const std::vector<std::string>& cands) {
    if (cands.empty()) return {};
    const char* arch = currentArch();
    if (arch[0]) {
        for (const auto& c : cands)
            if (c.find(arch) != std::string::npos) return c;
    }
    return cands.front();
}

// Trova il modulo caricabile di un bundle .vst3 (cartella o file singolo).
std::string scanBundle(const fs::path& bundle) {
    std::error_code ec;
    if (fs::is_regular_file(bundle, ec)) return bundle.string();
    if (!fs::is_directory(bundle, ec)) return {};

    std::vector<std::string> cands;

#if defined(__APPLE__)
    const fs::path bin = bundle / "Contents" / "MacOS";
    if (fs::is_directory(bin, ec)) collectModules(bin, 1, cands);
#else
    const fs::path contents = bundle / "Contents";
    if (fs::is_directory(contents, ec))
        collectModules(contents, 3, cands);
    else
        collectModules(bundle, 2, cands);   // bundle "portabile" senza Contents
#endif
    return pickByArch(cands);
}

// Percorso che il loader VST3 si aspetta:
//  - Linux/macOS: la cartella bundle "Nome.vst3" (il .so interno lo ricava da se);
//  - Windows: il file modulo (Nome.vst3).
std::string moduleLoadPath(const fs::path& in) {
    std::error_code ec;
#if defined(_WIN32)
    if (fs::is_regular_file(in, ec)) return in.string();
    std::vector<std::string> cands;
    if (fs::is_directory(in, ec)) collectModules(in, 3, cands);
    return pickByArch(cands);
#else
    fs::path cur = in;
    if (fs::is_regular_file(cur, ec)) cur = cur.parent_path();
    for (fs::path p = cur; !p.empty(); p = p.parent_path()) {
        if (p.extension() == ".vst3") return p.string();
        if (p == p.parent_path()) break;
    }
    return in.string();
#endif
}

// ---------------------------------------------------------------- stato --------
// IBStream in memoria per getState/setState dei plugin.
class MemStream : public IBStream {
public:
    MemStream() = default;
    explicit MemStream(std::vector<uint8_t> d) : data_(std::move(d)) {}
    virtual ~MemStream() = default;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IBStream::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const int32 avail = static_cast<int32>(data_.size() - pos_);
        const int32 n = (avail > 0) ? std::min(numBytes, avail) : 0;
        if (n > 0) { std::memcpy(buffer, data_.data() + pos_, n); pos_ += n; }
        if (numBytesRead) *numBytesRead = n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        if (pos_ + static_cast<size_t>(numBytes) > data_.size())
            data_.resize(pos_ + numBytes);
        std::memcpy(data_.data() + pos_, buffer, numBytes);
        pos_ += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result = nullptr) override {
        int64 np;
        switch (mode) {
            case kIBSeekSet: np = pos; break;
            case kIBSeekCur: np = static_cast<int64>(pos_) + pos; break;
            case kIBSeekEnd: np = static_cast<int64>(data_.size()) + pos; break;
            default: return kInvalidArgument;
        }
        if (np < 0) np = 0;
        pos_ = static_cast<size_t>(np);
        if (result) *result = np;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = static_cast<int64>(pos_);
        return kResultOk;
    }

    const std::vector<uint8_t>& bytes() const { return data_; }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    std::atomic<uint32> ref_{1};
};

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// ------------------------------------------------------------------ Impl ------
struct VstHost::Impl {
    VST3::Hosting::Module::Ptr module;
    IPtr<IComponent>       component;
    IPtr<IEditController>  controller;
    IPtr<IAudioProcessor>  processor;

    HostApp*     hostApp  = nullptr;
    CompHandler* handler  = nullptr;

    EventList        inEvents{256};
    ParameterChanges inParams;
    ParameterChanges outParams;
    ParameterChangeTransfer paramTransfer{256};

    ProcessData   data{};
    ProcessContext ctx{};

    int      numIn = 0;
    int      numOut = 0;
    int32    inChannels[8]  = {};
    int32    outChannels[8] = {};
    float*   inBuf[8][8]    = {};
    float*   outBuf[8][8]   = {};

    double   sampleRate  = 48000.0;
    int      blockSize   = 512;
    TSamples samplePos   = 0;

    bool processing = false;
    bool active     = false;

    std::string name;
    std::string path;
    std::string error;

    MidiQueue queue;

    // Note attualmente suonate (bitmask 0..127): usato per spegnere in modo
    // affidabile tutte le note durante il panic.
    std::atomic<uint64_t> onMaskA{0}, onMaskB{0};

    void markNoteOn(int pitch);
    void markNoteOff(int pitch);
    bool isNoteOn(int pitch) const;
    void clearOnMask();

    // --- editor grafico ---
    IPtr<IPlugView> view;
    IPtr<IPlugFrame> frame;
    SDL_Window* editorWin = nullptr;
    bool attached = false;

    struct PlugFrame : public IPlugFrame {
        Impl* owner;
        explicit PlugFrame(Impl* o) : owner(o) {}
        virtual ~PlugFrame() = default;
        tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
            if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
                FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
                *obj = this;
                addRef();
                return kResultOk;
            }
            *obj = nullptr;
            return kNoInterface;
        }
        uint32 PLUGIN_API addRef() override { return ++ref_; }
        uint32 PLUGIN_API release() override {
            const uint32 r = --ref_;
            if (r == 0) delete this;
            return r;
        }
        tresult PLUGIN_API resizeView(IPlugView* v, ViewRect* newSize) override {
            if (owner->editorWin && newSize)
                SDL_SetWindowSize(owner->editorWin, newSize->getWidth(),
                                  newSize->getHeight());
            if (v && newSize) v->onSize(newSize);
            return kResultOk;
        }
        std::atomic<uint32> ref_{1};
    };

    void clearBuffers();
    void freeBuffers();
    bool configure(double sr, int block);
    void deactivate();
    void renderAdd(float* out, int frames);
};

void VstHost::Impl::freeBuffers() {
    for (int b = 0; b < 8; ++b)
        for (int c = 0; c < 8; ++c) {
            delete[] inBuf[b][c];
            delete[] outBuf[b][c];
            inBuf[b][c]  = nullptr;
            outBuf[b][c] = nullptr;
        }
}

void VstHost::Impl::markNoteOn(int pitch) {
    if (pitch >= 0 && pitch < 64)
        onMaskA.fetch_or(uint64_t(1) << pitch, std::memory_order_relaxed);
    else if (pitch >= 64 && pitch < 128)
        onMaskB.fetch_or(uint64_t(1) << (pitch - 64), std::memory_order_relaxed);
}
void VstHost::Impl::markNoteOff(int pitch) {
    if (pitch >= 0 && pitch < 64)
        onMaskA.fetch_and(~(uint64_t(1) << pitch), std::memory_order_relaxed);
    else if (pitch >= 64 && pitch < 128)
        onMaskB.fetch_and(~(uint64_t(1) << (pitch - 64)), std::memory_order_relaxed);
}
bool VstHost::Impl::isNoteOn(int pitch) const {
    if (pitch >= 0 && pitch < 64)
        return (onMaskA.load(std::memory_order_relaxed) >> pitch) & 1u;
    if (pitch >= 64 && pitch < 128)
        return (onMaskB.load(std::memory_order_relaxed) >> (pitch - 64)) & 1u;
    return false;
}
void VstHost::Impl::clearOnMask() {
    onMaskA.store(0, std::memory_order_relaxed);
    onMaskB.store(0, std::memory_order_relaxed);
}

void VstHost::Impl::clearBuffers() {
    for (int b = 0; b < numOut && b < 8; ++b)
        for (int c = 0; c < outChannels[b] && c < 8; ++c)
            if (outBuf[b][c])
                std::memset(outBuf[b][c], 0, sizeof(float) * blockSize);
}

bool VstHost::Impl::configure(double sr, int block) {
    if (!component || !processor) return false;
    sampleRate = sr;
    blockSize  = block;

    numIn  = component->getBusCount(kAudio, kInput);
    numOut = component->getBusCount(kAudio, kOutput);
    if (numIn > 8) numIn = 8;
    if (numOut > 8) numOut = 8;
    if (numOut < 1) {
        error = "Il plugin non ha uscite audio.";
        return false;
    }

    // Arrangiamento bus: uscita principale stereo, ingressi vuoti.
    std::vector<SpeakerArrangement> inA(numIn, SpeakerArr::kEmpty);
    std::vector<SpeakerArrangement> outA(numOut, SpeakerArr::kEmpty);
    outA[0] = SpeakerArr::kStereo;
    processor->setBusArrangements(numIn ? inA.data() : nullptr, numIn,
                                  outA.data(), numOut);

    for (int i = 0; i < numOut; ++i) {
        SpeakerArrangement a = SpeakerArr::kEmpty;
        processor->getBusArrangement(kOutput, i, a);
        outChannels[i] = static_cast<int32>(SpeakerArr::getChannelCount(a));
        if (i == 0 && outChannels[i] < 1) outChannels[i] = 2;
    }
    for (int i = 0; i < numIn; ++i) {
        SpeakerArrangement a = SpeakerArr::kEmpty;
        processor->getBusArrangement(kInput, i, a);
        inChannels[i] = static_cast<int32>(SpeakerArr::getChannelCount(a));
    }

    // Attiva i bus secondo il default del plugin.
    for (int i = 0; i < numIn; ++i) {
        BusInfo info{};
        component->getBusInfo(kAudio, kInput, i, info);
        component->activateBus(kAudio, kInput, i, (info.flags & BusInfo::kDefaultActive) != 0);
    }
    for (int i = 0; i < numOut; ++i)
        component->activateBus(kAudio, kOutput, i, true);

    ProcessSetup setup{};
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate         = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk) {
        error = "setupProcessing non riuscito.";
        return false;
    }

    freeBuffers();
    for (int b = 0; b < numIn && b < 8; ++b)
        for (int c = 0; c < inChannels[b] && c < 8; ++c)
            inBuf[b][c] = new float[blockSize]();
    for (int b = 0; b < numOut && b < 8; ++b)
        for (int c = 0; c < outChannels[b] && c < 8; ++c)
            outBuf[b][c] = new float[blockSize]();

    component->setActive(true);
    active = true;
    processor->setProcessing(true);
    processing = true;

    ctx = ProcessContext{};
    ctx.sampleRate = sampleRate;
    ctx.tempo      = 120.0;
    ctx.timeSigNumerator   = 4;
    ctx.timeSigDenominator = 4;
    ctx.state = ProcessContext::kPlaying | ProcessContext::kTempoValid |
                ProcessContext::kTimeSigValid;
    samplePos = 0;
    return true;
}

void VstHost::Impl::deactivate() {
    if (processor && processing) processor->setProcessing(false);
    processing = false;
    if (component && active) component->setActive(false);
    active = false;
    clearBuffers();
}

void VstHost::Impl::renderAdd(float* out, int frames) {
    if (!processing || frames <= 0) return;
    const int n = std::min(frames, blockSize);

    inEvents.clear();
    inParams.clearQueue();
    outParams.clearQueue();

    // Modifiche dei parametri fatte nell'editor del plugin (thread UI).
    paramTransfer.transferChangesTo(inParams);

    MidiEv ev;
    while (queue.pop(ev)) {
        Event e{};
        const uint8_t type = ev.type & 0xF0;
        if (type == 0x90 && ev.d2 > 0) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel  = 0;
            e.noteOn.pitch    = static_cast<int16>(ev.d1);
            e.noteOn.velocity = static_cast<float>(ev.d2) / 127.0f;
            e.noteOn.length   = 0;
            e.noteOn.noteId   = -1;
        } else if (type == 0x80 || (type == 0x90 && ev.d2 == 0)) {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel  = 0;
            e.noteOff.pitch    = static_cast<int16>(ev.d1);
            e.noteOff.velocity = static_cast<float>(ev.d2) / 127.0f;
            e.noteOff.noteId   = -1;
        } else if (type == 0xB0) {
            e.type = Event::kLegacyMIDICCOutEvent;
            e.midiCCOut.controlNumber = ev.d1;
            e.midiCCOut.channel       = 0;
            e.midiCCOut.value         = static_cast<int8>(ev.d2);
            e.midiCCOut.value2        = 0;
        } else {
            continue;
        }
        inEvents.addEvent(e);
    }

    clearBuffers();

    AudioBusBuffers inBus[8]{};
    AudioBusBuffers outBus[8]{};
    for (int b = 0; b < numIn && b < 8; ++b) {
        inBus[b].numChannels     = inChannels[b];
        inBus[b].silenceFlags    = inChannels[b] ? ((1ull << inChannels[b]) - 1) : 0;
        inBus[b].channelBuffers32 = inBuf[b];
    }
    for (int b = 0; b < numOut && b < 8; ++b) {
        outBus[b].numChannels      = outChannels[b];
        outBus[b].silenceFlags     = 0;
        outBus[b].channelBuffers32 = outBuf[b];
    }

    data.processMode        = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples         = n;
    data.numInputs          = numIn;
    data.numOutputs         = numOut;
    data.inputs             = inBus;
    data.outputs            = outBus;
    data.inputParameterChanges  = &inParams;
    data.outputParameterChanges = &outParams;
    data.inputEvents        = &inEvents;
    data.outputEvents       = nullptr;
    data.processContext     = &ctx;

    ctx.projectTimeSamples    = samplePos;
    ctx.continousTimeSamples = static_cast<double>(samplePos);
    ctx.projectTimeMusic      = samplePos / sampleRate * (ctx.tempo / 60.0);

    processor->process(data);

    // somma le uscite stereo (interleaved) nel bus di mix del motore audio
    const float* l = (outChannels[0] > 0) ? outBuf[0][0] : nullptr;
    const float* r = (outChannels[0] > 1) ? outBuf[0][1] : l;
    for (int i = 0; i < n; ++i) {
        out[i * 2]     += (l ? l[i] : 0.0f);
        out[i * 2 + 1] += (r ? r[i] : 0.0f);
    }
    samplePos += n;
}

// ---------------------------------------------------------------- public -------
VstHost::VstHost() : impl_(new Impl) {}
VstHost::~VstHost() {
    releaseProcessing();
    unload();
    delete impl_;
}

bool VstHost::available() const { return true; }

std::vector<std::string> VstHost::scanPlugins() {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::vector<fs::path> dirs;

    if (const char* env = std::getenv("VST3_PATH")) {
        std::string s(env);
        size_t p = 0, q;
        while ((q = s.find(':', p)) != std::string::npos) {
            if (q > p) dirs.emplace_back(s.substr(p, q - p));
            p = q + 1;
        }
        if (p < s.size()) dirs.emplace_back(s.substr(p));
    }
    if (const char* home = std::getenv("HOME"))
        dirs.emplace_back(fs::path(home) / ".vst3");
    dirs.emplace_back("/usr/lib/vst3");
    dirs.emplace_back("/usr/lib64/vst3");
    dirs.emplace_back("/usr/local/lib/vst3");
    dirs.emplace_back("/usr/local/lib64/vst3");

    std::error_code ec;
    for (const auto& d : dirs) {
        if (!fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec)) {
            const std::string nm = e.path().filename().string();
            if (nm.size() < 6 || nm.compare(nm.size() - 5, 5, ".vst3") != 0) continue;
            const std::string m = scanBundle(e.path());
            if (!m.empty()) out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string VstHost::resolveModule(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty()) return {};
    if (!fs::exists(path, ec)) return {};
    if (fs::is_regular_file(path, ec)) return path;
    return scanBundle(fs::path(path));
}

bool VstHost::load(const std::string& modulePath) {
    // Il chiamante deve aver fermato l'AudioEngine: qui non c'e' audio attivo.
    releaseProcessing();
    closeEditor();

    if (impl_->component) unload();
    impl_->error.clear();
    impl_->queue.clear();
    impl_->paramTransfer.removeChanges();

    const std::string resolved = resolveModule(modulePath);
    if (resolved.empty()) {
        impl_->error = "Percorso non valido: " + modulePath;
        return false;
    }

    // Il loader vuole il bundle (Linux/mac) o il file modulo (Windows).
    const std::string loadPath = moduleLoadPath(fs::path(resolved));

    std::string err;
    impl_->module = VST3::Hosting::Module::create(loadPath, err);
    if (!impl_->module) {
        impl_->error = "Caricamento modulo fallito: " + err;
        return false;
    }

    const auto& factory = impl_->module->getFactory();
    VST3::Hosting::ClassInfo ci;
    bool found = false;
    for (const auto& info : factory.classInfos()) {
        if (info.category() == kVstAudioEffectClass) {
            ci = info;
            found = true;
            break;
        }
    }
    if (!found) {
        impl_->error = "Nessuna classe Audio Module trovata.";
        impl_->module.reset();
        return false;
    }

    impl_->name = ci.name();
    impl_->path = modulePath;

    impl_->component  = factory.createInstance<IComponent>(ci.ID());
    impl_->controller = factory.createInstance<IEditController>(ci.ID());
    if (!impl_->controller) {
        // controller separato: chiedi al component la sua classe
        TUID tuid = {};
        if (impl_->component &&
            impl_->component->getControllerClassId(tuid) == kResultOk) {
            const VST3::UID uid(tuid);
            impl_->controller = factory.createInstance<IEditController>(uid);
        }
    }
    if (!impl_->component) {
        impl_->error = "Creazione IComponent fallita.";
        impl_->module.reset();
        return false;
    }

    impl_->hostApp = new HostApp();

    if (impl_->controller) {
        impl_->handler = new CompHandler();
        impl_->handler->setTarget(&impl_->paramTransfer);
        impl_->controller->setComponentHandler(impl_->handler);

        FUnknownPtr<IConnectionPoint> cpC(impl_->component);
        FUnknownPtr<IConnectionPoint> cpE(impl_->controller);
        if (cpC && cpE) {
            cpC->connect(cpE);
            cpE->connect(cpC);
        }
        impl_->controller->initialize(impl_->hostApp);
    }
    impl_->component->initialize(impl_->hostApp);

    impl_->processor = FUnknownPtr<IAudioProcessor>(impl_->component);
    if (!impl_->processor) {
        impl_->error = "Il plugin non e' un processore audio.";
        unload();
        return false;
    }
    return true;
}

void VstHost::unload() {
    closeEditor();
    impl_->deactivate();
    impl_->freeBuffers();

    if (impl_->controller) {
        FUnknownPtr<IConnectionPoint> cpC(impl_->component);
        FUnknownPtr<IConnectionPoint> cpE(impl_->controller);
        if (cpC && cpE) {
            cpC->disconnect(cpE);
            cpE->disconnect(cpC);
        }
        impl_->controller->terminate();
    }
    if (impl_->component) impl_->component->terminate();

    impl_->processor = nullptr;
    impl_->controller = nullptr;
    impl_->component = nullptr;

    if (impl_->hostApp) { impl_->hostApp->release(); impl_->hostApp = nullptr; }
    if (impl_->handler) { impl_->handler->release(); impl_->handler = nullptr; }

    impl_->module.reset();
    impl_->name.clear();
    impl_->path.clear();
}

bool        VstHost::loaded() const { return impl_->component != nullptr; }
std::string VstHost::pluginName() const { return impl_->name; }
std::string VstHost::pluginPath() const { return impl_->path; }
std::string VstHost::lastError() const { return impl_->error; }

bool VstHost::prepare(int sampleRate, int blockSize) {
    if (!impl_->component) return false;
    if (impl_->processing) {
        // gia' pronto: se cambia il formato, riconfigura
        if (static_cast<int>(impl_->sampleRate) == sampleRate &&
            impl_->blockSize == blockSize)
            return true;
        impl_->deactivate();
    }
    // Eventi/parametri accumulati mentre l'audio era fermo non devono suonare.
    impl_->queue.clear();
    impl_->paramTransfer.removeChanges();
    impl_->clearOnMask();
    return impl_->configure(sampleRate, blockSize);
}

void VstHost::releaseProcessing() {
    impl_->deactivate();
    impl_->freeBuffers();
}

bool VstHost::prepared() const { return impl_->processing; }

void VstHost::renderAdd(float* out, int frames) {
    impl_->renderAdd(out, frames);
}

bool VstHost::saveState(const std::string& file) const {
    if (!impl_->component) return false;

    auto* cF = static_cast<FUnknown*>(impl_->component.get());
    auto* eF = static_cast<FUnknown*>(impl_->controller.get());
    const bool separate = impl_->controller && (cF != eF);

    MemStream c;
    if (impl_->component->getState(&c) != kResultOk) return false;
    MemStream e;
    if (separate && impl_->controller->getState(&e) != kResultOk) return false;

    std::vector<uint8_t> out;
    putU32(out, static_cast<uint32_t>(c.bytes().size()));
    putU32(out, static_cast<uint32_t>(e.bytes().size()));
    out.insert(out.end(), c.bytes().begin(), c.bytes().end());
    out.insert(out.end(), e.bytes().begin(), e.bytes().end());

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

bool VstHost::loadState(const std::string& file) {
    if (!impl_->component) return false;

    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (raw.size() < 8) return false;

    const uint32_t cLen = getU32(raw.data());
    const uint32_t eLen = getU32(raw.data() + 4);
    const size_t off = 8;
    if (off + cLen + eLen > raw.size()) return false;

    std::vector<uint8_t> cBytes(raw.begin() + off, raw.begin() + off + cLen);
    std::vector<uint8_t> eBytes(raw.begin() + off + cLen,
                                raw.begin() + off + cLen + eLen);

    MemStream c(std::move(cBytes));
    if (impl_->component->setState(&c) != kResultOk) return false; // NOLINT

    if (impl_->controller) {
        c.seek(0, static_cast<int32>(IBStream::kIBSeekSet));
        impl_->controller->setComponentState(&c);
        if (!eBytes.empty()) {
            MemStream e(std::move(eBytes));
            impl_->controller->setState(&e);
        }
    }
    return true;
}

bool VstHost::hasEditor() const {
    if (!impl_->controller) return false;
    IPlugView* v = impl_->controller->createView(ViewType::kEditor);
    if (!v) return false;
    v->release();
    return true;
}

bool VstHost::editorOpen() const { return impl_->view != nullptr; }

bool VstHost::openEditor() {
    if (!impl_->controller) {
        impl_->error = "Editor non disponibile (nessun controller).";
        return false;
    }
    if (impl_->view) return true;

    IPlugView* raw = impl_->controller->createView(ViewType::kEditor);
    if (!raw) {
        impl_->error = "Questo plugin non ha un'interfaccia grafica.";
        return false;
    }
    impl_->view = IPtr<IPlugView>(raw);

#ifndef FMK_HAVE_SYSWM
    impl_->error = "Piattaforma grafica non supportata per l'editor.";
    impl_->view = nullptr;
    return false;
#else
    ViewRect vr;
    impl_->view->getSize(&vr);
    int ww = vr.getWidth();
    int wh = vr.getHeight();
    if (ww < 64) ww = 640;
    if (wh < 64) wh = 400;

    SDL_Window* w = SDL_CreateWindow((impl_->name + " - fMidiKeyboard").c_str(),
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w) {
        impl_->error = std::string("Finestra editor: ") + SDL_GetError();
        impl_->view = nullptr;
        return false;
    }
    impl_->editorWin = w;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) {
        impl_->error = std::string("WM info: ") + SDL_GetError();
        closeEditor();
        return false;
    }

    FIDString type = nullptr;
    void* handle = nullptr;
#if defined(_WIN32)
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        handle = info.info.win.window;
        type   = kPlatformTypeHWND;
    }
#elif defined(__APPLE__)
    if (info.subsystem == SDL_SYSWM_COCOA) {
        handle = info.info.cocoa.window;
        type   = kPlatformTypeNSView;
    }
#elif defined(SDL_VIDEO_DRIVER_X11)
    if (info.subsystem == SDL_SYSWM_X11) {
        handle = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
        type   = kPlatformTypeX11EmbedWindowID;
    }
#endif
    if (!handle || !type) {
        impl_->error = "Piattaforma grafica non supportata per l'editor.";
        closeEditor();
        return false;
    }
    if (impl_->view->isPlatformTypeSupported(type) != kResultOk) {
        impl_->error = "L'editor non supporta l'embedding in questa piattaforma.";
        closeEditor();
        return false;
    }

    impl_->frame = new Impl::PlugFrame(impl_);
    impl_->view->setFrame(impl_->frame);
    if (impl_->view->attached(handle, type) != kResultOk) {
        impl_->error = "attached() dell'editor non riuscito.";
        closeEditor();
        return false;
    }
    impl_->attached = true;
    return true;
#endif
}

void VstHost::closeEditor() {
    if (impl_->view) {
        impl_->view->setFrame(nullptr);
        if (impl_->attached) impl_->view->removed();
        impl_->view = nullptr;
    }
    impl_->attached = false;
    impl_->frame = nullptr;
    if (impl_->editorWin) {
        SDL_DestroyWindow(impl_->editorWin);
        impl_->editorWin = nullptr;
    }
}

void VstHost::onEditorResized() {
    if (!impl_->view || !impl_->editorWin) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(impl_->editorWin, &ww, &wh);
    ViewRect r(0, 0, ww, wh);
    impl_->view->onSize(&r);
}

unsigned int VstHost::editorWindowId() const {
    return impl_->editorWin ? SDL_GetWindowID(impl_->editorWin) : 0;
}

void VstHost::noteOn(int pitch, int velocity) {
    impl_->markNoteOn(pitch);
    impl_->queue.push(MidiEv{0x90, static_cast<uint8_t>(pitch),
                             static_cast<uint8_t>(velocity)});
}
void VstHost::noteOff(int pitch) {
    impl_->markNoteOff(pitch);
    impl_->queue.push(MidiEv{0x80, static_cast<uint8_t>(pitch), 0});
}
void VstHost::controlChange(int cc, int value) {
    impl_->queue.push(MidiEv{0xB0, static_cast<uint8_t>(cc),
                             static_cast<uint8_t>(value)});
}
void VstHost::allNotesOff() {
    // Un note-off esplicito per ogni nota suonata: e' piu' affidabile del solo
    // CC 123 (All Notes Off), che alcuni plugin non gestiscono.
    const uint64_t a = impl_->onMaskA.load(std::memory_order_relaxed);
    const uint64_t b = impl_->onMaskB.load(std::memory_order_relaxed);
    for (int p = 0; p < 64; ++p)
        if ((a >> p) & 1ull)
            impl_->queue.push(MidiEv{0x80, static_cast<uint8_t>(p), 0});
    for (int p = 0; p < 64; ++p)
        if ((b >> p) & 1ull)
            impl_->queue.push(MidiEv{0x80, static_cast<uint8_t>(p + 64), 0});
    impl_->queue.push(MidiEv{0xB0, 120, 0});  // all sound off
    impl_->queue.push(MidiEv{0xB0, 123, 0});  // all notes off
}

#endif // FMK_HAVE_VST3
