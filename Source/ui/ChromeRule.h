// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// ChromeRule — the 1px full-width separator rules delimiting the chrome bands
// (below the header / above the status strip / above the on-screen keyboard /
// above the central mod-pill bar). Shared by HellcatEditor and the
// Synth/FxWorkspace middle seams so every rule in the UI is ONE family: same
// colour resolution (the active HellcatTheme's textSecondary via the inherited
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

#include "HellcatLookAndFeel.h"

namespace hellcat
{
// The depth-falloff height beside every rule (bounds reserve 1 + this).
constexpr int kRuleShadowH = 5;

class ChromeRule : public juce::Component
{
public:
    explicit ChromeRule (bool shadowBelow)
        : shadowBelow_ (shadowBelow), dark_ (false), strongestAtLine_ (true)
    {
        setInterceptsMouseClicks (false, false);
    }

    // DARK drop-shadow variant: the falloff is BLACK (a true shadow, ~50%
    // alpha) instead of the rule-colour veil. strongestAtLine picks the depth
    // direction: true = darkest AT the line, fading away (a recessed groove
    // under a raised band); false = darkest at the FAR edge of the falloff
    // (the band above), fading TO the line (a cast shadow from the band).
    ChromeRule (bool shadowBelow, bool darkShadow, bool strongestAtLine)
        : shadowBelow_ (shadowBelow), dark_ (darkShadow), strongestAtLine_ (strongestAtLine)
    {
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        const HellcatTheme* t = hellcat::themeFor (*this);
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
        const juce::Colour veil  = dark_ ? juce::Colours::black : rule;
        const float       vAlpha = dark_ ? 0.50f : 0.35f;
        // The dark variant's LINE is explicitly black (the border); the veil
        // variant keeps the rule colour.
        if (dark_)
            g.setColour (juce::Colours::black);
        juce::ColourGradient grad (veil.withMultipliedAlpha (vAlpha), 0.0f, 0.0f,
                                   veil.withMultipliedAlpha (0.0f),  0.0f, (float) kShadowH,
                                   false);
        if (shadowBelow_)
        {
            g.fillRect (0, 0, w, 1);                       // the rule
            grad.point1 = { 0.0f, strongestAtLine_ ? 1.0f : 1.0f + (float) kShadowH };
            grad.point2 = { 0.0f, strongestAtLine_ ? 1.0f + (float) kShadowH : 1.0f };
            g.setGradientFill (grad);
            g.fillRect (0, 1, w, kShadowH);                // falloff below
        }
        else
        {
            g.fillRect (0, h - 1, w, 1);                   // the rule
            const float lineY = (float) (h - 1);
            const float farY  = (float) (h - 1 - kShadowH);
            grad.point1 = { 0.0f, strongestAtLine_ ? lineY : farY };
            grad.point2 = { 0.0f, strongestAtLine_ ? farY : lineY };
            g.setGradientFill (grad);
            g.fillRect (0, h - 1 - kShadowH, w, kShadowH); // falloff above
        }
    }

private:
    bool shadowBelow_;
    bool dark_;
    bool strongestAtLine_;
};
}  // namespace hellcat
