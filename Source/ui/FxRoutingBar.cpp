// Copyright (c) 2026 805Labs Kft. / Hellcat.  See FxRoutingBar.h.

#include "FxRoutingBar.h"

#include "PluginProcessor.h"   // HellcatAudioProcessor complete type (getApvts)
#include "ThemeManager.h"
#include "HellcatTheme.h"
#include "HellcatLookAndFeel.h"   // dynamic_cast + appFont() in FxFlowDiagram
#include "FxSlotLabels.h"     // fxEqLowToString / fxEqDbToString (hoisted EQ readouts)

//==============================================================================
namespace
{
    // Vertical-column layout constants (px). The bar is now a slim full-height
    // column (column 0 of the 4-column FX top row), not a wide strip.
    constexpr int kPad        = 8;    // card edge inset (2026-08-23 harmonization: the synth ParamPage's kGroupPad — one module-container inset on both pages)
    constexpr float kCorner   = 7.0f; // card panel corner radius (synth GroupComponent parity)
    constexpr int kHeaderH    = 16;   // header band (painted "FX ROUTING" title)
    constexpr int kGap        = 6;    // vertical gap between sections
    constexpr int kLabelH     = 14;   // "Dry/Wet" caption height
    constexpr int kKnobSize   = 52;   // Mix rotary dial (synth-parity)
    constexpr int kFlowRowH   = 50;   // [◀][flow diagram][▶] row height (hosts the 44pt steppers)
    constexpr int kCtrlRowH   = 58;   // [Dry/Wet knob] row height (tightened)
    // [LowCut][Mid][High] EQ knob row. 14pt caption + 2pt gap + the 44pt dial
    // (kEqKnobSize, declared in the header for the HIG test) = 60: the row is
    // tall enough that the dial is no longer clamped by the cell (it was
    // drawing at 36px when the row was 52). At the DEFAULT editor size the
    // ctrl row below stays at its current (starved) allocation — see the
    // resized() comment.
    constexpr int kEqRowH     = 60;   // [LowCut][Mid][High] EQ knob row height

// FX master-EQ / mix readout strings (compact, <=5 chars, no space -> fit the
// 44px EQ dial above the painter's 9px floor). HOISTED into the gui-free
// ui/FxSlotLabels (fxEqLowToString / fxEqDbToString) so the HOST-VISIBLE
// parameter text (ParameterLayout.cpp) shares one implementation with these
// knobs; the lambdas below forward to them.
//   fx_eq_low  0..127 (0=off, else HP 20..1500 Hz) — FxChain.cpp:308-312.
//   fx_eq_mid/high 0..127 (64=unity, +-12 dB)      — FxChain.cpp:319/330.
// The Hz readout uses the synth's electronic-component k-notation ("1k5" for
// 1500 Hz) so the longest string is "999Hz" (5 chars).
}  // namespace

//==============================================================================
// FxFlowDiagram — a compact in->out signal-flow block chart for the 3 FX-chain
// topologies (Series / Parallel 1+2->3 / Parallel 1->2+3). A read-only visual
// that tracks fx_topo LIVE (an APVTS::Listener repaints it on any topology
// change — combo edit, ◀▶ stepper, host automation, preset load). Slot blocks are
// labelled "FX1".."FX3", falling back to the bare digit "1".."3" when a block is
// too narrow for the full label. File-scope (not anonymous) so FxRoutingBar.h's
// forward declaration + unique_ptr<FxFlowDiagram> resolve to this type.
class FxFlowDiagram : public juce::Component,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::AsyncUpdater
{
public:
    FxFlowDiagram (HellcatAudioProcessor& proc, ThemeManager& tm)
        : processor_ (proc), themeManager_ (tm)
    {
        setTitle ("FX signal flow");
        setDescription ("FX chain topology signal-flow diagram");
        processor_.getApvts().addParameterListener ("fx_topo", this);
    }

    ~FxFlowDiagram() override
    {
        processor_.getApvts().removeParameterListener ("fx_topo", this);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = themeManager_.getCurrentTheme();
        const juce::Colour trace   = t.accentSecondary;   // wires + BRIGHT FX-slot borders (FX accent)
        const juce::Colour blockBg = t.containerFill;     // FX-slot block fill (lifts above the card)
        const juce::Colour text    = t.textPrimary;       // FX-slot label
        const juce::Colour dim     = t.textSecondary;     // IN/OUT border + label grey

        constexpr float kBlockH    = 18.0f;
        constexpr float kEndH      = 14.0f;   // IN/OUT endpoint height (smaller — "utility")
        constexpr float kBlockMinW = 22.0f;
        constexpr float kBlockMaxW = 40.0f;
        constexpr float kRound     = 3.0f;
        constexpr float kWire      = 1.5f;
        constexpr float kBorder    = 1.5f;   // bright block-border weight

        const auto frame = getLocalBounds().toFloat();
        const auto plot  = frame.reduced (4.0f);
        const float cy     = plot.getCentreY();
        const float rowOff = kBlockH * 0.85f;              // parallel-branch vertical offset

        const int topo = currentTopoIndex();               // 0=Series 1=Par 1+2->3 2=Par 1->2+3
        // T15 (iPadOS audit): 12/10pt (was 10/8) — the diagram labels were
        // below touch readability. The FX-slot labels already degrade to a
        // bare digit when the block is too narrow, and IN/OUT are sized to
        // their glyphs at these sizes, so nothing overflows its block.
        const juce::Font blockFont (hellcat::labelFontExactFor (*this, 12.0f, juce::Font::bold));
        const juce::Font endFont   (hellcat::labelFontExactFor (*this, 10.0f, juce::Font::bold));   // IN/OUT (smaller utility label)

        auto nodeRect = [&] (float cx, float bcY, float w) -> juce::Rectangle<float>
        {
            return { cx - w * 0.5f, bcY - kBlockH * 0.5f, w, kBlockH };
        };
        // IN / OUT use a SHORTER rect so they read as utility endpoints, not peer
        // slots (centred on the midline, so wires still land at their centre).
        auto endRect = [&] (float cx, float bcY, float w) -> juce::Rectangle<float>
        {
            return { cx - w * 0.5f, bcY - kEndH * 0.5f, w, kEndH };
        };

        // IN / OUT end-blocks (narrower, sized to the "IN"/"OUT" glyphs).
        constexpr float kInW  = 20.0f;
        constexpr float kOutW = 26.0f;
        const auto inBlock  = endRect (plot.getX() + kInW * 0.5f, cy, kInW);
        const auto outBlock = endRect (plot.getRight() - kOutW * 0.5f, cy, kOutW);
        const float midLeft  = inBlock.getRight();
        const float midRight = outBlock.getX();
        const float midW     = midRight - midLeft;

        // FX slot-block rectangles (geometry only; rendered after the wires).
        const float topY = cy - rowOff, botY = cy + rowOff;
        juce::Rectangle<float> fx[3];
        if (topo == 0)                     // Series: IN-FX1-FX2-FX3-OUT with EQUAL gaps
        {
            constexpr float kMinGap = 6.0f;
            const float w   = juce::jlimit (kBlockMinW, kBlockMaxW, (midW - 4.0f * kMinGap) / 3.0f);
            const float gap = juce::jmax (2.0f, (midW - 3.0f * w) * 0.25f);   // equal inter-element gap
            for (int s = 0; s < 3; ++s)
                fx[s] = nodeRect (midLeft + gap + static_cast<float> (s) * (w + gap) + w * 0.5f, cy, w);
        }
        else                               // Parallel: 2 columns
        {
            const float w  = juce::jlimit (kBlockMinW, kBlockMaxW, midW * 0.36f);
            const float c0 = midLeft + midW * 0.30f;
            const float c1 = midLeft + midW * 0.70f;
            if (topo == 1)                 // (1 || 2) -> 3
            {
                fx[0] = nodeRect (c0, topY, w);
                fx[1] = nodeRect (c0, botY, w);
                fx[2] = nodeRect (c1, cy, w);
            }
            else                           // 1 -> (2 || 3)
            {
                fx[0] = nodeRect (c0, cy, w);
                fx[1] = nodeRect (c1, topY, w);
                fx[2] = nodeRect (c1, botY, w);
            }
        }

        auto wire = [&] (float x1, float y1, float x2, float y2)
        {
            g.setColour (trace);
            g.drawLine (x1, y1, x2, y2, kWire);
        };

        // No background frame: the flow renders directly on the card so the
        // diagram reads as lightly as possible. (frame/plot are geometry only.)

        // ---- Wires (drawn before the nodes so the nodes cover the endpoints) ----
        if (topo == 0)                     // Series: IN -> 1 -> 2 -> 3 -> OUT
        {
            wire (inBlock.getRight(), cy, fx[0].getX(), cy);
            wire (fx[0].getRight(), cy, fx[1].getX(), cy);
            wire (fx[1].getRight(), cy, fx[2].getX(), cy);
            wire (fx[2].getRight(), cy, outBlock.getX(), cy);
        }
        else if (topo == 1)                // Parallel (1 || 2) -> 3
        {
            const float j0 = fx[0].getX() - 4.0f;          // split junction (left of the pair)
            const float j1 = fx[0].getRight() + 4.0f;      // merge junction (right of the pair)
            wire (inBlock.getRight(), cy, j0, cy);
            wire (j0, cy, j0, topY);  wire (j0, topY, fx[0].getX(), topY);
            wire (j0, cy, j0, botY);  wire (j0, botY, fx[1].getX(), botY);
            wire (fx[0].getRight(), topY, j1, topY);  wire (j1, topY, j1, cy);
            wire (fx[1].getRight(), botY, j1, botY);  wire (j1, botY, j1, cy);
            wire (j1, cy, fx[2].getX(), cy);
            wire (fx[2].getRight(), cy, outBlock.getX(), cy);
        }
        else                               // Parallel 1 -> (2 || 3)
        {
            const float j0 = fx[0].getRight() + 4.0f;      // split junction (right of slot 1)
            const float j1 = fx[1].getRight() + 4.0f;      // merge junction (right of the pair)
            wire (inBlock.getRight(), cy, fx[0].getX(), cy);
            wire (fx[0].getRight(), cy, j0, cy);
            wire (j0, cy, j0, topY);  wire (j0, topY, fx[1].getX(), topY);
            wire (j0, cy, j0, botY);  wire (j0, botY, fx[2].getX(), botY);
            wire (fx[1].getRight(), topY, j1, topY);  wire (j1, topY, j1, cy);
            wire (fx[2].getRight(), botY, j1, botY);  wire (j1, botY, j1, cy);
            wire (j1, cy, outBlock.getX(), cy);
        }

        // ---- Nodes (drawn over the wire ends). FX slots are BRIGHT (filled +
        //      orange border + white label); the IN/OUT endpoints are MUTED (thin
        //      grey border + grey label, no fill) so they recede. ----
        auto drawNode = [&] (const juce::Rectangle<float>& r, const juce::String& label, const juce::Font& f,
                             juce::Colour border, juce::Colour labelCol, bool filled)
        {
            if (filled)
            {
                g.setColour (blockBg);
                g.fillRoundedRectangle (r, kRound);
            }
            g.setColour (border);
            g.drawRoundedRectangle (r, kRound, filled ? kBorder : 1.0f);
            g.setColour (labelCol);
            g.setFont (f);
            g.drawText (label, r, juce::Justification::centred);
        };

        drawNode (inBlock,  "IN",  endFont, dim, text, false);   // muted-but-visible endpoint (grey border, white label)
        drawNode (outBlock, "OUT", endFont, dim, text, false);
        for (int s = 0; s < 3; ++s)
        {
            const juce::String full  = "FX" + juce::String (s + 1);
            const juce::String label = static_cast<float> (juce::GlyphArrangement::getStringWidthInt (blockFont, full)) > (fx[s].getWidth() - 4.0f)
                                       ? juce::String (s + 1) : full;
            drawNode (fx[s], label, blockFont, trace, text, true);   // bright FX slot
        }
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
    }

private:
    void parameterChanged (const juce::String& id, float) override
    {
        if (id != "fx_topo")
            return;
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm != nullptr && mm->isThisTheMessageThread())
            repaint();
        else
            triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override { repaint(); }

    int currentTopoIndex() const
    {
        // fx_topo is an AudioParameterChoice: getValue() is normalized 0..1
        // across the 3 choices -> scale to 0..2.
        auto* p = processor_.getApvts().getParameter ("fx_topo");
        const float v = p != nullptr ? p->getValue() : 0.0f;
        return juce::jlimit (0, 2, juce::roundToInt (v * 2.0f));
    }

    HellcatAudioProcessor& processor_;
    ThemeManager&          themeManager_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxFlowDiagram)
};

//==============================================================================
FxRoutingBar::FxRoutingBar (HellcatAudioProcessor& processor, ThemeManager& themeManager)
    : processor_ (processor), themeManager_ (themeManager)
{
    // ---- in->out signal-flow block chart (tracks fx_topo live) + ◀ ▶ steppers ----
    flowDiagram_ = std::make_unique<FxFlowDiagram> (processor_, themeManager_);
    addAndMakeVisible (*flowDiagram_);

    prevButton_.setButtonText ("<");
    nextButton_.setButtonText (">");
    prevButton_.setTooltip (TRANS ("Previous FX topology"));
    nextButton_.setTooltip (TRANS ("Next FX topology"));
    addAndMakeVisible (prevButton_);
    addAndMakeVisible (nextButton_);
    prevButton_.onClick = [this] { stepTopology (-1); };
    nextButton_.onClick = [this] { stepTopology (+1); };

    // ---- "Dry/Wet" caption + the global wet/dry knob (synth-style rotary: the
    //      value is drawn centred in the ring by the editor-wide LookAndFeel,
    //      identical to the Mixer / Oscillator knobs). ----
    mixLabel_.setText (TRANS ("Dry/Wet"), juce::dontSendNotification);
    mixLabel_.setJustificationType (juce::Justification::centred);
    mixLabel_.setFont (hellcat::labelFontExactFor (*this, 12.0f));
    mixLabel_.setColour (juce::Label::textColourId, hellcat::onCardText (&themeManager_.getCurrentTheme(), themeManager_.getCurrentTheme().textSecondary));
    addAndMakeVisible (mixLabel_);

    mixKnob_.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    mixKnob_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);   // value drawn in-arc by the L&F
    mixKnob_.setScrollWheelEnabled (false);
    mixKnob_.setTooltip (TRANS ("Global FX wet/dry"));
    addAndMakeVisible (mixKnob_);
    mixAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor_.getApvts(), "fx_mix", mixKnob_);
    // NOTE: install the value->text formatter AFTER the SliderAttachment: its
    // constructor overwrites slider.textFromValueFunction with param.getText()
    // (raw 0..127), which is why the readout showed a bare 0..127 here.
    mixKnob_.textFromValueFunction = [] (double v) {
        return juce::String (juce::roundToInt (juce::jlimit (0.0, 127.0, v) / 127.0 * 100.0)) + "%";
    };

    // ---- 3-band master EQ (Low Cut / Mid / High): synth-style rotaries bound to
    //      fx_eq_low / fx_eq_mid / fx_eq_high (0..127). The value renders in-ring
    //      via the editor-wide LookAndFeel, identical to the Dry/Wet knob. The
    //      low band is a LOW-CUT high-pass, so its caption is "Low Cut" (user
    //      request), not "Low". ----
    const char* const eqIds[3]   = { "fx_eq_low", "fx_eq_mid", "fx_eq_high" };
    const char* const eqNames[3] = { "Low Cut", "Mid", "High" };
    for (std::size_t i = 0; i < 3; ++i)
    {
        eqLabels_[i].setText (TRANS (eqNames[i]), juce::dontSendNotification);
        eqLabels_[i].setJustificationType (juce::Justification::centred);
        eqLabels_[i].setFont (hellcat::labelFontExactFor (*this, 12.0f));
        eqLabels_[i].setColour (juce::Label::textColourId, hellcat::onCardText (&themeManager_.getCurrentTheme(), themeManager_.getCurrentTheme().textSecondary));
        addAndMakeVisible (eqLabels_[i]);

        eqKnobs_[i].setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        eqKnobs_[i].setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        eqKnobs_[i].setScrollWheelEnabled (false);
        eqKnobs_[i].setTooltip (TRANS ("FX master EQ ") + eqNames[i]);
        addAndMakeVisible (eqKnobs_[i]);
        eqAttach_[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor_.getApvts(), eqIds[i], eqKnobs_[i]);
        // Install AFTER the SliderAttachment (it overwrites textFromValueFunction
        // with raw param.getText() -> 0..127); otherwise the EQ readouts show a
        // bare 0..127 instead of Hz / dB.
        eqKnobs_[i].textFromValueFunction = (i == 0)
            ? [] (double v) { return fxEqLowToString (v); }
            : [] (double v) { return fxEqDbToString (v); };
    }
}

FxRoutingBar::~FxRoutingBar() = default;

//==============================================================================
void FxRoutingBar::stepTopology (int direction)
{
    // Cycle fx_topo by ±1 (wrap) by writing the choice index directly. The
    // diagram's APVTS listener picks up the change and repaints.
    auto& apvts = processor_.getApvts();
    auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("fx_topo"));
    const int n = (p != nullptr) ? p->choices.size() : 3;
    if (n <= 0)
        return;
    auto v = apvts.getParameterAsValue ("fx_topo");   // choice index (0..n-1) for AudioParameterChoice
    const int cur = juce::jlimit (0, n - 1, juce::roundToInt (v.getValue()));
    v = static_cast<float> ((cur + direction + n) % n);
}
//==============================================================================
void FxRoutingBar::paint (juce::Graphics& g)
{
    const auto& t = themeManager_.getCurrentTheme();

    // Sibling-card panel: the shared module-card painter — flat tonal lift on
    // every theme, liquid chrome on Y2K (matches the synth cards and the
    // FX-slot cards).
    hellcat::paintChromeCard (g, getLocalBounds().toFloat(), kCorner, &t);

    // Section header "FX ROUTING" — the SAME typography as the OSC 1 / MIXER
    // GroupComponent headers (bold 14px, UPPERCASE, textSecondary), drawn here
    // rather than via a Label so it is byte-identical to the synth card titles.
    // Y2K: the header role font (Michroma) on the on-chrome label colour.
    const juce::Font headerFont = hellcat::headerFontFor (*this, 14.0f);
    g.setColour (hellcat::onCardText (&t, t.textSecondary));
    g.setFont (headerFont);
    g.drawText (TRANS ("FX ROUTING").toUpperCase(),
                juce::Rectangle<int> (kPad + 2, kPad, getWidth() - 2 * kPad, kHeaderH),
                juce::Justification::centredLeft, true);
}

void FxRoutingBar::resized()
{
    // The bar is the slim column 0 of the 4-column FX top row. Layout (top to
    // bottom): a painted "FX ROUTING" header band, then the [◀][flow diagram][▶]
    // row (a FlexBox, horizontally centred), then the [Dry/Wet knob] row. The flow
    // + controls rows are vertically centred as a block in the remaining height
    // so a tall column breathes evenly.
    auto area = getLocalBounds().reduced (kPad);
    if (area.isEmpty())
        return;

    // Header band (the "FX ROUTING" title is painted in paint(), not a child).
    area.removeFromTop (kHeaderH);
    if (area.getHeight() > kGap) area.removeFromTop (kGap);

    // Reserve the three rows + their gaps, then centre the block vertically.
    // FIXED (checked in the W6/W7 review): the bar's height is the FIXED
    // FxWorkspace::kRouteModuleH (224 — unchanged by the 2026-08-23 cards'
    // +20px spaciousness bump), so flow (50) + EQ (60) + ctrl (58) + gaps
    // (12) = 180 fits and the Dry/Wet dial ALWAYS lays out (it draws at
    // ~42px). The old "starves to ~0 at 1280x634" note described the
    // pre-floor layout and is superseded — see FxWorkspace.h
    // kRouteModuleH/kCardModuleH.
    const int flowH = juce::jlimit (0, area.getHeight(), kFlowRowH);
    const int eqH   = juce::jlimit (0, juce::jmax (0, area.getHeight() - flowH - 2 * kGap), kEqRowH);
    const int ctrlH = juce::jlimit (0, juce::jmax (0, area.getHeight() - flowH - eqH - 3 * kGap), kCtrlRowH);
    const int blockH = flowH
                     + (eqH   > 0 ? kGap + eqH   : 0)
                     + (ctrlH > 0 ? kGap + ctrlH : 0);
    if (area.getHeight() > blockH)
        area.removeFromTop (juce::jmin ((area.getHeight() - blockH) / 2, 10));   // cap dead space (tighter)

    // ---- Flow row: [◀][flow diagram][▶] horizontally centred (FlexBox) ----
    if (flowH > 0 && flowDiagram_ != nullptr)
    {
        auto flowRow = area.removeFromTop (flowH);
        juce::FlexBox fb;
        fb.flexDirection  = juce::FlexBox::Direction::row;
        fb.justifyContent = juce::FlexBox::JustifyContent::center;
        fb.alignItems     = juce::FlexBox::AlignItems::center;
        fb.items.add (juce::FlexItem (prevButton_).withWidth ((float) kStepBtnW).withHeight ((float) kStepBtnH));
        fb.items.add (juce::FlexItem (*flowDiagram_).withFlex (1.0f).withHeight ((float) flowH));
        fb.items.add (juce::FlexItem (nextButton_).withWidth ((float) kStepBtnW).withHeight ((float) kStepBtnH));
        fb.performLayout (flowRow);
        if ((eqH > 0 || ctrlH > 0) && area.getHeight() > kGap) area.removeFromTop (kGap);
    }

    // ---- EQ row: [LowCut][Mid][High] synth-style rotary knobs (3 equal cells) ----
    if (eqH > 0)
    {
        auto eqRow = area.removeFromTop (eqH);
        const int cellW = eqRow.getWidth() / 3;
        for (std::size_t i = 0; i < 3; ++i)
        {
            auto cell = (i < 2) ? eqRow.removeFromLeft (cellW) : eqRow;
            eqLabels_[i].setBounds (cell.removeFromTop (kLabelH));
            cell.removeFromTop (2);
            const int ks = juce::jmin (kEqKnobSize, cell.getWidth(), cell.getHeight());
            eqKnobs_[i].setBounds (cell.withSizeKeepingCentre (ks, ks));
        }
        if (ctrlH > 0 && area.getHeight() > kGap) area.removeFromTop (kGap);
    }

    // ---- Controls row: [Dry/Wet knob + label], centred in the full row width ----
    if (ctrlH > 0)
    {
        auto row = area.removeFromTop (ctrlH);
        // Mix: "Dry/Wet" caption above a centred synth-parity dial.
        mixLabel_.setBounds (row.removeFromTop (kLabelH));
        row.removeFromTop (2);
        const int ks = juce::jmin (kKnobSize, row.getWidth(), row.getHeight());
        mixKnob_.setBounds (row.withSizeKeepingCentre (ks, ks));
    }
}

//==============================================================================
void FxRoutingBar::applyThemeColors()
{
    const auto& t = themeManager_.getCurrentTheme();
    mixLabel_.setColour (juce::Label::textColourId, hellcat::onCardText (&t, t.textSecondary));
    for (auto& l : eqLabels_)
        l.setColour (juce::Label::textColourId, hellcat::onCardText (&t, t.textSecondary));

    if (flowDiagram_ != nullptr)
        flowDiagram_->repaint();   // re-resolve trace/block colours from the new theme

    repaint();
}
