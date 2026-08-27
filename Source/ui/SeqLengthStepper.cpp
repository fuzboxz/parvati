// Copyright (c) 2026 805Labs Kft. / Hellcat.  See SeqLengthStepper.h.

#include "ui/SeqLengthStepper.h"

#include "ui/HellcatLookAndFeel.h"   // HellcatLookAndFeel::getTheme (number-label colours)
#include "ui/HellcatTheme.h"          // HellcatTheme tokens

SeqLengthStepper::SeqLengthStepper (HellcatAudioProcessor& processor,
                                    const PatchParamDescriptor& descriptor)
    : ParamControl (processor, descriptor)
{
    // The base ctor created the 1..16 slider + SliderAttachment (the value
    // backing) and the bold "Length" label. Hide the slider and overlay the
    // big number; TAPPING THE NUMBER opens a 44pt-row 1..16 picker. The
    // attachment stays alive so undo / host automation / preset load all flow
    // through the normal slider->param path.
    if (slider_ != nullptr)
    {
        slider_->setVisible (false);
        // The hidden slider still carries the value; mirror it to the number
        // label whenever it changes (picker pick OR external param write).
        slider_->onValueChange = [this] { refreshNumberLabel(); };
    }

    // F-ios-touch-2 (bug hunt 2026-08-19): the old − / + button pair rendered
    // at ~32x20 inside the 72x64 sequencer grid cell — far below the 44pt HIG
    // floor, and two 44x44 buttons provably cannot fit that cell (the prior
    // audit's STOPPED item T9a). Replacement interaction (the audit's own
    // option list): the NUMBER is the control — a full-cell tappable target —
    // opening a picker popup of 44pt rows (the T7 idiom). Keyboard up/down
    // remains for desktop (keyPressed below).
    numberLabel_ = std::make_unique<juce::Label> ("seqLenNum", juce::String());
    numberLabel_->setJustificationType (juce::Justification::centred);
    // 14pt bold — the app control/readout height (getComboBoxFont /
    // getTextButtonFont / popup rows are all 14pt). Was 17pt bold (raised in
    // the iOS wave for touch, reduced 2026-08-20 after user feedback: it was
    // the largest text on the SEQ page, reading oversized next to the 12pt
    // knob labels and ~12-14pt knob readouts). Bold is retained — the lone
    // numeral is the cell's value readout on the bright tier.
    numberLabel_->setFont (hellcat::dataFontExactFor (*this, 14.0f, juce::Font::bold));
    numberLabel_->setInterceptsMouseClicks (false, false);   // the CELL is the button
    // VISIBILITY FIX (UI hunt 2026-08-20): the number was INVISIBLE in every
    // theme. resized() gives tapBtn_ the full cell and — being created AFTER
    // the label — it painted its opaque drawButtonBackground fill
    // (TextButton::buttonColourId == backgroundPanel) straight over the
    // number; on Carbon that fill is near-identical to the page fill, so the
    // cell just read as empty. Three independent defences, each pinned by
    // hellcat_seq_stepper_test: (1) the button is added BEFORE the label so
    // it can never overpaint it, (2) the label is always-on-top (it does not
    // intercept mouse clicks, so taps still reach the button beneath), and
    // (3) the button's fill colours are fully transparent so its background
    // can never occlude anything no matter the stacking order.
    numberLabel_->setAlwaysOnTop (true);

    tapBtn_ = std::make_unique<juce::TextButton> (juce::String(), TRANS ("Set sequence length"));
    tapBtn_->setBounds ({});   // logic-only: the whole cell IS its hit band
    tapBtn_->setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
    tapBtn_->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x00000000));
    tapBtn_->addListener (this);
    // Button FIRST, number SECOND: the later sibling paints above (defence 1).
    addAndMakeVisible (*tapBtn_);
    addAndMakeVisible (*numberLabel_);

    applyNumberLabelStyle();   // no-op until a themed L&F is reachable
    refreshNumberLabel();
}

void SeqLengthStepper::buttonClicked (juce::Button*)
{
    showLengthPopup();
}

bool SeqLengthStepper::keyPressed (const juce::KeyPress& key)
{
    // Desktop keyboard parity with the old − / + buttons: up/down (and the
    // legacy +/- keys) nudge the length; every other key falls through.
    int delta = 0;
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey)  delta = +1;
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey) delta = -1;
    if (key.getTextCharacter() == '+') delta = +1;
    if (key.getTextCharacter() == '-') delta = -1;
    if (delta == 0)
        return ParamControl::keyPressed (key);
    nudge (delta);
    return true;
}

void SeqLengthStepper::nudge (int delta)
{
    if (slider_ == nullptr)
        return;
    // Bracket each step as its own undo step (programmatic setValue otherwise
    // coalesces a burst into one undo entry). Mirrors resetToDefault /
    // randomize (PluginEditor.cpp) and NoteStepControl's drag transactions.
    processor_.getUndoManager().beginNewTransaction();
    const int cur = juce::roundToInt (slider_->getValue());
    const int next = juce::jlimit (1, 16, cur + delta);
    // SYNC notification: this seam is message-thread-only by construction
    // (a picker item action / a key event), and the value must be visible to
    // the immediate caller (tests read the param synchronously).
    slider_->setValue (static_cast<double> (next), juce::sendNotificationSync);
}

void SeqLengthStepper::setValue (int v)
{
    if (slider_ == nullptr)
        return;
    processor_.getUndoManager().beginNewTransaction();
    slider_->setValue (static_cast<double> (juce::jlimit (1, 16, v)),
                       juce::sendNotificationSync);   // message-thread seam (see nudge)
}

void SeqLengthStepper::showLengthPopup()
{
    // F-ios-touch-2: 44pt-row picker (T7 idiom). SafePointer-guarded actions;
    // explicit setLookAndFeel so the popup is themed (PopupMenu L&F comes only
    // from setLookAndFeel — withTargetComponent does NOT theme it).
    juce::PopupMenu m;
    m.setLookAndFeel (&getLookAndFeel());
    const int current = slider_ != nullptr ? juce::roundToInt (slider_->getValue()) : 1;
    for (int v = 1; v <= 16; ++v)
    {
        juce::PopupMenu::Item item;
        item.text      = juce::String (v);
        item.itemID    = v;
        item.isTicked  = (v == current);
        juce::Component::SafePointer<SeqLengthStepper> safe { this };
        item.action    = [safe, v] { if (safe != nullptr) safe->setValue (v); };
        m.addItem (std::move (item));
    }
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (this)
                         .withStandardItemHeight (kPopupRowHeight));
}

void SeqLengthStepper::refreshNumberLabel()
{
    if (numberLabel_ == nullptr || slider_ == nullptr)
        return;
    numberLabel_->setText (juce::String (juce::roundToInt (slider_->getValue())),
                           juce::dontSendNotification);
}

void SeqLengthStepper::applyNumberLabelStyle()
{
    if (numberLabel_ == nullptr)
        return;
    if (const HellcatTheme* t = hellcat::themeFor (*this))
        numberLabel_->setColour (juce::Label::textColourId, t->textPrimary);
}

void SeqLengthStepper::lookAndFeelChanged()
{
    ParamControl::lookAndFeelChanged();   // category arc / mod tint / ring
    applyNumberLabelStyle();
}

void SeqLengthStepper::parentHierarchyChanged()
{
    ParamControl::parentHierarchyChanged();
    applyNumberLabelStyle();
}

juce::Rectangle<int> SeqLengthStepper::comboFieldRectForTest() const { return controlBand(); }

void SeqLengthStepper::paint (juce::Graphics& g)
{
    // Base first: the bold "Length" caption + the category arc / mod tint.
    ParamControl::paint (g);

    // ---- DROPDOWN AFFORDANCE (2026-08-23, see the header) ----
    // The app combo chrome — the same dark rounded field + right ▼ chevron
    // HellcatLookAndFeel::drawComboBox draws — painted behind the number so
    // the cell reads as a dropdown that opens the 1..16 picker. Drawn HERE
    // (component paint) rather than on the tap button so the three pinned
    // visibility defences stay untouched: the button keeps its fully
    // transparent fill and the always-on-top label still paints above this
    // field. The number label is inset from the field's right edge in
    // resized() so the centred digit clears the chevron.
    const auto field = controlBand();
    if (field.getWidth() < 24 || field.getHeight() < 14)
        return;   // degenerate cell (host squeeze): keep the bare number

    const HellcatTheme* t = hellcat::themeFor (*this);
    const juce::Colour baseFill = (t != nullptr && t->isDark)
                                    ? t->backgroundBase
                                    : juce::Colour (0xff2A2E35);   // light-theme dark-dropdown tone
    const auto fill = isMouseOver() ? baseFill.brighter (0.06f) : baseFill;
    g.setColour (fill);
    g.fillRoundedRectangle (field.toFloat(), 5.0f);

    // Crisp ▼ chevron, right-aligned — the exact geometry + tokens of
    // drawComboBox (chevronSize 5, light token) so the affordance is visually
    // identical to every real dropdown on the page.
    const auto chevronCol = (t != nullptr && t->isDark) ? t->textPrimary
                                                        : juce::Colour (0xfff6f6fa);
    const float cy = static_cast<float> (field.getCentreY()) - 0.5f;
    const float cx = static_cast<float> (field.getRight()) - 10.0f;
    constexpr float s = 5.0f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - s, cy - s * 0.5f);
    chevron.lineTo (cx + s, cy - s * 0.5f);
    chevron.lineTo (cx, cy + s * 0.5f);
    chevron.closeSubPath();
    g.setColour (chevronCol);
    g.fillPath (chevron);
}

void SeqLengthStepper::resized()
{
    // Base lays out the bold "Length" label + the (hidden) slider bounds; the
    // whole remaining cell is the tappable number target (F-ios-touch-2: one
    // >=44pt target instead of two sub-44 buttons).
    ParamControl::resized();

    const auto b = controlBand();

    // The tap button covers the FULL band (the hit target == the visual
    // field); the number label is inset on the right by the chevron's reserve
    // (~14pt, matching the combo text inset idiom) so the centred digit never
    // collides with the ▼.
    if (tapBtn_ != nullptr)
        tapBtn_->setBounds (b);
    if (numberLabel_ != nullptr)
        numberLabel_->setBounds (b.withTrimmedRight (b.getWidth() > 48 ? 14 : 0));
}
