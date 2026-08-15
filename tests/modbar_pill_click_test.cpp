// CentralModBar pill click-selection regression test (P3).
//
// User report: "I can no longer select a pill on desktop, only drag and drop."
// Investigated with real CGEvent clicks on the live standalone AND this
// headless harness: the click chain (mouseUp -> invokeClicked -> workspace
// lambda -> setActiveGenerator -> showGenerator) is intact, including after
// [MOD] mode toggling. The report matches [MOD] tap-assign mode being engaged
// (by design a pill click there arms the source instead of browsing — drag
// still works), which is why [4] pins both the suppression and the restore.
//
// Coverage:
//   [1] Hit-testing: getComponentAt at a generator pill's centre reaches the
//       pill through the full editor hierarchy (bar -> Viewport -> content).
//   [2] The user gesture: a clean click (down + up, no movement) on a
//       GENERATOR pill swaps the active generator editor (the workspace
//       host Viewport's viewed component changes) — the real wiring,
//       ModPill::mouseUp -> CentralModBar::invokeClicked -> SynthWorkspace
//       lambda -> setActiveGenerator -> showGenerator.
//   [3] The swap goes to the CLICKED generator's page.
//   [4] [MOD] tap-assign mode intentionally suppresses the selection, and
//       turning the mode OFF restores it.
//   [5] The FX workspace's bar wiring behaves the same (shared pages).
//
// The click is driven by calling the pill's mouseDown/mouseUp overrides with
// synthetic MouseEvents on the real main mouse source — the same technique as
// tests/keyboard_view_test.cpp — because peer-level event injection is not
// reliable headlessly (verified: ComponentPeer::handleMouseEvent delivers
// nothing even to a plain juce::TextButton in a temporary desktop window).
// Hit-testing, the other half of real event delivery, is covered by [1].
//
// Built by default. Run: ./build/parvati_modbar_click_test

#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/CentralModBar.h"
#include "ui/FxWorkspace.h"
#include "ui/SynthWorkspace.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// First component of type T in the subtree (DFS).
template <typename T>
T* findFirst (juce::Component* c)
{
    if (auto* t = dynamic_cast<T*> (c))
        return t;
    for (auto* child : c->getChildren())
        if (auto* t = findFirst<T> (child))
            return t;
    return nullptr;
}

// Every component of type T in the subtree.
template <typename T>
void collectAll (juce::Component* c, std::vector<T*>& out)
{
    if (auto* t = dynamic_cast<T*> (c))
        out.push_back (t);
    for (auto* child : c->getChildren())
        collectAll<T> (child, out);
}

// True when needle is c or lies in c's subtree.
bool isWithin (juce::Component* needle, juce::Component* c)
{
    while (needle != nullptr)
    {
        if (needle == c)
            return true;
        needle = needle->getParentComponent();
    }
    return false;
}

// The generator pill whose tooltip matches fullName (pills are the file-local
// ModPill; reachable only as SettableTooltipClient children of the bar's
// Viewport content).
juce::Component* findPillByTooltip (CentralModBar* bar, const juce::String& fullName)
{
    std::vector<juce::SettableTooltipClient*> tcs;
    collectAll (bar, tcs);
    for (auto* p : tcs)
        if (p->getTooltip() == fullName)
            return dynamic_cast<juce::Component*> (p);
    return nullptr;
}

// The active-editor host is the workspace's only DIRECT Viewport child (the
// bar's scrolling viewport lives deeper, inside the CentralModBar).
juce::Viewport* findHostViewport (juce::Component* workspace)
{
    for (auto* child : workspace->getChildren())
        if (auto* v = dynamic_cast<juce::Viewport*> (child))
            return v;
    return nullptr;
}

// A clean desktop click on comp: mouseDown then mouseUp, no movement, driven
// through the component's real mouse handlers on the main mouse source.
void click (juce::Component* comp)
{
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto pos = comp->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);

    comp->mouseDown (juce::MouseEvent (source, pos, mods,
                                       juce::MouseInputSource::defaultPressure,
                                       juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation,
                                       juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY,
                                       comp, comp, now, pos, now, 1, false));
    comp->mouseUp (juce::MouseEvent (source, pos, juce::ModifierKeys(),
                                     juce::MouseInputSource::defaultPressure,
                                     juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY,
                                     comp, comp, now, pos, now, 1, false));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    juce::AudioProcessorEditor* editor = proc.createEditor();
    check (editor != nullptr, "editor constructs");
    if (editor == nullptr)
        return 1;

    auto* workspace = findFirst<SynthWorkspace> (editor);
    check (workspace != nullptr, "SynthWorkspace found in the editor tree");

    auto* host = workspace != nullptr ? findHostViewport (workspace) : nullptr;
    check (host != nullptr, "active-editor host Viewport found (T4 restructure)");

    auto* bar = findFirst<CentralModBar> (editor);
    check (bar != nullptr, "CentralModBar found in the editor tree");

    if (workspace == nullptr || host == nullptr || bar == nullptr)
    {
        std::printf ("FAIL (prerequisites)\n");
        return 1;
    }

    // The editor's own visible flag gates getComponentAt (a freshly created,
    // never-shown editor still has it false — the standalone/plugin window
    // sets it in real use).
    editor->setVisible (true);

    // ---- [1] Hit-testing: the pill under the pointer IS the pill ----
    std::printf ("\n[1] Pill hit-testing\n");
    auto* lfoPill = findPillByTooltip (bar, "LFO 2");
    check (lfoPill != nullptr, "generator pill (LFO 2) located via tooltip");
    bool pillReachable = false;
    if (lfoPill != nullptr)
    {
        const auto centre = editor->getLocalArea (lfoPill, lfoPill->getLocalBounds()).getCentre();
        auto* hit = editor->getComponentAt (centre);
        pillReachable = isWithin (hit, lfoPill);
        check (pillReachable, "getComponentAt(pill centre) reaches the pill");
    }

    // ---- [2] The user gesture: a clean click selects the generator ----
    std::printf ("\n[2] Real click selects a generator pill\n");
    if (lfoPill != nullptr)
    {
        auto* before = host->getViewedComponent();
        click (lfoPill);
        auto* after = host->getViewedComponent();
        check (after != before, "click on LFO 2 pill swaps the active generator editor");
    }

    // ---- [3] A different generator swaps to THAT page ----
    std::printf ("\n[3] Click selects the CLICKED generator's page\n");
    auto* envPill = findPillByTooltip (bar, "Env 1");
    if (envPill != nullptr)
    {
        auto* before = host->getViewedComponent();
        click (envPill);
        auto* after = host->getViewedComponent();
        check (after != nullptr && after != before, "click on Env 1 swaps again (different page)");
    }

    // ---- [4] [MOD] tap-assign mode: selection is intentionally suppressed,
    //          and turning the mode OFF restores selection ----
    std::printf ("\n[4] [MOD] mode suppression + restore\n");
    if (lfoPill != nullptr)
    {
        ParamControl::setTapAssignActive (true);
        auto* before = host->getViewedComponent();
        click (lfoPill);
        check (host->getViewedComponent() == before, "[MOD] on: pill click does NOT swap the editor (by design)");

        ParamControl::setTapAssignActive (false);
        // Click a generator on a DIFFERENT page than the currently-active one
        // (section [3] left Env 1 active on the SHARED envelopes page — Env 2/3
        // register the same envPage with a different visible-group subset, so
        // showGenerator correctly keeps the viewed component and only swaps the
        // subset; a same-page probe here would be a false failure). LFO 2 lives
        // on the LFO page, so the viewed component must change.
        auto* lfoPillForRestore = findPillByTooltip (bar, "LFO 2");
        check (lfoPillForRestore != nullptr, "LFO 2 pill located for the restore probe");
        if (lfoPillForRestore != nullptr)
        {
            click (lfoPillForRestore);
            check (host->getViewedComponent() != before, "[MOD] off: pill click swaps the editor again");
        }
    }

    // ---- [5] The FX workspace's bar wiring (same plumbing, shared pages) ----
    std::printf ("\n[5] FX workspace pill selection\n");
    if (auto* fx = findFirst<FxWorkspace> (editor))
    {
        fx->setVisible (true);   // hit-testable + active, as the FX page flip leaves it
        auto* fxBar = findFirst<CentralModBar> (fx);
        check (fxBar != nullptr, "FX workspace's CentralModBar found");
        auto* fxHost = findHostViewport (fx);
        check (fxHost != nullptr, "FX active-editor host Viewport found");
        auto* fxLfoPill = fxBar != nullptr ? findPillByTooltip (fxBar, "LFO 3") : nullptr;
        check (fxLfoPill != nullptr, "FX-bar generator pill (LFO 3) located");
        if (fxHost != nullptr && fxLfoPill != nullptr)
        {
            auto* before = fxHost->getViewedComponent();
            click (fxLfoPill);
            check (fxHost->getViewedComponent() != before, "FX-bar pill click swaps the active generator editor");
        }
    }

    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    delete editor;
    return g_failures == 0 ? 0 : 1;
}
