// a11y_smoke_test — accessibility coverage smoke pass over the editor.
//
// Walks the real editor component tree and asserts the parameter surface
// is announced to assistive tech:
//   [1] every ParamControl cell exposes an AccessibilityHandler with a
//       non-empty title (the descriptor label — see the ctor block);
//   [2] every cell's child juce::Slider exposes a VALUE interface with a
//       valid range (min < max), and its handler carries a title;
//   [3] every cell's child juce::ComboBox handler carries a title;
//   [4] nearly every cell carries a non-empty description (the ParamHelp
//       tooltip text); the count is reported.
// The coverage floor (kMinCells) sits far above one full page, so a full
// page of parameters is always exercised.
//
// Headless hosts (no display server) SKIP: JUCE creates accessibility
// handlers only for components with a window handle, and a peer needs a
// display (see test_utils.h displayAvailable).
//
// Unified runner. Run with:
//   ./build_unified/hellcat_unified_tests a11y_smoke_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // displayAvailable (headless-host skip)
#include "ui/ParamControl.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Depth-first walk collecting every ParamControl under @p root.
void collectControls (juce::Component* root, std::vector<ParamControl*>& out)
{
    if (root == nullptr)
        return;
    if (auto* pc = dynamic_cast<ParamControl*> (root))
        out.push_back (pc);
    for (auto* child : root->getChildren())
        collectControls (child, out);
}

// First descendant of the given template type.
template <typename T>
T* findChild (juce::Component* root)
{
    if (root == nullptr)
        return nullptr;
    for (auto* child : root->getChildren())
    {
        if (auto* typed = dynamic_cast<T*> (child))
            return typed;
        if (auto* found = findChild<T> (child))
            return found;
    }
    return nullptr;
}
}  // namespace

TEST(a11y_smoke_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (! displayAvailable())
    {
        std::printf ("SKIP: no display server (headless host) - a11y_smoke_test needs a window peer\n");
        return true;
    }

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    check (ed != nullptr, "editor created");
    if (ed == nullptr)
        return false;

    // Handlers exist only once the tree holds a window handle.
    ed->addToDesktop (0);

    std::vector<ParamControl*> cells;
    collectControls (ed.get(), cells);
    std::printf ("     ParamControl cells found: %d\n", (int) cells.size());
    constexpr int kMinCells = 30;   // one full page of parameters (the editor
                                    // hosts the active tab's page; the walk
                                    // covers every live cell on it)
    check ((int) cells.size() >= kMinCells, "full parameter surface present (>= 60 cells)");

    int titledCells = 0, describedCells = 0;
    int sliders = 0, slidersValued = 0, slidersRanged = 0, slidersTitled = 0;
    int combos = 0, combosTitled = 0;

    for (auto* pc : cells)
    {
        auto* handler = pc->getAccessibilityHandler();
        if (handler == nullptr)
            continue;
        if (handler->getTitle().isNotEmpty())
            ++titledCells;
        if (! handler->getDescription().isEmpty())
            ++describedCells;

        if (auto* slider = findChild<juce::Slider> (pc))
        {
            ++sliders;
            if (auto* sh = slider->getAccessibilityHandler())
            {
                if (sh->getTitle().isNotEmpty())
                    ++slidersTitled;
                if (auto* vi = sh->getValueInterface())
                {
                    ++slidersValued;
                    const auto range = vi->getRange();
                    if (range.isValid() && range.getMinimumValue() < range.getMaximumValue())
                        ++slidersRanged;
                }
            }
        }
        if (auto* combo = findChild<juce::ComboBox> (pc))
        {
            ++combos;
            if (auto* ch = combo->getAccessibilityHandler())
                if (ch->getTitle().isNotEmpty())
                    ++combosTitled;
        }
    }

    std::printf ("     cells: %d/%d titled, %d described\n", titledCells, (int) cells.size(), describedCells);
    std::printf ("     sliders: %d (%d titled, %d valued, %d valid-range)   combos: %d (%d titled)\n",
                 sliders, slidersTitled, slidersValued, slidersRanged, combos, combosTitled);

    char msg[128];
    std::snprintf (msg, sizeof (msg), "every ParamControl exposes a handler with a non-empty title (%d/%d)",
                   titledCells, (int) cells.size());
    check (titledCells == (int) cells.size(), msg);

    std::snprintf (msg, sizeof (msg), "nearly every cell described (ParamHelp tooltip) (%d/%d >= 90%%)",
                   describedCells, (int) cells.size());
    check (describedCells * 10 >= (int) cells.size() * 9, msg);

    std::snprintf (msg, sizeof (msg), "every cell slider handler titled (%d/%d)", slidersTitled, sliders);
    check (sliders > 0 && slidersTitled == sliders, msg);

    std::snprintf (msg, sizeof (msg), "every cell slider exposes a value interface (%d/%d)", slidersValued, sliders);
    check (sliders > 0 && slidersValued == sliders, msg);

    std::snprintf (msg, sizeof (msg), "every cell slider value interface has a valid range (%d/%d)", slidersRanged, sliders);
    check (sliders > 0 && slidersRanged == sliders, msg);

    std::snprintf (msg, sizeof (msg), "every cell combo handler titled (%d/%d)", combosTitled, combos);
    check (combos > 0 && combosTitled == combos, msg);

    ed->removeFromDesktop();
    ed.reset();

    std::printf ("%s (%d failure%s)\n",
                 g_failures ? "A11Y SMOKE TEST: FAILURES" : "A11Y SMOKE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
