// Typography consistency + module-header contrast (UI feedback 2026-08-20).
//
// [1] MODULE/SECTION HEADERS on the textPrimary tier: every built-in theme's
//     GroupComponent title colour (via the themed ParvatiLookAndFeel) resolves
//     to theme.textPrimary — NOT the dim textSecondary the titles used before
//     the fix — with WCAG contrast >= 7:1 against BOTH the card fill
//     (containerFill — what drawGroupComponentOutline actually paints under
//     the title) and the panel background, and a measurable margin BRIGHTER
//     than the body-secondary tier (>= 2:1 more contrast than textSecondary
//     offers on the same surfaces, all built-in themes).
// [2] FONT UNIFICATION at the 14pt app-control height: the combo font, the
//     popup-menu (dropdown list) font, the text-button font, and the SEQ
//     length stepper's number are all exactly 14pt (the old popup/stepper
//     were 15/17pt — the user-visible "seq dropdown font too big").
// [3] A live theme switch keeps the title tier (setTheme re-colours the L&F).
//
// Run: ./build_unified/parvati_unified_tests ui_typography_test

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

bool sameHeight (float a, float b) { return std::abs (a - b) < 0.01f; }

juce::Label* findNumberLabel (juce::Component& c)
{
    for (auto* child : c.getChildren())
        if (auto* l = dynamic_cast<juce::Label*> (child))
            if (l->getName() == "seqLenNum")
                return l;
    return nullptr;
}

const PatchParamDescriptor& lengthDescriptor()
{
    for (const auto& d : getPatchParamDescriptors())
        if (juce::String (d.paramID) == "seq_length_1")
            return d;
    jassertfalse;
    return getPatchParamDescriptors().front();
}
}  // namespace

TEST(ui_typography_test)
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
        { "Y2K",          y2kTheme()          },
    };

    // ---- [1] Module-header tier + contrast (the reported dim headers) -------
    std::printf ("[1] Module headers: textPrimary tier + contrast >= 7:1\n");
    {
        ParvatiLookAndFeel lnf;   // defaults to Carbon
        juce::GroupComponent group;
        group.setLookAndFeel (&lnf);

        for (const auto& [name, theme] : themes)
        {
            lnf.setTheme (theme);
            const auto title = group.findColour (juce::GroupComponent::textColourId);

            check (title == theme.textPrimary,
                   (std::string (name) + ": title colour == textPrimary").c_str());
            check (title != theme.textSecondary,
                   (std::string (name) + ": title colour is NOT the dim secondary tier").c_str());

            const double cFill  = contrast (title, theme.containerFill);
            const double cPanel = contrast (title, theme.backgroundPanel);
            std::printf ("    %-9s  vs containerFill %5.2f:1   vs panel %5.2f:1\n",
                         name, cFill, cPanel);
            check (cFill >= 7.0 && cPanel >= 7.0,
                   (std::string (name) + ": title contrast >= 7:1 on card + panel").c_str());

            // Measurably brighter than the body-secondary tier: the title must
            // beat textSecondary's own contrast on the SAME surfaces by >= 2:1.
            const double secFill  = contrast (theme.textSecondary, theme.containerFill);
            const double secPanel = contrast (theme.textSecondary, theme.backgroundPanel);
            check (cFill - secFill >= 2.0 && cPanel - secPanel >= 2.0,
                   (std::string (name) + ": >= 2:1 more contrast than the secondary tier (was "
                    + std::to_string (secFill).substr (0, 4) + ":1)").c_str());

            // Light themes must not regress: they still exceed 7:1 (the
            // per-theme asserts above cover this; printed for the record).
        }

        // [3] a live theme switch keeps the tier (the L&F re-colours).
        lnf.setTheme (carbonTheme());
        const auto before = group.findColour (juce::GroupComponent::textColourId);
        lnf.setTheme (paperTheme());
        const auto after = group.findColour (juce::GroupComponent::textColourId);
        check (before == carbonTheme().textPrimary && after == paperTheme().textPrimary,
               "theme switch re-resolves the title tier on both dark + light");
    }

    // ---- [2] 14pt app-control font unification (the reported big dropdown) --
    std::printf ("\n[2] Font heights unified at the 14pt app-control height\n");
    {
        ParvatiLookAndFeel lnf;
        juce::ComboBox combo;
        juce::TextButton btn;
        combo.setLookAndFeel (&lnf);
        btn.setLookAndFeel (&lnf);

        const float comboH = lnf.getComboBoxFont (combo).getHeight();
        const float popupH = lnf.getPopupMenuFont().getHeight();
        const float btnH   = lnf.getTextButtonFont (btn, 30).getHeight();
        std::printf ("    combo %.1fpt   popup %.1fpt   button %.1fpt\n", comboH, popupH, btnH);

        check (sameHeight (comboH, 14.0f), "combo font is 14pt");
        check (sameHeight (popupH, comboH), "popup-menu (dropdown list) font == combo font");
        check (sameHeight (btnH, comboH), "text-button font == combo font");

        // The SEQ length stepper's number (the visible "seq dropdown" value).
        juce::Component host;
        auto stepper = std::make_unique<SeqLengthStepper> (proc, desc);
        auto* stepperPtr = stepper.get();
        host.addAndMakeVisible (stepper.release());
        host.setLookAndFeel (&lnf);
        stepperPtr->setSize (72, 64);
        if (auto* number = findNumberLabel (*stepperPtr))
        {
            const float fh = number->getFont().getHeight();
            std::printf ("    seq stepper number %.1fpt %s\n", fh,
                         number->getFont().isBold() ? "bold" : "plain");
            check (sameHeight (fh, comboH),
                   "seq stepper number == app control height (14pt; was 17pt)");
        }
        else
            check (false, "seq stepper number label found");
    }

    std::printf ("\nUI TYPOGRAPHY TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}
