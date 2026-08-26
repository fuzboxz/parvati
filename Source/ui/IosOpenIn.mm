// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// IosOpenIn.mm — the iOS half of the open-in loop (see IosOpenIn.h). iOS only;
// added to the build via an explicit target_sources line (mirroring
// SharedContainer.mm — the Source/ glob covers .cpp/.cc/.h only).

#include "ui/IosOpenIn.h"

#include <mutex>
#include <utility>

// App-level Obj-C++ does NOT inherit UIKit from juce_gui_basics.h (JUCE's
// iOS headers are module-internal): import the frameworks directly, the
// SharedContainer.mm pattern.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <juce_gui_basics/juce_gui_basics.h>   // juce::File

#include <objc/message.h>
#include <objc/runtime.h>

namespace
{
// The installed handler state (single install; the Standalone processor owns
// it for the app's lifetime — see the ctor hook).
std::mutex gOpenInMutex;
std::function<void (juce::File)> gOpenInCallback;
juce::File gOpenInUserDir;

// The IMP JUCE's delegate will call. Static C linkage: (id self, SEL _cmd,
// UIApplication*, NSURL*, NSDictionary*) — the UIApplicationDelegate
// application:openURL:options: signature. Runs ON THE MAIN THREAD (UIKit
// delegate delivery), which is exactly what the routed-file callback assumes.
void hellcatApplicationOpenURL (id, SEL, UIApplication*, NSURL* url, NSDictionary*)
{
    if (url == nullptr)
        return;

    // Open-in-place hands us a SECURITY-SCOPED URL: reads are only permitted
    // between start/stop. "Copy to Hellcat" Inbox copies are plain sandbox
    // files; start... returns NO there and the bracket is a harmless no-op.
    const bool scoped = [url startAccessingSecurityScopedResource];
    juce::File routed;
    {
        std::lock_guard<std::mutex> lock (gOpenInMutex);
        const juce::File source { juce::String (url.path.UTF8String) };
        routed = hellcat::routeOpenedFile (source, gOpenInUserDir);
    }
    if (scoped)
        [url stopAccessingSecurityScopedResource];

    // Deliver (invalid File ⇒ non-document/route failure: silently ignored).
    std::function<void (juce::File)> cb;
    {
        std::lock_guard<std::mutex> lock (gOpenInMutex);
        cb = gOpenInCallback;
    }
    if (routed.existsAsFile() && cb)
        cb (routed);
}
}  // namespace

namespace hellcat
{
void installOpenInHandler (const juce::File& userPatchDir,
                           std::function<void (juce::File)> onRouted)
{
    std::lock_guard<std::mutex> lock (gOpenInMutex);
    gOpenInUserDir = userPatchDir;
    gOpenInCallback = std::move (onRouted);

    // JuceAppStartupDelegate is file-private inside
    // juce_gui_basics/native/juce_Windowing_ios.mm (the delegate UIApplicationMain
    // installs for the Standalone app) — resolve it BY NAME at runtime.
    Class delegateClass = objc_getClass ("JuceAppStartupDelegate");
    if (delegateClass == nullptr)
        return;   // not the JUCE iOS app path — nothing to install onto

    static bool installed = false;   // idempotent across processor instances
    if (installed)
        return;

    const SEL openSel = sel_registerName ("application:openURL:options:");
    Method existing = class_getInstanceMethod (delegateClass, openSel);
    if (existing == nullptr)
    {
        // The JUCE 9 shape: the selector is ABSENT — add it outright
        // (category-style, no swizzle). "v@:@@@": void return, self, _cmd,
        // UIApplication*, NSURL*, NSDictionary*.
        class_addMethod (delegateClass, openSel,
                         (IMP) hellcatApplicationOpenURL, "v@:@@@");
    }
    else
    {
        // Future-proofing (a JUCE that implements the selector): add ours
        // under a PRIVATE selector and exchange implementations — calls to
        // the public selector run ours; the private one keeps the original
        // IMP available for a call-through if that is ever wanted.
        const SEL privateSel = sel_registerName ("hellcat_application:openURL:options:");
        class_addMethod (delegateClass, privateSel,
                         (IMP) hellcatApplicationOpenURL, "v@:@@@");
        Method ours = class_getInstanceMethod (delegateClass, privateSel);
        if (ours != nullptr)
            method_exchangeImplementations (existing, ours);
    }
    installed = true;
}
}  // namespace hellcat
