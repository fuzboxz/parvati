// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MulExportDialog implementation — see MulExportDialog.h.
//
// Copy note: the strategy labels + descriptions are deliberately PLAIN
// LANGUAGE (no solver vocabulary like "largest remainder" or "bitmask") — the
// audience is a musician saving a patch for hardware, not the person who wrote
// the solver. Each label states WHAT happens; the description under the combo
// (always visible — tooltips are invisible on touch) states HOW; the preview
// + summary state the COST.

#include "MulExportDialog.h"

namespace
{
using parvati::mul_export::Strategy;

// ComboBox ids = index + 1 (0 reserved). Order = menu order; the first item
// is the recommended default.
struct StrategyItem
{
    Strategy s;
    const char* label;         // what happens (shown in the combo)
    const char* description;   // how it works (shown under the combo)
};
const StrategyItem kItems[] = {
    { Strategy::Proportional, "Share the voicecards fairly (recommended)",
      "Each part gets voicecards in proportion to how many voices it uses now — the busiest parts keep the most polyphony." },
    { Strategy::EvenSplit, "Give every part the same",
      "Every active part gets an equal number of voicecards, no matter how many voices it requested." },
    { Strategy::Priority, "Let the first parts win",
      "Part 1 keeps as many voices as it can use, then Part 2, and so on — later parts get whatever is left over." },
    { Strategy::MonoFold, "Keep them fat instead of polyphonic",
      "Shares fairly like the first option, but every part that loses voices switches to Mono: all of its voicecards then play each note together (the classic unison character), so nothing sounds thin." },
    { Strategy::ChainSplit, "Use two or more chained Ambikas",
      "Writes one extra file per additional Ambika (\"-2.MUL\", \"-3.MUL\", ...). Connect the units by MIDI, load one file into each, and they play as one big synth — keeping every voice." },
    { Strategy::AsIs, "Keep the current card assignment",
      "Exports the voicecards exactly as assigned on the Patch page and simply leaves the extra voice settings out — nothing is re-arranged." },
};

juce::String toJuceString (const std::string& s) { return juce::String (s); }
}  // namespace

MulExportDialog::MulExportDialog (const parvati::mul_export::Setup& setup,
                                  const std::vector<juce::String>& partNames,
                                  DoneCallback onDone)
    : setup_ (setup), onDone_ (std::move (onDone))
{
    // Preview context: part display names (or the empty fallback -> "Part N").
    for (int p = 0; p < parvati::mul_export::kParts; ++p)
        ctx_.names.push_back ((size_t) p < partNames.size()
                                  ? partNames[(size_t) p].toStdString()
                                  : std::string());

    heading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setText (TRANS ("This setup uses more voices than one Ambika has (6 voicecards).\n"
                             "Choose how to fit it onto the hardware:"),
                      juce::dontSendNotification);
    addAndMakeVisible (heading_);

    strategyCaption_.setText (TRANS ("How to fit it"), juce::dontSendNotification);
    strategyCaption_.setFont (juce::FontOptions (11.0f));
    strategyCaption_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (strategyCaption_);

    for (size_t i = 0; i < sizeof (kItems) / sizeof (kItems[0]); ++i)
        strategyCombo_.addItem (TRANS (kItems[i].label), static_cast<int> (i) + 1);
    strategyCombo_.setSelectedId (1, juce::dontSendNotification);   // recommended default
    strategyCombo_.onChange = [this] { refreshPreview(); };
    strategyCombo_.getProperties().set ("parvatiComboVisualH", 24);
    addAndMakeVisible (strategyCombo_);

    // Always-visible plain-language description of the selected strategy
    // (under the combo — a tooltip would be invisible on touch).
    descriptionLabel_.setFont (juce::FontOptions (12.0f));
    descriptionLabel_.setJustificationType (juce::Justification::topLeft);
    descriptionLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (descriptionLabel_);

    // The outcome in one line (e.g. "Fits on one Ambika. Only 6 of your 24
    // voices will play at once on the hardware.") — the honest cost up front,
    // above the per-part detail.
    summaryLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    summaryLabel_.setJustificationType (juce::Justification::topLeft);
    summaryLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (summaryLabel_);

    previewLabel_.setFont (juce::FontOptions (12.0f));
    previewLabel_.setJustificationType (juce::Justification::topLeft);
    previewLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (previewLabel_);
    refreshPreview();

    saveButton_.onClick = [this]
    {
        if (fired_ || onDone_ == nullptr) return;
        fired_ = true;
        onDone_ (static_cast<int> (kItems[(size_t) (strategyCombo_.getSelectedId() - 1)].s));
    };
    addAndMakeVisible (saveButton_);
    cancelButton_.onClick = [this]
    {
        if (fired_ || onDone_ == nullptr) return;
        fired_ = true;
        onDone_ (-1);
    };
    addAndMakeVisible (cancelButton_);
}

void MulExportDialog::paint (juce::Graphics&) {}

void MulExportDialog::resized()
{
    auto b = getLocalBounds().reduced (16);
    heading_.setBounds (b.removeFromTop (52));
    b.removeFromTop (10);
    {
        auto row = b.removeFromTop (44);
        strategyCaption_.setBounds (row.removeFromLeft (70).removeFromTop (14));
        strategyCombo_.setBounds (row.withSizeKeepingCentre (row.getWidth(), 24));
    }
    descriptionLabel_.setBounds (b.removeFromTop (58));
    b.removeFromTop (8);
    summaryLabel_.setBounds (b.removeFromTop (40));
    b.removeFromTop (6);
    previewLabel_.setBounds (b.removeFromTop (std::max (110, b.getHeight() - 40)));
    b.removeFromTop (8);
    {
        auto row = b.removeFromTop (30);
        cancelButton_.setBounds (row.removeFromRight (110));
        row.removeFromRight (10);
        saveButton_.setBounds (row.removeFromRight (110));
    }
}

void MulExportDialog::refreshPreview()
{
    using namespace parvati::mul_export;
    const int idx = strategyCombo_.getSelectedId() - 1;
    if (idx < 0) return;
    const auto& item = kItems[(size_t) idx];
    const auto strat = item.s;

    descriptionLabel_.setText (TRANS (item.description), juce::dontSendNotification);
    summaryLabel_.setText (TRANS (toJuceString (summarize (setup_, strat))),
                           juce::dontSendNotification);

    juce::String text;
    if (strat == Strategy::ChainSplit)
    {
        const auto units = solveChain (setup_);
        if (units.size() > 1)
        {
            text << TRANS ("Voicecards per unit") << ":\n";
            for (size_t u = 0; u < units.size(); ++u)
            {
                text << "\n" << TRANS ("Ambika") << " " << (u + 1) << " (\""
                     << (u == 0 ? "-.MUL" : "-" + juce::String (u + 1) + ".MUL") << "\"):\n";
                for (const auto& line : previewLines (setup_, units[u], (int) u, &ctx_))
                    text << "  " << toJuceString (line) << "\n";
            }
        }
    }
    else
    {
        const auto sol = solve (setup_, strat);
        text << TRANS ("Voicecards per part") << ":\n";
        for (const auto& line : previewLines (setup_, sol, 0, &ctx_))
            text << "  " << toJuceString (line) << "\n";
    }
    previewLabel_.setText (text, juce::dontSendNotification);
}

void MulExportDialog::launch (juce::Component* parent,
                              const parvati::mul_export::Setup& setup,
                              const std::vector<juce::String>& partNames,
                              DoneCallback onDone)
{
    juce::ignoreUnused (parent);   // centring needs a live peer; launchAsync is fine headless+GUI
    auto* content = new MulExportDialog (setup, partNames, std::move (onDone));
    content->setSize (520, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.set (content, true);
    o.dialogTitle = TRANS ("Export to Ambika");
    o.dialogBackgroundColour = juce::Colour (0xff191919);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;
    o.launchAsync();
}
