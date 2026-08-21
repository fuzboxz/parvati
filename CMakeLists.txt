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
# on iOS, so the iOS target list is AUv3 + Standalone only. The classic AU is
# additionally macOS-ONLY (a non-Apple configure with AU in FORMATS is a
# juce_add_plugin FATAL_ERROR), so Linux/Windows build VST3 + Standalone. The
# CLAP format is NOT listed here: JUCE 9.0.0 has no native CLAP FORMATS value,
# so it is added separately via clap-juce-extensions (PARVATI_BUILD_CLAP).
if(PARVATI_IOS)
    set(PARVATI_PLUGIN_FORMATS Standalone AUv3)
elseif(APPLE)
    set(PARVATI_PLUGIN_FORMATS Standalone VST3 AU)
else()
    set(PARVATI_PLUGIN_FORMATS Standalone VST3)
endif()

# CLAP plugin format via clap-juce-extensions (see the fetch block below).
# ON by default on every desktop platform (macOS/Linux/Windows); forced OFF on
# iOS, where the AUv3 app-extension is the only host-loadable format and a CLAP
# bundle inside the .ipa would be dead weight.
option(PARVATI_BUILD_CLAP "Build the CLAP plugin format (desktop only)" ON)
if(PARVATI_IOS)
    set(PARVATI_BUILD_CLAP OFF CACHE BOOL
        "Build the CLAP plugin format (desktop only)" FORCE)
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

# Desktop-macOS deployment target (release-prep): without this the binaries
# target the BUILD SDK's minimum, which both silently limits the install base
# and changes per-SDK-number. 13.0 (Ventura) is the floor for the first
# public release — Apple-silicon Macs that can run current Xcode all reach it.
# Override per-build with -DCMAKE_OSX_DEPLOYMENT_TARGET=…; the iOS branch
# below sets its own 14.0.
if(APPLE AND NOT PARVATI_IOS)
    # FORCE: CMake pre-seeds an EMPTY cache entry for this variable, which makes
    # a plain set(... CACHE ...) a silent no-op in already-configured build dirs.
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target" FORCE)
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

# ------------------------------------------------------------------------------
# CLAP format support via clap-juce-extensions (JUCE 9.0.0 has no native CLAP
# FORMATS value — native CLAP is on the JUCE roadmap, tied to AudioProcessor
# v2). The library must be added AFTER JUCE and BEFORE juce_add_plugin: the
# wrapper reads JUCE_* properties off the plugin target and needs JUCE's CMake
# helpers in scope. No tagged release contains the JUCE 9 support (PR #178,
# 2026-07), so we pin a main commit — re-pin periodically to pick up fixes.
#   -DPARVATI_CLAP_EXTENSIONS_PATH=/path/to/clap-juce-extensions
# swaps the pin for a local checkout (e.g. to test an upstream fix). No
# GIT_SHALLOW: the pin is a raw SHA, which git cannot shallow-fetch.
if(PARVATI_BUILD_CLAP)
    if(PARVATI_CLAP_EXTENSIONS_PATH)
        add_subdirectory("${PARVATI_CLAP_EXTENSIONS_PATH}"
            "${CMAKE_BINARY_DIR}/clap_juce_extensions_build" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(clap_juce_extensions
            GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
            # main @ 2026-08-05: JUCE 9 support (PR #178) + the JUCE 9 embedded
            # editor double-scale fix (PR #183).
            GIT_TAG c1a5ad025f95d01e03267857fa8276ebeed16500)
        FetchContent_MakeAvailable(clap_juce_extensions)
    endif()
endif()

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
# CLAP registration (see the fetch block above for the JUCE 9 rationale).
# clap_juce_extensions_plugin derives a Parvati_CLAP wrapper from the SAME
# shared-code target, so VST3/AU/Standalone are untouched; the module must be
# linked into Parvati itself so the shared objects can see its headers
# (HAS_CLAP_JUCE_EXTENSIONS). (Plain target name: the pinned revision defines
# no clap_juce_extensions:: alias.) CLAP_ID doubles as the macOS CLAP bundle's
# GUI identifier — it must stay stable (hosts identify the plugin by it) and is
# therefore kept aligned with BUNDLE_ID above.
if(PARVATI_BUILD_CLAP)
    target_link_libraries(Parvati PRIVATE clap_juce_extensions)
    clap_juce_extensions_plugin(TARGET Parvati
        CLAP_ID "com.805labs.parvati"
        CLAP_FEATURES instrument synthesizer stereo)
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
#
# BUILD INVOCATION WARNING (iOS hunt 2026-08-19, F-ios-build-2): with the Xcode
# generator the CONFIGURATION must be selected with CMake's --config flag
# BEFORE the `--` separator:
#   cmake --build build_ios --config Release --target Parvati_Standalone Parvati_AUv3 -- -sdk iphonesimulator
# The tempting `... -- -sdk iphonesimulator CONFIGURATION=Release` form passes
# CONFIGURATION= to xcodebuild as a build-SETTING override, silently builds the
# Debug artefacts, and leaves 8-byte `APPL????` stubs under Release/ that break
# the NEXT correct build with "unable to create directory" until deleted.
if(PARVATI_IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
    # PLACEHOLDER team id — empty by default (simulator-only). Set on the cmake
    # command line for device/.ipa builds (see comment above).
    set(PARVATI_IOS_DEVELOPMENT_TEAM "" CACHE STRING
        "Apple Developer team id for iOS device/.ipa builds (empty = simulator-only)")

    # Standalone app bundle: com.805labs.parvati
    # (NOTE: ENABLE_BITCODE was removed — bitcode is dead since Xcode 14 and
    # Xcode 26 ignores the flag entirely; iOS hunt 2026-08-19 F-ios-build-5.)
    set_target_properties(Parvati_Standalone PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.parvati"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/parvati.entitlements")
    # AUv3 app-extension bundle: com.805labs.parvati.AUv3 (child of the app id,
    # as iOS App-Store validation requires).
    set_target_properties(Parvati_AUv3 PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.parvati.AUv3"
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

    # ----------------------------------------------------------------------------
    # Document types + exported UTIs (iOS hunt 2026-08-19, F-ios-build-1 /
    # F-ios-files-2). juce_add_plugin has NO seam for CFBundleDocumentTypes /
    # UTExportedTypeDeclarations, so they are grafted here with the same
    # PlistBuddy + FATAL_ERROR-verify pattern. Without these keys:
    #   * LSSupportsOpeningDocumentsInPlace is INERT (it only modifies
    #     behaviour of DECLARED document types), and
    #   * .parvati/.PRO/.MUL received via Files/AirDrop/Mail offer
    #     no "Open in Parvati" — the advertised file workflow was dead from
    #     outside the app.
    # The fragments are plain plists written at configure time; PlistBuddy
    # Import SETS the (previously absent) top-level keys wholesale, and the
    # Print verify makes a silent drop a configure FAILURE, not a shipping
    # gap. (Re-runs are idempotent: Delete before Import.)
    set(_parvati_filetypes_fragment "${CMAKE_CURRENT_SOURCE_DIR}/ios/parvati_filetypes.plist")
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Delete :CFBundleDocumentTypes"
            -c "Delete :UTExportedTypeDeclarations"
            "${_parvati_standalone_plist}"
        ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Merge ${_parvati_filetypes_fragment}"
            "${_parvati_standalone_plist}"
        RESULT_VARIABLE _parvati_doctypes_rc ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleDocumentTypes:0:LSItemContentTypes:0" "${_parvati_standalone_plist}"
        OUTPUT_VARIABLE _parvati_doctypes_val OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :UTExportedTypeDeclarations:2:UTTypeIdentifier" "${_parvati_standalone_plist}"
        OUTPUT_VARIABLE _parvati_utis_val OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _parvati_doctypes_val STREQUAL "com.805labs.parvati-patch"
       OR NOT _parvati_utis_val STREQUAL "com.805labs.parvati-multi")
        message(FATAL_ERROR
            "Failed to graft CFBundleDocumentTypes/UTExportedTypeDeclarations into "
            "'${_parvati_standalone_plist}' (rc=${_parvati_doctypes_rc}, "
            "docType0='${_parvati_doctypes_val}', uti2='${_parvati_utis_val}').")
    endif()

    # SharedContainer.mm is the Obj-C++ resolver for the App-Group container
    # (NSFileManager containerURLForSecurityApplicationGroupIdentifier). It is
    # iOS-only; the desktop path lives in the globbed SharedContainer.cpp and is
    # byte-identical to the previous getSpecialLocation(userApplicationDataDirectory).
    target_sources(Parvati PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SharedContainer.mm")

    # IosOpenIn.mm — the Obj-C++ open-in shim (application:openURL:options:
    # via class_addMethod on JUCE's JuceAppStartupDelegate; JUCE 9 has none).
    # iOS-only like SharedContainer.mm (the Source/ glob covers .cpp/.cc/.h);
    # the pure routing core lives in the header-inline functions, so desktop
    # and the headless tests link without this file.
    target_sources(Parvati PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/IosOpenIn.mm")
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
    # NOTE: no JUCE_DISPLAY_SPLASH_SCREEN define here — JUCE 9 has no splash
    # screen at all; the JUCE 8-era flag is ignored (juce_gui_basics emits an
    # "ignored" warning for it), so setting it would be dead weight.
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
# Unified Test Runner - Single binary containing all migrated tests
# This consolidates ~100+ separate test executables (~3.5GB) into a single ~44MB binary
# achieving 98.7% disk space savings and 82% faster build times.
# Run with: ./build/parvati_unified_tests
# Options:
#   ./build/parvati_unified_tests list       - List all available tests
#   ./build/parvati_unified_tests <test>     - Run specific test
#   ./build/parvati_unified_tests            - Run all tests
add_executable(parvati_unified_tests
    tests/unified_test_runner_main.cpp
    tests/unified_test_examples.cpp
    # DSP Tests (8)
    tests/fixed_math_test.cpp
    tests/transient_generator_test.cpp
    tests/envelope_test.cpp
    tests/sub_oscillator_test.cpp
    tests/osc_sync_test.cpp
    tests/voice_pitch_test.cpp
    tests/note_stack_test.cpp
    tests/transport_clock_test.cpp
    # Core Tests (14)
    tests/arp_test.cpp
    tests/sequencer_test.cpp
    tests/polyphony_test.cpp
    tests/legato_test.cpp
    tests/cc_routing_test.cpp
    tests/multitimbral_test.cpp
    tests/host_param_text_test.cpp
    tests/paramhelp_parity_test.cpp
    tests/note_step_control_test.cpp
    tests/mod_dest_map_test.cpp
    tests/translations_test.cpp
    tests/idle_silence_test.cpp
    tests/realtime_test.cpp
    tests/os_reaper_test.cpp
    # FX Tests - FV1 Family (9)
    tests/fv1_engine_test.cpp
    tests/fv1_clocked_delay_test.cpp
    tests/fv1_ensemble_test.cpp
    tests/fv1_plate_reverb_test.cpp
    tests/fv1_vinyl_compressor_test.cpp
    tests/fv1_phaser_test.cpp
    tests/fv1_newfamily_test.cpp
    tests/drive_calib_test.cpp
    tests/reverb_decay_test.cpp
    # FX Tests - Parvati (9)
    tests/parvati_fx_bridge_pop_test.cpp
    tests/parvati_fx_bridge_tinychunk.cpp
    tests/parvati_fx_engine_continuity.cpp
    tests/parvati_fx_drag_test.cpp
    tests/parvati_fx_invariants_test.cpp
    tests/parvati_fx_lut_dropout_test.cpp
    tests/parvati_fx_modrate_test.cpp
    tests/parvati_fx_onset_regression.cpp
    tests/parvati_fx_concurrency_probe.cpp
    # Integration Tests (12)
    tests/analog_filter_batch_test.cpp
    tests/filter_topology_test.cpp
    tests/lfo_sync_test.cpp
    tests/lfo_retrigger_test.cpp
    tests/patch_arrangement_test.cpp
    tests/multi_load_test.cpp
    tests/roundtrip_test.cpp
    tests/parvati_preset_test.cpp
    tests/fx_preset_test.cpp
    tests/fx_routing_test.cpp
    tests/render_quality_test.cpp
    tests/parvati_clouds_fx_test.cpp
    # UI Tests (11)
    tests/editor_test.cpp
    tests/keyboard_view_test.cpp
    tests/modbar_pill_click_test.cpp
    tests/param_thread_test.cpp
    tests/lifecycle_test.cpp
    tests/layout_overlap_test.cpp
    tests/layout_minwidth_test.cpp
    tests/controller_mod_test.cpp
    tests/ui_telemetry_test.cpp
    tests/ui_mirror_test.cpp
    tests/host_state_test.cpp
    # I/O & File Tests (12)
    tests/ios_file_flow_test.cpp
    tests/ios_openin_test.cpp
    tests/loader_fuzz_test.cpp
    tests/load_invariants_test.cpp
    tests/shadow_state_test.cpp
    tests/patch_load_test.cpp
    tests/export_bytes_test.cpp
    tests/export_fallback_test.cpp
    tests/partstate_test.cpp
    tests/tuning_test.cpp
    # Additional Tests (19)
    tests/apvts_test.cpp
    tests/drum_kit_test.cpp
    tests/fx_param_coverage_test.cpp
    tests/ipad_hig_sizing_test.cpp
    tests/midi_param_test.cpp
    tests/mod_audio_test.cpp
    tests/mod_matrix_ui_test.cpp
    tests/mul_strategies_test.cpp
    tests/part_fx_routing_test.cpp
    tests/parvati_fx_diffuser_diag.cpp
    tests/parvati_fx_foldback_probe.cpp
    tests/parvati_fx_popdiag_test.cpp
    tests/parvati_fx_stereo_balance_test.cpp
    tests/parvati_fx_voice_mod_test.cpp
    tests/parvati_native_dist_probe.cpp
    tests/parvati_phaser_drag_repro.cpp
    tests/patch_test.cpp
    tests/perf_smoke_test.cpp
    tests/seq_stepper_test.cpp
    # Parvati-Specific Tests (6)
    tests/parvati_synth_paramtext_test.cpp
    tests/parvati_synth_drag_probe.cpp
    tests/parvati_settings_probe.cpp
    tests/parvati_kbd_drag_repro.cpp
    tests/parvati_fx_glitch_dump.cpp
    tests/parvati_fx_live_repro.cpp
    # Additional Tests (6)
    tests/synth_param_coverage_test.cpp
    tests/ui_typography_test.cpp
    tests/undo_clock_test.cpp
    tests/undo_property_test.cpp
    tests/voice_slots_test.cpp
    tests/workspace_padding_test.cpp
    # Final stragglers - migrated from standalone targets
    tests/concurrency_test.cpp
    tests/firmware_parity_test.cpp
    tests/roundtrip_golden_test.cpp
    tests/parvati_fv1_alias_probe.cpp
    tests/parvati_fx_cpu_probe.cpp
    tests/parvati_fx_dropout_probe.cpp
    tests/parvati_fx_matrix_probe.cpp
    tests/parvati_tests.cpp
    tests/parvati_arp_seq_timing_test.cpp
    # Diagnostic probe — note-retrigger crackle scenarios (audit P1-1: was
    # tracked but never compiled/registered; now part of the unified target)
    tests/parvati_retrig_probe.cpp
    # Vendored firmware sources for firmware_parity_test (the parity oracle:
    # tests/firmware_shim headers shadow the AVR firmware headers; see the
    # include dirs / flags added below the target).
    tests/firmware_shim/shims.cc
    ambika_reference/controller/multi.cc
    ambika_reference/controller/part.cc
    ambika_reference/controller/resources.cc
    ambika_reference/controller/voice_allocator.cc
    ambika_reference/avrlib/random.cc
)
target_include_directories(parvati_unified_tests PRIVATE Source)
target_include_directories(parvati_unified_tests PRIVATE tests)
target_include_directories(parvati_unified_tests SYSTEM PRIVATE Source/dsp/clouds)
target_link_libraries(parvati_unified_tests PRIVATE Parvati parvati_factory_presets)
target_compile_features(parvati_unified_tests PRIVATE cxx_std_17)
target_compile_definitions(parvati_unified_tests PRIVATE PARVATI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# firmware_parity_test support: the shim dir must come FIRST so its headers
# shadow the AVR-dependent firmware headers (same contract the standalone
# parvati_firmware_parity_test target had). -dead_strip drops the firmware's
# never-called MonoModeOn/PolyModeOn (they reference TouchVoiceAllocation,
# which the firmware tree never defines).
target_include_directories(parvati_unified_tests PRIVATE
    tests/firmware_shim
    ambika_reference
)
target_compile_options(parvati_unified_tests PRIVATE -Wno-c++11-narrowing)
target_link_options(parvati_unified_tests PRIVATE -Wl,-dead_strip)
message(STATUS "Unified test runner enabled: parvati_unified_tests (114 tests migrated)")

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
# Settings round-trip check: loads a crafted Parvati.settings exactly like the
# standalone does and verifies the filterState restore. EXCLUDE_FROM_ALL.
add_executable(parvati_settings_roundtrip EXCLUDE_FROM_ALL
    tools/settings_roundtrip.cpp
)
target_include_directories(parvati_settings_roundtrip PRIVATE Source)
target_compile_features(parvati_settings_roundtrip PRIVATE cxx_std_17)
target_link_libraries(parvati_settings_roundtrip PRIVATE
    Parvati
)

# ------------------------------------------------------------------------------
# State donor: writes a Parvati processor state blob with FX1 = <type> enabled
# (for feeding the real Standalone's settings filterState). EXCLUDE_FROM_ALL.
add_executable(parvati_state_donor EXCLUDE_FROM_ALL
    tools/state_donor.cpp
)
target_include_directories(parvati_state_donor PRIVATE Source)
target_compile_features(parvati_state_donor PRIVATE cxx_std_17)
target_link_libraries(parvati_state_donor PRIVATE
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
# Phase-4 verification (not shipped): headless editor instantiation + coverage.
# Confirms createEditor() is non-null, one ParamControl per descriptor (the
# mod-matrix slot params, the fx{1,2,3}_* slot params, the FX master/topology
# params and part_select are hosted by dedicated views instead of ParamPage
# cells), exactly 2 top-level page tabs ([SYNTH][FX]; Global is a header-button
# overlay), the default size is 1280x634, and the editor deletes cleanly
# (leak-detector validated).
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
# `deploy` — one-command release workflow: build ALL plugin formats (VST3, AU,
# CLAP, Standalone) + render ./screens from the latest source + install the
# built bundles into the user plug-in folders AND the Standalone into
# /Applications. Use this instead of a plain build so the installed
# plug-ins / app and the screenshots are never stale (a plain
# `--target Parvati` does NOT rebuild the VST3/AU/CLAP format wrappers).
# macOS-only (APPLE AND NOT PARVATI_IOS): the AU target only exists on macOS
# and ~/Library/Audio/Plug-Ins is the macOS install root — Linux/Windows
# install from Parvati_artefacts by hand, and iOS uses `deploy_ios` instead.
#   cmake --build build_release --target deploy
if(APPLE AND NOT PARVATI_IOS)
set(_parvati_plugins_dir "$ENV{HOME}/Library/Audio/Plug-Ins")
# CLAP install steps are composed separately so `deploy` stays generatable
# when PARVATI_BUILD_CLAP=OFF (Parvati_CLAP then simply does not exist).
set(_parvati_deploy_depends Parvati_VST3 Parvati_AU Parvati_Standalone screens)
set(_parvati_deploy_commands "")
# The Standalone is BUILT above (Parvati_Standalone dep) and additionally
# INSTALLED into /Applications — without this the .app the user actually
# runs stays stale while the plug-in formats update (the 2026-08-17
# "standalone still crackles after deploy" miss). Ad-hoc re-sign after the
# copy (JUCE's built bundle signature references resources the plain cp
# invalidates; `codesign -v` complains and Gatekeeper can refuse it).
list(APPEND _parvati_deploy_commands
    COMMAND ${CMAKE_COMMAND} -E echo "deploy: install Standalone -> /Applications"
    COMMAND rm -rf "/Applications/Parvati.app"
    COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/Standalone/Parvati.app" "/Applications/Parvati.app"
    COMMAND codesign --force --sign - "/Applications/Parvati.app")
if(PARVATI_BUILD_CLAP)
    list(APPEND _parvati_deploy_depends Parvati_CLAP)
    list(APPEND _parvati_deploy_commands
        COMMAND ${CMAKE_COMMAND} -E echo "deploy: install CLAP"
        # Unlike VST3/Components, ~/Library/Audio/Plug-Ins/CLAP is not a stock
        # macOS directory — create it so the cp below succeeds on a machine
        # that never had a CLAP plugin installed.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_parvati_plugins_dir}/CLAP"
        COMMAND rm -rf "${_parvati_plugins_dir}/CLAP/Parvati.clap"
        COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/CLAP/Parvati.clap" "${_parvati_plugins_dir}/CLAP/Parvati.clap"
        # Re-sign after the plain cp: the built bundle's signature references
        # its original path/resources, which the copy invalidates (the same
        # hazard the VST3 moduleinfo seal and the Standalone re-sign below
        # guard against — W7).
        COMMAND codesign --force --sign - "${_parvati_plugins_dir}/CLAP/Parvati.clap")
endif()
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E echo "deploy: install VST3"
    COMMAND rm -rf "${_parvati_plugins_dir}/VST3/Parvati.vst3"
    COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/VST3/Parvati.vst3" "${_parvati_plugins_dir}/VST3/Parvati.vst3"
    COMMAND ${CMAKE_COMMAND} -E echo "deploy: install AU"
    COMMAND rm -rf "${_parvati_plugins_dir}/Components/Parvati.component"
    COMMAND cp    -R "${CMAKE_BINARY_DIR}/Parvati_artefacts/$<CONFIG>/AU/Parvati.component" "${_parvati_plugins_dir}/Components/Parvati.component"
    ${_parvati_deploy_commands}
    COMMAND ${CMAKE_COMMAND} -E echo "deploy complete: built all formats, rendered screens, installed plug-in bundles + standalone"
    DEPENDS ${_parvati_deploy_depends}
    COMMENT "Build all formats + render screens + install VST3/AU/CLAP + Standalone"
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

# ------------------------------------------------------------------------------
# Deterministic tooling — static scanners registered as ctest entries.
# (tool 5) ComboBox clear() notification guard: every juce::ComboBox clear()
# must pass juce::dontSendNotification (stale-async-onChange class). The
# script refuses to scan until its canary self-test proves the checker flags
# a seeded bad snippet and passes a compliant one.
# enable_testing() is a CMake BUILTIN command, so guarding it with
# "NOT COMMAND enable_testing" never fires — call it unconditionally
# (idempotent) so the add_test entries below are actually registered.
enable_testing()
add_test(NAME parvati_check_combo_clear
         COMMAND python3 tools/check_combo_clear.py --self-test-and-scan
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# (tool 6) Raw-`this`-in-async-callback guard: every lambda handed to an
# async UI API (PopupMenu showMenuAsync / setAction / addItem on a tracked
# PopupMenu, ModalCallbackFunction::create, chooser launchAsync, static
# dialog launch helpers) that captures `this` must be SafePointer/
# WeakReference-guarded (or allowlisted with a hand-verified justification —
# e.g. member-owned FileChooser completions, safe by the JUCE destructor
# contract). The crash-window class: popup/dialog completions firing after
# the host tore the editor down. Canary-gated like tool 5.
add_test(NAME parvati_check_async_this
         COMMAND python3 tools/check_async_this.py --self-test-and-scan
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# (tool 10) TRANS-key completeness: every TRANS() string literal in Source/
# must be a key in BOTH the FR and the DE table of
# Source/ui/Translations.cpp (parsed exactly as juce::LocalisedStrings
# parses them at runtime — escaped quotes, \xNN hex byte escapes, adjacent-
# literal concatenation), unless listed in tools/trans_allowlist.txt
# (## intentional proper nouns / ## known-missing tracked gaps). Also fails
# on FR/DE asymmetry and duplicate keys within a table block. Canary-gated
# like tools 5/6: an in-memory table with one key stripped must be reported
# before the real scan runs.
add_test(NAME parvati_check_translations
         COMMAND python3 tools/check_translations.py --self-test-and-scan
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# One build target that runs every static scanner in sequence (tools 5/6/10)
# so a single `cmake --build --target parvati_static_checks` gates them in
# CI without needing ctest. Fails (non-zero) if any scanner fails.
add_custom_target(parvati_static_checks
    COMMAND python3 tools/check_combo_clear.py --self-test-and-scan
    COMMAND python3 tools/check_async_this.py --self-test-and-scan
    COMMAND python3 tools/check_translations.py --self-test-and-scan
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running static scanners (combo-clear / async-this / translations)"
    VERBATIM)
