cmake_minimum_required(VERSION 3.22)

project(Parvati
    VERSION 0.1.0
    DESCRIPTION "Parvati — a JUCE port of the Mutable Instruments Ambika synthesizer"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# Platform detection: iOS vs desktop. CMake sets IOS=TRUE for
# -DCMAKE_SYSTEM_NAME=iOS; CMAKE_SYSTEM_NAME is the robust fallback. Drives the
# plugin FORMATS, the iOS toolchain/signing block, and skips the macOS-only
# `deploy`/codesign steps so an iOS configure never references a VST3 target.
if(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PARVATI_IOS ON)
else()
    set(PARVATI_IOS OFF)
endif()
# Plugin formats per platform. iOS hosts (GarageBand/AUM/BeatMaker) load an AUv3
# app-extension; VST3 and the classic AU are desktop formats JUCE silently drops
# on iOS, so the iOS target list is AUv3 + Standalone only.
if(PARVATI_IOS)
    set(PARVATI_PLUGIN_FORMATS Standalone AUv3)
else()
    set(PARVATI_PLUGIN_FORMATS Standalone VST3 AU)
endif()

# iOS-only Info.plist keys passed NATIVELY through juce_add_plugin (see the
# call below). juceaide generates the plist with these keys at CONFIGURE time
# inside juce_add_plugin itself, so they flow into every downstream copy
# (the CMake generate-time CMakeFiles/<target>.dir/Info.plist, the built app,
# and xcodebuild archives) with no custom patch step and no configure/build
# ordering hazard. Desktop configure gets an EMPTY list so the desktop
# plists/builds stay byte-identical. Keys with no juce_add_plugin seam
# (LSSupportsOpeningDocumentsInPlace, the AUv3 CFBundleIdentifier) are still
# patched below — see the PlistBuddy block in the PARVATI_IOS section.
#   TARGETED_DEVICE_FAMILY 2        -> iPad-only: the UI's 1024pt width floor
#                                      cannot fit iPhone AUv3 panes (~330-560pt);
#                                      revisit when a compact layout exists.
#   IPHONE/IPAD_SCREEN_ORIENTATIONS -> landscape-only on BOTH device families:
#                                      the UI has no portrait layout, and the
#                                      plist arrays must match the runtime
#                                      Desktop::setOrientationsEnabled lock.
#   REQUIRES_FULL_SCREEN TRUE       -> ITMS-90474: an iPad app that opts into
#                                      Slide Over/Split View (the plist default
#                                      UIRequiresFullScreen=false) MUST support
#                                      all four orientations. Landscape-only
#                                      therefore REQUIRES full screen — which
#                                      also disables Split View, as intended
#                                      for a performance instrument.
#   BACKGROUND_AUDIO_ENABLED TRUE   -> UIBackgroundModes:audio, keep playing
#                                      when backgrounded.
#   FILE_SHARING_ENABLED TRUE       -> UIFileSharingEnabled, the app's Documents
#                                      folder is browsable in the Files app.
if(PARVATI_IOS)
    set(PARVATI_IOS_PLIST_ARGS
        TARGETED_DEVICE_FAMILY 2
        IPHONE_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        IPAD_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        REQUIRES_FULL_SCREEN TRUE
        BACKGROUND_AUDIO_ENABLED TRUE
        FILE_SHARING_ENABLED TRUE)
else()
    set(PARVATI_IOS_PLIST_ARGS "")
endif()

# Use ccache when available so reconfigures / rebuilds reuse compiled objects
# instead of recompiling the ~1161 memory-heavy JUCE + DSP translation units
# every time. (A clean build fan-out is what thrashes a 16 GB machine; ccache
# turns repeat builds near-instant.) Auto-detected; no-op when not installed.
# ccache DISABLED: it was serving stale compiled objects on source changes
# (screen renders / installed plugins fell behind the code). Re-enable only on
# explicit request; the `deploy` target rebuilds reliably without it.
# find_program(CCACHE_PROGRAM ccache)
# if(CCACHE_PROGRAM)
#     set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
#     set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
#     message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
# endif()
unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
unset(CMAKE_C_COMPILER_LAUNCHER CACHE)

# Build NATIVE arm64-only binaries (Apple Silicon). x86_64 / Rosetta support is
# dropped — Rosetta is being removed by Apple, and a single arch halves object
# + binary sizes (and build time). Force a universal build for distribution
# with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64".
if(APPLE)
    if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(CMAKE_OSX_ARCHITECTURES "arm64")
    endif()
endif()

# Default to a Debug build (the config this project is developed/tested with);
# artefacts then land in build/Parvati_artefacts/Debug/. Override with
# -DCMAKE_BUILD_TYPE=Release for an optimized build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

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
    # iOS targets AUv3 (app-extension) + Standalone only; VST3/AU are desktop
    # formats JUCE drops on iOS. See PARVATI_PLUGIN_FORMATS (gated on platform).
    FORMATS                 ${PARVATI_PLUGIN_FORMATS}
    PRODUCT_NAME            "Parvati"
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    # Native iOS plist keys (empty on desktop) — see the PARVATI_IOS_PLIST_ARGS
    # block above for the rationale of each key.
    ${PARVATI_IOS_PLIST_ARGS}
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
# Vendored Mutable Instruments clouds/stmlib DSP (the Clouds FX modules). Marked
# SYSTEM so the upstream headers' C-style conversions do not trip Parvati's
# strict warning set (these are third-party sources kept verbatim for upstream
# sync; the one required portability patch is in stmlib/dsp/dsp.h).
target_include_directories(Parvati SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)

# The vendored Clouds/stmlib sources also include compiled .cc files (units.cc,
# mu_law.cc, and later correlator/pvoc/etc.) kept VERBATIM from upstream. They
# predate Parvati's strict -Werror hygiene (upstream uses double literals in
# float LUTs, sign/int conversions, etc.); exempt them from warnings rather than
# patching the upstream data. (Headers are already suppressed via the SYSTEM
# include root above; this covers the compiled .cc sources.)
file(GLOB_RECURSE _parvati_clouds_cc
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds/*.cc")
if(_parvati_clouds_cc)
    if(NOT MSVC)
        set_source_files_properties(${_parvati_clouds_cc} PROPERTIES COMPILE_OPTIONS "-w")
    else()
        set_source_files_properties(${_parvati_clouds_cc} PROPERTIES COMPILE_OPTIONS "/w")
    endif()
endif()

# ------------------------------------------------------------------------------
# iOS toolchain + signing (M1). Sets the iOS deployment target, distinct bundle
# identifiers for the Standalone app and the AUv3 app-extension, App-Group
# entitlements (so both share one presets/state container — see
# Source/ui/SharedContainer.cpp), and an Automatic signing style. (The device
# family and app-level plist keys are set natively via PARVATI_IOS_PLIST_ARGS in
# the juce_add_plugin call; this block only patches the two keys with no native
# seam.)
#
# The iOS Simulator SDK (iphonesimulator) does NOT require a signing identity or
# a provisioning profile, so it builds with PARVATI_IOS_DEVELOPMENT_TEAM empty.
# A real-device build or App-Store .ipa export requires the user's paid Apple
# Developer team id:
#   cmake -S . -B build_ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
#         -DPARVATI_IOS_DEVELOPMENT_TEAM=XXXXXXXXXX
# (team id / bundle ids are PLACEHOLDERS pending a product/signing decision.)
if(PARVATI_IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
    # PLACEHOLDER team id — empty by default (simulator-only). Set on the cmake
    # command line for device/.ipa builds (see comment above).
    set(PARVATI_IOS_DEVELOPMENT_TEAM "" CACHE STRING
        "Apple Developer team id for iOS device/.ipa builds (empty = simulator-only)")

    # Standalone app bundle: com.805labs.parvati
    set_target_properties(Parvati_Standalone PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.parvati"
        XCODE_ATTRIBUTE_ENABLE_BITCODE NO
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/parvati.entitlements")
    # AUv3 app-extension bundle: com.805labs.parvati.AUv3 (child of the app id,
    # as iOS App-Store validation requires).
    set_target_properties(Parvati_AUv3 PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.parvati.AUv3"
        XCODE_ATTRIBUTE_ENABLE_BITCODE NO
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/parvati-auv3.entitlements")

    if(PARVATI_IOS_DEVELOPMENT_TEAM)
        set_target_properties(Parvati_Standalone Parvati_AUv3 PROPERTIES
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${PARVATI_IOS_DEVELOPMENT_TEAM}")
    endif()

    # ----------------------------------------------------------------------------
    # Info.plist patching — ONLY the keys with no juce_add_plugin seam.
    # Everything else (device family, orientations, UIRequiresFullScreen,
    # background audio, file sharing) is generated NATIVELY by juceaide from the
    # PARVATI_IOS_PLIST_ARGS keyword arguments (see the top of this file), so it
    # cannot be dropped by a configure/build ordering mishap. Two keys remain:
    #
    #   1. LSSupportsOpeningDocumentsInPlace (Standalone) — pairs with
    #      UIFileSharingEnabled: lets the Files app open saved patches in place
    #      rather than copying them out. No juce_add_plugin argument exists.
    #
    #   2. AUv3 CFBundleIdentifier — juceaide hard-codes the extension id as
    #      "<app-id>.<last-component>AUv3" (com.805labs.parvati.parvatiAUv3),
    #      which DISAGREES with this project's PRODUCT_BUNDLE_IDENTIFIER and
    #      entitlements application-identifier (both …parvati.AUv3). The plist
    #      literal wins in the built .appex, so automatic signing / App-Store
    #      validation would fail on a device/.ipa build with an
    #      application-identifier mismatch. Rewrite it to the intended id.
    #
    # Both plists are patched at their SOURCE — the juceaide-generated
    # Parvati_artefacts/JuceLibraryCode/<target>/Info.plist — which juce_add_plugin
    # has already produced during THIS configure (it runs juceaide via
    # execute_process while evaluating the call), so the file provably exists
    # here. CMake then copies it to CMakeFiles/<target>.dir/Info.plist (the real
    # INFOPLIST_FILE) at generate time, AFTER all configure code has run, so the
    # patched content reaches the built bundle. juceaide regenerates the plist
    # fresh on every configure, so Delete/Add is idempotent across re-runs; a
    # final Print + hard FATAL_ERROR replaces the old silent ERROR_QUIET skip —
    # a missing key must FAIL the configure, not silently ship without it.
    set(_parvati_standalone_plist
        "${CMAKE_BINARY_DIR}/Parvati_artefacts/JuceLibraryCode/Parvati_Standalone/Info.plist")
    set(_parvati_auv3_plist
        "${CMAKE_BINARY_DIR}/Parvati_artefacts/JuceLibraryCode/Parvati_AUv3/Info.plist")
    foreach(_plist IN LISTS _parvati_standalone_plist _parvati_auv3_plist)
        if(NOT EXISTS "${_plist}")
            message(FATAL_ERROR
                "juceaide did not generate '${_plist}' during this configure — "
                "the Parvati plist patch step must move after juce_add_plugin.")
        endif()
    endforeach()

    # Standalone: LSSupportsOpeningDocumentsInPlace (see rationale above).
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Delete :LSSupportsOpeningDocumentsInPlace"
            -c "Add :LSSupportsOpeningDocumentsInPlace bool true"
            "${_parvati_standalone_plist}"
        RESULT_VARIABLE _parvati_lssupports_rc ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :LSSupportsOpeningDocumentsInPlace" "${_parvati_standalone_plist}"
        OUTPUT_VARIABLE _parvati_lssupports_val OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _parvati_lssupports_val STREQUAL "true")
        message(FATAL_ERROR
            "Failed to patch LSSupportsOpeningDocumentsInPlace into "
            "'${_parvati_standalone_plist}' (PlistBuddy rc=${_parvati_lssupports_rc}, "
            "value='${_parvati_lssupports_val}').")
    endif()

    # AUv3: rewrite the juceaide-hard-coded CFBundleIdentifier to the intended
    # com.805labs.parvati.AUv3 so it matches PRODUCT_BUNDLE_IDENTIFIER and the
    # entitlements application-identifier (see rationale above). Set (not Add)
    # overwrites the always-present juceaide value.
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Set :CFBundleIdentifier com.805labs.parvati.AUv3"
            "${_parvati_auv3_plist}"
        RESULT_VARIABLE _parvati_auv3_id_rc ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "${_parvati_auv3_plist}"
        OUTPUT_VARIABLE _parvati_auv3_id_val OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _parvati_auv3_id_val STREQUAL "com.805labs.parvati.AUv3")
        message(FATAL_ERROR
            "Failed to set the AUv3 CFBundleIdentifier to com.805labs.parvati.AUv3 "
            "(PlistBuddy rc=${_parvati_auv3_id_rc}, value='${_parvati_auv3_id_val}').")
    endif()

    # SharedContainer.mm is the Obj-C++ resolver for the App-Group container
    # (NSFileManager containerURLForSecurityApplicationGroupIdentifier). It is
    # iOS-only; the desktop path lives in the globbed SharedContainer.cpp and is
    # byte-identical to the previous getSpecialLocation(userApplicationDataDirectory).
    target_sources(Parvati PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SharedContainer.mm")
endif()

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

# ------------------------------------------------------------------------------
# Brand logo (parvati_logo.svg at the repo root) embedded as its OWN
# binary-data target (namespace ParvatiLogo). It is NOT folded into
# parvati_factory_presets: that target's installer iterates every resource and
# would otherwise try to extract the SVG as a preset. parvati_logo.svg is TRUE
# vector art (outlined <path>/<g>, no raster); the editor parses it once with
# JUCE's SVG renderer (Drawable::createFromSVGString) and paints a vector
# Drawable. Accessed via ParvatiLogo::getNamedResource in PluginEditor.cpp
# (forward-declared there — see comment for why we do not #include the
# generated BinaryData.h).
juce_add_binary_data(parvati_logo_assets
    NAMESPACE ParvatiLogo
    SOURCES parvati_logo.svg)
target_link_libraries(Parvati PRIVATE parvati_logo_assets)

# ------------------------------------------------------------------------------
# Compile the JUCE module stack EXACTLY ONCE and share it.
#
# JUCE attaches every module's sources as INTERFACE_SOURCES, so each target
# that links juce::* recompiles the whole stack (~260 MB of objects per target).
# With ~20 test executables that is ~4 GB of duplicated JUCE objects per build
# dir. `Parvati` is a STATIC library that already compiles JUCE once (the 3
# plugin formats share it); we make it the SINGLE source of JUCE for everything:
#   * ensure Parvati compiles the full module set the tests need, and
#   * re-export the modules' USAGE REQUIREMENTS (include dirs, compile defs,
#     compile features, and NON-module link deps such as system frameworks) on
#     Parvati's INTERFACE — but deliberately NOT their INTERFACE_SOURCES, which
#     is what forces the recompilation. Test/tool targets then link ONLY Parvati
#     (JUCE objects + headers + defines + frameworks, compiled once).
set(PARVATI_JUCE_MODULES
    juce::juce_audio_basics    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_audio_formats   juce::juce_audio_devices
    juce::juce_core            juce::juce_data_structures  juce::juce_dsp
    juce::juce_events          juce::juce_graphics
    juce::juce_gui_basics      juce::juce_gui_extra)
# Parvati already compiles these modules once via its own target_link_libraries
# block above; here we only RE-EXPORT their usage requirements (include dirs,
# compile defs, compile features, and NON-module link deps like system
# frameworks) on Parvati's INTERFACE — deliberately NOT their INTERFACE_SOURCES,
# which is what forces every consumer to recompile JUCE. Test/tool targets then
# link ONLY Parvati (JUCE objects + headers + defines + frameworks, once).
foreach(_m ${PARVATI_JUCE_MODULES})
    get_target_property(_inc ${_m} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
        target_include_directories(Parvati INTERFACE ${_inc})
    endif()
    get_target_property(_def ${_m} INTERFACE_COMPILE_DEFINITIONS)
    if(_def)
        target_compile_definitions(Parvati INTERFACE ${_def})
    endif()
    get_target_property(_feat ${_m} INTERFACE_COMPILE_FEATURES)
    if(_feat)
        target_compile_features(Parvati INTERFACE ${_feat})
    endif()
    # Propagate link deps (system frameworks etc.) but NOT other juce::* modules
    # — those would drag their INTERFACE_SOURCES back in and re-trigger the
    # per-target recompilation we are eliminating.
    get_target_property(_libs ${_m} INTERFACE_LINK_LIBRARIES)
    if(_libs)
        foreach(_l ${_libs})
            if(NOT (_l MATCHES "^juce::" OR _l MATCHES "^juce_"))
                target_link_libraries(Parvati INTERFACE ${_l})
            endif()
        endforeach()
    endif()
endforeach()

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
    PARVATI_VERSION="${CMAKE_PROJECT_VERSION}"
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
# JUCE 9 ad-hoc-signs the VST3 bundle, THEN generates Contents/Resources/
# moduleinfo.json (the VST3 manifest), which invalidates the signature seal.
# Hosts then refuse to load it ("a sealed resource is missing or invalid") and
# cache the failure. Re-sign AFTER JUCE's own post-build steps so moduleinfo.json
# is sealed. (The AU has no such file and loads fine without this.) macOS-only
# (VST3 does not exist on iOS — guard so an iOS configure never references the
# VST3 target).
if(APPLE AND NOT PARVATI_IOS)
    add_custom_command(TARGET Parvati_VST3 POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:Parvati_VST3>"
        COMMENT "Re-sign Parvati.vst3 (seal moduleinfo.json)")
endif()

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
)

# ------------------------------------------------------------------------------
# Patch page (Phase 1) arrangement-template verification: apply each built-in
# arrangement, assert the engine state matches the design-spec template table
# (per-part voicecard count, MIDI channel, key zone, polyphony), then assert
# inferArrangement round-trips. Built by default.
# Run with: ./build/parvati_patch_arrangement_test
add_executable(parvati_patch_arrangement_test
    tests/patch_arrangement_test.cpp
)
target_include_directories(parvati_patch_arrangement_test PRIVATE Source)
target_compile_features(parvati_patch_arrangement_test PRIVATE cxx_std_17)
target_link_libraries(parvati_patch_arrangement_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# Per-part FX serialization verification. Proves the Parvati-exclusive FX section
# (3 slots + 16-slot FX mod matrix) round-trips through .parvati (multi + patch)
# and the host binary state, and is DROPPED by the Ambika .PRO/.MUL formats.
# Built by default. Run with: ./build/parvati_fx_preset_test
add_executable(parvati_fx_preset_test
    tests/fx_preset_test.cpp
)
target_include_directories(parvati_fx_preset_test PRIVATE Source)
target_compile_features(parvati_fx_preset_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_preset_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# Per-part FX ROUTING verification. Proves FxChain::process() renders finite,
# wet output for every topology (Series / Parallel12to3 / Parallel1to23) x every
# order permutation, and that an all-disabled chain is a dry passthrough.
# Built by default. Run with: ./build/parvati_fx_routing_test
add_executable(parvati_fx_routing_test
    tests/fx_routing_test.cpp
)
target_include_directories(parvati_fx_routing_test PRIVATE Source)
target_compile_features(parvati_fx_routing_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_routing_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# Clouds FX module (Diffuser / Pitch Shifter / Clouds Reverb) port verification.
# Each vendored module builds via the factory, reports the correct FxType, renders
# a finite in-place stereo block at host rate, and produces audible wet output.
# Built by default. Run with: ./build/parvati_clouds_fx_test
add_executable(parvati_clouds_fx_test
    tests/parvati_clouds_fx_test.cpp
)
target_include_directories(parvati_clouds_fx_test PRIVATE Source)
target_compile_features(parvati_clouds_fx_test PRIVATE cxx_std_17)
target_link_libraries(parvati_clouds_fx_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# FV-1 hardware-emulation FX family verification (Clocked Delay / Ensemble /
# Plate Reverb / Vinyl Compressor / Phaser) + the shared FV-1 framework.
# Each effect is a JUCE-FREE Fv1FxProcessor; these tests compile the effect
# .cpp directly (no JUCE link) and run under ASan+UBSan. Built by default.
# Run with: ./build/parvati_fv1_engine_test  ...and the five per-effect binaries.
set(_parvati_fv1_tests
    fv1_engine_test
    fv1_clocked_delay_test
    fv1_ensemble_test
    fv1_plate_reverb_test
    fv1_vinyl_compressor_test
    fv1_phaser_test
)
foreach(_t ${_parvati_fv1_tests})
    add_executable(parvati_${_t} tests/${_t}.cpp)
    target_include_directories(parvati_${_t} PRIVATE Source)
    target_compile_features(parvati_${_t} PRIVATE cxx_std_17)
    # The framework + every effect is JUCE-free, so each effect test compiles its
    # effect .cpp directly (no Parvati/JUCE link) and stays a fast build.
    if(_t STREQUAL "fv1_clocked_delay_test")
        target_sources(parvati_${_t} PRIVATE Source/dsp/fx/fv1/Fv1ClockedDelay.cpp)
    elseif(_t STREQUAL "fv1_ensemble_test")
        target_sources(parvati_${_t} PRIVATE Source/dsp/fx/fv1/Fv1Ensemble.cpp)
    elseif(_t STREQUAL "fv1_plate_reverb_test")
        target_sources(parvati_${_t} PRIVATE Source/dsp/fx/fv1/Fv1PlateReverb.cpp)
    elseif(_t STREQUAL "fv1_vinyl_compressor_test")
        target_sources(parvati_${_t} PRIVATE Source/dsp/fx/fv1/Fv1VinylCompressor.cpp)
    elseif(_t STREQUAL "fv1_phaser_test")
        target_sources(parvati_${_t} PRIVATE Source/dsp/fx/fv1/Fv1Phaser.cpp)
    endif()
endforeach()

# ------------------------------------------------------------------------------
# FX stereo-balance investigation: (1) proves parallel FX routing is TRUE
# parallel (each branch processes the full stereo pair), and (2) measures whether
# the Spectral effect (Clouds phase vocoder) produces a systematic L/R energy
# imbalance, with Resonator (known odd/even split) and Diffuser (symmetric) as
# controls. Headless: feeds mono-correlated and uncorrelated stereo noise,
# measures R/L RMS ratio over the param sweep. Built by default.
# Run with: ./build_release/parvati_fx_stereo_balance_test
add_executable(parvati_fx_stereo_balance_test
    tests/parvati_fx_stereo_balance_test.cpp
)
target_include_directories(parvati_fx_stereo_balance_test PRIVATE Source)
target_include_directories(parvati_fx_stereo_balance_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_stereo_balance_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_stereo_balance_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# FX mod-matrix @ 980 Hz (internal control rate) verification: proves the FX
# mod matrix now sub-chunks the host block at the ~980 Hz internal-block cadence
# (FxChain::process ~6x per 256-block @48k), that every FX (incl. the 6x-OS
# Wavefolder/RingMod + HostRateBridge Clouds) survives variable sub-block sizes
# (1..512), and the dry path stays bit-identical. Built by default.
# Run with: ./build/parvati_fx_modrate_test
add_executable(parvati_fx_modrate_test
    tests/parvati_fx_modrate_test.cpp
)
target_include_directories(parvati_fx_modrate_test PRIVATE Source)
target_compile_features(parvati_fx_modrate_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_modrate_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# FX mod-matrix voice-source handling: AC/DC coupling (mirrors the synth voice
# path) + representative-voice tracking (most-recently-triggered active voice)
# with a de-click crossfade on voice change. Built by default.
# Run with: ./build/parvati_fx_voice_mod_test
add_executable(parvati_fx_voice_mod_test
    tests/parvati_fx_voice_mod_test.cpp
)
target_include_directories(parvati_fx_voice_mod_test PRIVATE Source)
target_compile_features(parvati_fx_voice_mod_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_voice_mod_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# Synth readout formatter (SynthParamLabels::paramValueTextSynth): pure-function
# input->output checks for every raw-numeric synth knob family. Built by default.
# Run with: ./build/parvati_synth_paramtext_test
add_executable(parvati_synth_paramtext_test
    tests/parvati_synth_paramtext_test.cpp
)
target_include_directories(parvati_synth_paramtext_test PRIVATE Source)
target_compile_features(parvati_synth_paramtext_test PRIVATE cxx_std_17)
target_link_libraries(parvati_synth_paramtext_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# FX pop diagnostic — measures sample-to-sample discontinuities (pops) when
# modulating FX params, to find the root cause. Diagnostic only (prints a table).
# Run with: ./build/parvati_fx_popdiag_test
add_executable(parvati_fx_popdiag_test
    tests/parvati_fx_popdiag_test.cpp
)
target_include_directories(parvati_fx_popdiag_test PRIVATE Source)
target_compile_features(parvati_fx_popdiag_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_popdiag_test PRIVATE
    Parvati
)

# Run with: ./build/parvati_fx_diffuser_diag_test
add_executable(parvati_fx_diffuser_diag_test EXCLUDE_FROM_ALL
    tests/parvati_fx_diffuser_diag.cpp
)
target_include_directories(parvati_fx_diffuser_diag_test PRIVATE Source)
target_include_directories(parvati_fx_diffuser_diag_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_diffuser_diag_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_diffuser_diag_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# Full-engine Clouds-FX continuity regression — renders a sustained note through
# the real processBlock path with each Clouds FX (no mod) and asserts the main
# bus is click-free. This is the ONLY test that exercises renderPartFx's drift-
# corrected sub-chunking end-to-end, so it catches the HostRateBridge m==0
# dropout (and any future orchestration glitch) that isolated fx->process tests
# miss. Built by default. Run with: ./build/parvati_fx_engine_continuity_test
add_executable(parvati_fx_engine_continuity_test
    tests/parvati_fx_engine_continuity.cpp
)
target_include_directories(parvati_fx_engine_continuity_test PRIVATE Source)
target_include_directories(parvati_fx_engine_continuity_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_engine_continuity_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_engine_continuity_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# HostRateBridge tiny-chunk (m==0) dropout regression — asserts a 1-sample
# block (renderPartFx's drift-corrected sub-chunking periodically emits one)
# does NOT zero the output (the audible Clouds-FX crackle root cause). Built by
# default. FAILS if the m==0 hold fix in HostRateBridge is reverted.
# Run with: ./build/parvati_fx_bridge_tinychunk_test
add_executable(parvati_fx_bridge_tinychunk_test
    tests/parvati_fx_bridge_tinychunk.cpp
)
target_include_directories(parvati_fx_bridge_tinychunk_test PRIVATE Source)
target_include_directories(parvati_fx_bridge_tinychunk_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_bridge_tinychunk_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_bridge_tinychunk_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# HostRateBridge sub-chunk pop regression — asserts every Clouds (bridge) FX is
# pop-clean when processed in 980 Hz sub-chunks (the engine's normal cadence).
# FAILS if the bridge head-overlap fix is reverted. Built by default.
# Run with: ./build/parvati_fx_bridge_pop_test
add_executable(parvati_fx_bridge_pop_test
    tests/parvati_fx_bridge_pop_test.cpp
)
target_include_directories(parvati_fx_bridge_pop_test PRIVATE Source)
target_include_directories(parvati_fx_bridge_pop_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_bridge_pop_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_bridge_pop_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# Virtual-keyboard interaction test (glissando drag-retarget note ordering,
# press/release routing, off-key release guard). Built by default.
# Run with: ./build/parvati_keyboard_view_test
add_executable(parvati_keyboard_view_test
    tests/keyboard_view_test.cpp
)
target_include_directories(parvati_keyboard_view_test PRIVATE Source)
target_compile_features(parvati_keyboard_view_test PRIVATE cxx_std_17)
target_link_libraries(parvati_keyboard_view_test PRIVATE
    Parvati
)

# CentralModBar pill click-selection regression test (P3). Injects real
# peer-level mouse clicks on generator pills in the live editor and asserts
# the active generator editor swaps. Built by default.
# Run: ./build/parvati_modbar_click_test
add_executable(parvati_modbar_click_test
    tests/modbar_pill_click_test.cpp
)
target_include_directories(parvati_modbar_click_test PRIVATE Source)
target_compile_features(parvati_modbar_click_test PRIVATE cxx_std_17)
target_link_libraries(parvati_modbar_click_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# iPad M5 HIG sizing-contract test. Asserts the iOS layout constants equal the
# HIG touch minimums (44/48/8) under an iOS build and the desktop constants are
# UNCHANGED (every HIG change is #if JUCE_IOS). Static + runtime asserts guard
# the compile-time-gated sizing contract in CI. Built by default.
# Run with: ./build/parvati_ipad_hig_sizing_test
add_executable(parvati_ipad_hig_sizing_test
    tests/ipad_hig_sizing_test.cpp
)
target_include_directories(parvati_ipad_hig_sizing_test PRIVATE Source)
target_compile_features(parvati_ipad_hig_sizing_test PRIVATE cxx_std_17)
target_link_libraries(parvati_ipad_hig_sizing_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# Dev tool: render the full Parvati editor screen for each page to a PNG
# (offscreen, via paintEntireComponent — no screen-recording permission needed)
# so they can be fed to a UI-redesign agent. Mirrors parvati_multigui_test's link set.
# Build: cmake --build build_release --target parvati_screen_shots
# Run:   ./build_release/parvati_screen_shots [outputDir] [scale]   (defaults: ./screens, 2)
#
# CANONICAL regeneration (builds the tool from the LATEST source, then runs it):
#   cmake --build build_release --target screens
# Do NOT run ./build_release/parvati_screen_shots by hand after editing UI code
# unless you rebuilt that target first — a stale binary renders stale screens.
add_executable(parvati_screen_shots EXCLUDE_FROM_ALL
    tools/screen_shots.cpp
)
target_include_directories(parvati_screen_shots PRIVATE Source)
target_compile_features(parvati_screen_shots PRIVATE cxx_std_17)
target_link_libraries(parvati_screen_shots PRIVATE
    Parvati
)

# `screens`: the foolproof way to regenerate ./screens. DEPENDS forces a Parvati
# recompile + parvati_screen_shots relink from the latest source whenever any UI
# code changed, THEN runs the tool. Always use this instead of invoking the
# binary directly (which can render from a stale build).
add_custom_target(screens
    COMMAND "$<TARGET_FILE:parvati_screen_shots>" "${CMAKE_SOURCE_DIR}/screens" 2
    DEPENDS parvati_screen_shots
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating ./screens from the latest Parvati build"
    VERBATIM
)

# ------------------------------------------------------------------------------
# Dev tool: render Parvati's themed right-click / popup menus to PNGs (offscreen,
# via the LookAndFeel draw hooks the live PopupMenu calls) so they can be fed to
# an LLM. Mirrors parvati_screen_shots's link set.
# Build: cmake --build build_release --target parvati_menu_shots
# Run:   ./build_release/parvati_menu_shots [outputDir] [scale]   (defaults: ./screens/menus, 2)
add_executable(parvati_menu_shots EXCLUDE_FROM_ALL
    tools/menu_shots.cpp
)
target_include_directories(parvati_menu_shots PRIVATE Source)
target_compile_features(parvati_menu_shots PRIVATE cxx_std_17)
target_link_libraries(parvati_menu_shots PRIVATE
    Parvati
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
)
target_include_directories(parvati_patch_test PRIVATE Source)
target_compile_features(parvati_patch_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_patch_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_patch_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# Arp/Sequencer timing + note-lifecycle unit tests (direct, no engine). Proves
# the prescaler-gating fix (seq runs at the arp's step rate, not 24x) and the
# allNotesOff / defensive self-clean (no stuck note). Built by default.
# Run with: ./build/parvati_arp_seq_timing_test
add_executable(parvati_arp_seq_timing_test
    tests/parvati_arp_seq_timing_test.cpp
)
target_include_directories(parvati_arp_seq_timing_test PRIVATE Source)
target_compile_features(parvati_arp_seq_timing_test PRIVATE cxx_std_17)
target_link_libraries(parvati_arp_seq_timing_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# ModDestMap unit test: the paramID <-> MOD_DST mapping table + per-slot amount
# aggregation (aggregateAmount / slotsForDest) used by the mod-ring, hover-
# highlight, and drag-and-drop features. Built by default.
# Run with: ./build/parvati_mod_dest_map_test
add_executable(parvati_mod_dest_map_test
    tests/mod_dest_map_test.cpp
)
target_include_directories(parvati_mod_dest_map_test PRIVATE Source)
target_compile_features(parvati_mod_dest_map_test PRIVATE cxx_std_17)
target_link_libraries(parvati_mod_dest_map_test PRIVATE
    Parvati
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
)

# Diagnostic / regression guard (built by default, runs in the suite): create a
# fresh processor + editor and assert the amp-envelope modulation
# (mod11_amount = ENV3->VCA) is preserved and the engine stays audible after
# the editor (ModMatrixView + ring listeners + drag affordance) is created.
# Guards the subtle "editor creation clobbers the amp routing -> silence"
# regression. Run with: ./build/parvati_mod_audio_test
add_executable(parvati_mod_audio_test
    tests/mod_audio_test.cpp
)
target_include_directories(parvati_mod_audio_test PRIVATE Source)
target_compile_features(parvati_mod_audio_test PRIVATE cxx_std_17)
target_link_libraries(parvati_mod_audio_test PRIVATE
    Parvati
)

add_executable(parvati_concurrency_test
    tests/concurrency_test.cpp
)
target_include_directories(parvati_concurrency_test PRIVATE Source)
target_compile_features(parvati_concurrency_test PRIVATE cxx_std_17)
target_compile_definitions(parvati_concurrency_test PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(parvati_concurrency_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# Mono-legato regression test (overlap/slide-back silence bug).
add_executable(parvati_legato_test
    tests/legato_test.cpp
)
target_include_directories(parvati_legato_test PRIVATE Source)
target_compile_features(parvati_legato_test PRIVATE cxx_std_17)
target_link_libraries(parvati_legato_test PRIVATE
    Parvati
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
)

add_executable(parvati_patch_load_test
    tests/patch_load_test.cpp
)
target_include_directories(parvati_patch_load_test PRIVATE Source)
target_compile_features(parvati_patch_load_test PRIVATE cxx_std_17)
target_link_libraries(parvati_patch_load_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# Host plugin-state persistence verification. Built by default.
# Run with: ./build/parvati_host_state_test
add_executable(parvati_host_state_test
    tests/host_state_test.cpp
)
target_include_directories(parvati_host_state_test PRIVATE Source)
target_compile_features(parvati_host_state_test PRIVATE cxx_std_17)
target_link_libraries(parvati_host_state_test PRIVATE
    Parvati
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
)

# ------------------------------------------------------------------------------
# COMPREHENSIVE SYNTH PARAMETER COVERAGE (consolidated, one ~6.5MB binary).
# Sweeps EVERY synth descriptor parameter (105 patch/part + 67 sequencer +
# 5 arp + 4 options = 181 params) and verifies the INTENDED vs REAL outcome:
# each param routes to the correct Patch/Part byte (or engine state) AND, where
# audio-meaningful, changes the rendered audio. Consolidates what would
# otherwise be ~20 separate 6.5MB binaries into one. Built by default.
# Run with: ./build_release/parvati_synth_param_coverage_test
add_executable(parvati_synth_param_coverage_test
    tests/synth_param_coverage_test.cpp
)
target_include_directories(parvati_synth_param_coverage_test PRIVATE Source)
target_compile_features(parvati_synth_param_coverage_test PRIVATE cxx_std_17)
target_link_libraries(parvati_synth_param_coverage_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# COMPREHENSIVE FX PARAMETER + MODULE COVERAGE (consolidated, one ~6.5MB binary).
# Covers every one of the 16 FX types (type/prepare/reset/finite/wet-differs),
# every effect's 5 slot params (swept min/mid/max -> output changes), every FX
# topology + order permutation, the master section (mix + 3-band EQ), and the
# full 16x18 FX mod matrix (every destination reaches the DSP via debugGetParam).
# Consolidates what would otherwise be ~15 separate 6.5MB binaries into one.
# Built by default. Run with: ./build_release/parvati_fx_param_coverage_test
add_executable(parvati_fx_param_coverage_test
    tests/fx_param_coverage_test.cpp
)
target_include_directories(parvati_fx_param_coverage_test PRIVATE Source)
target_include_directories(parvati_fx_param_coverage_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)
target_compile_features(parvati_fx_param_coverage_test PRIVATE cxx_std_17)
target_link_libraries(parvati_fx_param_coverage_test PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# `deploy` — one-command release workflow: build ALL plugin formats (VST3, AU,
# Standalone) + render ./screens from the latest source + install the VST3 and
# AU bundles into the user plug-in folders. Use this instead of a plain build so
# the installed plug-ins and the screenshots are never stale (a plain
# `--target Parvati` does NOT rebuild the VST3/AU format wrappers).
# macOS-only: iOS has no VST3/AU targets and uses `deploy_ios` instead.
#   cmake --build build_release --target deploy
if(NOT PARVATI_IOS)
set(_parvati_plugins_dir "$ENV{HOME}/Library/Audio/Plug-Ins")
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E echo "deploy: install VST3"
    COMMAND rm -rf "${_parvati_plugins_dir}/VST3/Parvati.vst3"
    COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/VST3/Parvati.vst3" "${_parvati_plugins_dir}/VST3/Parvati.vst3"
    COMMAND ${CMAKE_COMMAND} -E echo "deploy: install AU"
    COMMAND rm -rf "${_parvati_plugins_dir}/Components/Parvati.component"
    COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/AU/Parvati.component" "${_parvati_plugins_dir}/Components/Parvati.component"
    COMMAND ${CMAKE_COMMAND} -E echo "deploy complete: built all formats, rendered screens, installed VST3 + AU"
    DEPENDS Parvati_VST3 Parvati_AU Parvati_Standalone screens
    COMMENT "Build all formats + render screens + install VST3/AU"
    VERBATIM
)
endif()

# ------------------------------------------------------------------------------
# `deploy_ios` — TEMPLATE: archive the iOS Standalone app + AUv3 app-extension
# into an .ipa via `xcodebuild archive` + `-exportArchive`. This is a documented
# placeholder: a real device/.ipa export REQUIRES a signing identity + provisioning
# profile (set PARVATI_IOS_DEVELOPMENT_TEAM and supply a matching exportOptions.plist
# with your method/provisioning profile). With no identity it will fail at the
# codesign step — expected until the user supplies signing. iOS-only.
#   cmake --build build_ios --target deploy_ios
if(PARVATI_IOS)
    add_custom_target(deploy_ios
        COMMAND ${CMAKE_COMMAND} -E echo
            "deploy_ios: archive + export .ipa (requires a signing identity + PARVATI_IOS_DEVELOPMENT_TEAM)"
        COMMAND xcodebuild -archivePath "${CMAKE_BINARY_DIR}/Parvati.xcarchive"
                         -scheme Parvati_Standalone
                         -sdk iphoneos
                         -configuration Release
                         archive
        COMMAND xcodebuild -exportArchive
                         -archivePath "${CMAKE_BINARY_DIR}/Parvati.xcarchive"
                         -exportPath "${CMAKE_BINARY_DIR}/ipa"
                         -exportOptionsPlist "${CMAKE_CURRENT_SOURCE_DIR}/ios/exportOptions.plist"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Archive the iOS Standalone + AUv3 and export an .ipa (needs signing)"
        VERBATIM
    )
endif()
