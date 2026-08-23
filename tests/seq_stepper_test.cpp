// SeqLengthStepper visibility regression (UI hunt 2026-08-20).
//
// The length NUMBER was invisible in every theme: resized() gives the full-cell
// tap button its bounds and it was created AFTER the number label, so its
// opaque TextButton background fill (backgroundPanel — near-identical to the
// page fill on Carbon) painted straight over the number. This test pins:
//   [1] the occlusion fix (button transparent + added before the label, label
//       always-on-top, label bounds non-empty, hit band >= 44pt),
//   [2] per-theme: the number's colour == theme.textPrimary (the value-readout
//       tier, not the dim caption tier) with WCAG contrast >= 4.5:1 against
//       BOTH backgroundPanel and backgroundBase, font height == the app
//       CONTROL height (14pt, matching combos/buttons/popup rows) and bold,
//   [3] value text updates through the real slider backing (set + keyboard
//       nudge + 1..16 clamp),
//   [4] a live theme switch re-resolves the colour (the lookAndFeelChanged
//       path — the "colour read before theme attach" trap).
//
// Built by default. Run with: ./build/parvati_seq_stepper_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ui/ParvatiLookAndFeel.h"
#include "ui/ParvatiTheme.h"
#include "ui/SeqLengthStepper.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// WCAG 2.x relative luminance of a juce::Colour.
double luminance (const juce::Colour& c)
{
    auto chan = [] (uint8_t v) -> double
    {
        const double s = static_cast<double> (v) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow ((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * chan (c.getRed()) + 0.7152 * chan (c.getGreen())
         + 0.0722 * chan (c.getBlue());
}

double contrast (const juce::Colour& a, const juce::Colour& b)
{
    const double la = luminance (a), lb = luminance (b);
    const double hi = std::max (la, lb), lo = std::min (la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

// The number label (name "seqLenNum") / the tap button, found by child search
// (both are private members; name/type addressing needs no production seams).
juce::Label* findNumberLabel (juce::Component& c)
{
    for (auto* child : c.getChildren())
        if (auto* l = dynamic_cast<juce::Label*> (child))
            if (l->getName() == "seqLenNum")
                return l;
    return nullptr;
}

juce::TextButton* findTapButton (juce::Component& c)
{
    for (auto* child : c.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*> (child))
            return b;
    return nullptr;
}

const PatchParamDescriptor& lengthDescriptor()
{
    for (const auto& d : getPatchParamDescriptors())
        if (juce::String (d.paramID) == "seq_length_1")
            return d;
    // Unreachable: the descriptor table always carries seq_length_{1,2,3}.
    jassertfalse;
    return getPatchParamDescriptors().front();
}

// The sequencer grid cell size (PluginEditor.cpp configureGroupLayouts:
// stepGrid groups get cellW = 72, cellH = 64).
constexpr int kCellW = 72, kCellH = 64;
// The app's CONTROL font height (combos / buttons / popup rows are all 14pt
// since the 2026-08-20 popup unification); the seq number deliberately MATCHES
// it (was 17pt bold — read oversized next to every neighbouring font).
constexpr float kControlFontHeight = 14.0f;
}  // namespace

TEST(seq_stepper_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    ParvatiAudioProcessor proc;
    const auto& desc = lengthDescriptor();

    std::vector<std::pair<const char*, const ParvatiTheme&>> themes {
        { "Carbon",      carbonTheme()      },
        { "Midnight",    midnightTheme()    },
        { "Obsidian",    obsidianTheme()    },
        { "Paper",       paperTheme()       },
        { "Crimson",     crimsonTheme()     },
        {"Immutable",   immutableTheme()      },
        { "Swedish Red", swedishRedTheme()  },
    };

    std::printf ("[1] Occlusion fix (the reported bug)\n");
    {
        SeqLengthStepper stepper (proc, desc);
        stepper.setSize (kCellW, kCellH);

        auto* number = findNumberLabel (stepper);
        auto* tap    = findTapButton (stepper);
        check (number != nullptr, "number label found (child \"seqLenNum\")");
        check (tap    != nullptr, "tap button found (only TextButton child)");
        if (number != nullptr && tap != nullptr)
        {
            // Defence 3: the button's fills are fully transparent — its
            // drawButtonBackground can never occlude the number.
            check (tap->findColour (juce::TextButton::buttonColourId).getAlpha() == 0,
                   "tap buttonColourId is transparent");
            check (tap->findColour (juce::TextButton::buttonOnColourId).getAlpha() == 0,
                   "tap buttonOnColourId is transparent");
            // Defence 2: the label is always-on-top of its siblings.
            check (number->isAlwaysOnTop(), "number label is always-on-top");
            // Defence 1: the button is an EARLIER sibling (added first).
            int numberIdx = -1, tapIdx = -1;
            const auto& siblings = stepper.getChildren();
            for (int i = 0; i < siblings.size(); ++i)
            {
                if (siblings[i] == number) numberIdx = i;
                if (siblings[i] == tap)    tapIdx    = i;
            }
            check (tapIdx < numberIdx, "tap button added before the number label (z-order)");
            // Geometry: the number band is a real, visible rectangle.
            check (! number->getBounds().isEmpty()
                       && number->getWidth() >= 44 && number->getHeight() >= 20,
                   "number bounds are a visible band (>= 44x20)");
            check (tap->getWidth() >= 44 && tap->getHeight() >= 20,
                   "tap hit band spans the cell (>= 44pt target)");

            // DROPDOWN AFFORDANCE (2026-08-23): the painted combo-style field
            // IS the tap band (visual == hit target), and the number label is
            // inset from the field's right edge so the centred digit clears
            // the painted ▼ chevron reserve.
            const auto field = stepper.comboFieldRectForTest();
            check (! field.isEmpty() && field.getWidth() >= 44 && field.getHeight() >= 14,
                   "dropdown affordance field is a real band (>= 44x14)");
            check (field == tap->getBounds(),
                   "affordance field == tap hit band (visual == hit target)");
            if (field.getWidth() > 48)
                check (number->getBounds().getRight() <= field.getRight() - 8,
                       "number label clears the chevron reserve (>= 8pt)");
        }
    }

    std::printf ("\n[2] Per-theme colour tier + contrast + font\n");
    {
        ParvatiLookAndFeel lnf;   // defaults to Carbon
        juce::Component host;
        auto stepper = std::make_unique<SeqLengthStepper> (proc, desc);
        auto* stepperPtr = stepper.get();
        host.addAndMakeVisible (stepper.release());
        if (stepperPtr != nullptr)
        {
            stepperPtr->setSize (kCellW, kCellH);
            host.setLookAndFeel (&lnf);   // fires lookAndFeelChanged on host + children

            auto* number = findNumberLabel (*stepperPtr);
            check (number != nullptr, "number label found under themed L&F");
            if (number != nullptr)
            {
                for (const auto& [name, theme] : themes)
                {
                    lnf.setTheme (theme);
                    host.sendLookAndFeelChange();

                    const auto col = number->findColour (juce::Label::textColourId);
                    const double cPanel = contrast (col, theme.backgroundPanel);
                    const double cBase  = contrast (col, theme.backgroundBase);
                    std::printf ("    %-9s  vs panel %5.2f:1   vs base %5.2f:1\n",
                                 name, cPanel, cBase);
                    check (col == theme.textPrimary,
                           (std::string (name) + ": colour == textPrimary (value tier)").c_str());
                    check (cPanel >= 4.5 && cBase >= 4.5,
                           (std::string (name) + ": WCAG contrast >= 4.5:1 on both surfaces").c_str());

                    const float fh = number->getFont().getHeight();
                    check (fh > 0.0f && std::abs (fh - kControlFontHeight) < 0.01f,
                           (std::string (name) + ": font height == app control height (14)").c_str());
                    check (number->getFont().isBold(), "font is bold (value-tier emphasis)");
                }
                check (std::abs (number->getFont().getHeight() - 14.0f) < 0.01f, "font height is 14pt (app control height)");
            }
        }
        else
            check (false, "stepper constructed under host");
    }

    std::printf ("\n[3] Value text follows the backing slider\n");
    {
        SeqLengthStepper stepper (proc, desc);
        stepper.setSize (kCellW, kCellH);
        auto* number = findNumberLabel (stepper);
        check (number != nullptr, "number label found");
        if (number != nullptr)
        {
            stepper.setValueForTest (7);
            check (number->getText() == "7", "setValue(7) -> label \"7\"");
            stepper.keyPressedForTest (juce::KeyPress (juce::KeyPress::upKey));
            check (number->getText() == "8", "up-key nudge -> label \"8\"");
            stepper.keyPressedForTest (juce::KeyPress (juce::KeyPress::downKey));
            check (number->getText() == "7", "down-key nudge -> label \"7\"");
            stepper.setValueForTest (99);
            check (number->getText() == "16", "setValue(99) clamps -> label \"16\"");
            stepper.setValueForTest (0);
            check (number->getText() == "1", "setValue(0) clamps -> label \"1\"");
        }
    }

    std::printf ("\n[4] Live theme switch re-resolves the colour\n");
    {
        ParvatiLookAndFeel lnf;   // Carbon
        juce::Component host;
        auto stepper = std::make_unique<SeqLengthStepper> (proc, desc);
        auto* stepperPtr = stepper.get();
        host.addAndMakeVisible (stepper.release());
        check (stepperPtr != nullptr, "stepper for switch test");
        if (stepperPtr != nullptr)
        {
            stepperPtr->setSize (kCellW, kCellH);
            host.setLookAndFeel (&lnf);
            auto* number = findNumberLabel (*stepperPtr);
            if (number != nullptr)
            {
                check (number->findColour (juce::Label::textColourId) == carbonTheme().textPrimary,
                       "initial (Carbon) colour is Carbon textPrimary");
                lnf.setTheme (paperTheme());        // dark -> light switch
                host.sendLookAndFeelChange();
                check (number->findColour (juce::Label::textColourId) == paperTheme().textPrimary,
                       "after switch: colour is Paper textPrimary (re-resolved)");
                lnf.setTheme (obsidianTheme());
                host.sendLookAndFeelChange();
                check (number->findColour (juce::Label::textColourId) == obsidianTheme().textPrimary,
                       "after switch: colour is Obsidian textPrimary (re-resolved)");
            }
            else
                check (false, "number label for switch test");
        }
    }
    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
