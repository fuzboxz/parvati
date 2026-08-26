// Layout overlap regression test (R3).
//
// User report: "there should be a minimum height for all components, right now
// compacting them means that certain things overlap." When the editor is
// resized to its minimum (or a short AUv3 host pane / zoomed-out frame), rows
// computed as remainders can shrink to (near-)zero while their children keep
// fixed bounds — controls then paint over each other.
//
// This test is the systematic reproducer AND the permanent gate for that bug
// class. For every swept editor size it walks the FULL visible component tree
// on each top-level page (SYNTH / FX / Patch) and asserts:
//   [1] SIBLING OVERLAP: no two visible sibling components that are both
//       interactive (Button / Slider / ComboBox / Label / ScrollBar / ListBox
//       / Viewport content) intersect by more than the tolerance in BOTH axes.
//   [2] PARENT ESCAPE: no visible component extends beyond its parent's bounds
//       by more than the tolerance (pages sized to their natural height must
//       be CLIPPED by a viewport/column, never spill onto the next row).
//
// The overlap-only-when-scrollable exemption: a component inside a juce
// .Viewport may extend beyond the VIEWPORT's own bounds by design ONLY when
// the viewport actually scrolls that axis (the T4 safety-net pattern — the
// content is reachable by scrolling, not painted over siblings). The check
// accepts out-of-bounds children of a Viewport on axes where the viewport has
// non-zero maximum scroll. Everything else must stay inside.
//
// Run: ./build_unified/hellcat_unified_tests layout_overlap_test

#if defined (__GNUC__) || defined (__clang__)
    #include <cxxabi.h>   // abi::__cxa_demangle (GCC/Clang ABI)
#endif
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Best-effort human-readable component description for failure output.
juce::String describe (juce::Component* c)
{
    if (auto* b = dynamic_cast<juce::TextButton*> (c))   return "TextButton('" + b->getButtonText() + "')";
    if (auto* b = dynamic_cast<juce::ToggleButton*> (c)) return "ToggleButton('" + b->getButtonText() + "')";
    if (auto* b = dynamic_cast<juce::Button*> (c))       return juce::String (b->getName()) + "(Button)";
    if (auto* s = dynamic_cast<juce::Slider*> (c))       return juce::String (s->getName()) + "(Slider)";
    if (auto* cb = dynamic_cast<juce::ComboBox*> (c))    return juce::String (cb->getName()) + "(ComboBox)";
    if (auto* l = dynamic_cast<juce::Label*> (c))        return juce::String (l->getName()) + "(Label '" + l->getText().substring (0, 16) + "')";
    if (dynamic_cast<juce::Viewport*> (c) != nullptr)    return juce::String (c->getName()) + "(Viewport)";
    if (dynamic_cast<juce::ScrollBar*> (c) != nullptr)   return juce::String (c->getName()) + "(ScrollBar)";
    if (auto* tc = dynamic_cast<juce::TooltipClient*> (c))
    {
        const auto t = tc->getTooltip();
        if (t.isNotEmpty())
            return juce::String (c->getName()) + "(tooltip '" + t.substring (0, 24) + "')";
    }
    int status = 0;
#if defined (__GNUC__) || defined (__clang__)
    char* dem = abi::__cxa_demangle (typeid (*c).name(), nullptr, nullptr, &status);
    juce::String cn = (status == 0 && dem != nullptr) ? juce::String (dem) : juce::String (typeid (*c).name());
    ::free (dem);
#else
    // MSVC type names are already readable; no demangle step exists.
    juce::String cn (typeid (*c).name());
#endif
    const auto cut = cn.lastIndexOf (":");
    if (cut >= 0) cn = cn.substring (cut + 1);
    return juce::String (c->getName()) + "(" + cn + ")";
}

// Interactive = something the user reads or operates. Two of these overlapping
// is always a bug; decorative containers are excluded (their children are
// checked individually).
bool isInteractive (juce::Component* c)
{
    return dynamic_cast<juce::Button*> (c)         != nullptr
        || dynamic_cast<juce::Slider*> (c)         != nullptr
        || dynamic_cast<juce::ComboBox*> (c)       != nullptr
        || dynamic_cast<juce::Label*> (c)          != nullptr
        || dynamic_cast<juce::ScrollBar*> (c)      != nullptr
        || dynamic_cast<juce::TabbedButtonBar*> (c) != nullptr;
}

// Per-axis allowance for hairline rendering geometry (borders, centring).
constexpr int kTolerance = 2;

// A child may exceed its parent's bounds without a violation in exactly two
// by-design situations:
//   1. It is the VIEWED COMPONENT of an ancestor juce::Viewport, and only
//      along an axis that viewport actually scrolls (the T4 safety-net
//      pattern — content is reachable by scrolling, never painted over a
//      sibling outside the viewport).
//   2. Its parent clips children (setClipToBounds): sub-minimum space then
//      degrades to CLIPPED, not overlaid — the R3 degradation contract.
// (Handled by the caller: a component parked FULLY offscreen — e.g. the
// closed settings drawer at x >= parent width — is a visibility trick, not
// an overlap; it is skipped before this check.)
bool scrollOrClipExempt (juce::Component* child, juce::Component* parent)
{
    for (auto* p = parent; p != nullptr; p = p->getParentComponent())
        if (auto* vp = dynamic_cast<juce::Viewport*> (p))
            if (vp->getViewedComponent() == child)
            {
                const bool vScrolls = child->getHeight() > vp->getViewHeight() + kTolerance;
                const bool hScrolls = child->getWidth()  > vp->getViewWidth()  + kTolerance;
                const bool escapesV = child->getBoundsInParent().getY() < -kTolerance
                                       || child->getBoundsInParent().getBottom() > vp->getViewHeight() + kTolerance;
                const bool escapesH = child->getBoundsInParent().getX() < -kTolerance
                                       || child->getBoundsInParent().getRight() > vp->getViewWidth() + kTolerance;
                if (escapesV && ! vScrolls)
                    return false;
                if (escapesH && ! hScrolls)
                    return false;
                return true;
            }
    return false;
}

struct Violation { juce::String what; };

// Collects violations for the subtree rooted at `root`.
void audit (juce::Component* root, std::vector<Violation>& out)
{
    for (auto* child : root->getChildren())
    {
        if (! child->isVisible() || child->getWidth() <= 0 || child->getHeight() <= 0)
            continue;

        // [2] parent escape (tolerance on every edge). A child parked FULLY
        // offscreen is a hidden-drawer idiom, not an overlap — skipped.
        const auto cb = child->getBoundsInParent();
        const bool fullyOff = cb.getX() >= root->getWidth() || cb.getY() >= root->getHeight()
                              || cb.getRight() <= 0 || cb.getBottom() <= 0;
        if (! fullyOff && ! scrollOrClipExempt (child, root))
        {
            if (cb.getX() < -kTolerance || cb.getY() < -kTolerance
                || cb.getRight() > root->getWidth() + kTolerance
                || cb.getBottom() > root->getHeight() + kTolerance)
                out.push_back ({ describe (child) + " escapes parent " + describe (root)
                                 + " (child " + cb.toString() + " vs parent "
                                 + root->getLocalBounds().toString() + ")" });
        }

        audit (child, out);
    }

    // [1] pairwise sibling overlap among interactive children.
    std::vector<juce::Component*> interactive;
    for (auto* c : root->getChildren())
        if (c->isVisible() && c->getWidth() > 0 && c->getHeight() > 0 && isInteractive (c))
            interactive.push_back (c);
    for (size_t i = 0; i < interactive.size(); ++i)
        for (size_t j = i + 1; j < interactive.size(); ++j)
        {
            const auto a = interactive[i]->getBoundsInParent();
            const auto b = interactive[j]->getBoundsInParent();
            const auto ix = a.getIntersection (b);
            if (! ix.isEmpty()
                && ix.getWidth() > kTolerance && ix.getHeight() > kTolerance)
                out.push_back ({ describe (interactive[i]) + " overlaps sibling "
                                 + describe (interactive[j]) + " at " + ix.toString() });
        }
}

const char* pageName (int idx) { return idx == 0 ? "SYNTH" : idx == 1 ? "FX" : "Patch"; }

void sweepSize (HellcatEditor& editor, int w, int h)
{
    std::printf ("\n=== %dx%d ===\n", w, h);
    // setSize below the resize-limit minimum deliberately bypasses the limits
    // (documented in HellcatEditor::resized comments for headless tests).
    editor.setSize (w, h);
    for (int page = 0; page < 3; ++page)
    {
        editor.setCurrentTopPage (page);
        std::vector<Violation> v;
        audit (&editor, v);
        std::printf ("  [%s] %zu violation%s\n", pageName (page), v.size(), v.size() == 1 ? "" : "s");
        for (const auto& x : v)
            std::printf ("    %s\n", x.what.toRawUTF8());
        check (v.empty(), (juce::String ("no layout violations on ") + pageName (page) + " page").toRawUTF8());
    }
}
} // namespace

TEST(layout_overlap_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    auto* editorRaw = proc.createEditor();
    check (editorRaw != nullptr, "editor constructs");
    if (editorRaw == nullptr)
        return false;
    auto* editor = dynamic_cast<HellcatEditor*> (editorRaw);
    check (editor != nullptr, "editor is a HellcatEditor");
    if (editor == nullptr)
    {
        delete editorRaw;
        return false;
    }
    editor->setVisible (true);

    // The sweep: the documented resize floor, then increasingly compact frames
    // (the AUv3 pane / zoom-out regime the report came from).
    sweepSize (*editor, 1024, 500);
    sweepSize (*editor, 1024, 440);
    sweepSize (*editor, 1024, 300);
    sweepSize (*editor, 800, 400);
    // Sanity bookend: the tuned design size must stay clean too.
    sweepSize (*editor, 1280, 634);

    delete editor;
    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
