cmake_minimum_required(VERSION 3.16)
project(fMidiKeyboard VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Permette di compilare CMakeLists di terze parti piu' vecchie con CMake >= 4.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
include(FetchContent)

# Consente la vecchia FetchContent_Populate() senza warning (CMake >= 3.30).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Se OFF usa solo le librerie di sistema (nessuna rete).
option(FMK_FETCH_DEPS "Scarica da internet le dipendenze mancanti" ON)

# ---------------------------------------------------------------- SDL2 --------
set(SDL2_LINK "")
find_package(SDL2 QUIET CONFIG)
if(TARGET SDL2::SDL2-static)
    set(SDL2_LINK SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(SDL2_LINK SDL2::SDL2)
endif()

if(NOT SDL2_LINK AND PkgConfig_FOUND)
    pkg_check_modules(SDL2_PC QUIET sdl2)
    if(SDL2_PC_FOUND)
        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        target_include_directories(SDL2::SDL2 INTERFACE ${SDL2_PC_INCLUDE_DIRS})
        target_link_directories(SDL2::SDL2 INTERFACE ${SDL2_PC_LIBRARY_DIRS})
        target_link_libraries(SDL2::SDL2 INTERFACE ${SDL2_PC_LINK_LIBRARIES})
        set(SDL2_LINK SDL2::SDL2)
    endif()
endif()

if(NOT SDL2_LINK)
    if(NOT FMK_FETCH_DEPS)
        message(FATAL_ERROR "SDL2 non trovato (installa SDL2-devel o abilita FMK_FETCH_DEPS).")
    endif()
    message(STATUS "SDL2 non trovato: lo scarico e compilo da sorgente (statico).")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.30.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL2)
    if(TARGET SDL2::SDL2-static)
        set(SDL2_LINK SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
        set(SDL2_LINK SDL2::SDL2)
    elseif(TARGET SDL2)
        set(SDL2_LINK SDL2)
    endif()
endif()

# --------------------------------------------------------------- RtMidi -------
# Compiliamo RtMidi direttamente dal sorgente quando serve: il suo CMakeLists
# creerebbe un target "uninstall" che va in conflitto con quello di SDL2.
set(RTMIDI_LINK "")
find_package(RtMidi QUIET CONFIG)
if(TARGET RtMidi::rtmidi)
    set(RTMIDI_LINK RtMidi::rtmidi)
elseif(TARGET rtmidi)
    set(RTMIDI_LINK rtmidi)
elseif(TARGET RtMidi::rtmidi-static)
    set(RTMIDI_LINK RtMidi::rtmidi-static)
elseif(TARGET rtmidi-static)
    set(RTMIDI_LINK rtmidi-static)
endif()

if(NOT RTMIDI_LINK AND PkgConfig_FOUND)
    pkg_check_modules(RTMIDI_PC QUIET rtmidi)
    if(RTMIDI_PC_FOUND)
        add_library(rtmidi INTERFACE IMPORTED)
        target_include_directories(rtmidi INTERFACE ${RTMIDI_PC_INCLUDE_DIRS})
        target_link_directories(rtmidi INTERFACE ${RTMIDI_PC_LIBRARY_DIRS})
        target_link_libraries(rtmidi INTERFACE ${RTMIDI_PC_LINK_LIBRARIES})
        set(RTMIDI_LINK rtmidi)
    endif()
endif()

if(NOT RTMIDI_LINK)
    if(NOT FMK_FETCH_DEPS)
        message(FATAL_ERROR "RtMidi non trovato (installa rtmidi-devel o abilita FMK_FETCH_DEPS).")
    endif()
    message(STATUS "RtMidi non trovato: lo scarico da sorgente.")
    FetchContent_Declare(rtmidi_wrap
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG        6.0.0
        GIT_SHALLOW    TRUE)
    FetchContent_GetProperties(rtmidi_wrap)
    if(NOT rtmidi_wrap_POPULATED)
        FetchContent_Populate(rtmidi_wrap)
    endif()

    add_library(rtmidi STATIC ${rtmidi_wrap_SOURCE_DIR}/RtMidi.cpp)
    target_include_directories(rtmidi PUBLIC ${rtmidi_wrap_SOURCE_DIR})

    if(WIN32)
        target_compile_definitions(rtmidi PRIVATE __WINDOWS_MM__)
        target_link_libraries(rtmidi PUBLIC winmm)
    elseif(APPLE)
        target_compile_definitions(rtmidi PRIVATE __MACOSX_CORE__)
        target_link_libraries(rtmidi PUBLIC
            "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
    else()
        find_package(ALSA REQUIRED)
        target_compile_definitions(rtmidi PRIVATE __LINUX_ALSA__)
        target_include_directories(rtmidi PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(rtmidi PUBLIC ${ALSA_LIBRARIES})
    endif()
    set(RTMIDI_LINK rtmidi)
endif()

# ------------------------------------------------------------- VST3 (host) -----
# Hosting di plugin VST3 dentro l'app. Scarica l'SDK Steinberg e usa gli
# obiettivi "sdk_hosting"/"sdk". Disattivabile con -DFMK_VST3=OFF.
option(FMK_VST3 "Abilita l'hosting di plugin VST3" ON)
set(FMK_VST3_OK OFF)

if(FMK_VST3)
    if(NOT FMK_FETCH_DEPS)
        # Usa un SDK gia' presente sul sistema (SMTG dir o vst3sdk_SOURCE_DIR).
        if(DEFINED ENV{VST3_SDK_ROOT})
            set(vst3sdk_SOURCE_DIR "$ENV{VST3_SDK_ROOT}" CACHE PATH "" FORCE)
        endif()
    endif()

    if(NOT DEFINED vst3sdk_SOURCE_DIR)
        message(STATUS "VST3: scarico l'SDK Steinberg (operazione lunga la prima volta).")
        set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
        set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG        v3.8.1_build_84
            GIT_SUBMODULES_RECURSE ON
            GIT_SHALLOW    FALSE)
    endif()
    FetchContent_MakeAvailable(vst3sdk)

    if(TARGET sdk_hosting)
        set(FMK_VST3_OK ON)
    elseif(TARGET sdk)
        set(FMK_VST3_OK ON)
    endif()

    if(NOT FMK_VST3_OK)
        message(WARNING "VST3: SDK presente ma nessun target 'sdk_hosting'/'sdk' trovato.")
    endif()
endif()

add_executable(fMidiKeyboard
    src/main.cpp
    src/midi.cpp
    src/notes.cpp
    src/text.cpp
    src/chord.cpp
    src/gfx.cpp
    src/audio.cpp
    src/sampler.cpp
    src/piano.cpp
    src/rhodes.cpp
    src/synth.cpp
    src/vsthost.cpp)

if(FMK_VST3_OK)
    target_compile_definitions(fMidiKeyboard PRIVATE FMK_HAVE_VST3=1)
endif()

target_include_directories(fMidiKeyboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_compile_definitions(fMidiKeyboard PRIVATE FMK_VERSION="${PROJECT_VERSION}")

target_link_libraries(fMidiKeyboard PRIVATE
    ${SDL2_LINK}
    ${RTMIDI_LINK}
    Threads::Threads)

if(FMK_VST3_OK)
    if(TARGET sdk_hosting)
        target_link_libraries(fMidiKeyboard PRIVATE sdk_hosting)
    endif()
    if(TARGET sdk)
        target_link_libraries(fMidiKeyboard PRIVATE sdk)
    endif()
    if(TARGET sdk_common)
        target_link_libraries(fMidiKeyboard PRIVATE sdk_common)
    endif()
    if(TARGET base)
        target_link_libraries(fMidiKeyboard PRIVATE base)
    endif()
    if(TARGET pluginterfaces)
        target_link_libraries(fMidiKeyboard PRIVATE pluginterfaces)
    endif()

    # Il caricatore di moduli e' specifico per piattaforma e non fa parte
    # delle librerie dell'SDK: lo compiliamo direttamente.
    if(WIN32)
        target_sources(fMidiKeyboard PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
    elseif(APPLE)
        enable_language(OBJCXX)
        target_sources(fMidiKeyboard PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
    else()
        target_sources(fMidiKeyboard PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
    endif()
endif()

if(MSVC)
    target_compile_options(fMidiKeyboard PRIVATE /O2 /W3)
else()
    target_compile_options(fMidiKeyboard PRIVATE -O3 -fno-math-errno -Wall -Wextra)
endif()

# Font leggibile incorporato: viene copiato accanto all'eseguibile in build/.
add_custom_command(TARGET fMidiKeyboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:fMidiKeyboard>/assets
    COMMENT "Copia gli asset (font) accanto all'eseguibile")

install(TARGETS fMidiKeyboard RUNTIME DESTINATION bin)
install(DIRECTORY assets DESTINATION bin)

set(CPACK_PACKAGE_NAME "fMidiKeyboard")
set(CPACK_PACKAGE_VENDOR "FrancyCoso2010")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Visualizzatore MIDI per pianoforte con synth e VST3")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "fMidiKeyboard")
set(CPACK_PACKAGE_FILE_NAME "fMidiKeyboard-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ;ZIP")
endif()

include(CPack)
