cmake_minimum_required(VERSION 3.22)

project(Parvati
    VERSION 0.1.0
    DESCRIPTION "Parvati — a JUCE port of the Mutable Instruments Ambika synthesizer"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emit compile_commands.json (consumed by clang-tidy / IDE tooling). No-op when
# the generator cannot write it (e.g. Xcode).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------------------------------
# Developer tooling options (all OFF by default; opt-in). Enable as needed:
#   -DPARVATI_ENABLE_ASAN=ON   -DPARVATI_ENABLE_UBSAN=ON
#   -DPARVATI_ENABLE_TSAN=ON   -DPARVATI_WARNINGS_AS_ERRORS=ON
# Sanitizers apply to the whole build (JUCE + Parvati + tests) so they catch
# bugs across the full call graph; reconfigure from a clean build dir when
# toggling them. (AddressSanitizer and ThreadSanitizer are mutually exclusive.)
# ------------------------------------------------------------------------------
option(PARVATI_ENABLE_ASAN            "Enable AddressSanitizer"            OFF)
option(PARVATI_ENABLE_UBSAN           "Enable UndefinedBehaviorSanitizer"  OFF)
option(PARVATI_ENABLE_TSAN            "Enable ThreadSanitizer"             OFF)
option(PARVATI_WARNINGS_AS_ERRORS     "Treat compiler warnings as errors"  OFF)

# AddressSanitizer and ThreadSanitizer cannot be combined (both hijack the
# memory allocator). Fail fast at configure time rather than at link/run.
if(PARVATI_ENABLE_ASAN AND PARVATI_ENABLE_TSAN)
    message(FATAL_ERROR "PARVATI_ENABLE_ASAN and PARVATI_ENABLE_TSAN are mutually exclusive (ASan+TSan cannot be combined).")
endif()

set(_parvati_sanitizers "")
if(PARVATI_ENABLE_ASAN)
    list(APPEND _parvati_sanitizers address)
endif()
if(PARVATI_ENABLE_UBSAN)
    list(APPEND _parvati_sanitizers undefined)
endif()
if(PARVATI_ENABLE_TSAN)
    list(APPEND _parvati_sanitizers thread)
endif()
if(_parvati_sanitizers)
    list(JOIN _parvati_sanitizers "," _parvati_sanitizer_flags)
    message(STATUS "Parvati: sanitizers enabled (${_parvati_sanitizer_flags})")
    # -fsanitize is GCC/Clang-only. MSVC AddressSanitizer (/fsanitize=address)
    # is a different mechanism and is intentionally not wired here.
    if(NOT MSVC)
        add_compile_options(-fsanitize=${_parvati_sanitizer_flags} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${_parvati_sanitizer_flags})
    else()
        message(WARNING "Parvati: -fsanitize is GCC/Clang-only; not applied under MSVC.")
    endif()
endif()
if(PARVATI_WARNINGS_AS_ERRORS)
    # GCC/Clang use -Werror; MSVC uses /WX.
    if(NOT MSVC)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror> $<$<COMPILE_LANGUAGE:C>:-Werror>)
    else()
        add_compile_options(/WX)
    endif()
endif()

# JUCE is a global checkout at ~/JUCE (no installed JUCEConfig.cmake), so we
# consume it via add_subdirectory. Override with -DJUCE_GLOBAL_PATH=/path if
# JUCE lives elsewhere. JUCE requires the C language to be enabled.
if(NOT DEFINED JUCE_GLOBAL_PATH)
    # $ENV{HOME} is unset on native Windows shells; fall back to USERPROFILE.
    if(WIN32 AND (NOT DEFINED ENV{HOME} OR "$ENV{HOME}" STREQUAL ""))
        set(JUCE_GLOBAL_PATH "$ENV{USERPROFILE}/JUCE")
    else()
        set(JUCE_GLOBAL_PATH "$ENV{HOME}/JUCE")
    endif()
endif()

if(NOT EXISTS "${JUCE_GLOBAL_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at '${JUCE_GLOBAL_PATH}'. "
        "Pass -DJUCE_GLOBAL_PATH=/path/to/JUCE.")
endif()

add_subdirectory("${JUCE_GLOBAL_PATH}" "${CMAKE_BINARY_DIR}/juce_build" EXCLUDE_FROM_ALL)

juce_add_plugin(Parvati
    COMPANY_NAME            "805LABS"
    BUNDLE_ID               "com.805labs.parvati"
    PLUGIN_MANUFACTURER_CODE "805L"       # >=1 uppercase letter (AU requirement)
    PLUGIN_CODE             "Prvt"        # first letter upper, rest lower (GarageBand safe)
    FORMATS                 Standalone VST3 AU
    PRODUCT_NAME            "Parvati"
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# NOTE: Source/ is globbed (CONFIGURE_DEPENDS) so new DSP files added by
# subagents are picked up automatically. The read-only ambika_reference/ tree
# lives OUTSIDE Source/ and is therefore never pulled into the build.
file(GLOB_RECURSE PARVATI_SOURCES CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.cc"
    "Source/*.h")
target_sources(Parvati PRIVATE ${PARVATI_SOURCES})
target_include_directories(Parvati PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# ------------------------------------------------------------------------------
# Factory presets (Ambika "goldencard" GPL-3.0 banks), organized under presets/
# as FACTORY/<bank>/ (A/B/F/S) + FACTORY_MULTI/. EMBEDDED into the plugin binary
# via juce_add_binary_data, then extracted to the user app-data dir on first run
# (parvati::ensureFactoryPresetsInstalled, see Source/ui/FactoryPresetInstaller.h)
# so they show up in the Patch combo out of the box.
#
# The four program banks share filenames (each has 000.PRO, 001.PRO ...), so they
# cannot be embedded by their raw names. Stage them into a build dir with
# bank-unique names first: <BANK>__<file> (A__000.PRO) and MULTI__<file>. The
# installer parses the "__" delimiter to reconstruct FACTORY/<bank>/<file>.
set(_parvati_preset_stage "${CMAKE_BINARY_DIR}/parvati_preset_stage")
file(REMOVE_RECURSE "${_parvati_preset_stage}")
file(MAKE_DIRECTORY "${_parvati_preset_stage}")
# Re-stage when the source presets change (CONFIGURE_DEPENDS -> reconfigure).
file(GLOB _cfgdep_pro CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/FACTORY/*/*.PRO")
file(GLOB _cfgdep_mul CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/FACTORY_MULTI/*.MUL")
file(GLOB _cfgdep_tpl CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/TEMPLATES/*.parvati")
set(_staged_sources "")
foreach(_bank A B F S)
    file(GLOB _files "${CMAKE_CURRENT_SOURCE_DIR}/presets/FACTORY/${_bank}/*.PRO")
    foreach(_f ${_files})
        get_filename_component(_n "${_f}" NAME)
        set(_out "${_parvati_preset_stage}/${_bank}__${_n}")
        configure_file("${_f}" "${_out}" COPYONLY)
        list(APPEND _staged_sources "${_out}")
    endforeach()
endforeach()
file(GLOB _mulfiles "${CMAKE_CURRENT_SOURCE_DIR}/presets/FACTORY_MULTI/*.MUL")
foreach(_f ${_mulfiles})
    get_filename_component(_n "${_f}" NAME)
    set(_out "${_parvati_preset_stage}/MULTI__${_n}")
    configure_file("${_f}" "${_out}" COPYONLY)
    list(APPEND _staged_sources "${_out}")
endforeach()
# Stock init templates (full-fidelity .parvati multis) — staged as TEMPLATE__<file>.
file(GLOB _tplfiles "${CMAKE_CURRENT_SOURCE_DIR}/presets/TEMPLATES/*.parvati")
foreach(_f ${_tplfiles})
    get_filename_component(_n "${_f}" NAME)
    set(_out "${_parvati_preset_stage}/TEMPLATE__${_n}")
    configure_file("${_f}" "${_out}" COPYONLY)
    list(APPEND _staged_sources "${_out}")
endforeach()
# Guard: juce_add_binary_data errors on an empty SOURCES list. The factory banks
# are always checked in, so this only matters for a hypothetical empty-presets
# checkout (configure stays valid; the installer simply has nothing to extract).
if(_staged_sources)
    juce_add_binary_data(parvati_factory_presets
        NAMESPACE FactoryPresets
        SOURCES ${_staged_sources})
    target_link_libraries(Parvati PRIVATE parvati_factory_presets)
endif()

# Hardware-accelerated GUI rendering is ON BY DEFAULT in JUCE 9.0.0 — no define
# is required (verified against ~/JUCE):
#   * Windows: HWNDComponentPeer paints via an unconditional Direct2D device
#     context + swap chain (juce_Windowing_windows.cpp: direct2DContext is a
#     member initializer, handlePaint() calls direct2DContext->startFrame).
#   * macOS:    JUCE uses CoreGraphics, which is itself GPU-accelerated.
# NOTE: the old `JUCE_DIRECT2D=1` compile flag was a JUCE 8-era opt-in; it is
# OBSOLETE in JUCE 9 (the bare token is referenced nowhere in the module tree —
# only the unrelated JUCE_DIRECT2D_METRICS profiling toggle remains). Do NOT
# re-add it: it would be an inert no-op. The only D2D-related define is
# JUCE_DIRECT2D_METRICS (profiling, off by default) — intentionally not set.
target_compile_definitions(Parvati PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(Parvati
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_lto_flags
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ------------------------------------------------------------------------------
# Headless Layer-3a verification (not shipped): instantiate the processor,
# push a NoteOn, assert non-silent audio. Build with:
#   cmake --build build --target parvati_headless_test && ./build/parvati_headless_test
add_executable(parvati_headless_test EXCLUDE_FROM_ALL
    tools/headless_test.cpp
)
target_include_directories(parvati_headless_test PRIVATE Source)
target_link_libraries(parvati_headless_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
)

# ------------------------------------------------------------------------------
# Phase-3 verification harness (behavioral/spectral). Built by default so
# `cmake --build build` produces it alongside the plugin. It links against the
# Parvati shared-code library (which compiles all of Source/dsp/*) and the JUCE
# modules those objects reference. Run with: ./build/parvati_tests
add_executable(parvati_tests
    tests/parvati_tests.cpp
)
target_include_directories(parvati_tests PRIVATE Source)
target_compile_features(parvati_tests PRIVATE cxx_std_17)
target_link_libraries(parvati_tests PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Tempo-synced LFO verification (directly drives ambika::dsp::Voice). Built by
# default. Run with: ./build/parvati_lfo_sync_test
add_executable(parvati_lfo_sync_test
    tests/lfo_sync_test.cpp
)
target_include_directories(parvati_lfo_sync_test PRIVATE Source)
target_compile_features(parvati_lfo_sync_test PRIVATE cxx_std_17)
target_link_libraries(parvati_lfo_sync_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_dsp
)

# ------------------------------------------------------------------------------
# LFO retrigger-on-note verification (firmware Part::RetriggerLfos). Built by
# default. Run with: ./build/parvati_lfo_retrigger_test
add_executable(parvati_lfo_retrigger_test
    tests/lfo_retrigger_test.cpp
)
target_include_directories(parvati_lfo_retrigger_test PRIVATE Source)
target_compile_features(parvati_lfo_retrigger_test PRIVATE cxx_std_17)
target_link_libraries(parvati_lfo_retrigger_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_dsp
)

# ------------------------------------------------------------------------------
# Ambika .MUL (multi) loading verification. Built by default.
# Run with: ./build/parvati_multi_load_test
add_executable(parvati_multi_load_test
    tests/multi_load_test.cpp
)
target_include_directories(parvati_multi_load_test PRIVATE Source)
target_compile_features(parvati_multi_load_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_multi_load_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_multi_load_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Patch SAVE/LOAD round-trip verification: .PRO and .MUL writer == inverse of
# parser (unit), plus end-to-end load -> save -> load into a 2nd processor
# (APVTS + per-part routing equality). Built by default.
# Run with: ./build/parvati_roundtrip_test
add_executable(parvati_roundtrip_test
    tests/roundtrip_test.cpp
)
target_include_directories(parvati_roundtrip_test PRIVATE Source)
target_compile_features(parvati_roundtrip_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_roundtrip_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_roundtrip_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Parvati-native YAML preset format verification. Proves the .parvati format
# carries Parvati-only options (vca_curve/filter_card) + arp that .PRO drops.
# Built by default. Run with: ./build/parvati_preset_test
add_executable(parvati_preset_test
    tests/parvati_preset_test.cpp
)
target_include_directories(parvati_preset_test PRIVATE Source)
target_compile_features(parvati_preset_test PRIVATE cxx_std_17)
target_link_libraries(parvati_preset_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Ambika .PRO patch-file support verification. Built by default.
# Run with: ./build/parvati_patch_test
add_executable(parvati_patch_test
    tests/patch_test.cpp
)

# ------------------------------------------------------------------------------
# Filter-card topology verification (3 selectable voicecard filters). Built by
# default. Run with: ./build_release/parvati_filter_topology_test
add_executable(parvati_filter_topology_test
    tests/filter_topology_test.cpp
)
target_include_directories(parvati_filter_topology_test PRIVATE Source)
target_compile_features(parvati_filter_topology_test PRIVATE cxx_std_17)
target_link_libraries(parvati_filter_topology_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Multitimbral GUI smoke test (editor builds + Part selector + per-part MIDI
# channel). Built by default. Run with: ./build/parvati_multigui_test
add_executable(parvati_multigui_test
    tests/editor_test.cpp
)
target_include_directories(parvati_multigui_test PRIVATE Source)
target_compile_features(parvati_multigui_test PRIVATE cxx_std_17)
target_link_libraries(parvati_multigui_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_gui_basics
)

# ------------------------------------------------------------------------------
# Multitimbral verification (6 Parts, MIDI channel routing, per-part patches).
# Built by default. Run with: ./build/parvati_multitimbral_test
add_executable(parvati_multitimbral_test
    tests/multitimbral_test.cpp
)
target_include_directories(parvati_multitimbral_test PRIVATE Source)
target_compile_features(parvati_multitimbral_test PRIVATE cxx_std_17)
target_link_libraries(parvati_multitimbral_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)
target_include_directories(parvati_patch_test PRIVATE Source)
target_compile_features(parvati_patch_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_patch_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_patch_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Part-state fidelity: arp/sequencer data-loss regression (.MUL save from a
# non-current part) + all-parts-audible-init. Built by default.
# Run with: ./build/parvati_partstate_test
add_executable(parvati_partstate_test
    tests/partstate_test.cpp
)
target_include_directories(parvati_partstate_test PRIVATE Source)
target_compile_features(parvati_partstate_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_partstate_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_partstate_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Step-sequencer verification. Built by default.
# Run with: ./build/parvati_sequencer_test
add_executable(parvati_sequencer_test
    tests/sequencer_test.cpp
)
target_include_directories(parvati_sequencer_test PRIVATE Source)
target_compile_features(parvati_sequencer_test PRIVATE cxx_std_17)
target_link_libraries(parvati_sequencer_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Layer-3b verification: the APVTS parameter bridge drives the engine. Built by
# default. Run with: ./build/parvati_apvts_test
add_executable(parvati_apvts_test
    tests/apvts_test.cpp
)
target_include_directories(parvati_apvts_test PRIVATE Source)
target_compile_features(parvati_apvts_test PRIVATE cxx_std_17)
target_link_libraries(parvati_apvts_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Startup rumble regression: the ENV->VCA modulation is multiplicative, so an
# idle voice with a modulation amount < 63 leaks a sub-audio oscillator tone as
# a low-frequency rumble at load. This sets mod11 (ENV3->VCA) to 32 to construct
# the condition, then renders with NO MIDI and asserts near-silence. Built by
# default. Run with: ./build/parvati_idle_silence_test
add_executable(parvati_idle_silence_test
    tests/idle_silence_test.cpp
)
target_include_directories(parvati_idle_silence_test PRIVATE Source)
target_compile_features(parvati_idle_silence_test PRIVATE cxx_std_17)
target_link_libraries(parvati_idle_silence_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Real-time / buffer-robustness smoke test. Renders a dense chord at
# 32..1024-sample buffers in Hardware(6) and Extended(16) modes; asserts finite
# + audible + no crash. Built by default. Run with: ./build/parvati_realtime_test
add_executable(parvati_realtime_test
    tests/realtime_test.cpp
)
target_include_directories(parvati_realtime_test PRIVATE Source)
target_compile_features(parvati_realtime_test PRIVATE cxx_std_17)
target_link_libraries(parvati_realtime_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

add_executable(parvati_concurrency_test
    tests/concurrency_test.cpp
)
target_include_directories(parvati_concurrency_test PRIVATE Source)
target_compile_features(parvati_concurrency_test PRIVATE cxx_std_17)
target_link_libraries(parvati_concurrency_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# MIDI CC/NRPN -> parameter mapping verification (spec F.4). Proves incoming
# CC/NRPN drive the APVTS parameters and reach the engine bytes exactly as the
# Ambika hardware. Built by default. Run with: ./build/parvati_midi_param_test
add_executable(parvati_midi_param_test
    tests/midi_param_test.cpp
)
target_include_directories(parvati_midi_param_test PRIVATE Source)
target_compile_features(parvati_midi_param_test PRIVATE cxx_std_17)
target_link_libraries(parvati_midi_param_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Arpeggiator verification (host-tempo-driven arp generates notes when the
# transport is playing, and is silent when stopped). Built by default.
# Run with: ./build/parvati_arp_test
add_executable(parvati_arp_test
    tests/arp_test.cpp
)
target_include_directories(parvati_arp_test PRIVATE Source)
target_compile_features(parvati_arp_test PRIVATE cxx_std_17)
target_link_libraries(parvati_arp_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Polyphony mode verification (Mono/Poly/Unison2x/Cyclic/Chain allocator).
add_executable(parvati_polyphony_test
    tests/polyphony_test.cpp
)
target_include_directories(parvati_polyphony_test PRIVATE Source)
target_compile_features(parvati_polyphony_test PRIVATE cxx_std_17)
target_link_libraries(parvati_polyphony_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_gui_basics
)

add_executable(parvati_voice_modes_test
    tests/voice_modes_test.cpp
)
target_include_directories(parvati_voice_modes_test PRIVATE Source)
target_compile_features(parvati_voice_modes_test PRIVATE cxx_std_17)
target_link_libraries(parvati_voice_modes_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_gui_basics
)

add_executable(parvati_patch_load_test
    tests/patch_load_test.cpp
)
target_include_directories(parvati_patch_load_test PRIVATE Source)
target_compile_features(parvati_patch_load_test PRIVATE cxx_std_17)
target_link_libraries(parvati_patch_load_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_gui_basics
)

# ------------------------------------------------------------------------------
# Controller-modulation verification (mod wheel CC1 / breath CC2 / foot CC4 /
# channel pressure): proves the global controllers + MPE per-voice expression are
# wired to their mod-matrix sources and are AUDIBLE when routed to cutoff.
# Replaces the prior throwaway mod-wheel/MPE smoke binaries. Built by default.
# Run with: ./build/parvati_controller_mod_test
add_executable(parvati_controller_mod_test
    tests/controller_mod_test.cpp
)
target_include_directories(parvati_controller_mod_test PRIVATE Source)
target_compile_features(parvati_controller_mod_test PRIVATE cxx_std_17)
target_link_libraries(parvati_controller_mod_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Phase-4 verification (not shipped): headless editor instantiation + coverage.
# Confirms createEditor() is non-null, every one of the 104 descriptors has a
# control + attachment, there are exactly 6 tab pages, the default size is
# 980x660, and the editor deletes cleanly (leak-detector validated).
#   cmake --build build --target parvati_editor_test && ./build/parvati_editor_test
add_executable(parvati_editor_test EXCLUDE_FROM_ALL
    tools/editor_test.cpp
)
target_include_directories(parvati_editor_test PRIVATE Source)
target_compile_features(parvati_editor_test PRIVATE cxx_std_17)
target_link_libraries(parvati_editor_test PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# ------------------------------------------------------------------------------
# Template-bank generator (dev tool, EXCLUDE_FROM_ALL). Produces the 5 stock init
# templates as full-fidelity .parvati multis into presets/TEMPLATES/, which are
# then embedded into the plugin binary. Run from the repo root:
#   cmake --build build --target parvati_gen_templates && ./build/parvati_gen_templates
add_executable(parvati_gen_templates EXCLUDE_FROM_ALL
    tools/gen_templates.cpp
)
target_include_directories(parvati_gen_templates PRIVATE Source)
target_compile_features(parvati_gen_templates PRIVATE cxx_std_17)
target_link_libraries(parvati_gen_templates PRIVATE
    Parvati
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
)
