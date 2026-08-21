// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// workspace_padding_test — pins batch-3 part C (2026-08-20):
//   [1] SynthWorkspace's top-row padding == FxWorkspace's (FX-page parity for
//       the synth module-panel whitespace): kRowGap constants equal, 8pt, and
//       the rowPaddingForTest() accessors agree.
//   [2] FX-matrix rows use the synth-matrix icon idiom: the delete X
//       (IconButton) is present and the RIGHTMOST control; the mute/bypass
//       lamp is present with a >=44pt hit target; NO TextButton with the old
//       "Clear"/"M" labels remains anywhere in a row; and the ACTIONS still
//       fire through the same view seams (X clears the slot, lamp toggles
//       the session mute).
// Built by default. Run: ./build/parvati_workspace_padding_test

#include <cstdio>
#include "unified_test_runner.h"
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/FxMatrixView.h"
#include "ui/FxWorkspace.h"
#include "ui/IconButton.h"
#include "ui/SynthWorkspace.h"
#include "ui/ThemeManager.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void collectAll (juce::Component* c, std::vector<juce::Component*>& out)
{
    if (c == nullptr)
        return;
    out.push_back (c);
    for (auto* child : c->getChildren())
        collectAll (child, out);
}

IconButton* findIconButton (juce::Component* row)
{
    std::vector<juce::Component*> children;
    collectAll (row, children);
    for (auto* c : children)
        if (auto* b = dynamic_cast<IconButton*> (c))
            return b;
    return nullptr;
}

juce::Button* findTitledButton (juce::Component* row, const juce::String& title)
{
    std::vector<juce::Component*> children;
    collectAll (row, children);
    for (auto* c : children)
        if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getTitle() == title)
            return b;
    return nullptr;
}

// A synthetic clean click (down + up, no movement) — fires Button::onClick
// synchronously (the mod_matrix_ui_test technique).
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
}  // namespace

TEST(workspace_padding_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[1] workspace top-row padding parity\n");
    {
        ThemeManager themeManager;
        SynthWorkspace synth (themeManager);
        FxWorkspace   fx (themeManager);

        check (SynthWorkspace::kRowGap == 8, "SynthWorkspace::kRowGap == 8 (FX-page value)");
        check (FxWorkspace::kRowGap == 8, "FxWorkspace::kRowGap == 8");
        check (SynthWorkspace::kRowGap == FxWorkspace::kRowGap,
               "synth top-row gap equals the FX top-row gap");
        check (synth.rowPaddingForTest() == fx.rowPaddingForTest(),
               "rowPaddingForTest() accessors agree");
    }

    std::printf ("\n[2] FX-matrix rows use the icon idiom (lamp + X, no text buttons)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        ThemeManager themeManager;

        juce::Component host;
        host.setBounds (0, 0, 900, 600);
        FxMatrixView view (proc, themeManager);
        host.addAndMakeVisible (view);
        view.setBounds (0, 0, 880, 560);

        // Activate slot 0 directly through the APVTS (the same seam a loaded
        // preset / the add-button path writes), then refresh so the row shows.
        // NOTE: fxmod1_amount spans -63..+63, so normalized 0.5 is EXACTLY
        // ZERO — use a normalized value that maps to a nonzero depth.
        if (auto* p = proc.getApvts().getParameter ("fxmod1_amount"))
            p->setValueNotifyingHost (p->convertTo0to1 (32.0f));
        view.refresh();

        auto* row = view.rowForSlotForTest (0);
        check (row != nullptr, "rowForSlotForTest(0) returns the row");
        if (row == nullptr)
        {
            std::printf ("\nWORKSPACE PADDING TEST: %s (%d failures)\n",
                         g_failures ? "FAILURES" : "ALL CHECKS PASSED", g_failures);
            return g_failures == 0;
        }

        // (a) No text "Clear"/"M" button remains anywhere in the row.
        {
            std::vector<juce::Component*> children;
            collectAll (row, children);
            int oldLabelled = 0;
            for (auto* c : children)
            {
                auto* tb = dynamic_cast<juce::TextButton*> (c);
                if (tb == nullptr)
                    continue;
                const auto text = tb->getButtonText();
                if (text == "M" || text == "Clear" || text == TRANS ("M") || text == TRANS ("Clear"))
                    ++oldLabelled;
            }
            check (oldLabelled == 0, "no 'Clear'/'M' TextButton remains in the row");
        }

        // (b) The delete X (IconButton) is present, titled, and the RIGHTMOST
        // direct child of the row.
        auto* x = findIconButton (row);
        check (x != nullptr, "row has an IconButton (delete X)");
        if (x != nullptr)
        {
            check (x->getTitle().isNotEmpty(), "X carries an accessible title");
            check (x->getWidth() >= 44, "X hit target >= 44pt wide");
            int maxRight = -1;
            for (auto* c : row->getChildren())
                maxRight = juce::jmax (maxRight, c->getBounds().getRight());
            check (x->getBounds().getRight() == maxRight, "X is the rightmost control in the row");

            // Action parity: the X clears the slot (the old "Clear" action).
            check (view.amountForSlot (0) != 0, "slot 0 carries a nonzero depth before the X click");
            click (x);
            view.refresh();
            check (view.amountForSlot (0) == 0, "X click clears the slot (old Clear action)");
        }

        // (c) The mute/bypass lamp replaces the old text "M" button: present,
        // titled, 44pt hit target, and the action still toggles the mute.
        auto* lamp = findTitledButton (row, TRANS ("Mute / bypass this modulation"));
        check (lamp != nullptr, "mute/bypass lamp present (titled button)");
        if (lamp != nullptr)
        {
            check (lamp->getWidth() >= 44, "lamp hit target >= 44pt wide");
            // Border-ring stroke pin (2026-08-20 "a tiny bit thicker"): the
            // lamp draws the shared constant (asserted in mod_matrix_ui_test
            // too — one value, two pins).
            check (ParvatiModuleLamp::kLampBorderWidth >= 2.0f
                       && ParvatiModuleLamp::kLampBorderWidth <= 3.0f,
                   "lamp border ring stroke is the slightly-thicker value (2-3pt)");
            const bool before = view.isSlotMuted (0);
            click (lamp);
            const bool after = view.isSlotMuted (0);
            check (before != after, "lamp click toggles the session mute (old M action)");
            // Restore (session-only state; leave the view tidy).
            if (after)
                click (lamp);
        }
    }

    std::printf ("\nWORKSPACE PADDING TEST: %s (%d failures)\n",
                 g_failures ? "FAILURES" : "ALL CHECKS PASSED", g_failures);
    return g_failures == 0;
}
