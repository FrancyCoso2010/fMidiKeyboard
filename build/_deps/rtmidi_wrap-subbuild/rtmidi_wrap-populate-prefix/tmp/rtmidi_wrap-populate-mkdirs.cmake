# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-src")
  file(MAKE_DIRECTORY "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-src")
endif()
file(MAKE_DIRECTORY
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-build"
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix"
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/tmp"
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/src/rtmidi_wrap-populate-stamp"
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/src"
  "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/src/rtmidi_wrap-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/src/rtmidi_wrap-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/francycoso2010/Documenti/Coding/fMidiKeyboard/build/_deps/rtmidi_wrap-subbuild/rtmidi_wrap-populate-prefix/src/rtmidi_wrap-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
