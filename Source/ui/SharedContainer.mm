// Copyright (c) 2026 805Labs Kft. / Hellcat.  See SharedContainer.h.
//
// iOS-only Obj-C++ resolver for the App-Group container. Compiled ONLY on iOS
// (added to the Hellcat target under `if(HELLCAT_IOS)` in CMakeLists.txt). On
// non-iOS this file is not built and the desktop path in SharedContainer.cpp is
// used instead (byte-identical to the prior behaviour).

#include "SharedContainer.h"

#include <Foundation/Foundation.h>

namespace hellcat
{
juce::File appGroupContainerRootIOS()
{
    @autoreleasepool
    {
        NSURL* const url = [[NSFileManager defaultManager]
            containerURLForSecurityApplicationGroupIdentifier:
                @"group.com.805labs.hellcat"];
        if (url != nil)
            return juce::File { juce::String::fromCFString (
                (__bridge CFStringRef) [url path]) };
    }
    return juce::File {};
}

} // namespace hellcat
