// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MulExportDialog implementation — see MulExportDialog.h.

#include "MulExportDialog.h"

namespace
{
using parvati::mul_export::Strategy;

// ComboBox ids = strategy value + 1 (0 reserved).
struct StrategyItem { Strategy s; const char* label; const char* tip; };
const StrategyItem kItems[] = {
    { Strategy::Proportional, "Proportional",
      "Split the 6 voicecards by each part's requested voices (largest remainder)." },
    { Strategy::Priority, "Priority",
      "Parts are served in order until the voicecards run out (firmware first-wins)." },
    { Strategy::EvenSplit, "Even Split",
      "Equal voicecards per active part, capped by each part's request." },
    { Strategy::MonoFold, "Mono Fold",
      "Proportional, and parts that lose polyphony fold to Mono (full-card unison character)." },
    { Strategy::ChainSplit, "Chain Split",
      "Write extra \"-2.MUL\" unit files for physically chained Ambikas (CHAIN heads forward overflow)." },
    { Strategy::AsIs, "As-Is (faithful bitmasks)",
      "Write the engine's voicecard bitmasks unchanged; voice slots are ignored." },
};
}  // namespace

MulExportDialog::MulExportDialog (const parvati::mul_export::Setup& setup, DoneCallback onDone)
    : setup_ (setup), onDone_ (std::move (onDone))
{
    heading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setText (TRANS ("This setup uses more voices than the 6 hardware voicecards can express.\n"
                             "Choose how to map the voices for the Ambika export:"),
                      juce::dontSendNotification);
    addAndMakeVisible (heading_);

    strategyCaption_.setText (TRANS ("Strategy"), juce::dontSendNotification);
    strategyCaption_.setFont (juce::FontOptions (11.0f));
    strategyCaption_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (strategyCaption_);

    for (size_t i = 0; i < sizeof (kItems) / sizeof (kItems[0]); ++i)
        strategyCombo_.addItem (TRANS (kItems[i].label), static_cast<int> (i) + 1);
    strategyCombo_.setSelectedId (1, juce::dontSendNotification);   // Proportional default
    strategyCombo_.setTooltip (TRANS ("How the requested voices map onto the 6 hardware voicecards."));
    strategyCombo_.onChange = [this] { refreshPreview(); };
    strategyCombo_.getProperties().set ("parvatiComboVisualH", 24);
    addAndMakeVisible (strategyCombo_);

    previewLabel_.setFont (juce::FontOptions (12.0f));
    previewLabel_.setJustificationType (juce::Justification::topLeft);
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
    b.removeFromTop (10);
    previewLabel_.setBounds (b.removeFromTop (std::max (110, b.getHeight() - 44)));
    b.removeFromTop (10);
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
    const auto strat = kItems[(size_t) idx].s;

    juce::String text;
    if (strat == Strategy::ChainSplit)
    {
        const auto units = solveChain (setup_);
        text << TRANS ("Chain units") << ": " << units.size() << "\n";
        for (size_t u = 0; u < units.size(); ++u)
        {
            text << "\n" << TRANS ("Unit") << " " << (u + 1) << ":\n";
            for (const auto& line : previewLines (setup_, units[u], (int) u))
                text << "  " << line << "\n";
        }
    }
    else
    {
        const auto sol = solve (setup_, strat);
        for (const auto& line : previewLines (setup_, sol))
            text << line << "\n";
    }
    previewLabel_.setText (text, juce::dontSendNotification);
}

void MulExportDialog::launch (juce::Component* parent,
                              const parvati::mul_export::Setup& setup,
                              DoneCallback onDone)
{
    juce::ignoreUnused (parent);   // centring needs a live peer; launchAsync is fine headless+GUI
    auto* content = new MulExportDialog (setup, std::move (onDone));
    content->setSize (460, 420);

    juce::DialogWindow::LaunchOptions o;
    o.content.set (content, true);
    o.dialogTitle = TRANS ("Ambika Multi Export");
    o.dialogBackgroundColour = juce::Colour (0xff191919);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;
    o.launchAsync();
}
