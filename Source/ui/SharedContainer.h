// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// SharedContainer — resolves the presets/state root directory.
//
// On iOS the Standalone .app and the AUv3 .appex run in SEPARATE sandboxes, so
// without a shared container each would extract its own factory-presets copy and
// never see the other's user saves. getSharedContainerRoot() returns the App
// Group container (group.com.805labs.hellcat) on iOS so both share one tree,
// falling back to the app's sandbox data dir when the entitlement is unset
// (simulator without provisioning returns nil).
//
// On non-iOS (desktop) it returns userApplicationDataDirectory UNCHANGED — the
// exact same directory the four preset/state roots in PluginProcessor.cpp used
// before, so desktop behaviour is byte-identical. All iOS logic is compiled out
// under `#if ! JUCE_IOS`.

#pragma once

#include <juce_core/juce_core.h>

namespace hellcat
{
// The shared presets/state root. See file header. Never returns an empty File.
juce::File getSharedContainerRoot();

#if JUCE_IOS
// iOS-only Obj-C++ resolver (Source/ui/SharedContainer.mm). Returns the App
// Group container URL, or an EMPTY File when the entitlement is unavailable
// (simulator without a provisioning profile). Desktop never references this.
juce::File appGroupContainerRootIOS();
#endif
} // namespace hellcat
