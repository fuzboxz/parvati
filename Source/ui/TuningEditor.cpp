// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// TuningEditor implementation — see TuningEditor.h for the design summary.

#include "TuningEditor.h"

#include "ScalaImport.h"
#include "TuningTables.h"

#include <cstdint>

namespace
{
// Note-class labels, index = rawNote % 12 (the table's index domain).
const char* const kNoteClassNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// HIG touch minimum for every interactive row band (the same floor as the
// rest of the editor — see tests/ipad_hig_sizing_test.cpp).
constexpr int kRowHeight = 44;

// units -> quantized cents readout ("+15.63ct"). 1 unit = 100/128 ¢ (the
// hardware's 1/128-semitone pitch quantum — the readout shows exactly what
// the engine will do, never a finer promise).
juce::String unitsToCentsText (int units)
{
    if (units == (int) parvati::kTuningSilence)
        return juce::String::charToString ((juce::juce_wchar) 0x2014);   // em dash —
    const double ct = units * 100.0 / 128.0;
    return (ct > 0.0 ? "+" : juce::String()) + juce::String (ct, 2) + "ct";
}
}  // namespace

//==============================================================================
// One note-class row: [class label][linear drag control][cents readout].
// The drag control is a borderless horizontal slider (NoTextBox — the readout
// label IS the value box) with 1-unit steps over -127..127 and a
// double-click-to-zero affordance. A muted row (sentinel) shows "—" and keeps
// the mute until the user actually drags it.
class TuningEditor::Row : public juce::Component
{
public:
    Row (TuningEditor& owner, int noteClass)
        : owner_ (owner), noteClass_ (noteClass)
    {
        classLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        classLabel_.setJustificationType (juce::Justification::centredRight);
        classLabel_.setText (TRANS (kNoteClassNames[(size_t) noteClass_]),
                             juce::dontSendNotification);
        addAndMakeVisible (classLabel_);

        // Linear drag, no text box: the row's readout label shows the value
        // (quantized cents — the honest hardware resolution). 1-unit steps;
        // double-click snaps the class back to 12-EDO (0).
        slider_.setSliderStyle (juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider_.setRange (-127.0, 127.0, 1.0);
        slider_.setDoubleClickReturnValue (true, 0.0);
        // T4 sibling of the Patch page rows: a touch drag on the control must
        // not also scroll the enclosing rows viewport.
        slider_.setViewportIgnoreDragFlag (true);
        slider_.onValueChange = [this]
        {
            muted_ = false;   // a real edit replaces an imported mute
            owner_.applyTable();
        };
        addAndMakeVisible (slider_);

        readoutLabel_.setFont (juce::FontOptions (13.0f));
        readoutLabel_.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (readoutLabel_);
    }

    // Fill from a table entry (sentinel -> muted row, control at 0).
    void setFromUnits (int units)
    {
        muted_ = units == (int) parvati::kTuningSilence;
        slider_.setValue (muted_ ? 0.0 : (double) units, juce::dontSendNotification);
        updateReadout();
    }

    // The row's stored units (sentinel while muted and untouched).
    int units() const
    {
        return muted_ ? (int) parvati::kTuningSilence
                      : juce::roundToInt (slider_.getValue());
    }

    void updateReadout()
    {
        readoutLabel_.setText (unitsToCentsText (units()), juce::dontSendNotification);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        // Full-height 44pt band: the slider's bounds ARE the touch target
        // (HIG); the drawn knob travels inside it.
        classLabel_.setBounds (b.removeFromLeft (40));
        b.removeFromLeft (6);
        readoutLabel_.setBounds (b.removeFromRight (88));
        b.removeFromRight (6);
        slider_.setBounds (b);
    }

private:
    TuningEditor& owner_;
    const int noteClass_;
    bool muted_ = false;
    juce::Label classLabel_;
    juce::Label readoutLabel_;
    juce::Slider slider_;
};

//==============================================================================
TuningEditor::TuningEditor (SynthEngine& engine, int partIndex, ChangeCallback onChanged)
    : engine_ (engine), partIndex_ (partIndex), onChanged_ (std::move (onChanged))
{
    // Heading: "Tuning — Part N" (+ the user name when set). The part's
    // currently RESOLVED table prefills the rows, so "Custom…" opened on top
    // of a raga preset starts from that preset's offsets (tweak-into-custom).
    juce::String title = TRANS ("Tuning") + " \xE2\x80\x94 " + TRANS ("Part") + " "
                       + juce::String (partIndex_ + 1);
    const auto name = engine_.getPartName (partIndex_);
    if (name.isNotEmpty())
        title << " \xC2\xB7 " << name;   // "·" separator
    heading_.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setText (title, juce::dontSendNotification);
    addAndMakeVisible (heading_);

    hintLabel_.setFont (juce::FontOptions (11.0f));
    hintLabel_.setJustificationType (juce::Justification::centredLeft);
    hintLabel_.setText (TRANS ("Custom scale, per note class. Steps of 1/128 semitone (\xE2\x89\x88 0.78 \xC2\xA2). "
                               "Double-click a row to reset it."),
                        juce::dontSendNotification);
    addAndMakeVisible (hintLabel_);

    for (int c = 0; c < 12; ++c)
    {
        rows_[(size_t) c] = std::make_unique<Row> (*this, c);
        rowsHost_.addAndMakeVisible (*rows_[(size_t) c]);
    }
    int16_t prefill[12] = {};
    engine_.resolveTuningOffsets (partIndex_, prefill);
    for (int c = 0; c < 12; ++c)
        rows_[(size_t) c]->setFromUnits ((int) prefill[c]);

    // Rows scroll vertically in short AUv3 panes (the MulExportDialog preview
    // pattern); at the natural height they all fit and no scrollbar appears.
    rowsViewport_.setScrollBarsShown (true, false, false, false);
    rowsViewport_.setViewedComponent (&rowsHost_, false);   // member-owned
    addAndMakeVisible (rowsViewport_);

    messageLabel_.setFont (juce::FontOptions (12.0f));
    messageLabel_.setJustificationType (juce::Justification::topLeft);
    messageLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (messageLabel_);

    errorLabel_.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    errorLabel_.setJustificationType (juce::Justification::topLeft);
    errorLabel_.setMinimumHorizontalScale (1.0f);
    errorLabel_.setColour (juce::Label::textColourId, juce::Colours::orangered);
    addAndMakeVisible (errorLabel_);

    // HIG touch: 44pt buttons.
    importButton_.setButtonText (TRANS ("Import .scl/.kbm\xE2\x80\xA6"));
    importButton_.onClick = [this]
    {
        chooser_ = std::make_unique<juce::FileChooser> (
            TRANS ("Import Scala tuning"),
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.scl;*.kbm");
        constexpr auto flags = juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectMultipleItems;
        // The chooser member keeps the FileChooser alive for the async
        // callback and is released only AFTER fc is fully consumed (the same
        // lifetime pattern as ParvatiEditor's save/load choosers).
        chooser_->launchAsync (flags, [this] (const juce::FileChooser& fc)
        {
            const auto results = fc.getResults();
            if (results.isEmpty())
            {
                chooser_ = nullptr;   // cancelled — table + engine untouched
                return;
            }
            // Exactly one .scl (+ optionally one .kbm), order-independent.
            const juce::File* scl = nullptr;
            juce::String kbmText;
            auto badSelection = [this]
            {
                showMessage (TRANS ("Select exactly one .scl file (plus optionally one .kbm)."), true);
                chooser_ = nullptr;
            };
            for (const auto& f : results)
            {
                if (f.hasFileExtension (".scl"))
                {
                    if (scl != nullptr)
                    {
                        badSelection();
                        return;
                    }
                    scl = &f;
                }
                else if (f.hasFileExtension (".kbm"))
                {
                    if (kbmText.isNotEmpty())
                    {
                        badSelection();
                        return;
                    }
                    kbmText = f.loadFileAsString();
                }
            }
            if (scl == nullptr)
            {
                badSelection();
                return;
            }
            // An omitted .kbm keeps the Scala defaults (see ScalaImport.h).
            const auto sclText = scl->loadFileAsString();
            applyScalaText (sclText, kbmText);
            chooser_ = nullptr;   // release only after fc is fully consumed
        });
    };
    addAndMakeVisible (importButton_);

    clearButton_.setButtonText (TRANS ("Clear"));
    clearButton_.onClick = [this] { onClearClicked(); };
    addAndMakeVisible (clearButton_);

    doneButton_.setButtonText (TRANS ("Done"));
    doneButton_.onClick = [this] { onDoneClicked(); };
    addAndMakeVisible (doneButton_);

    clearMessage();
    setSize (560, 700);
}

TuningEditor::~TuningEditor()
{
    rowsViewport_.setViewedComponent (nullptr, false);
}

void TuningEditor::paint (juce::Graphics&) {}

void TuningEditor::resized()
{
    auto b = getLocalBounds().reduced (16);

    heading_.setBounds (b.removeFromTop (26));
    hintLabel_.setBounds (b.removeFromTop (18));
    b.removeFromTop (8);

    // Message band: fixed height so appearing/disappearing warnings never
    // reflow the rows above them (the band simply shows empty text).
    messageLabel_.setBounds (b.removeFromTop (64));
    errorLabel_.setBounds (b.removeFromTop (20));
    b.removeFromTop (6);

    // Rows fill the remaining space minus the footer; the viewport scrolls
    // whenever the pane is too short for all 12 rows.
    b.removeFromBottom (44 + 8);
    rowsViewport_.setBounds (b);
    rowsHost_.setSize (rowsViewport_.getWidth() - rowsViewport_.getScrollBarThickness(),
                       12 * kRowHeight);
    for (int c = 0; c < 12; ++c)
        rows_[(size_t) c]->setBounds (0, c * kRowHeight, rowsHost_.getWidth(), kRowHeight);

    {
        auto footer = getLocalBounds().reduced (16).removeFromBottom (44);
        doneButton_.setBounds (footer.removeFromRight (110));
        footer.removeFromRight (10);
        clearButton_.setBounds (footer.removeFromRight (110));
        footer.removeFromRight (10);
        importButton_.setBounds (footer.removeFromLeft (180));
    }
}

void TuningEditor::applyTable()
{
    int16_t table[12] = {};
    for (int c = 0; c < 12; ++c)
    {
        const int u = rows_[(size_t) c]->units();
        table[c] = (u == (int) parvati::kTuningSilence)
                       ? parvati::kTuningSilence
                       : static_cast<int16_t> (juce::jlimit (-127, 127, u));
    }
    engine_.setPartTuningCustom (partIndex_, table);
    for (auto& r : rows_)
        r->updateReadout();
    if (onChanged_)
        onChanged_();
}

void TuningEditor::applyScalaText (const juce::String& sclText, const juce::String& kbmText)
{
    const auto result = parvati::importScala (sclText, kbmText);
    if (! result.ok)
    {
        // Inline error; the table (and the engine) stay untouched.
        showMessage (result.error, true);
        return;
    }
    clearMessage();
    for (int c = 0; c < 12; ++c)
        rows_[(size_t) c]->setFromUnits ((int) result.offsets[c]);
    if (! result.warnings.isEmpty())
        showMessage (result.warnings.joinIntoString ("\n"), false);
    applyTable();
}

void TuningEditor::showMessage (const juce::String& text, bool isError)
{
    if (isError)
    {
        errorLabel_.setText (text, juce::dontSendNotification);
        messageLabel_.setText ("", juce::dontSendNotification);
    }
    else
    {
        messageLabel_.setText (text, juce::dontSendNotification);
        errorLabel_.setText ("", juce::dontSendNotification);
    }
}

void TuningEditor::clearMessage()
{
    messageLabel_.setText ("", juce::dontSendNotification);
    errorLabel_.setText ("", juce::dontSendNotification);
}

void TuningEditor::onClearClicked()
{
    for (auto& r : rows_)
        r->setFromUnits (0);
    clearMessage();
    applyTable();   // stays in Custom mode with a zeroed table (by design)
}

void TuningEditor::onDoneClicked()
{
    // Edits are already applied live; just close the hosting dialog (and do
    // nothing harmful when instantiated directly in a test).
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

void TuningEditor::launch (juce::Component* parent, SynthEngine& engine, int partIndex,
                           ChangeCallback onChanged)
{
    auto* content = new TuningEditor (engine, partIndex, std::move (onChanged));

    // Inherit the launching editor's LookAndFeel so the popover matches the
    // active Parvati theme (the DialogWindow is its own desktop window).
    if (parent != nullptr)
        content->setLookAndFeel (&parent->getLookAndFeel());

    // Height caps to the usable screen area (an AUv3 pane can be shorter than
    // the natural height; the row viewport absorbs the shortfall).
    const int maxH = juce::jlimit (360, 720,
        static_cast<int> (juce::Desktop::getInstance().getDisplays()
                              .getPrimaryDisplay()->userArea.getHeight() * 0.85));
    content->setSize (560, juce::jmin (700, maxH));

    juce::DialogWindow::LaunchOptions o;
    o.content.set (content, true);
    o.dialogTitle = TRANS ("Tuning");
    o.dialogBackgroundColour = content->findColour (juce::DocumentWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
// Test hooks.
int TuningEditor::rowUnits (int noteClass) const
{
    if (noteClass < 0 || noteClass >= 12)
        return 0;
    return rows_[(size_t) noteClass]->units();
}

void TuningEditor::setRowUnitsForTest (int noteClass, int units)
{
    if (noteClass < 0 || noteClass >= 12)
        return;
    rows_[(size_t) noteClass]->setFromUnits (units);
    applyTable();   // the same live-apply path a user drag runs
}

juce::String TuningEditor::rowReadout (int noteClass) const
{
    if (noteClass < 0 || noteClass >= 12)
        return {};
    return unitsToCentsText (rows_[(size_t) noteClass]->units());
}

bool TuningEditor::importScalaForTest (const juce::String& sclText, const juce::String& kbmText)
{
    const auto before = rowUnits (0);
    applyScalaText (sclText, kbmText);
    // Report the parser's ok flag the file-callback path would have acted on;
    // on error the table must be untouched (verify the invariant here so a
    // regression fails loudly in the test, not silently in the UI).
    if (errorLabel_.getText().isNotEmpty() && rowUnits (0) != before)
    {
        jassertfalse;   // an error path must never mutate the table
        return false;
    }
    return errorLabel_.getText().isEmpty();
}

juce::String TuningEditor::messageText() const
{
    // Error takes precedence in the display; expose whichever is shown.
    if (errorLabel_.getText().isNotEmpty())
        return errorLabel_.getText();
    return messageLabel_.getText();
}

void TuningEditor::clearForTest() { onClearClicked(); }
