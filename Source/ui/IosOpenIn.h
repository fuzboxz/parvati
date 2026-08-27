// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// IosOpenIn — completes the iOS "Open in Hellcat" loop (bug hunt 2026-08-19,
// open-in follow-up to F-ios-build-1/F-ios-files-2).
//
// Document types + exported UTIs were grafted into the Standalone plist
// (ios/hellcat_filetypes.plist), so iOS offers "Open in Hellcat" /
// "Copy to Hellcat" for .yml/.PRO/.MUL — but JUCE 9's iOS glue
// has NO application:openURL:options: implementation (checked: no openURL in
// juce_Windowing_ios.mm / juce_MessageManager_ios.mm /
// juce_audio_plugin_client; the only openURL in ~/JUCE SENDS one). Without
// this shim the file event is dropped: the app launches and nothing happens.
//
// Shape:
//   * openInKindForFile / routeOpenedFile are PURE C++ (header-inline) — the
//     deterministic test core, compiled on every platform.
//   * installOpenInHandler (IosOpenIn.mm, iOS only) resolves JUCE's
//     JuceAppStartupDelegate class BY NAME at runtime (it is file-private in
//     juce_Windowing_ios.mm) and adds application:openURL:options: via
//     class_addMethod — the selector is genuinely absent in JUCE 9, so this
//     is a clean category-style addition, no swizzling. Desktop gets an
//     inline no-op so call sites compile everywhere.
// The handler brackets the URL read with security-scoped resource access
// (open-in-place delivers a scoped URL; "Copy to Hellcat" Inbox copies are
// plain sandbox files and the bracket is a harmless no-op there).

#pragma once

#include <functional>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ui/PresetBrowser.h"   // importIntoUserTree (the atomic USER import)

namespace hellcat
{
//==========================================================================
// What kind of open-in payload a file is. The extension table is the single
// source of truth for both routing and (post-route) load decisions.
// (.scl/.kbm were removed with the custom-tuning subsystem, 2026-08-19.)
enum class OpenInKind
{
    None,     // not a Hellcat document — ignore silently
    Preset    // .yml / .PRO / .MUL → import into the USER tree + load
};

/** Pure predicate: extension → OpenInKind (case-insensitive, JUCE's
    hasFileExtension). Table-tested by hellcat_ios_openin_test [3]. */
inline OpenInKind openInKindForFile (const juce::File& f) noexcept
{
    if (f.hasFileExtension (".yml") || f.hasFileExtension (".pro") || f.hasFileExtension (".mul"))
        return OpenInKind::Preset;
    return OpenInKind::None;
}

/** Route an opened document into Hellcat's shared storage (PURE — the
    deterministic core; no iOS APIs).
      @param source       the file iOS handed us (security-scoped bracket is
                          the CALLER's job — see installOpenInHandler)
      @param userPatchDir the USER preset tree (the app-group Hellcat/USER)
      @returns the routed destination, or the source ITSELF when it already
               lives inside the destination tree (still deliverable — do not
               duplicate), or an invalid File for non-documents / failures
               (non-fatal; the caller simply does nothing).
    Presets  → USER/<name>   (atomic import, PresetBrowser::importIntoUserTree —
                              TemporaryFile + rename, overwrite on collision) */
inline juce::File routeOpenedFile (const juce::File& source, const juce::File& userPatchDir)
{
    switch (openInKindForFile (source))
    {
        case OpenInKind::Preset:
        {
            if (userPatchDir.getFullPathName().isEmpty())
                return {};
            // Already in the tree: deliver as-is (no duplicate copy).
            if (source.isAChildOf (userPatchDir))
                return source;
            return PresetBrowser::importIntoUserTree (source, userPatchDir);
        }
        case OpenInKind::None:
        default:
            return {};
    }
}

#if JUCE_IOS
/** Install the open-in handler (iOS only; IosOpenIn.mm). Resolves JUCE's
    JuceAppStartupDelegate by runtime name and adds
    application:openURL:options: (absent in JUCE 9) via class_addMethod.
    Idempotent; safe to call from any thread (installs once, later calls only
    replace the routed-file callback).
      @param userPatchDir the USER preset tree handed to routeOpenedFile
      @param onRouted     invoked ON THE MAIN THREAD with the routed
                          destination (invalid File ⇒ ignored) for every
                          successfully routed open-in document. */
void installOpenInHandler (const juce::File& userPatchDir,
                           std::function<void (juce::File)> onRouted);
#else
/** Desktop no-op (the pure core above stays usable in tests everywhere). */
inline void installOpenInHandler (const juce::File&, std::function<void (juce::File)>)
{
}
#endif

}  // namespace hellcat
