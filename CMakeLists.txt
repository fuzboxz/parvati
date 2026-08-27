cmake_minimum_required(VERSION 3.22)

project(Hellcat
    VERSION 0.1.0
    DESCRIPTION "Hellcat — a JUCE port of the Mutable Instruments Ambika synthesizer"
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
    set(HELLCAT_IOS ON)
else()
    set(HELLCAT_IOS OFF)
endif()
# Plugin formats per platform. iOS hosts (GarageBand/AUM/BeatMaker) load an AUv3
# app-extension; VST3 and the classic AU are desktop formats JUCE silently drops
# on iOS, so the iOS target list is AUv3 + Standalone only. The classic AU is
# additionally macOS-ONLY (a non-Apple configure with AU in FORMATS is a
# juce_add_plugin FATAL_ERROR), so Linux/Windows build VST3 + Standalone. The
# CLAP format is NOT listed here: JUCE 9.0.0 has no native CLAP FORMATS value,
# so it is added separately via clap-juce-extensions (HELLCAT_BUILD_CLAP).
#
# HELLCAT_FORMATS (cache) overrides the platform set. EMPTY = TESTS-ONLY
# configure: NO plugin format targets at all (only Hellcat_SharedCode + the
# tools/tests) — for CI and sanitizer dirs, where nothing but
# hellcat_unified_tests ever runs; pair with -DPARVATI_BUILD_CLAP=OFF to also
# drop the CLAP wrapper. Default = the platform set below, so a plain
# `cmake -B build` is unchanged. Format-target references later in this file
# (codesign, deploy, iOS plist patching) are all if(TARGET ...)-guarded so a
# reduced format set (or none) configures cleanly.
if(HELLCAT_IOS)
    set(_parvati_formats_default Standalone AUv3)
elseif(APPLE)
    set(_parvati_formats_default Standalone VST3 AU)
else()
    set(_parvati_formats_default Standalone VST3)
endif()
string(JOIN ";" _parvati_formats_default_joined ${_parvati_formats_default})
set(HELLCAT_FORMATS "${_parvati_formats_default_joined}" CACHE STRING
    "Plugin formats to build, ;-separated (e.g. 'Standalone;VST3;AU'). EMPTY = tests-only: no plugin format targets (pair with -DPARVATI_BUILD_CLAP=OFF). Default: platform set (${_parvati_formats_default_joined}).")
if(HELLCAT_FORMATS STREQUAL "")
    # Tests-only configure: JUCE handles an empty FORMATS list by creating no
    # wrapper targets (active_formats stays empty in _juce_configure_plugin_targets).
    set(HELLCAT_PLUGIN_FORMATS "")
else()
    set(HELLCAT_PLUGIN_FORMATS ${HELLCAT_FORMATS})
endif()

# CLAP plugin format via clap-juce-extensions (see the fetch block below).
# ON by default on every desktop platform (macOS/Linux/Windows); forced OFF on
# iOS, where the AUv3 app-extension is the only host-loadable format and a CLAP
# bundle inside the .ipa would be dead weight.
option(HELLCAT_BUILD_CLAP "Build the CLAP plugin format (desktop only)" ON)
if(HELLCAT_IOS)
    set(HELLCAT_BUILD_CLAP OFF CACHE BOOL
        "Build the CLAP plugin format (desktop only)" FORCE)
endif()

# ------------------------------------------------------------------------------
# Vendored firmware reference (firmware_parity_test's oracle). ambika_reference/
# is the original Mutable Instruments Ambika AVR firmware/controller tree
# (GPL3, deliberately UNTRACKED in this repo — see NOTICES.md for licensing and
# provenance). hellcat_unified_tests compiles nine of its sources (controller
# + voicecard audio oracle) directly, so a fresh clone without the tree cannot
# even CONFIGURE. Fail fast
# with instructions instead of a cryptic "Cannot find source file" deep in the
# test section. Obtain the tree from the Ambika firmware release (see
# NOTICES.md) and place it at the repo root as ambika_reference/.
set(HELLCAT_AMBIKA_FIRMWARE_SOURCES
    ambika_reference/controller/multi.cc
    ambika_reference/controller/part.cc
    ambika_reference/controller/resources.cc
    ambika_reference/controller/voice_allocator.cc
    ambika_reference/avrlib/random.cc
    # (The voicecard audio-oracle closure is NOT listed here: the controller
    # and voicecard resources.cc share table symbol names with DIFFERENT
    # contents, so the voicecard tree is compiled wrapped in namespace fwvc
    # by tests/firmware_shim/voicecard_oracle_closure.cc instead.)
)
foreach(_ambika_src IN LISTS HELLCAT_AMBIKA_FIRMWARE_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_ambika_src}")
        message(FATAL_ERROR
            "Missing vendored firmware reference: ${_ambika_src}\n"
            "ambika_reference/ is the original Mutable Instruments Ambika AVR "
            "firmware/controller tree (GPL3, untracked by design — see "
            "NOTICES.md). The unified test target compiles nine of its "
            "sources for firmware_parity_test. Clone/obtain the Ambika "
            "firmware tree and place it at the repo root as "
            "ambika_reference/ so the listed files exist.")
    endif()
endforeach()

# iOS-only Info.plist keys passed NATIVELY through juce_add_plugin (see the
# call below). juceaide generates the plist with these keys at CONFIGURE time
# inside juce_add_plugin itself, so they flow into every downstream copy
# (the CMake generate-time CMakeFiles/<target>.dir/Info.plist, the built app,
# and xcodebuild archives) with no custom patch step and no configure/build
# ordering hazard. Desktop configure gets an EMPTY list so the desktop
# plists/builds stay byte-identical. Keys with no juce_add_plugin seam
# (LSSupportsOpeningDocumentsInPlace, the AUv3 CFBundleIdentifier) are still
# patched below — see the PlistBuddy block in the HELLCAT_IOS section.
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
if(HELLCAT_IOS)
    set(HELLCAT_IOS_PLIST_ARGS
        TARGETED_DEVICE_FAMILY 2
        IPHONE_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        IPAD_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        REQUIRES_FULL_SCREEN TRUE
        BACKGROUND_AUDIO_ENABLED TRUE
        FILE_SHARING_ENABLED TRUE)
else()
    set(HELLCAT_IOS_PLIST_ARGS "")
endif()

# ccache is DISABLED: it served stale compiled objects on source changes
# (screen renders / installed plugins fell behind the code). The unset pair
# below clears any cached launcher a previous configure may have set.
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
if(APPLE AND NOT HELLCAT_IOS)
    # FORCE: CMake pre-seeds an EMPTY cache entry for this variable, which makes
    # a plain set(... CACHE ...) a silent no-op in already-configured build dirs.
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target" FORCE)
    endif()
endif()

# Default to a Debug build (the config this project is developed/tested with);
# artefacts then land in build/Hellcat_artefacts/Debug/. Override with
# -DCMAKE_BUILD_TYPE=Release for an optimized build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Emit compile_commands.json (consumed by clang-tidy / IDE tooling). No-op when
# the generator cannot write it (e.g. Xcode).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------------------------------
# Developer tooling options. Sanitizers stay OFF by default (opt-in):
#   -DPARVATI_ENABLE_ASAN=ON   -DPARVATI_ENABLE_UBSAN=ON
#   -DPARVATI_ENABLE_TSAN=ON
# Sanitizers apply to the whole build (JUCE + Hellcat + tests) so they catch
# bugs across the full call graph; reconfigure from a clean build dir when
# toggling them. (AddressSanitizer and ThreadSanitizer are mutually exclusive.)
# ------------------------------------------------------------------------------
option(HELLCAT_ENABLE_ASAN            "Enable AddressSanitizer"            OFF)
option(HELLCAT_ENABLE_UBSAN           "Enable UndefinedBehaviorSanitizer"  OFF)
option(HELLCAT_ENABLE_TSAN            "Enable ThreadSanitizer"             OFF)
option(HELLCAT_WARNINGS_AS_ERRORS     "Treat compiler warnings as errors"  ON)
option(HELLCAT_TEST_EXAMPLES          "Register the 8 example_* demo tests in hellcat_unified_tests (example_failing_test deliberately fails; default run stays green without them)" OFF)
option(HELLCAT_INSTALL_ON_BUILD       "macOS release convenience: install every built plug-in bundle + the Standalone app as a POST-BUILD step of its own format target (same destinations as `deploy`, minus the screen renders). Opt in per build dir — build_release uses it; build_unified / sanitizer trees stay plain." OFF)

# AddressSanitizer and ThreadSanitizer cannot be combined (both hijack the
# memory allocator). Fail fast at configure time rather than at link/run.
if(HELLCAT_ENABLE_ASAN AND HELLCAT_ENABLE_TSAN)
    message(FATAL_ERROR "HELLCAT_ENABLE_ASAN and HELLCAT_ENABLE_TSAN are mutually exclusive (ASan+TSan cannot be combined).")
endif()

set(_parvati_sanitizers "")
if(HELLCAT_ENABLE_ASAN)
    list(APPEND _parvati_sanitizers address)
endif()
if(HELLCAT_ENABLE_UBSAN)
    list(APPEND _parvati_sanitizers undefined)
endif()
if(HELLCAT_ENABLE_TSAN)
    list(APPEND _parvati_sanitizers thread)
endif()
if(_parvati_sanitizers)
    list(JOIN _parvati_sanitizers "," _parvati_sanitizer_flags)
    message(STATUS "Hellcat: sanitizers enabled (${_parvati_sanitizer_flags})")
    # -fsanitize is GCC/Clang-only. MSVC AddressSanitizer (/fsanitize=address)
    # is a different mechanism and is intentionally not wired here.
    if(NOT MSVC)
        add_compile_options(-fsanitize=${_parvati_sanitizer_flags} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${_parvati_sanitizer_flags})
    else()
        message(WARNING "Hellcat: -fsanitize is GCC/Clang-only; not applied under MSVC.")
    endif()
endif()
if(HELLCAT_WARNINGS_AS_ERRORS)
    # GCC/Clang use -Werror; MSVC uses /WX.
    if(NOT MSVC)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror> $<$<COMPILE_LANGUAGE:C>:-Werror>)
    else()
        add_compile_options(/WX)
    endif()
endif()

# ------------------------------------------------------------------------------
# libc++ hardened mode (memory-safety wave 2026-08-22): makes libc++'s own
# bounds/validity checks (std::array/std::vector operator[], iterator
# derefs, etc.) ACTIVE IN OPTIMIZED BUILDS. Debug builds already get full
# _LIBCPP_DEBUG-style checks via JUCE's assert paths; Release shipped the
# checks OFF — the sized-array migration leans on exactly these checks, so
# they should not vanish in the artifacts users run. FAST mode: checks that
# are O(1) and branch-predictable only (near-zero overhead; no iterator
# unwrapping). AppleClang/Clang + libc++ 16+ (Xcode 15+); the define is an
# inert unused macro on older toolchains or libstdc++, so no version gate is
# needed. NOT applied under sanitizers (redundant double instrumentation) or
# in Debug (already checked).
if(NOT MSVC AND NOT _parvati_sanitizers
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
   AND CMAKE_BUILD_TYPE MATCHES "Rel")
    add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
    message(STATUS "Hellcat: libc++ hardened mode FAST (${CMAKE_BUILD_TYPE})")
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
if(HELLCAT_BUILD_CLAP)
    if(HELLCAT_CLAP_EXTENSIONS_PATH)
        add_subdirectory("${HELLCAT_CLAP_EXTENSIONS_PATH}"
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

juce_add_plugin(Hellcat
    COMPANY_NAME            "805LABS"
    BUNDLE_ID               "com.805labs.hellcat"
    PLUGIN_MANUFACTURER_CODE "805L"       # >=1 uppercase letter (AU requirement)
    PLUGIN_CODE             "Hlct"        # first letter upper, rest lower (GarageBand safe)
    # iOS targets AUv3 (app-extension) + Standalone only; VST3/AU are desktop
    # formats JUCE drops on iOS. See HELLCAT_PLUGIN_FORMATS (gated on platform).
    FORMATS                 ${HELLCAT_PLUGIN_FORMATS}
    PRODUCT_NAME            "Hellcat"
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    # Native iOS plist keys (empty on desktop) — see the HELLCAT_IOS_PLIST_ARGS
    # block above for the rationale of each key.
    ${HELLCAT_IOS_PLIST_ARGS}
)

# NOTE: Source/ is globbed (CONFIGURE_DEPENDS) so new DSP files added by
# subagents are picked up automatically. The read-only ambika_reference/ tree
# lives OUTSIDE Source/ and is therefore never pulled into the build.
file(GLOB_RECURSE HELLCAT_SOURCES CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.cc"
    "Source/*.h")
target_sources(Hellcat PRIVATE ${HELLCAT_SOURCES})
target_include_directories(Hellcat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
# Vendored Mutable Instruments clouds/stmlib DSP (the Clouds FX modules). Marked
# SYSTEM so the upstream headers' C-style conversions do not trip Hellcat's
# strict warning set (these are third-party sources kept verbatim for upstream
# sync; the one required portability patch is in stmlib/dsp/dsp.h).
target_include_directories(Hellcat SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds)

# The vendored Clouds/stmlib sources also include compiled .cc files (units.cc,
# mu_law.cc, and later correlator/pvoc/etc.) kept VERBATIM from upstream. They
# predate Hellcat's strict -Werror hygiene (upstream uses double literals in
# float LUTs, sign/int conversions, etc.); exempt them from warnings rather than
# patching the upstream data. (Headers are already suppressed via the SYSTEM
# include root above; this covers the compiled .cc sources.)
file(GLOB_RECURSE _hellcat_clouds_cc
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp/clouds/*.cc")
if(_hellcat_clouds_cc)
    if(NOT MSVC)
        set_source_files_properties(${_hellcat_clouds_cc} PROPERTIES COMPILE_OPTIONS "-w")
    else()
        set_source_files_properties(${_hellcat_clouds_cc} PROPERTIES COMPILE_OPTIONS "/w")
    endif()
endif()

# ------------------------------------------------------------------------------
# CLAP registration (see the fetch block above for the JUCE 9 rationale).
# clap_juce_extensions_plugin derives a Hellcat_CLAP wrapper from the SAME
# shared-code target, so VST3/AU/Standalone are untouched; the module must be
# linked into Hellcat itself so the shared objects can see its headers
# (HAS_CLAP_JUCE_EXTENSIONS). (Plain target name: the pinned revision defines
# no clap_juce_extensions:: alias.) CLAP_ID doubles as the macOS CLAP bundle's
# GUI identifier — it must stay stable (hosts identify the plugin by it) and is
# therefore kept aligned with BUNDLE_ID above.
if(HELLCAT_BUILD_CLAP)
    target_link_libraries(Hellcat PRIVATE clap_juce_extensions)
    clap_juce_extensions_plugin(TARGET Hellcat
        CLAP_ID "com.805labs.hellcat"
        CLAP_FEATURES instrument synthesizer stereo)
endif()

# ------------------------------------------------------------------------------
# iOS toolchain + signing (M1). Sets the iOS deployment target, distinct bundle
# identifiers for the Standalone app and the AUv3 app-extension, App-Group
# entitlements (so both share one presets/state container — see
# Source/ui/SharedContainer.cpp), and an Automatic signing style. (The device
# family and app-level plist keys are set natively via HELLCAT_IOS_PLIST_ARGS in
# the juce_add_plugin call; this block only patches the two keys with no native
# seam.)
#
# The iOS Simulator SDK (iphonesimulator) does NOT require a signing identity or
# a provisioning profile, so it builds with HELLCAT_IOS_DEVELOPMENT_TEAM empty.
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
#   cmake --build build_ios --config Release --target Hellcat_Standalone Hellcat_AUv3 -- -sdk iphonesimulator
# The tempting `... -- -sdk iphonesimulator CONFIGURATION=Release` form passes
# CONFIGURATION= to xcodebuild as a build-SETTING override, silently builds the
# Debug artefacts, and leaves 8-byte `APPL????` stubs under Release/ that break
# the NEXT correct build with "unable to create directory" until deleted.
if(HELLCAT_IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
    # PLACEHOLDER team id — empty by default (simulator-only). Set on the cmake
    # command line for device/.ipa builds (see comment above).
    set(HELLCAT_IOS_DEVELOPMENT_TEAM "" CACHE STRING
        "Apple Developer team id for iOS device/.ipa builds (empty = simulator-only)")

    # Standalone app bundle: com.805labs.hellcat
    # (NOTE: ENABLE_BITCODE was removed — bitcode is dead since Xcode 14 and
    # Xcode 26 ignores the flag entirely; iOS hunt 2026-08-19 F-ios-build-5.)
    # TARGET guards: a HELLCAT_FORMATS override (e.g. the tests-only empty
    # set) must never reference format targets that do not exist.
    if(TARGET Hellcat_Standalone)
    set_target_properties(Hellcat_Standalone PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.hellcat"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/hellcat.entitlements")
    endif()
    # AUv3 app-extension bundle: com.805labs.hellcat.AUv3 (child of the app id,
    # as iOS App-Store validation requires).
    if(TARGET Hellcat_AUv3)
    set_target_properties(Hellcat_AUv3 PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.805labs.hellcat.AUv3"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/hellcat-auv3.entitlements")
    endif()

    if(HELLCAT_IOS_DEVELOPMENT_TEAM)
        set(_parvati_ios_sign_targets "")
        foreach(_t Hellcat_Standalone Hellcat_AUv3)
            if(TARGET ${_t})
                list(APPEND _parvati_ios_sign_targets ${_t})
            endif()
        endforeach()
        if(_parvati_ios_sign_targets)
            set_target_properties(${_parvati_ios_sign_targets} PROPERTIES
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${HELLCAT_IOS_DEVELOPMENT_TEAM}")
        endif()
    endif()

    # ----------------------------------------------------------------------------
    # Info.plist patching — ONLY the keys with no juce_add_plugin seam.
    # Everything else (device family, orientations, UIRequiresFullScreen,
    # background audio, file sharing) is generated NATIVELY by juceaide from the
    # HELLCAT_IOS_PLIST_ARGS keyword arguments (see the top of this file), so it
    # cannot be dropped by a configure/build ordering mishap. Two keys remain:
    #
    #   1. LSSupportsOpeningDocumentsInPlace (Standalone) — pairs with
    #      UIFileSharingEnabled: lets the Files app open saved patches in place
    #      rather than copying them out. No juce_add_plugin argument exists.
    #
    #   2. AUv3 CFBundleIdentifier — juceaide hard-codes the extension id as
    #      "<app-id>.<last-component>AUv3" (com.805labs.hellcat.HellcatAUv3),
    #      which DISAGREES with this project's PRODUCT_BUNDLE_IDENTIFIER and
    #      entitlements application-identifier (both …hellcat.AUv3). The plist
    #      literal wins in the built .appex, so automatic signing / App-Store
    #      validation would fail on a device/.ipa build with an
    #      application-identifier mismatch. Rewrite it to the intended id.
    #
    # Both plists are patched at their SOURCE — the juceaide-generated
    # Hellcat_artefacts/JuceLibraryCode/<target>/Info.plist — which juce_add_plugin
    # has already produced during THIS configure (it runs juceaide via
    # execute_process while evaluating the call), so the file provably exists
    # here. CMake then copies it to CMakeFiles/<target>.dir/Info.plist (the real
    # INFOPLIST_FILE) at generate time, AFTER all configure code has run, so the
    # patched content reaches the built bundle. juceaide regenerates the plist
    # fresh on every configure, so Delete/Add is idempotent across re-runs; a
    # final Print + hard FATAL_ERROR replaces the old silent ERROR_QUIET skip —
    # a missing key must FAIL the configure, not silently ship without it.
    # Per-format guards: HELLCAT_FORMATS may legitimately omit either format
    # (juceaide then produces no plist for it); every patch below is skipped
    # for absent formats instead of failing the configure.
    set(_parvati_standalone_plist
        "${CMAKE_BINARY_DIR}/Hellcat_artefacts/JuceLibraryCode/Hellcat_Standalone/Info.plist")
    set(_parvati_auv3_plist
        "${CMAKE_BINARY_DIR}/Hellcat_artefacts/JuceLibraryCode/Hellcat_AUv3/Info.plist")
    if(TARGET Hellcat_Standalone AND NOT EXISTS "${_parvati_standalone_plist}")
        message(FATAL_ERROR
            "juceaide did not generate '${_parvati_standalone_plist}' during this configure — "
            "the Hellcat plist patch step must move after juce_add_plugin.")
    endif()
    if(TARGET Hellcat_AUv3 AND NOT EXISTS "${_parvati_auv3_plist}")
        message(FATAL_ERROR
            "juceaide did not generate '${_parvati_auv3_plist}' during this configure — "
            "the Hellcat plist patch step must move after juce_add_plugin.")
    endif()

    # Standalone: LSSupportsOpeningDocumentsInPlace (see rationale above).
    if(TARGET Hellcat_Standalone)
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
    endif()  # TARGET Hellcat_Standalone (LSSupports patch)

    # AUv3: rewrite the juceaide-hard-coded CFBundleIdentifier to the intended
    # com.805labs.hellcat.AUv3 so it matches PRODUCT_BUNDLE_IDENTIFIER and the
    # entitlements application-identifier (see rationale above). Set (not Add)
    # overwrites the always-present juceaide value.
    if(TARGET Hellcat_AUv3)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Set :CFBundleIdentifier com.805labs.hellcat.AUv3"
            "${_parvati_auv3_plist}"
        RESULT_VARIABLE _parvati_auv3_id_rc ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "${_parvati_auv3_plist}"
        OUTPUT_VARIABLE _parvati_auv3_id_val OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _parvati_auv3_id_val STREQUAL "com.805labs.hellcat.AUv3")
        message(FATAL_ERROR
            "Failed to set the AUv3 CFBundleIdentifier to com.805labs.hellcat.AUv3 "
            "(PlistBuddy rc=${_parvati_auv3_id_rc}, value='${_parvati_auv3_id_val}').")
    endif()
    endif()  # TARGET Hellcat_AUv3 (bundle-id patch)

    # ----------------------------------------------------------------------------
    # Document types + exported UTIs (iOS hunt 2026-08-19, F-ios-build-1 /
    # F-ios-files-2). juce_add_plugin has NO seam for CFBundleDocumentTypes /
    # UTExportedTypeDeclarations, so they are grafted here with the same
    # PlistBuddy + FATAL_ERROR-verify pattern. Without these keys:
    #   * LSSupportsOpeningDocumentsInPlace is INERT (it only modifies
    #     behaviour of DECLARED document types), and
    #   * .yml/.PRO/.MUL received via Files/AirDrop/Mail offer
    #     no "Open in Hellcat" — the advertised file workflow was dead from
    #     outside the app.
    # The fragments are plain plists written at configure time; PlistBuddy
    # Import SETS the (previously absent) top-level keys wholesale, and the
    # Print verify makes a silent drop a configure FAILURE, not a shipping
    # gap. (Re-runs are idempotent: Delete before Import.)
    if(TARGET Hellcat_Standalone)
    set(_hellcat_filetypes_fragment "${CMAKE_CURRENT_SOURCE_DIR}/ios/hellcat_filetypes.plist")
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Delete :CFBundleDocumentTypes"
            -c "Delete :UTExportedTypeDeclarations"
            "${_parvati_standalone_plist}"
        ERROR_QUIET OUTPUT_QUIET)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
            -c "Merge ${_hellcat_filetypes_fragment}"
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
    if(NOT _parvati_doctypes_val STREQUAL "com.805labs.hellcat-patch"
       OR NOT _parvati_utis_val STREQUAL "com.805labs.hellcat-multi")
        message(FATAL_ERROR
            "Failed to graft CFBundleDocumentTypes/UTExportedTypeDeclarations into "
            "'${_parvati_standalone_plist}' (rc=${_parvati_doctypes_rc}, "
            "docType0='${_parvati_doctypes_val}', uti2='${_parvati_utis_val}').")
    endif()
    endif()  # TARGET Hellcat_Standalone (document types / UTIs)

    # SharedContainer.mm is the Obj-C++ resolver for the App-Group container
    # (NSFileManager containerURLForSecurityApplicationGroupIdentifier). It is
    # iOS-only; the desktop path lives in the globbed SharedContainer.cpp and is
    # byte-identical to the previous getSpecialLocation(userApplicationDataDirectory).
    target_sources(Hellcat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SharedContainer.mm")

    # IosOpenIn.mm — the Obj-C++ open-in shim (application:openURL:options:
    # via class_addMethod on JUCE's JuceAppStartupDelegate; JUCE 9 has none).
    # iOS-only like SharedContainer.mm (the Source/ glob covers .cpp/.cc/.h);
    # the pure routing core lives in the header-inline functions, so desktop
    # and the headless tests link without this file.
    target_sources(Hellcat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/IosOpenIn.mm")
endif()

# ------------------------------------------------------------------------------
# Factory presets, organized under presets/ as AFACTORY/<bank>/ (A/B/F/S) +
# AFACTORY_MULTI/ (the GPL-3.0 Ambika "goldencard" banks) + TEMPLATES/ (stock
# .yml multis) + HFACTORY/ (the 64 ORIGINAL Hellcat .yml patches — no Ambika
# data). EMBEDDED into the plugin binary via juce_add_binary_data, then
# extracted to the user app-data dirs on first run
# (hellcat::ensureFactoryPresetsInstalled, see Source/ui/FactoryPresetInstaller.h)
# so they show up in the Patch combo out of the box.
#
# The four program banks share filenames (each has 000.PRO, 001.PRO ...), so they
# cannot be embedded by their raw names. Stage them into a build dir with
# bank-unique names first: <BANK>__<file> (A__000.PRO) and MULTI__<file>. The
# installer parses the "__" delimiter to reconstruct AFACTORY/<bank>/<file>.
set(_parvati_preset_stage "${CMAKE_BINARY_DIR}/hellcat_preset_stage")
file(REMOVE_RECURSE "${_parvati_preset_stage}")
file(MAKE_DIRECTORY "${_parvati_preset_stage}")
# Re-stage when the source presets change (CONFIGURE_DEPENDS -> reconfigure).
file(GLOB _cfgdep_pro CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/AFACTORY/*/*.PRO")
file(GLOB _cfgdep_mul CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/AFACTORY_MULTI/*.MUL")
file(GLOB _cfgdep_tpl CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/TEMPLATES/*.yml")
file(GLOB _cfgdep_hct CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/HFACTORY/*.yml")
set(_staged_sources "")
foreach(_bank A B F S)
    file(GLOB _files "${CMAKE_CURRENT_SOURCE_DIR}/presets/AFACTORY/${_bank}/*.PRO")
    foreach(_f ${_files})
        get_filename_component(_n "${_f}" NAME)
        set(_out "${_parvati_preset_stage}/${_bank}__${_n}")
        configure_file("${_f}" "${_out}" COPYONLY)
        list(APPEND _staged_sources "${_out}")
    endforeach()
endforeach()
file(GLOB _mulfiles "${CMAKE_CURRENT_SOURCE_DIR}/presets/AFACTORY_MULTI/*.MUL")
foreach(_f ${_mulfiles})
    get_filename_component(_n "${_f}" NAME)
    set(_out "${_parvati_preset_stage}/MULTI__${_n}")
    configure_file("${_f}" "${_out}" COPYONLY)
    list(APPEND _staged_sources "${_out}")
endforeach()
# Stock init templates (full-fidelity .yml multis) — staged as TEMPLATE__<file>.
file(GLOB _tplfiles "${CMAKE_CURRENT_SOURCE_DIR}/presets/TEMPLATES/*.yml")
foreach(_f ${_tplfiles})
    get_filename_component(_n "${_f}" NAME)
    set(_out "${_parvati_preset_stage}/TEMPLATE__${_n}")
    configure_file("${_f}" "${_out}" COPYONLY)
    list(APPEND _staged_sources "${_out}")
endforeach()
# Hellcat ORIGINAL factory bank (64 single-part .yml patches) — staged as
# HFACTORY__<file>. The installer extracts these to their own app-data root
# (HFACTORY/), separate from the GPL Ambika AFACTORY/ banks.
file(GLOB _hctfiles "${CMAKE_CURRENT_SOURCE_DIR}/presets/HFACTORY/*.yml")
foreach(_f ${_hctfiles})
    get_filename_component(_n "${_f}" NAME)
    set(_out "${_parvati_preset_stage}/HFACTORY__${_n}")
    configure_file("${_f}" "${_out}" COPYONLY)
    list(APPEND _staged_sources "${_out}")
endforeach()
# Guard: juce_add_binary_data errors on an empty SOURCES list. The factory banks
# are always checked in, so this only matters for a hypothetical empty-presets
# checkout (configure stays valid; the installer simply has nothing to extract).
if(_staged_sources)
    juce_add_binary_data(hellcat_factory_presets
        NAMESPACE FactoryPresets
        SOURCES ${_staged_sources})
    target_link_libraries(Hellcat PRIVATE hellcat_factory_presets)
endif()

# ------------------------------------------------------------------------------
# Brand logo (hellcat_logo.svg at the repo root) embedded as its OWN
# binary-data target (namespace HellcatLogo). It is NOT folded into
# hellcat_factory_presets: that target's installer iterates every resource and
# would otherwise try to extract the SVG as a preset. hellcat_logo.svg is TRUE
# vector art (outlined <path>/<g>, no raster); the editor parses it once with
# JUCE's SVG renderer (Drawable::createFromSVGString) and paints a vector
# Drawable. Accessed via HellcatLogo::getNamedResource in PluginEditor.cpp
# (forward-declared there — see comment for why we do not #include the
# generated BinaryData.h).
juce_add_binary_data(hellcat_logo_assets
    NAMESPACE HellcatLogo
    SOURCES hellcat_logo.svg)
target_link_libraries(Hellcat PRIVATE hellcat_logo_assets)

# Y2K-theme typefaces (Michroma / PT Sans), all SIL Open Font License
# 1.1 — commercial use, embedding and redistribution are permitted (see
# assets/fonts/OFL.txt and NOTICES.md). Embedded as a dedicated binary-data
# target (NAMESPACE HellcatFonts) and resolved via getNamedResource() in
# HellcatLookAndFeel.cpp — the same forward-declaration pattern as
# hellcat_logo_assets above (two other binary-data targets also generate a
# BinaryData.h, so a direct include would be ambiguous). The former VT323
# "console" face was retired 2026-XX-XX (data readouts use the default font).
juce_add_binary_data(hellcat_font_assets
    NAMESPACE HellcatFonts
    SOURCES assets/fonts/Michroma-Regular.ttf
            assets/fonts/PT_Sans-Web-Regular.ttf
            assets/fonts/PT_Sans-Web-Bold.ttf)
target_link_libraries(Hellcat PRIVATE hellcat_font_assets)

# ------------------------------------------------------------------------------
# Compile the JUCE module stack EXACTLY ONCE and share it.
#
# JUCE attaches every module's sources as INTERFACE_SOURCES, so each target
# that links juce::* recompiles the whole stack (~260 MB of objects per target).
# With ~20 test executables that is ~4 GB of duplicated JUCE objects per build
# dir. `Hellcat` is a STATIC library that already compiles JUCE once (the 3
# plugin formats share it); we make it the SINGLE source of JUCE for everything:
#   * ensure Hellcat compiles the full module set the tests need, and
#   * re-export the modules' USAGE REQUIREMENTS (include dirs, compile defs,
#     compile features, and NON-module link deps such as system frameworks) on
#     Hellcat's INTERFACE — but deliberately NOT their INTERFACE_SOURCES, which
#     is what forces the recompilation. Test/tool targets then link ONLY Hellcat
#     (JUCE objects + headers + defines + frameworks, compiled once).
set(HELLCAT_JUCE_MODULES
    juce::juce_audio_basics    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_audio_formats   juce::juce_audio_devices
    juce::juce_core            juce::juce_data_structures  juce::juce_dsp
    juce::juce_events          juce::juce_graphics
    juce::juce_gui_basics      juce::juce_gui_extra)
# Hellcat already compiles these modules once via its own target_link_libraries
# block above; here we only RE-EXPORT their usage requirements (include dirs,
# compile defs, compile features, and NON-module link deps like system
# frameworks) on Hellcat's INTERFACE — deliberately NOT their INTERFACE_SOURCES,
# which is what forces every consumer to recompile JUCE. Test/tool targets then
# link ONLY Hellcat (JUCE objects + headers + defines + frameworks, once).
foreach(_m ${HELLCAT_JUCE_MODULES})
    get_target_property(_inc ${_m} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
        target_include_directories(Hellcat INTERFACE ${_inc})
    endif()
    get_target_property(_def ${_m} INTERFACE_COMPILE_DEFINITIONS)
    if(_def)
        target_compile_definitions(Hellcat INTERFACE ${_def})
    endif()
    get_target_property(_feat ${_m} INTERFACE_COMPILE_FEATURES)
    if(_feat)
        target_compile_features(Hellcat INTERFACE ${_feat})
    endif()
    # Propagate link deps (system frameworks etc.) but NOT other juce::* modules
    # — those would drag their INTERFACE_SOURCES back in and re-trigger the
    # per-target recompilation we are eliminating.
    get_target_property(_libs ${_m} INTERFACE_LINK_LIBRARIES)
    if(_libs)
        foreach(_l ${_libs})
            if(NOT (_l MATCHES "^juce::" OR _l MATCHES "^juce_"))
                target_link_libraries(Hellcat INTERFACE ${_l})
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
target_compile_definitions(Hellcat PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # NOTE: no JUCE_DISPLAY_SPLASH_SCREEN define here — JUCE 9 has no splash
    # screen at all; the JUCE 8-era flag is ignored (juce_gui_basics emits an
    # "ignored" warning for it), so setting it would be dead weight.
    HELLCAT_VERSION="${CMAKE_PROJECT_VERSION}"
)

target_link_libraries(Hellcat
    PRIVATE
        # The module set from HELLCAT_JUCE_MODULES (set above): one list drives
        # both this private link and the interface re-export loop.
        ${HELLCAT_JUCE_MODULES}
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
# VST3 target; TARGET guard so a HELLCAT_FORMATS set without VST3 stays clean).
if(APPLE AND NOT HELLCAT_IOS AND TARGET Hellcat_VST3)
    add_custom_command(TARGET Hellcat_VST3 POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:Hellcat_VST3>"
        COMMENT "Re-sign Hellcat.vst3 (seal moduleinfo.json)")
endif()

# ------------------------------------------------------------------------------
# Unified Test Runner - Single binary containing all migrated tests
# This consolidates ~100+ separate test executables (~3.5GB) into a single
# ~31MB binary, with a configure-time orphan guard (see below) that makes
# the one-binary policy self-enforcing.
# Run with: ./build/hellcat_unified_tests
# Options:
#   ./build/hellcat_unified_tests list       - List all available tests
#   ./build/hellcat_unified_tests <test>     - Run specific test
#   ./build/hellcat_unified_tests            - Run all tests
# The 8 example_* harness demos are compiled in only with
# -DPARVATI_TEST_EXAMPLES=ON (default OFF; example_failing_test deliberately
# fails, so the option must stay off for a meaningful green run).
add_executable(hellcat_unified_tests
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
    # FX Tests - Hellcat (9)
    tests/hellcat_fx_bridge_pop_test.cpp
    tests/hellcat_fx_bridge_tinychunk.cpp
    tests/hellcat_fx_engine_continuity.cpp
    tests/hellcat_fx_drag_test.cpp
    tests/hellcat_fx_invariants_test.cpp
    tests/hellcat_fx_lut_dropout_test.cpp
    tests/hellcat_fx_modrate_test.cpp
    tests/hellcat_fx_onset_regression.cpp
    tests/hellcat_fx_concurrency_probe.cpp
    # Integration Tests (12)
    tests/analog_filter_batch_test.cpp
    tests/filter_topology_test.cpp
    tests/lfo_sync_test.cpp
    tests/lfo_retrigger_test.cpp
    tests/patch_arrangement_test.cpp
    tests/multi_load_test.cpp
    tests/roundtrip_test.cpp
    tests/hellcat_preset_test.cpp
    tests/fx_preset_test.cpp
    tests/fx_routing_test.cpp
    tests/render_quality_test.cpp
    tests/hellcat_clouds_fx_test.cpp
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
    # I/O & File Tests (10)
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
    tests/hellcat_fx_diffuser_diag.cpp
    tests/hellcat_fx_foldback_probe.cpp
    tests/hellcat_fx_popdiag_test.cpp
    tests/hellcat_fx_stereo_balance_test.cpp
    tests/hellcat_fx_voice_mod_test.cpp
    tests/hellcat_native_dist_probe.cpp
    tests/hellcat_phaser_drag_repro.cpp
    tests/patch_test.cpp
    tests/perf_smoke_test.cpp
    tests/seq_stepper_test.cpp
    # Hellcat-Specific Tests (6)
    tests/hellcat_synth_paramtext_test.cpp
    tests/hellcat_synth_drag_probe.cpp
    tests/hellcat_settings_probe.cpp
    tests/hellcat_kbd_drag_repro.cpp
    tests/hellcat_fx_glitch_dump.cpp
    tests/hellcat_fx_live_repro.cpp
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
    tests/hellcat_fv1_alias_probe.cpp
    tests/hellcat_fx_cpu_probe.cpp
    tests/hellcat_fx_dropout_probe.cpp
    tests/hellcat_fx_matrix_probe.cpp
    tests/hellcat_tests.cpp
    tests/hellcat_arp_seq_timing_test.cpp
    # Diagnostic probe — note-retrigger crackle scenarios (audit P1-1: was
    # tracked but never compiled/registered; now part of the unified target)
    tests/hellcat_retrig_probe.cpp
# Unified test sources (keep alphabetical-ish; new_test.sh appends here)
    tests/modbar_strip_theme_test.cpp
    tests/preset_name_restore_test.cpp
    tests/factory_bank_test.cpp
    tests/fx_slot_tooltip_test.cpp
    tests/a11y_smoke_test.cpp
    tests/filter_loudness_test.cpp
    tests/live_part_params_test.cpp
    tests/perf_baseline_test.cpp
    tests/fx_render_golden_test.cpp
    tests/modbar_pill_paint_split_test.cpp
    tests/osc_preview_live_test.cpp
    tests/theme_switch_preview_test.cpp
    tests/fx_silence_gate_test.cpp
    tests/lfo_shape_preview_test.cpp
    tests/release_vca_glide_test.cpp
    tests/mono_retrigger_continuity_test.cpp
    tests/patch_sanitizer_test.cpp
    tests/dsp_memory_safety_test.cpp
    tests/build_policy_test.cpp
    # Vendored firmware sources for firmware_parity_test (the parity oracle:
    # tests/firmware_shim headers shadow the AVR firmware headers; see the
    # include dirs / flags added below the target).
    tests/firmware_shim/shims.cc
    tests/firmware_shim/voicecard_oracle.cc
    tests/firmware_shim/voicecard_oracle_closure.cc
    ${HELLCAT_AMBIKA_FIRMWARE_SOURCES}
)
if(NOT MSVC AND HELLCAT_ENABLE_UBSAN)
    # UBSan return-checker vs the vendored firmware oracle: the AVR firmware
    # declares `uint8_t TransientGenerator::Trigger()` but returns nothing
    # (ambika_reference/voicecard/transient_generator.h:53). No caller reads
    # the value. The checker aborts the oracle on first trigger, so
    # firmware_parity_test cannot finish section 10. Scope the checker away
    # from the three oracle TUs only. Hellcat's port declares `void Trigger()`
    # (Source/dsp/transient_generator.h) and stays checked.
    set_property(SOURCE
        tests/firmware_shim/shims.cc
        tests/firmware_shim/voicecard_oracle.cc
        tests/firmware_shim/voicecard_oracle_closure.cc
        APPEND PROPERTY COMPILE_OPTIONS "-fno-sanitize=return")
endif()
target_link_libraries(hellcat_unified_tests PRIVATE Hellcat hellcat_factory_presets)
target_compile_features(hellcat_unified_tests PRIVATE cxx_std_17)
target_compile_definitions(hellcat_unified_tests PRIVATE HELLCAT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(hellcat_unified_tests SYSTEM PRIVATE Source/dsp/clouds)

# firmware_parity_test support: within this one call, tests/firmware_shim
# precedes ambika_reference so its headers shadow the AVR-dependent firmware
# headers (the contract the pre-unification standalone parity harness had);
# Source/tests stay AHEAD of the shim so the rest of the suite keeps resolving
# same-named headers (parameter.h, op.h, midi_dispatcher.h, ...) from Source.
# ambika_reference is listed SYSTEM below (NOT here): CMake appends SYSTEM
# include dirs after these, so the shadow order is unchanged while the
# vendored headers get standard system-header warning suppression.
target_include_directories(hellcat_unified_tests PRIVATE
    Source
    tests
    tests/firmware_shim
)
# Narrowing suppression: Clang and GCC spell the flag differently. GCC drops
# unknown -Wno-* options silently. The Clang spelling then turns the
# suppressed narrowing into a hard error under GCC. MSVC needs no flag.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(hellcat_unified_tests PRIVATE -Wno-narrowing)
elseif(NOT MSVC)
    target_compile_options(hellcat_unified_tests PRIVATE -Wno-c++11-narrowing)
endif()
# Vendored-tree warnings under HELLCAT_WARNINGS_AS_ERRORS=ON (the default):
# the firmware oracle is READ-ONLY (see the header comment) and its intentional
# int<->uint8 arithmetic trips -Wimplicit-int-conversion/-Wsign-conversion & co.
# Two scoped escapes keep -Werror live for ALL editable code:
#   1) the ambika_reference include dir is SYSTEM below, so warnings from its
#      HEADERS are suppressed in the ~15 test TUs that include them;
#   2) the nine vendored .cc files compiled as MAIN files keep their warnings
#      VISIBLE but non-fatal (-Wno-error per source).
target_include_directories(hellcat_unified_tests SYSTEM PRIVATE ambika_reference)
set_source_files_properties(${HELLCAT_AMBIKA_FIRMWARE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-error")

# Dead-code strip. It drops the firmware's never-called MonoModeOn and
# PolyModeOn; they reference TouchVoiceAllocation, which the firmware tree
# never defines. -Wl,-dead_strip works on Apple ld64 only. GNU ld and lld
# reject it. ELF builds strip per section instead. MSVC drops unreferenced
# COMDAT functions without a flag.
if(APPLE)
    target_link_options(hellcat_unified_tests PRIVATE -Wl,-dead_strip)
elseif(NOT MSVC)
    target_compile_options(hellcat_unified_tests PRIVATE
        -ffunction-sections -fdata-sections)
    target_link_options(hellcat_unified_tests PRIVATE -Wl,--gc-sections)
endif()
# --- Single-binary test policy: configure-time orphan guard -----------------
# Every tests/*.cpp must be part of hellcat_unified_tests — there are no
# per-test targets. This makes "add a test file but forget the source list"
# (the hellcat_retrig_probe orphan class of bug, audit P1-1) a CONFIGURE
# FAILURE instead of a test that silently never compiles or runs. Headers and
# the shim dir are not globbed; the runner main and the gated demos are
# whitelisted because they are support files, not test harnesses.
set(HELLCAT_UNIFIED_TEST_WHITELIST
    unified_test_runner_main.cpp   # runner main() (the only one in the binary)
    unified_test_examples.cpp      # gated demos (HELLCAT_TEST_EXAMPLES, default OFF)
)
file(GLOB HELLCAT_TESTS_GLOB CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
get_target_property(HELLCAT_UNIFIED_TEST_SOURCES hellcat_unified_tests SOURCES)
set(HELLCAT_UNIFIED_TEST_SOURCES_ABS "")
foreach(_src IN LISTS HELLCAT_UNIFIED_TEST_SOURCES)
    cmake_path(ABSOLUTE_PATH _src BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND HELLCAT_UNIFIED_TEST_SOURCES_ABS "${_src}")
endforeach()
set(HELLCAT_UNIFIED_ORPHANS "")
foreach(_f IN LISTS HELLCAT_TESTS_GLOB)
    get_filename_component(_fname "${_f}" NAME)
    if("${_fname}" IN_LIST HELLCAT_UNIFIED_TEST_WHITELIST)
        continue()
    endif()
    if(NOT "${_f}" IN_LIST HELLCAT_UNIFIED_TEST_SOURCES_ABS)
        list(APPEND HELLCAT_UNIFIED_ORPHANS "${_fname}")
    endif()
endforeach()
if(HELLCAT_UNIFIED_ORPHANS)
    list(SORT HELLCAT_UNIFIED_ORPHANS)
    string(REPLACE ";" ", " _parvati_orphan_names "${HELLCAT_UNIFIED_ORPHANS}")
    message(FATAL_ERROR
        "Single-binary test policy violated: ${_parvati_orphan_names} are on "
        "disk in tests/ but NOT in the hellcat_unified_tests source list, so "
        "they would never be compiled or run. Add them to the "
        "add_executable(hellcat_unified_tests ...) source list — per-test "
        "add_executable() targets are forbidden (the suite is ONE binary).")
endif()
# Dynamic counts for the status message below (no more hand-maintained
# "N tests" strings drifting stale).
list(LENGTH HELLCAT_TESTS_GLOB _parvati_glob_count)
list(LENGTH HELLCAT_UNIFIED_TEST_WHITELIST _parvati_whitelist_count)
math(EXPR HELLCAT_UNIFIED_HARNESS_COUNT "${_parvati_glob_count} - ${_parvati_whitelist_count}")
list(LENGTH HELLCAT_UNIFIED_TEST_SOURCES HELLCAT_UNIFIED_SOURCE_COUNT)

if(HELLCAT_TEST_EXAMPLES)
    target_compile_definitions(hellcat_unified_tests PRIVATE HELLCAT_TEST_EXAMPLES=1)
    message(STATUS "Unified test runner enabled: hellcat_unified_tests (${HELLCAT_UNIFIED_HARNESS_COUNT} test harnesses, ${HELLCAT_UNIFIED_SOURCE_COUNT} sources; example_* demos ON: HELLCAT_TEST_EXAMPLES, example_failing_test is expected to fail)")
else()
    message(STATUS "Unified test runner enabled: hellcat_unified_tests (${HELLCAT_UNIFIED_HARNESS_COUNT} test harnesses, ${HELLCAT_UNIFIED_SOURCE_COUNT} sources; example_* demos off, enable with HELLCAT_TEST_EXAMPLES=ON)")
endif()

# ------------------------------------------------------------------------------
# hellcat_add_tool(<name> <source>): one-line registration for the dev/verify
# tools below — EXCLUDE_FROM_ALL (never in a default build), Source include
# dir, uniform C++17, linked against the Hellcat static lib. Collapses what
# used to be 7 hand-rolled 6-line blocks that had drifted apart
# (hellcat_headless_test was missing its cxx_std_17).
function(hellcat_add_tool name source)
    add_executable(${name} EXCLUDE_FROM_ALL ${source})
    target_include_directories(${name} PRIVATE Source)
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_link_libraries(${name} PRIVATE Hellcat)
endfunction()

# ------------------------------------------------------------------------------
# Headless Layer-3a verification (not shipped): instantiate the processor,
# push a NoteOn, assert non-silent audio. Build with:
#   cmake --build build_unified --target hellcat_headless_test && ./build_unified/hellcat_headless_test
hellcat_add_tool(hellcat_headless_test tools/headless_test.cpp)

# ------------------------------------------------------------------------------
# Settings round-trip check: loads a crafted Hellcat.settings exactly like the
# standalone does and verifies the filterState restore. EXCLUDE_FROM_ALL.
hellcat_add_tool(hellcat_settings_roundtrip tools/settings_roundtrip.cpp)

# ------------------------------------------------------------------------------
# State donor: writes a Hellcat processor state blob with FX1 = <type> enabled
# (for feeding the real Standalone's settings filterState). EXCLUDE_FROM_ALL.
hellcat_add_tool(hellcat_state_donor tools/state_donor.cpp)

# ------------------------------------------------------------------------------
# Dev tool: render the full Hellcat editor screen for each page to a PNG
# (offscreen, via paintEntireComponent — no screen-recording permission needed)
# so they can be fed to a UI-redesign agent. Same link set as the other tools (Hellcat static lib).
# Build: cmake --build build_release --target hellcat_screen_shots
# Run:   ./build_release/hellcat_screen_shots [outputDir] [scale]   (defaults: ./screens, 2)
# build_release is a release-workflow dir, not part of a fresh clone.
# Configure it first: cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
#
# CANONICAL regeneration (builds the tool from the LATEST source, then runs it):
#   cmake --build build_release --target screens
# Do NOT run ./build_release/hellcat_screen_shots by hand after editing UI code
# unless you rebuilt that target first — a stale binary renders stale screens.
hellcat_add_tool(hellcat_screen_shots tools/screen_shots.cpp)

# `screens`: the foolproof way to regenerate ./screens. DEPENDS forces a Hellcat
# recompile + hellcat_screen_shots relink from the latest source whenever any UI
# code changed, THEN runs the tool. Always use this instead of invoking the
# binary directly (which can render from a stale build).
add_custom_target(screens
    COMMAND "$<TARGET_FILE:hellcat_screen_shots>" "${CMAKE_SOURCE_DIR}/screens" 2
    DEPENDS hellcat_screen_shots
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating ./screens from the latest Hellcat build"
    VERBATIM
)

# ------------------------------------------------------------------------------
# Dev tool: render Hellcat's themed right-click / popup menus to PNGs (offscreen,
# via the LookAndFeel draw hooks the live PopupMenu calls) so they can be fed to
# an LLM. Mirrors hellcat_screen_shots's link set.
# Build: cmake --build build_release --target hellcat_menu_shots
# Run:   ./build_release/hellcat_menu_shots [outputDir] [scale]   (defaults: ./screens/menus, 2)
# build_release is a release-workflow dir; configure it first (see the
# hellcat_screen_shots note above for the exact line).
hellcat_add_tool(hellcat_menu_shots tools/menu_shots.cpp)

# ------------------------------------------------------------------------------
# Phase-4 verification (not shipped): headless editor instantiation + coverage.
# Confirms createEditor() is non-null, one ParamControl per descriptor (the
# mod-matrix slot params, the fx{1,2,3}_* slot params, the FX master/topology
# params and part_select are hosted by dedicated views instead of ParamPage
# cells), exactly 2 top-level page tabs ([SYNTH][FX]; Global is a header-button
# overlay), the default size is 1280x634, and the editor deletes cleanly
# (leak-detector validated).
#   cmake --build build_unified --target hellcat_editor_coverage_check && ./build_unified/hellcat_editor_coverage_check
hellcat_add_tool(hellcat_editor_coverage_check tools/editor_coverage_check.cpp)

# ------------------------------------------------------------------------------
# Virtual MIDI source probe for the real Standalone app (manual testing):
# creates a CoreMIDI virtual source "HellcatProbe" and sends held notes to
# whatever connects to it. Zero Hellcat/JUCE dependencies — deliberately NOT
# registered via hellcat_add_tool (that would link the whole Hellcat lib);
# a plain target with the two macOS frameworks it actually needs.
#   cmake --build build_unified --target hellcat_vmidi_probe && ./build_unified/hellcat_vmidi_probe <delay> <hold> [note] [vel]
# Apple-only target: -framework is an Apple linker spelling. GCC on Linux
# and cl on Windows reject it, so the target stays unregistered there.
if(APPLE)
add_executable(hellcat_vmidi_probe EXCLUDE_FROM_ALL tools/vmidi_probe.cpp)
target_compile_features(hellcat_vmidi_probe PRIVATE cxx_std_17)
target_link_libraries(hellcat_vmidi_probe PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()

# ------------------------------------------------------------------------------
# Template-bank generator (dev tool, EXCLUDE_FROM_ALL). Produces the 5 stock init
# templates as full-fidelity .yml multis into presets/TEMPLATES/, which are
# then embedded into the plugin binary. Run from the repo root:
#   cmake --build build_unified --target hellcat_gen_templates && ./build_unified/hellcat_gen_templates
hellcat_add_tool(hellcat_gen_templates tools/gen_templates.cpp)

# ------------------------------------------------------------------------------
# NOTE: the ORIGINAL Hellcat factory bank (presets/HFACTORY/*.yml) is
# authored DIRECTLY as human-editable .yml patch files — there is no generator
# tool. The bank is guarded by tests/factory_bank_test.cpp (load, byte-exact
# round-trip, FX + spectral category rules). See presets/ATTRIBUTION.md for the
# authoring workflow.

# ------------------------------------------------------------------------------
# User-manual PDF generator (dev tool, always out of the default build). Builds
# docs/manual/Hellcat-Manual.pdf from docs/manual/Hellcat-Manual.md plus the
# LIVE parameter tables (Source/ParameterLayout.cpp + Source/ui/ParamHelp.cpp
# at run time), so the "Parameter Reference" appendix cannot drift from the
# code. Needs python3 with fpdf2 >= 2.7 (see tools/gen_manual.py; alt text
# and the outline need the 2.x API). No C++
# sources, no Hellcat link: the target only runs the script.
#   cmake --build build_unified --target hellcat_gen_manual
add_custom_target(hellcat_gen_manual
    COMMAND "${CMAKE_COMMAND}" -E echo "NOTE: re-rendering manual screenshots (they are gitignored artifacts)"
    COMMAND $<TARGET_FILE:hellcat_screen_shots> "${CMAKE_SOURCE_DIR}/docs/manual/screens" 2
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_manual.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS hellcat_screen_shots
    COMMENT "Generating docs/manual/Hellcat-Manual.pdf (manual + screenshots + live parameter appendix)"
    VERBATIM
)

# ------------------------------------------------------------------------------
# `deploy` — one-command release workflow: build ALL plugin formats (VST3, AU,
# CLAP, Standalone) + render ./screens from the latest source + install the
# built bundles into the user plug-in folders AND the Standalone into
# /Applications. Use this instead of a plain build so the installed
# plug-ins / app and the screenshots are never stale (a plain
# `--target Hellcat` does NOT rebuild the VST3/AU/CLAP format wrappers).
# For installation WITHOUT the screen renders, build_release is configured
# with HELLCAT_INSTALL_ON_BUILD=ON (see the post-build block below): every
# format target installs itself as it relinks.
# macOS-only (APPLE AND NOT HELLCAT_IOS): the AU target only exists on macOS
# and ~/Library/Audio/Plug-Ins is the macOS install root — Linux/Windows
# install from Hellcat_artefacts by hand, and iOS uses `deploy_ios` instead.
#   cmake --build build_release --target deploy
# build_release is a release-workflow dir, not part of a fresh clone.
# Configure it first: cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
# (tools/release/README.md documents the full release flow.)
if(APPLE AND NOT HELLCAT_IOS)
set(_parvati_plugins_dir "$ENV{HOME}/Library/Audio/Plug-Ins")

# One artefact's install commands, shared by `deploy` and the
# HELLCAT_INSTALL_ON_BUILD post-build steps below so the two paths can never
# drift: replace any stale copy, move the fresh bundle in, and (RESIGN)
# ad-hoc re-sign — a plain cp -R invalidates the built bundle's signature
# (Gatekeeper / codesign -v reject the copy; see the Standalone note below).
# Sets the command list (COMMAND rm ... COMMAND cp ... [COMMAND codesign ...])
# in the named parent-scope variable.
function(hellcat_install_commands out_var artefact dest)
    set(_cmds
        COMMAND rm -rf "${dest}"
        COMMAND cp    -R "${artefact}" "${dest}")
    if("RESIGN" IN_LIST ARGN)
        list(APPEND _cmds COMMAND codesign --force --sign - "${dest}")
    endif()
    set(${out_var} ${_cmds} PARENT_SCOPE)
endfunction()

# One install table drives BOTH consumers below: the `deploy` target and
# the HELLCAT_INSTALL_ON_BUILD post-build steps, so the two paths can never
# drift apart again. Fields, split on `|`:
#   target | label | artefact under Hellcat_artefacts/$<CONFIG> |
#   destination | flags | deploy-echo tail
# Flags, comma-separated: RESIGN re-signs the copied bundle ad-hoc; MKDIR
# creates the destination directory first (unlike VST3/Components,
# ~/Library/Audio/Plug-Ins/CLAP is not a stock macOS directory). The
# Standalone also installs to /Applications: without that copy the app the
# user runs stays stale while the plug-in formats update (the 2026-08-17
# "standalone still crackles after deploy" miss); its RESIGN exists because
# a plain cp invalidates JUCE's built bundle signature (`codesign -v`
# rejects the copy, Gatekeeper may refuse it).
# Hellcat_CLAP exists only while HELLCAT_BUILD_CLAP is ON, so the TARGET
# guard below covers the option too. Rows without a live target are
# skipped, so `deploy` stays generatable under ANY HELLCAT_FORMATS set
# (tests-only configure: no plugin targets, no deploy target). Row order
# matches the historical VST3 -> AU -> Standalone -> CLAP install sequence.
set(HELLCAT_INSTALL_TABLE
    "Hellcat_VST3|VST3|VST3/Hellcat.vst3|${_parvati_plugins_dir}/VST3/Hellcat.vst3||"
    "Hellcat_AU|AU|AU/Hellcat.component|${_parvati_plugins_dir}/Components/Hellcat.component||"
    "Hellcat_Standalone|Standalone|Standalone/Hellcat.app|/Applications/Hellcat.app|RESIGN|/Applications"
    "Hellcat_CLAP|CLAP|CLAP/Hellcat.clap|${_parvati_plugins_dir}/CLAP/Hellcat.clap|RESIGN,MKDIR|")

# Parse one HELLCAT_INSTALL_TABLE row into _pi_* variables. Macro: no new
# scope, the variables land in the caller (the foreach loops below).
macro(hellcat_parse_install_row row)
    string(REPLACE "|" ";" _pi_fields "${row}")
    list(GET _pi_fields 0 _pi_target)
    list(GET _pi_fields 1 _pi_label)
    list(GET _pi_fields 2 _pi_artefact)
    list(GET _pi_fields 3 _pi_dest)
    list(GET _pi_fields 4 _pi_flags)
    list(GET _pi_fields 5 _pi_echo_tail)
    set(_pi_resign "")
    if(_pi_flags MATCHES "RESIGN")
        set(_pi_resign RESIGN)
    endif()
endmacro()

set(_parvati_deploy_depends screens)
set(_parvati_deploy_commands "")
foreach(_pi_row IN LISTS HELLCAT_INSTALL_TABLE)
    hellcat_parse_install_row("${_pi_row}")
    if(NOT TARGET ${_pi_target})
        continue()
    endif()
    hellcat_install_commands(_install_cmds
        "${CMAKE_BINARY_DIR}/Hellcat_artefacts/$<CONFIG>/${_pi_artefact}"
        "${_pi_dest}" ${_pi_resign})
    set(_pi_echo "deploy: install ${_pi_label}")
    if(_pi_echo_tail)
        string(APPEND _pi_echo " -> ${_pi_echo_tail}")
    endif()
    list(APPEND _parvati_deploy_depends ${_pi_target})
    list(APPEND _parvati_deploy_commands
        COMMAND ${CMAKE_COMMAND} -E echo "${_pi_echo}")
    if(_pi_flags MATCHES "MKDIR")
        get_filename_component(_pi_dest_dir "${_pi_dest}" DIRECTORY)
        list(APPEND _parvati_deploy_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_pi_dest_dir}")
    endif()
    list(APPEND _parvati_deploy_commands ${_install_cmds})
endforeach()
if(_parvati_deploy_commands)
    add_custom_target(deploy
        ${_parvati_deploy_commands}
        COMMAND ${CMAKE_COMMAND} -E echo "deploy complete: built all formats, rendered screens, installed plug-in bundles + standalone"
        DEPENDS ${_parvati_deploy_depends}
        COMMENT "Build all formats + render screens + install VST3/AU/CLAP + Standalone"
        VERBATIM
    )
endif()

# ------------------------------------------------------------------------------
# HELLCAT_INSTALL_ON_BUILD — installation as PART OF the release build: each
# format target installs its own artefact via a POST-BUILD step the moment it
# relinks, so a plain `cmake --build build_release` (any target set) leaves
# the plug-ins in ~/Library/Audio/Plug-Ins and the app in /Applications
# up to date — no separate `deploy` invocation. Same destinations + re-sign
# rules as `deploy` (shared command composition above); `deploy` remains the
# one-shot that ALSO re-renders ./screens. Off by default; macOS desktop only.
if(HELLCAT_INSTALL_ON_BUILD)
    foreach(_pi_row IN LISTS HELLCAT_INSTALL_TABLE)
        hellcat_parse_install_row("${_pi_row}")
        if(NOT TARGET ${_pi_target})
            continue()
        endif()
        hellcat_install_commands(_install_cmds
            "${CMAKE_BINARY_DIR}/Hellcat_artefacts/$<CONFIG>/${_pi_artefact}"
            "${_pi_dest}" ${_pi_resign})
        get_filename_component(_pi_dest_dir "${_pi_dest}" DIRECTORY)
        set(_pi_pre "")
        if(_pi_flags MATCHES "MKDIR")
            set(_pi_pre COMMAND ${CMAKE_COMMAND} -E make_directory "${_pi_dest_dir}")
        endif()
        add_custom_command(TARGET ${_pi_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "install: ${_pi_label} -> ${_pi_dest_dir}"
            ${_pi_pre}
            ${_install_cmds}
            VERBATIM)
    endforeach()
endif()
endif()

# ------------------------------------------------------------------------------
# `deploy_ios` — TEMPLATE: archive the iOS Standalone app + AUv3 app-extension
# into an .ipa via `xcodebuild archive` + `-exportArchive`. This is a documented
# placeholder: a real device/.ipa export REQUIRES a signing identity + provisioning
# profile (set HELLCAT_IOS_DEVELOPMENT_TEAM and supply a matching exportOptions.plist
# with your method/provisioning profile). With no identity it will fail at the
# codesign step — expected until the user supplies signing. iOS-only.
#   cmake --build build_ios --target deploy_ios
if(HELLCAT_IOS)
    add_custom_target(deploy_ios
        COMMAND ${CMAKE_COMMAND} -E echo
            "deploy_ios: archive + export .ipa (requires a signing identity + HELLCAT_IOS_DEVELOPMENT_TEAM)"
        COMMAND xcodebuild -archivePath "${CMAKE_BINARY_DIR}/Hellcat.xcarchive"
                         -scheme Hellcat_Standalone
                         -sdk iphoneos
                         -configuration Release
                         archive
        COMMAND xcodebuild -exportArchive
                         -archivePath "${CMAKE_BINARY_DIR}/Hellcat.xcarchive"
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
#
# Interpreter resolution: Windows ships no python3 on PATH. The Microsoft
# Store alias fails, and python.org installs provide python.exe only.
# find_package locates both. Plain python3 stays as the fallback.
find_package(Python3 COMPONENTS Interpreter)
set(HELLCAT_PYTHON3 python3)
if(Python3_Interpreter_FOUND)
    set(HELLCAT_PYTHON3 "${Python3_EXECUTABLE}")
endif()
enable_testing()
add_test(NAME hellcat_check_combo_clear
         COMMAND ${HELLCAT_PYTHON3} tools/check_combo_clear.py --self-test-and-scan
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# (tool 6) Raw-`this`-in-async-callback guard: every lambda handed to an
# async UI API (PopupMenu showMenuAsync / setAction / addItem on a tracked
# PopupMenu, ModalCallbackFunction::create, chooser launchAsync, static
# dialog launch helpers) that captures `this` must be SafePointer/
# WeakReference-guarded (or allowlisted with a hand-verified justification —
# e.g. member-owned FileChooser completions, safe by the JUCE destructor
# contract). The crash-window class: popup/dialog completions firing after
# the host tore the editor down. Canary-gated like tool 5.
add_test(NAME hellcat_check_async_this
         COMMAND ${HELLCAT_PYTHON3} tools/check_async_this.py --self-test-and-scan
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
add_test(NAME hellcat_check_translations
         COMMAND ${HELLCAT_PYTHON3} tools/check_translations.py --self-test-and-scan
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Firmware-oracle integrity pin: ambika_reference/ is READ-ONLY upstream
# Ambika firmware (see NOTICES.md). firmware_parity_test asserts parity
# against it, so an unverified edit silently weakens the oracle. The check
# hashes every content file (excludes .git/, .DS_Store, ._* noise) and
# compares with the pinned manifest tools/reference_integrity.sha256.
# Re-pinning is deliberate and needs an intended upstream sync:
#   HELLCAT_PIN=1 tools/check_reference_integrity.sh
add_test(NAME hellcat_check_reference_integrity
         COMMAND bash tools/check_reference_integrity.sh
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# One build target that runs every static scanner in sequence (tools 5/6/10)
# so a single `cmake --build --target hellcat_static_checks` gates them in
# CI without needing ctest. Fails (non-zero) if any scanner fails.
add_custom_target(hellcat_static_checks
    COMMAND ${HELLCAT_PYTHON3} tools/check_combo_clear.py --self-test-and-scan
    COMMAND ${HELLCAT_PYTHON3} tools/check_async_this.py --self-test-and-scan
    COMMAND ${HELLCAT_PYTHON3} tools/check_translations.py --self-test-and-scan
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running static scanners (combo-clear / async-this / translations)"
    VERBATIM)
