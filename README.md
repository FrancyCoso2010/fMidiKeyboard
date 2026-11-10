# fMidiKeyboard

Visualizzatore MIDI per pianoforte con sintetizzatore integrato, in stile Synthesia.
Scritto in C++17 con SDL2, RtMidi e hosting di plugin VST3.

## Funzionalità

- Visualizzazione delle note MIDI in arrivo in tempo reale
- Suono dei tasti tramite mouse e tastiera del computer
- Strumenti integrati: pianoforte sintetizzato, piano elettrico Rhodes e synth
- Hosting di plugin VST3 (`--plugin`, `--vst-scan`, `--vst-info`)
- Caricamento di strumenti `.sfz` / `.dspreset` (`--instrument`)
- Pannello di controllo, nomi delle note, accordi e schermo intero

## Requisiti

- CMake >= 3.16
- Compilatore C++17 (GCC, Clang o MSVC)
- Linux: ALSA (`libasound2-dev` su Debian/Ubuntu, `alsa-lib-devel` su Fedora)

SDL2, RtMidi e l'SDK VST3 vengono scaricati e compilati automaticamente da CMake
se non sono già presenti sul sistema. Su Linux e macOS SDL2 viene compilato
staticamente, così il pacchetto è più portabile.

## Compilazione

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Esecuzione:

```bash
./build/fMidiKeyboard
```

Opzioni utili a CMake:

- `-DFMK_VST3=OFF` disabilita l'hosting VST3 (build più veloce)
- `-DFMK_FETCH_DEPS=OFF` usa solo le librerie già installate, senza rete

## Utilizzo

```bash
./build/fMidiKeyboard --help
./build/fMidiKeyboard --plugin /percorso/plugin.vst3
./build/fMidiKeyboard --instrument /percorso/strumento.sfz
./build/fMidiKeyboard --piano
./build/fMidiKeyboard --rhodes
./build/fMidiKeyboard --synth
```

Scorciatoie in finestra:

| Tasto | Funzione |
|-------|----------|
| `S` | pannello |
| `Y` | synth |
| `O` | plugin |
| `I` | strumento |
| `E` | editor |
| `TAB` | MIDI |
| `V` | direzione |
| `N` | nomi note |
| `A` | accordo |
| `Su`/`Giù` | velocità |
| `P` | panic |
| `H` | aiuto |
| `F` | schermo intero |
| `Esc` | esci |

## Creare i pacchetti

Su ogni piattaforma:

```bash
cpack --config build/CPackConfig.cmake -C Release -B build
```

Genera un archivio `fMidiKeyboard-<versione>-<sistema>-<architettura>` (ZIP e TGZ
su Linux/macOS, ZIP su Windows) contenente l'eseguibile e la cartella `assets/`.

In alternativa il workflow GitHub Actions `.github/workflows/build.yml` compila
automaticamente per Linux, Windows e macOS e carica i pacchetti come artifact
(Actions > build > Run workflow, oppure push di un tag `v*`).

## Struttura del progetto

```
assets/       font e licenze
src/          codice sorgente
third_party/  header di terze parti (stb)
CMakeLists.txt
```

## Licenze di terze parti

Il font `LiberationSans-Regular.ttf` è distribuito secondo la licenza inclusa in
`assets/LICENSE-LiberationSans.txt`. Le altre dipendenze (SDL2, RtMidi, VST3 SDK,
stb) mantengono le rispettive licenze originali.
