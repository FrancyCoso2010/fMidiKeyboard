#pragma once

#include <string>
#include <vector>

// Hosting di plugin VST3 (strumenti) con uscita audio via SDL.
// Se il programma e' stato compilato senza l'SDK VST3 (FMK_VST3=OFF),
// tutti i metodi sono stub che non fanno nulla e available() == false.
class VstHost {
public:
    VstHost();
    ~VstHost();

    VstHost(const VstHost&) = delete;
    VstHost& operator=(const VstHost&) = delete;

    // Ricerca i plugin VST3 nei percorsi standard; restituisce i percorsi
    // dei moduli (il file .so/.dll effettivo da caricare).
    static std::vector<std::string> scanPlugins();

    // Dato un bundle ".vst3" (o un modulo gia' risolto) restituisce il
    // percorso del modulo binario effettivo. Stringa vuota se non valido.
    static std::string resolveModule(const std::string& path);

    bool        available() const;              // hosting compilato?
    bool        load(const std::string& modulePath);
    void        unload();
    bool        loaded() const;
    std::string pluginName() const;
    std::string pluginPath() const;
    std::string lastError() const;

    // Rendering audio: il dispositivo e' gestito da AudioEngine, che prepara
    // il plugin e ne richiama renderAdd() dentro il proprio callback.
    bool prepare(int sampleRate, int blockSize);
    void releaseProcessing();
    bool prepared() const;
    // Somma l'uscita stereo interleaved del plugin nel buffer (mixaggio).
    void renderAdd(float* out, int frames);

    // Stato del plugin (preset/setup, es. lo strumento caricato in un sampler).
    bool saveState(const std::string& file) const;
    bool loadState(const std::string& file);

    // Interfaccia grafica del plugin (editor).
    bool hasEditor() const;
    bool openEditor();
    void closeEditor();
    bool editorOpen() const;
    void onEditorResized();
    unsigned int editorWindowId() const; // id finestra SDL dell'editor (0 se chiusa)

    // Eventi inviati al plugin (chiamare dal thread UI).
    void noteOn(int pitch, int velocity);
    void noteOff(int pitch);
    void controlChange(int cc, int value);
    void allNotesOff();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
