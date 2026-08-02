cmake_minimum_required(VERSION 3.22)

project(Parvati
    VERSION 0.1.0
    DESCRIPTION "Parvati — a JUCE port of the Mutable Instruments Ambika synthesizer"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is a global checkout at ~/JUCE (no installed JUCEConfig.cmake), so we
# consume it via add_subdirectory. Override with -DJUCE_GLOBAL_PATH=/path if
# JUCE lives elsewhere. JUCE requires the C language to be enabled.
if(NOT DEFINED JUCE_GLOBAL_PATH)
    set(JUCE_GLOBAL_PATH "$ENV{HOME}/JUCE")
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
