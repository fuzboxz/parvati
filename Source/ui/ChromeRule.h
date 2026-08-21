// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ChromeRule — the 1px full-width separator rules delimiting the chrome bands
// (below the header / above the status strip / above the on-screen keyboard /
// above the central mod-pill bar). Shared by ParvatiEditor and the
// Synth/FxWorkspace middle seams so every rule in the UI is ONE family: same
// colour resolution (the active ParvatiTheme's textSecondary via the inherited
// L&F), same 1px rule + ~5px soft depth falloff.
//
// A dedicated NON-INTERACTIVE child, not a stroke in a paint(): children
// overdraw the parent's own paint, so a painted rule under pageSelector_ /
// overlays is invisible however it is coloured. Add rules LAST in the owner's
// child order so they stay above the content + overlays.
//
// DEPTH SEMANTICS (the family idiom): the shadow falls AWAY from the chrome
// band into the adjacent content — the chrome reads raised, the content
// recessed. shadowBelow=true puts the 1px rule at the TOP of the bounds with
// the falloff below it (e.g. the header rule: header above, workspace below);
// shadowBelow=false puts the rule at the BOTTOM of the bounds with the
// falloff above it (e.g. the status/keyboard rules: the footer / keyboard
// strip below reads raised over the content above it).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParvatiLookAndFeel.h"

namespace parvati
{
// The depth-falloff height beside every rule (bounds reserve 1 + this).
constexpr int kRuleShadowH = 5;

class ChromeRule : public juce::Component
{
public:
    explicit ChromeRule (bool shadowBelow)
        : shadowBelow_ (shadowBelow)
    {
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
        const ParvatiTheme* t = lnf != nullptr ? lnf->getTheme() : nullptr;
        const juce::Colour rule (t != nullptr ? t->textSecondary
                                              : juce::Colours::darkgrey);
        constexpr int kShadowH = kRuleShadowH;
        const int w = getWidth();
        const int h = getHeight();
        if (h <= 1 || w <= 0)
        {
            g.fillAll (rule);   // degenerate (no shadow room): plain rule
            return;
        }
        juce::ColourGradient grad (rule.withMultipliedAlpha (0.35f), 0.0f, 0.0f,
                                   rule.withMultipliedAlpha (0.0f),  0.0f, (float) kShadowH,
                                   false);
        if (shadowBelow_)
        {
            g.fillRect (0, 0, w, 1);                       // the rule
            grad.point1 = { 0.0f, 1.0f };
            grad.point2 = { 0.0f, 1.0f + (float) kShadowH };
            g.setGradientFill (grad);
            g.fillRect (0, 1, w, kShadowH);                // falloff below
        }
        else
        {
            g.fillRect (0, h - 1, w, 1);                   // the rule
            grad.point1 = { 0.0f, (float) (h - 1) };
            grad.point2 = { 0.0f, (float) (h - 1 - kShadowH) };
            g.setGradientFill (grad);
            g.fillRect (0, h - 1 - kShadowH, w, kShadowH); // falloff above
        }
    }

private:
    bool shadowBelow_;
};
}  // namespace parvati
