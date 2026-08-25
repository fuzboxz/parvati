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
//
// Touch note (iPadOS): every interactive target is a 44pt band (HIG minimum,
// same floor as the rest of the editor): the strategy combo's BOUNDS fill a
// 44pt row while the L&F's "parvatiComboVisualH" property draws the compact
// 24pt box inside it (the Patch-page pattern), the popup rows use the shared
// kPopupRowHeight (44), and Save/Cancel are 44pt tall. The preview text lives
// in a Viewport so long chain summaries SCROLL (touch-drag) instead of
// clipping. Esc-less touch dismissal = the Cancel button.

#include "MulExportDialog.h"

#include "ParvatiLookAndFeel.h"

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
      "Each chained Ambika after the first gets one extra file (\"-2.MUL\", \"-3.MUL\", ...). Connect the units by MIDI. Load one file into each unit. The units then play as one big synth. Every voice stays." },
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

    heading_.setFont (parvati::headerFontExactFor (*this, 15.0f));
    heading_.setJustificationType (juce::Justification::centredLeft);
    // Two single-line TRANS fragments (not one \n literal): the tables are
    // LocalisedStrings line-parsed, so a raw newline inside a key can never
    // hit a table row — the same suffix-key idiom the FX cards use.
    heading_.setText (TRANS ("This setup uses more voices than one Ambika has (6 voicecards).")
                           + "\n"
                           + TRANS ("Choose how to fit it onto the hardware:"),
                      juce::dontSendNotification);
    addAndMakeVisible (heading_);

    strategyCaption_.setText (TRANS ("How to fit it"), juce::dontSendNotification);
    strategyCaption_.setFont (parvati::labelFontExactFor (*this, 11.0f));
    strategyCaption_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (strategyCaption_);

    // HIG touch: the combo's BOUNDS are the 44pt tap band; the L&F draws the
    // compact 24pt visual box inside it (see resized + "parvatiComboVisualH").
    for (size_t i = 0; i < sizeof (kItems) / sizeof (kItems[0]); ++i)
        strategyCombo_.addItem (TRANS (kItems[i].label), static_cast<int> (i) + 1);
    strategyCombo_.setSelectedId (1, juce::dontSendNotification);   // recommended default
    // Popup rows: 44pt via the inherited ParvatiLookAndFeel's
    // getIdealPopupMenuItemSize override (menus without an explicit
    // standardItemHeight are raised to the HIG floor) — see launch().
    strategyCombo_.onChange = [this] { refreshPreview(); };
    strategyCombo_.getProperties().set ("parvatiComboVisualH", 24);
    addAndMakeVisible (strategyCombo_);

    // Always-visible plain-language description of the selected strategy
    // (under the combo — a tooltip would be invisible on touch).
    descriptionLabel_.setFont (parvati::labelFontExactFor (*this, 12.0f));
    descriptionLabel_.setJustificationType (juce::Justification::topLeft);
    descriptionLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (descriptionLabel_);

    // The outcome in one line (e.g. "Fits on one Ambika. Only 6 of your 24
    // voices will play at once on the hardware.") — the honest cost up front,
    // above the per-part detail.
    summaryLabel_.setFont (parvati::dataFontExactFor (*this, 13.0f, juce::Font::bold));
    summaryLabel_.setJustificationType (juce::Justification::topLeft);
    summaryLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (summaryLabel_);

    previewLabel_.setFont (parvati::dataFontExactFor (*this, 12.0f));
    previewLabel_.setJustificationType (juce::Justification::topLeft);
    previewLabel_.setMinimumHorizontalScale (1.0f);
    // The preview scrolls inside a Viewport (a 4-unit chain summary far
    // exceeds the fixed area): touch-drag to scroll, desktop wheel too.
    previewViewport_.setViewedComponent (&previewLabel_, false);
    previewViewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (previewViewport_);
    refreshPreview();

    // HIG touch: 44pt buttons.
    saveButton_.setButtonText (TRANS ("Save"));
    saveButton_.onClick = [this]
    {
        if (fired_ || onDone_ == nullptr) return;
        fired_ = true;
        onDone_ (static_cast<int> (kItems[(size_t) (strategyCombo_.getSelectedId() - 1)].s));
    };
    addAndMakeVisible (saveButton_);
    cancelButton_.setButtonText (TRANS ("Cancel"));
    cancelButton_.onClick = [this]
    {
        if (fired_ || onDone_ == nullptr) return;
        fired_ = true;
        onDone_ (-1);
    };
    addAndMakeVisible (cancelButton_);
}

MulExportDialog::~MulExportDialog()
{
    // F-ui-2: drop the OWNED LookAndFeel reference BEFORE the unique_ptr
    // member is destroyed (a lingering raw L&F pointer would dangle into the
    // member-destruction window).
    if (ownedLnf_ != nullptr)
        setLookAndFeel (nullptr);
    previewViewport_.setViewedComponent (nullptr, false);
}

void MulExportDialog::paint (juce::Graphics&) {}

void MulExportDialog::resized()
{
    auto b = getLocalBounds().reduced (16);
    heading_.setBounds (b.removeFromTop (52));
    b.removeFromTop (10);
    {
        // 44pt tap band; the combo fills it (the L&F draws the 24pt visual).
        auto row = b.removeFromTop (44);
        strategyCaption_.setBounds (row.removeFromLeft (70).removeFromTop (14));
        strategyCombo_.setBounds (row);
    }
    descriptionLabel_.setBounds (b.removeFromTop (58));
    b.removeFromTop (8);
    summaryLabel_.setBounds (b.removeFromTop (40));
    b.removeFromTop (6);
    previewViewport_.setBounds (b.removeFromTop (b.getHeight() - 52));
    // The label is sized to its content so the viewport can scroll it.
    previewLabel_.setSize (previewViewport_.getWidth() - 8,
                           juce::jmax (previewViewport_.getHeight() - 8,
                                       (int) (previewLabel_.getFont().getHeight() * 1.4f)
                                           * previewLineCount_));
    b.removeFromTop (8);
    {
        auto row = b.removeFromTop (44);
        cancelButton_.setBounds (row.removeFromRight (130));
        row.removeFromRight (10);
        saveButton_.setBounds (row.removeFromRight (130));
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
                // Unit 1's file IS the file being saved (no "-1" suffix is
                // written — the siblings are -2, -3, ...); label it "(this
                // file)" instead of the old literal "-.MUL" placeholder.
                text << "\n" << TRANS ("Ambika") << " " << (u + 1) << " (\""
                     << (u == 0 ? TRANS ("(this file)") : "-" + juce::String (u + 1) + ".MUL") << "\"):\n";
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
    previewLineCount_ = text.length() - text.replace ("\n", "").length() + 1;
    resized();   // re-fit the scrolled label to the new content
}

void MulExportDialog::launch (juce::Component* parent,
                              const parvati::mul_export::Setup& setup,
                              const std::vector<juce::String>& partNames,
                              DoneCallback onDone)
{
    auto* content = new MulExportDialog (setup, partNames, std::move (onDone));

    // Theme: the dialog OWNS a ParvatiLookAndFeel copied from the launching
    // editor's active theme (F-ui-2, bug hunt 2026-08-18). The DialogWindow is
    // its own desktop window and CAN OUTLIVE the editor (host closes the
    // plugin window mid-dialog) — borrowing &parent->getLookAndFeel() painted
    // through freed memory in that window. The owned copy keeps the themed
    // look while staying editor-independent (the builtin theme structs are
    // immortal function-local statics). Null parent
    // (tests) keeps the default look.
    if (parent != nullptr)
        if (auto* plnf = dynamic_cast<ParvatiLookAndFeel*> (&parent->getLookAndFeel()))
        {
            content->ownedLnf_ = std::make_unique<ParvatiLookAndFeel>();
            content->ownedLnf_->setTheme (*plnf->getTheme());
            content->setLookAndFeel (content->ownedLnf_.get());
        }

    // Height caps to the usable screen area (an AUv3 pane can be shorter than
    // the natural dialog height; the preview viewport absorbs the shortfall).
    // The display can be null in headless/automation contexts — fall back to
    // the un-capped height rather than dereference null (W7).
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const int maxH = juce::jlimit (360, 600,
        display != nullptr
            ? static_cast<int> (display->userBounds.getHeight() * 0.85)
            : 600);
    content->setSize (540, juce::jmin (500, maxH));

    juce::DialogWindow::LaunchOptions o;
    o.content.set (content, true);
    o.dialogTitle = TRANS ("Export to Ambika");
    o.dialogBackgroundColour = content->findColour (juce::DocumentWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;
    o.launchAsync();
}
