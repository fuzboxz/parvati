// Copyright (c) 2026 805Labs Kft. / Hellcat.  See SharedContainer.h.

#include "SharedContainer.h"

namespace hellcat
{
juce::File getSharedContainerRoot()
{
#if JUCE_IOS
    // iOS: prefer the shared App-Group container so the Standalone app and the
    // AUv3 app-extension share one presets/state tree. Fall back to the app's
    // own sandbox data dir when the entitlement is unset (the simulator without
    // a provisioning profile returns nil for the container URL).
    const auto group = appGroupContainerRootIOS();
    if (group.getFullPathName().isNotEmpty())
        return group;
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#else
    // Desktop: return the user app-data dir UNCHANGED. This is byte-identical to
    // the previous getSpecialLocation(userApplicationDataDirectory) used by every
    // preset/state root in PluginProcessor.cpp.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#endif
}

} // namespace hellcat
