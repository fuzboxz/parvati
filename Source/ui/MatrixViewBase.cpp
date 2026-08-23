// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See MatrixViewBase.h.

#include "MatrixViewBase.h"

#include "FormatHelpers.h"        // signedAmountPercent (amount labels)
#include "IconButton.h"           // IconButton (row delete X)
#include "ModMatrixHighlight.h"   // hover / slot-select / assign bus
#include "ModDestMap.h"           // isFxDest / kFxModDstOffset (FX dest domain)
#include "PluginProcessor.h"      // ParvatiAudioProcessor (complete type)
#include "ThemeManager.h"

#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachments + AudioParameterChoice

//==============================================================================
namespace parvati::matrixview
{
juce::Colour sourceCategoryColour (const ParvatiTheme& t, const juce::String& sourceName)
{
    if (sourceName.startsWith ("Env"))                                 return t.catEnv;   // Envelopes (teal)
    if (sourceName.startsWith ("LFO") || sourceName == "Voice LFO")    return t.catLfo;   // LFOs (magenta)
    if (sourceName.startsWith ("Seq"))                                 return t.catSeq;   // Sequencer (mint)
    if (sourceName.startsWith ("Arp"))                                 return t.catArp;   // Arpeggiator (mint — sequencer family)
    if (sourceName.startsWith ("Op"))                                  return t.catMod;   // Modifier outputs M1-4 (purple)
    if (sourceName.startsWith ("Const"))                               return t.catConst; // Constants C4..C255 (indigo)
    if (sourceName == "Velocity" || sourceName == "Aftertouch"
        || sourceName == "Pitch Bend" || sourceName.startsWith ("Wheel")
        || sourceName == "Expression" || sourceName == "Note")         return t.catPerf;  // keyboard / performance (amber)
    if (sourceName == "Gate" || sourceName == "Noise" || sourceName == "Random")
                                                                       return t.catUtil;  // utility (orange)
    return {};   // transparent (alpha 0) => no tint
}

juce::Colour rowCategoryColour (const ParvatiTheme& t, const juce::String& sourceName)
{
    const juce::Colour cat = sourceCategoryColour (t, sourceName);
    return cat.isTransparent() ? t.accentPrimary : cat;
}

juce::Font appFontOr (const juce::Component& c, float height)
{
    if (const auto* lnf = dynamic_cast<const ParvatiLookAndFeel*> (&c.getLookAndFeel()))
        return lnf->appFont (height, juce::Font::plain);
    return juce::Font (juce::FontOptions (height));
}
}  // namespace parvati::matrixview

//==============================================================================
// Local bipolar depth-slider LookAndFeel: a thin track, a fill drawn FROM the
// CENTRE to the value (row colour right of centre, dimmed left of centre), a
// centre zero-detent tick and a thumb. Self-contained. It never touches the
// shared ParvatiLookAndFeel. MatrixSliderGeometry seeds the per-view values.
class MatrixViewBase::BipolarSliderLNF : public juce::LookAndFeel_V4
{
public:
    BipolarSliderLNF (ThemeManager& tm, const MatrixSliderGeometry& geometry)
        : themeManager_ (tm), geometry_ (geometry) {}

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           juce::Slider::SliderStyle /*style*/, juce::Slider& slider) override
    {
        const auto& t = themeManager_.getCurrentTheme();
        // Flat bipolar depth slider. A clean dark track echoes the knob
        // tracks. The fill runs from the CENTRE to the value in this ROW's
        // category colour. The negative side is a dimmed same-hue version.
        // The centre tick reads as a zero detent. The thumb is a flat solid
        // circle with no 3D or gradient.
        const juce::Colour trackCol = t.trackEmpty;
        // Per-row category fill (pushed onto the slider as "parvatiRowFill"
        // by MatrixRow::applyThemeColors); falls back to the accent when unset.
        const juce::Colour rowFill = [&]
        {
            const auto* v = slider.getProperties().getVarPointer ("parvatiRowFill");
            return (v != nullptr && v->isInt()) ? juce::Colour ((uint32_t) (int) *v) : t.accentPrimary;
        }();
        const juce::Colour posFill  = rowFill;
        const juce::Colour negFill  = rowFill.darker (0.40f);
        const juce::Colour thumbCol = t.textPrimary;

        const float cy      = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        const float trackH  = geometry_.trackHeight;
        const float radius  = trackH * 0.5f;
        const float left    = static_cast<float> (x);
        const float right   = static_cast<float> (x + width);
        const float centreX = left + static_cast<float> (width) * 0.5f;
        const float sp      = juce::jlimit (left, right, sliderPos);

        // Empty track.
        g.setColour (trackCol);
        g.fillRoundedRectangle (juce::Rectangle<float> (left, cy - radius, right - left, trackH), radius);

        // Fill from centre to value: row colour (positive) / dimmed (negative).
        if (sp >= centreX)
        {
            g.setColour (posFill);
            if (sp > centreX)
                g.fillRoundedRectangle (juce::Rectangle<float> (centreX, cy - radius, sp - centreX, trackH), radius);
        }
        else
        {
            g.setColour (negFill);
            g.fillRoundedRectangle (juce::Rectangle<float> (sp, cy - radius, centreX - sp, trackH), radius);
        }

        // Centre zero-detent tick (taller than the track so it reads as a detent).
        g.setColour (thumbCol.withAlpha (slider.isEnabled() ? 0.9f : 0.4f));
        g.fillRect (juce::Rectangle<float> (centreX - 0.5f, cy - trackH, 1.0f, trackH * 2.0f));

        // Flat solid circle thumb (no 3D/gradient / no outline ring).
        const float tr = geometry_.thumbProportional
                             ? juce::jmax (3.0f, static_cast<float> (height) * 0.30f)
                             : 7.5f;
        const auto  thumbRect = juce::Rectangle<float> (tr * 2.0f, tr * 2.0f).withCentre (juce::Point<float> (sp, cy));
        g.setColour (slider.isEnabled() ? thumbCol : thumbCol.withAlpha (0.4f));
        g.fillEllipse (thumbRect);
    }

private:
    ThemeManager& themeManager_;
    MatrixSliderGeometry geometry_;
};

//==============================================================================
// One matrix row. Owns its source/dest combos plus a bipolar depth slider.
// The synth view binds both combos through APVTS attachments. The FX view
// attaches the source combo only. It rebuilds its dest combo by hand (see
// FxMatrixView::refresh) and writes picks back by index. Attachments live
// for the view's lifetime. Buttons call back into the view.
struct MatrixRow : public juce::Component,
                   private juce::Slider::Listener,
                   private juce::ComboBox::Listener
{
    MatrixRow (MatrixViewBase& owner, int slot, juce::LookAndFeel& sliderLnf)
        : owner_ (owner), slot_ (slot)
    {
        const auto& cfg = owner_.config();
        auto& apvts = owner_.processor().getApvts();
        const juce::String srcId = owner_.slotParamFor (slot, "_source");
        const juce::String dstId = owner_.slotParamFor (slot, "_dest");
        const juce::String amtId = owner_.slotParamFor (slot, "_amount");

        // Populate the combos from the registered choice params. The lists
        // stay in lock-step with the APVTS without copying string tables.
        // A detached dest combo (FX) builds its items elsewhere.
        if (auto* sp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (srcId)))
            sourceCombo_.addItemList (sp->choices, 1);
        if (cfg.destComboAttached)
            if (auto* dp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (dstId)))
                destCombo_.addItemList (dp->choices, 1);

        indexLabel_.setText (juce::String (slot + 1), juce::dontSendNotification);
        indexLabel_.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (indexLabel_);

        // Mute/bypass LAMP: the module-disable widget style. A compact
        // bordered dot with a full-bounds hit area. Dot = accentPrimary while
        // the routing is ACTIVE, the inactive grey while MUTED. A click
        // routes through the view's toggleMute (the stash/restore seam).
        // The lamp stays clickable so a muted row can unmute.
        muteLamp_ = std::make_unique<ParvatiModuleLamp>();
        muteLamp_->setTitle (TRANS ("Mute / bypass this modulation"));
        muteLamp_->setTooltip (TRANS ("Mute / bypass this modulation"));
        muteLamp_->onClick = [this] { owner_.toggleMute (slot_); };
        if (cfg.lampDiameter > 0.0f)
            muteLamp_->setLampDiameter (cfg.lampDiameter);
        addAndMakeVisible (*muteLamp_);

        addAndMakeVisible (sourceCombo_);
        addAndMakeVisible (destCombo_);

        depthSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
        depthSlider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        depthSlider_.setDoubleClickReturnValue (true, 0.0);   // double-click = reset to 0 (centre)
        depthSlider_.setScrollWheelEnabled (false);           // wheel scrolls the page (ParamControl idiom)
        depthSlider_.setLookAndFeel (&sliderLnf);
        addAndMakeVisible (depthSlider_);

        valueLabel_.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (valueLabel_);

        // Delete X (IconButton, path-drawn glyph): the RIGHTMOST control of
        // the row. 44pt HIG hit target with a visually compact glyph. Same
        // action as the former Clear button: free the slot through the view.
        clearButton_ = std::make_unique<IconButton> (IconButton::Icon::Close);
        clearButton_->setTooltip (TRANS ("Delete modulation"));
        clearButton_->setGlyphInset (11.0f);
        clearButton_->onClick = [this] { owner_.clearSlot (slot_); };
        addAndMakeVisible (*clearButton_);

        // Bind to the APVTS AFTER the widgets are populated. A detached dest
        // combo never gets an attachment; its owner reconciles it instead.
        srcAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, srcId, sourceCombo_);
        if (cfg.destComboAttached)
            dstAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, dstId, destCombo_);
        depthAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>  (apvts, amtId, depthSlider_);

        depthSlider_.addListener (this);
        // Re-resolve the row's category colour whenever a new source is
        // picked, so the tint follows live.
        sourceCombo_.addListener (this);
        // A detached dest combo needs the pick written back by hand.
        if (! cfg.destComboAttached)
            destCombo_.addListener (this);
        refreshValueDisplay();

        // Register this row as a MouseListener on every child. A hover
        // anywhere over the row drives the dest highlight bus, not just the
        // bare gaps between widgets. `false` => child events only.
        indexLabel_.addMouseListener (this, false);
        muteLamp_->addMouseListener (this, false);
        sourceCombo_.addMouseListener (this, false);
        destCombo_.addMouseListener (this, false);
        depthSlider_.addMouseListener (this, false);
        valueLabel_.addMouseListener (this, false);
        clearButton_->addMouseListener (this, false);

        // Accessibility-only: name the row after its slot ("Mod 5" /
        // "FX Mod 5"; the suffix-key i18n idiom).
        setTitle (TRANS (cfg.rowA11yPrefix) + juce::String (slot_ + 1));
    }

    ~MatrixRow() override
    {
        depthSlider_.removeListener (this);
        sourceCombo_.removeListener (this);
        destCombo_.removeListener (this);
        indexLabel_.removeMouseListener (this);
        muteLamp_->removeMouseListener (this);
        sourceCombo_.removeMouseListener (this);
        destCombo_.removeMouseListener (this);
        depthSlider_.removeMouseListener (this);
        valueLabel_.removeMouseListener (this);
        clearButton_->removeMouseListener (this);
        // Drop the custom L&F before the slider is destroyed. The L&F is
        // owned by the view and outlives this row. Unsetting keeps the
        // contract clean.
        depthSlider_.setLookAndFeel (nullptr);
    }

    // Accessibility-only: a labelled `group` container for this row's
    // widgets, so screen readers announce the row as a structured unit.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this,
                juce::AccessibilityRole::group);
    }

    int slot() const noexcept { return slot_; }

    // The row's dest combo. The owning FX view rebuilds and syncs it by hand
    // (its dest combo carries no APVTS attachment).
    juce::ComboBox& destCombo() noexcept { return destCombo_; }

    void sliderValueChanged (juce::Slider*) override { refreshValueDisplay(); }

    void comboBoxChanged (juce::ComboBox* c) override
    {
        if (c == &destCombo_ && ! owner_.config().destComboAttached)
        {
            // Manual index binding for the detached dest combo: a user pick
            // writes the item index (itemId-1) back to the slot's _dest
            // param. The STORED value stays the index. Presets and
            // serialization are unaffected by any relabelling.
            const int id = destCombo_.getSelectedId();
            if (id > 0)
            {
                auto& apvts = owner_.processor().getApvts();
                if (auto* p = apvts.getParameter (owner_.slotParamFor (slot_, "_dest")))
                    p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (id - 1)));
            }
            return;
        }
        // A new source was picked: re-resolve this row's category colour.
        // The row tint, the slider fill and the combo tag live in
        // applyThemeColors.
        applyThemeColors();
        repaint();
    }

    // ---- Hover / selection emphasis (driven via the ModMatrixHighlight bus) ----
    void mouseEnter (const juce::MouseEvent&) override
    {
        // The FX view highlights the hovered row locally as well; its bus
        // dest is offset-encoded above the synth range.
        if (owner_.config().highlightSelfOnHover)
            setHighlighted (true);
        const int d = owner_.destForSlot (slot_);
        const int offset = owner_.config().destBusOffset;
        const int bus = (offset == 0) ? d : (d < 0 ? -1 : d + offset);
        parvati::ModMatrixHighlight::instance().setHighlightedDest (bus);
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        // Only clear when the mouse has actually left the row: moving
        // between the row's child widgets also fires mouseExit (this row is
        // their MouseListener).
        const auto rel = e.getEventRelativeTo (this);
        if (! getLocalBounds().contains (rel.position.toInt()))
        {
            setHighlighted (false);
            parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
        }
    }

    // Hovered: this row's dest is the highlighted target.
    void setHighlighted (bool on) { if (highlighted_ != on) { highlighted_ = on; repaint(); } }
    // Flashed: a knob double-click or a source-pill click jumped here.
    void setFlashed (bool on) { if (flashed_ != on) { flashed_ = on; repaint(); } }
    bool highlighted_ = false;
    bool flashed_ = false;

    void refreshValueDisplay()
    {
        const int amt = owner_.isSlotMuted (slot_) ? owner_.stashedAmount (slot_)
                                                    : owner_.amountForSlot (slot_);
        valueLabel_.setText (signedAmountPercent (static_cast<double> (amt)), juce::dontSendNotification);
    }

    void setMutedLook (bool muted)
    {
        sourceCombo_.setEnabled (! muted);
        destCombo_.setEnabled (! muted);
        depthSlider_.setEnabled (! muted);
        // Lamp semantics = module-disable parity: accent (toggle ON) while
        // the routing is ACTIVE, grey while muted. The lamp stays enabled so
        // a click can unmute the row.
        muteLamp_->setToggleState (! muted, juce::dontSendNotification);
        muteLamp_->setEnabled (true);
        const auto& t = owner_.themeManager().getCurrentTheme();
        valueLabel_.setColour (juce::Label::textColourId, muted ? t.textSecondary : t.textPrimary);
        refreshValueDisplay();
        repaint();
    }

    void applyThemeColors()
    {
        const auto& t = owner_.themeManager().getCurrentTheme();
        const juce::Font f = parvati::matrixview::appFontOr (*this, 12.0f);
        indexLabel_.setFont (f);
        valueLabel_.setFont (f);
        indexLabel_.setColour (juce::Label::textColourId, t.textSecondary);
        valueLabel_.setColour (juce::Label::textColourId,
                               owner_.isSlotMuted (slot_) ? t.textSecondary : t.textPrimary);

        // SOURCE combo: a uniformly DARK dropdown tagged with a 4px
        // family-colour STRIP on its far-left edge. The strip colour is this
        // row's routing-source FAMILY. The DEST combo gets NO tag.
        const juce::Colour famCol = parvati::matrixview::rowCategoryColour (t, owner_.sourceNameForSlot (slot_));
        // Lamp ON colour: the row's modulator category colour, or the theme
        // accent when the view keeps the lamp neutral (FX).
        if (owner_.config().lampCarriesCategoryColour)
            muteLamp_->setOnColour (famCol);
        sourceCombo_.getProperties().set ("parvatiComboTag", (int) famCol.getARGB());
        sourceCombo_.removeColour (juce::ComboBox::backgroundColourId);
        destCombo_.getProperties().remove ("parvatiComboTag");
        destCombo_.removeColour (juce::ComboBox::backgroundColourId);

        // Per-row depth-slider fill colour: this row's routing-source
        // CATEGORY colour. BipolarSliderLNF reads "parvatiRowFill" so each
        // slider matches its own row. The negative side is a dimmed same-hue
        // version, derived in the LNF.
        depthSlider_.getProperties().set ("parvatiRowFill",
            (int) parvati::matrixview::rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).getARGB());

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using parvati::matrixview::appFontOr;
        using parvati::matrixview::rowCategoryColour;
        using parvati::matrixview::sourceCategoryColour;
        const auto& t = owner_.themeManager().getCurrentTheme();

        // FULL-ROW CATEGORY TINT: a very low-opacity (0.08) fill of this
        // row's routing-source category colour. The WHOLE row reads in its
        // source's hue at a glance. Painted first so a stronger
        // flash/highlight overlay still wins.
        g.setColour (rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).withAlpha (0.08f));
        g.fillAll();

        // Emphasis when this row is the highlighted modulation target or the
        // transient selection. A faint accent background makes the row read
        // as the target. The flash is the stronger of the two.
        if (flashed_ || highlighted_)
        {
            g.setColour (t.accentPrimary.withAlpha (flashed_ ? 0.18f : 0.10f));
            g.fillAll();
        }

        // Left accent bar in the source's functional-category colour
        // (brighter / thicker while this row is emphasised).
        const juce::Colour cat = sourceCategoryColour (t, owner_.sourceNameForSlot (slot_));
        if (! cat.isTransparent())
        {
            g.setColour (cat.withMultipliedAlpha ((flashed_ || highlighted_) ? 1.0f : 0.85f));
            g.fillRect (getLocalBounds().removeFromLeft ((flashed_ || highlighted_) ? 3 : 2));
        }

        // Outline while emphasised so the target row is unmistakable.
        if (flashed_ || highlighted_)
        {
            g.setColour (t.accentPrimary.withAlpha (flashed_ ? 0.85f : 0.55f));
            g.drawRect (getLocalBounds(), 1);
        }

        // Arrow glyph between the source and dest combos.
        g.setColour (t.textSecondary);
        g.setFont (appFontOr (*this, 13.0f));
        const int arrowX = sourceCombo_.getBounds().getRight() + 4;
        g.drawText (TRANS (">"),
                    juce::Rectangle<int> (arrowX, 0, 10, getHeight()),
                    juce::Justification::centred, false);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (4, 2);

        // F-ios-touch: the right-column ACTION targets and the value readout
        // are FIXED 44pt-floor widgets. Reserve them FIRST so a narrow row
        // squeezes the proportional COMBOS, never the buttons. The delete X
        // is the RIGHTMOST control. The value readout sits left of it.
        clearButton_->setBounds (b.removeFromRight (44));   // delete X hit target (unified)
        b.removeFromRight (8);
        valueLabel_.setBounds (b.removeFromRight (46));
        b.removeFromRight (8);

        // LEFT cluster: the mute/bypass lamp, then the row index. Rows are
        // NOT drag sources. Modulators are dragged only from the
        // CentralModBar pills.
        muteLamp_->setBounds (b.removeFromLeft (44));   // mute lamp hit target (unified)
        b.removeFromLeft (4);
        // INDEX LABEL: JUCE Label's DEFAULT border is 5px per side. Border
        // zeroed (the text is centred anyway) and the width taken from the
        // shared constant.
        indexLabel_.setBorderSize (juce::BorderSize<int> (0));
        indexLabel_.setBounds (b.removeFromLeft (parvati::matrixview::kMatrixIndexLabelW));
        b.removeFromLeft (4);

        // Source + dest combos share the REST with the depth slider. The
        // SLIDER leads: it keeps the width remaining after the combos'
        // measured floors. The combo floors are fit-to-widest-item, capped
        // so a long choice list can never push the slider under ~35% of the
        // row.
        const int rowW = b.getWidth();
        const int sliderFloor = juce::jmax (96, rowW * 35 / 100);
        const int comboBudget = juce::jmax (0, rowW - sliderFloor - 14 - 8);
        int srcW = juce::jmax (56, juce::jmin (comboBudget * 5 / 9, 130));
        int dstW = juce::jmax (60, juce::jmin (comboBudget - 14 - srcW, 150));
        if (owner_.config().comboShrinkFallback)
        {
            // When even the floors cannot fit, shrink the SOURCE first (the
            // narrower semantic: the fixed source list), then hard-floor
            // both at 44: the HIG minimum for a functional combo on touch.
            if (srcW + 14 + dstW > comboBudget)
                srcW = juce::jmax (44, comboBudget - 14 - dstW);
            if (srcW + 14 + dstW > comboBudget)
                dstW = juce::jmax (44, comboBudget - 14 - srcW);
        }
        sourceCombo_.setBounds (b.removeFromLeft (srcW));
        b.removeFromLeft (14);   // arrow gap
        destCombo_.setBounds (b.removeFromLeft (dstW));
        b.removeFromLeft (8);

        // iOS HIG: the depth slider fills the remaining row area, so on the
        // 48pt row it is ~44pt tall: a large invisible hit zone while the
        // visual thumb stays small. No custom hitTest needed.
        depthSlider_.setBounds (b);
    }

    MatrixViewBase& owner_;
    const int slot_;

    juce::Label    indexLabel_;
    std::unique_ptr<ParvatiModuleLamp> muteLamp_;   // mute/bypass lamp
    juce::ComboBox sourceCombo_;
    juce::ComboBox destCombo_;
    juce::Slider   depthSlider_;
    juce::Label    valueLabel_;
    std::unique_ptr<IconButton> clearButton_;       // delete X (far right)

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> srcAttach_, dstAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   depthAttach_;
};

//==============================================================================
MatrixViewBase::MatrixViewBase (ParvatiAudioProcessor& processor, ThemeManager& themeManager,
                                MatrixViewConfig config)
    : processor_ (processor), themeManager_ (themeManager), config_ (config)
{
    jassert (config_.numSlots >= 1 && config_.numSlots <= 16);

    bipolarLnf_ = std::make_unique<BipolarSliderLNF> (themeManager_, config_.sliderGeometry);

    headerLabel_.setText ("0 " + TRANS (config_.usedSuffixKey), juce::dontSendNotification);
    headerLabel_.setJustificationType (juce::Justification::centredLeft);
    headerLabel_.setFont (parvati::matrixview::appFontOr (*this, 13.0f));
    addAndMakeVisible (headerLabel_);

    addButton_ = std::make_unique<juce::TextButton> (TRANS ("+ Add Modulation"));
    addButton_->setTooltip (TRANS ("Assign the next free slot"));
    addButton_->onClick = [this] { addSlot(); };
    content_.addAndMakeVisible (*addButton_);

    viewport_.setScrollBarsShown (true, false, false, false);   // vertical scroll only (shown when content overflows)
    viewport_.setViewedComponent (&content_, false);            // view owns content_, not its deletion
    addAndMakeVisible (viewport_);

    for (int i = 0; i < config_.numSlots; ++i)
    {
        rows_[(size_t) i] = std::make_unique<MatrixRow> (*this, i, *bipolarLnf_);
        content_.addAndMakeVisible (*rows_[(size_t) i]);
    }

    applyThemeColors();
    startTimerHz (30);   // stay live across preset load / undo / automation (message thread)

    // Subscribe to the highlight bus. A hovered knob or matching row glows
    // every row routed to the same dest. A knob double-click scrolls to and
    // flashes the routed row. The callbacks are SafePointer-guarded AND
    // unsubscribed in the dtor. The assign handler is view-specific (each
    // view guards its own dest domain), so the derived ctor registers it.
    juce::Component::SafePointer<MatrixViewBase> safe (this);
    destHighlightSub_ = parvati::ModMatrixHighlight::instance().onDestHighlighted (
        [safe] (int modDst) { if (safe != nullptr) safe->onHighlightDest (modDst); });
    slotSelectSub_ = parvati::ModMatrixHighlight::instance().onSlotSelected (
        [safe] (int slot) { if (safe != nullptr) safe->onSelectSlot (slot); });
}

MatrixViewBase::~MatrixViewBase()
{
    stopTimer();

    if (destHighlightSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (destHighlightSub_);
    if (slotSelectSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (slotSelectSub_);
    if (assignSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (assignSub_);

    // Mute is SESSION-ONLY: restore every stashed amount so the persisted
    // APVTS state (host state, presets) never keeps the mute-induced zeros
    // that would silently DELETE routings across a restart. During the
    // session the engine was truly bypassed (amount==0); here the real
    // amount is written back. The view is destroyed BEFORE the processor
    // serializes state, so the saved state reflects the restored values.
    // (Edge case: a host that saves project state WHILE the editor is open
    // and a slot muted still captures the 0. Accepted; mute is documented
    // as not preset-safe.)
    for (int i = 0; i < config_.numSlots; ++i)
        if (muted_[(size_t) i])
            setAmountForSlot (i, stashedAmount_[(size_t) i]);
}

//==============================================================================
juce::String MatrixViewBase::slotParamFor (int slot, const char* suffix) const
{
    return config_.paramPrefix + juce::String (slot + 1) + suffix;
}

juce::Component* MatrixViewBase::rowAtOrNull (int slot) const noexcept
{
    if (slot < 0 || slot >= config_.numSlots)
        return nullptr;
    return rows_[(size_t) slot].get();
}

juce::ComboBox* MatrixViewBase::rowDestCombo (int slot) const noexcept
{
    // Only called by the owning view, with a slot it just enumerated.
    const auto* row = static_cast<const MatrixRow*> (rowAtOrNull (slot));
    return row != nullptr ? const_cast<juce::ComboBox*> (&row->destCombo_) : nullptr;
}

int MatrixViewBase::amountForSlot (int slot) const
{
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParamFor (slot, "_amount")))
        return juce::jlimit (-63, 63, juce::roundToInt (raw->load()));
    return 0;
}

void MatrixViewBase::setAmountForSlot (int slot, int amount)
{
    amount = juce::jlimit (-63, 63, amount);
    if (auto* p = processor_.getApvts().getParameter (slotParamFor (slot, "_amount")))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (amount)));
}

bool MatrixViewBase::isSlotMuted (int slot) const noexcept
{
    return muted_[(size_t) juce::jlimit (0, config_.numSlots - 1, slot)];
}

int MatrixViewBase::stashedAmount (int slot) const noexcept
{
    return stashedAmount_[(size_t) juce::jlimit (0, config_.numSlots - 1, slot)];
}

bool MatrixViewBase::isSlotActive (int slot) const
{
    return amountForSlot (slot) != 0
        || muted_[(size_t) juce::jlimit (0, config_.numSlots - 1, slot)];
}

int MatrixViewBase::firstFreeSlot() const
{
    for (int i = 0; i < config_.numSlots; ++i)
        if (amountForSlot (i) == 0 && ! muted_[(size_t) i])
            return i;
    return -1;
}

juce::String MatrixViewBase::sourceNameForSlot (int slot) const
{
    const juce::String id = slotParamFor (slot, "_source");
    auto& apvts = processor_.getApvts();
    if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
    {
        if (auto* raw = apvts.getRawParameterValue (id))
        {
            const int idx = juce::jlimit (0, juce::jmax (0, cp->choices.size() - 1),
                                          juce::roundToInt (raw->load()));
            if (idx >= 0 && idx < cp->choices.size())
                return cp->choices[idx];
        }
    }
    return {};
}

int MatrixViewBase::sourceForSlot (int slot) const
{
    slot = juce::jlimit (0, config_.numSlots - 1, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParamFor (slot, "_source")))
        return juce::roundToInt (raw->load());
    return -1;
}

int MatrixViewBase::destForSlot (int slot) const
{
    slot = juce::jlimit (0, config_.numSlots - 1, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParamFor (slot, "_dest")))
        return juce::roundToInt (raw->load());
    return -1;
}

void MatrixViewBase::onHighlightDest (int modDst)
{
    // Emphasise every row now routed to the highlighted dest (read live so
    // a row whose dest was just edited follows immediately). The FX view
    // encodes its dests above the synth range: the guard rejects synth
    // broadcasts, then the offset decodes the raw FX index. -1 clears all.
    const bool active = modDst >= 0
        && (config_.destBusOffset == 0 || parvati::ModDestMap::isFxDest (modDst));
    const int raw = active ? modDst - config_.destBusOffset : -1;
    for (const auto& r : rows_)
        if (r)
            r->setHighlighted (active && destForSlot (r->slot()) == raw);
}

void MatrixViewBase::onSelectSlot (int slotIndex)
{
    // Clear any prior flash, then flash + scroll the target row in. A slot
    // outside this view's domain (synth-encoded vs FX-offset-encoded) is
    // rejected, so a knob double-click never flashes the other view's rows.
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    if (slotIndex < 0)
        return;
    if (config_.destBusOffset > 0 && ! parvati::ModDestMap::isFxDest (slotIndex))
        return;

    const int s = slotIndex - config_.destBusOffset;
    if (s < 0 || s >= config_.numSlots)
        return;

    flashSlots_.add (s);
    flashStartMs_ = juce::Time::getMillisecondCounter();

    if (rows_[(size_t) s] != nullptr)
    {
        rows_[(size_t) s]->setFlashed (true);
        if (rows_[(size_t) s]->isVisible())
            ensureRowVisible (s);
    }
}

void MatrixViewBase::ensureRowVisible (int slot)
{
    if (slot < 0 || slot >= config_.numSlots || rows_[(size_t) slot] == nullptr)
        return;
    // Scroll the viewport the minimal amount so the row is fully on screen.
    const auto rowBounds = rows_[(size_t) slot]->getBounds();   // in content coords
    const int viewH    = viewport_.getMaximumVisibleHeight();
    const int curY     = viewport_.getViewPositionY();
    const int maxScroll = juce::jmax (0, content_.getHeight() - viewH);
    int targetY = curY;
    if (rowBounds.getY() < curY)
        targetY = rowBounds.getY() - 4;
    else if (rowBounds.getBottom() > curY + viewH)
        targetY = rowBounds.getBottom() - viewH + 4;
    viewport_.setViewPosition (0, juce::jlimit (0, maxScroll, targetY));
}

void MatrixViewBase::flashTick()
{
    // Auto-expire the transient flash. The single-slot jump and the
    // multi-row source-flash share this one timed expiry.
    if (! flashSlots_.isEmpty()
        && (juce::Time::getMillisecondCounter() - flashStartMs_) > (juce::uint32) kFlashMs)
    {
        for (const int s : flashSlots_)
            if (s >= 0 && s < config_.numSlots && rows_[(size_t) s] != nullptr)
                rows_[(size_t) s]->setFlashed (false);
        flashSlots_.clearQuick();
    }
}

void MatrixViewBase::flashRowsForSource (int sourceEnum)
{
    // Clear any prior flash, then flash every ACTIVE row now routed FROM
    // @p sourceEnum (read live so a freshly-edited source combo follows).
    // The first matching row is scrolled into view. The flash auto-expires
    // via flashTick on the timer.
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    int firstVisible = -1;
    for (int i = 0; i < config_.numSlots; ++i)
    {
        if (! isSlotActive (i))
            continue;
        if (sourceForSlot (i) != sourceEnum)
            continue;
        flashSlots_.add (i);
        if (rows_[(size_t) i] != nullptr)
            rows_[(size_t) i]->setFlashed (true);
        if (firstVisible < 0)
            firstVisible = i;
    }

    if (! flashSlots_.isEmpty())
    {
        flashStartMs_ = juce::Time::getMillisecondCounter();
        if (firstVisible >= 0)
            ensureRowVisible (firstVisible);
    }
}

void MatrixViewBase::toggleMute (int slot)
{
    slot = juce::jlimit (0, config_.numSlots - 1, slot);
    if (muted_[(size_t) slot])
    {
        // Unmute: restore the stashed amount (true modulation resumes).
        muted_[(size_t) slot] = false;
        setAmountForSlot (slot, stashedAmount_[(size_t) slot]);
    }
    else
    {
        // Mute: stash the live amount, write 0 (true engine bypass), keep
        // the row visible.
        stashedAmount_[(size_t) slot] = amountForSlot (slot);
        muted_[(size_t) slot] = true;
        setAmountForSlot (slot, 0);
    }
    refresh();
}

void MatrixViewBase::clearSlot (int slot)
{
    slot = juce::jlimit (0, config_.numSlots - 1, slot);
    // Zero the amount (frees the slot by convention) and drop any mute. The
    // source/dest selections are left intact (non-destructive). A later Add
    // overwrites the chosen free slot's source/dest/amount, so nothing leaks
    // back into the active list.
    muted_[(size_t) slot] = false;
    setAmountForSlot (slot, 0);
    refresh();
}

void MatrixViewBase::addSlot()
{
    assignNextFreeSlot (config_.addDefaultSource, config_.addDefaultDest, 32);
}

bool MatrixViewBase::assignNextFreeSlot (int sourceEnum, int destEnum, int amount)
{
    // Defensive: a dest outside this view's domain is never a valid target
    // here. Reject it rather than let convertTo0to1 clamp it into a bogus
    // slot. The other view's handler owns that dest domain.
    if (destEnum >= config_.rejectDestAtOrAbove)
        return false;

    const int s = firstFreeSlot();
    if (s < 0)
        return false;   // matrix full — caller (button / drop) is a no-op

    muted_[(size_t) s] = false;
    auto setChoice = [this] (int slot, const char* suffix, int choiceIndex)
    {
        if (auto* p = processor_.getApvts().getParameter (slotParamFor (slot, suffix)))
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (choiceIndex)));
    };
    setChoice (s, "_source", sourceEnum);
    setChoice (s, "_dest",   destEnum);
    setAmountForSlot (s, amount);   // a visible non-zero depth so the row appears
    refresh();
    return true;
}

//==============================================================================
juce::String MatrixViewBase::buildSignature() const
{
    juce::String s;
    s.preallocateBytes (16);
    for (int i = 0; i < config_.numSlots; ++i)
    {
        if (muted_[(size_t) i])
            s << 'M';
        else if (amountForSlot (i) != 0)
            s << 'A';
        else
            s << '.';
    }
    return s;
}

void MatrixViewBase::refresh()
{
    // Reconcile transient mute with external amount changes: if a muted
    // slot's amount was changed outside (preset load / undo / automation to
    // non-zero), the mute is stale — drop it so the row reflects reality.
    for (int i = 0; i < config_.numSlots; ++i)
        if (muted_[(size_t) i] && amountForSlot (i) != 0)
            muted_[(size_t) i] = false;

    const auto sig = buildSignature();
    if (sig == lastSignature_)
        return;
    lastSignature_ = sig;
    rebuildLayout();
    repaint();
}

void MatrixViewBase::rebuildLayout()
{
    const int w = juce::jmax (0, content_.getWidth());

    int y = 4;
    int activeCount = 0;
    for (int i = 0; i < config_.numSlots; ++i)
    {
        const bool active = isSlotActive (i);
        rows_[(size_t) i]->setVisible (active);
        rows_[(size_t) i]->setMutedLook (muted_[(size_t) i]);
        rows_[(size_t) i]->applyThemeColors();
        if (active)
        {
            rows_[(size_t) i]->setBounds (4, y, juce::jmax (0, w - 8), kRowHeight);
            y += kRowHeight + 4;
            ++activeCount;
        }
    }

    // "+ Add Modulation" row at the bottom of the active list (or "Matrix
    // Full"). Full row height (44pt HIG target): the rows above scroll
    // inside the viewport, so the taller button costs only scroll length.
    const bool full = (activeCount >= config_.numSlots);
    addButton_->setButtonText (full ? TRANS (config_.matrixFullKey) : TRANS ("+ Add Modulation"));
    addButton_->setEnabled (! full);
    addButton_->setBounds (4, y + 4, juce::jmax (0, w - 8), kAddButtonH);
    y += kAddButtonH + 8;

    headerLabel_.setText (juce::String (activeCount) + " " + TRANS (config_.usedSuffixKey),
                          juce::dontSendNotification);

    content_.setSize (w, y);
}

//==============================================================================
void MatrixViewBase::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

void MatrixViewBase::resized()
{
    auto area = getLocalBounds().reduced (4);

    headerLabel_.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);
    viewport_.setBounds (area);

    // Size the scrolled content to the viewport width minus the
    // always-reserved vertical-scrollbar thickness, so the rows are never
    // clipped behind the scrollbar. Height is recomputed in rebuildLayout().
    const int cw = juce::jmax (0, viewport_.getWidth() - viewport_.getScrollBarThickness());
    content_.setSize (cw, content_.getHeight());
    rebuildLayout();
}

void MatrixViewBase::applyThemeColors()
{
    const auto& t = themeManager_.getCurrentTheme();
    headerLabel_.setColour (juce::Label::textColourId, t.textPrimary);
    headerLabel_.setFont (parvati::matrixview::appFontOr (*this, 13.0f));

    addButton_->setColour (juce::TextButton::buttonColourId, t.containerFill);
    addButton_->setColour (juce::TextButton::buttonOnColourId, t.accentPrimary);
    addButton_->setColour (juce::TextButton::textColourOffId, t.textPrimary);
    addButton_->setColour (juce::TextButton::textColourOnId, t.backgroundBase);

    content_.bg = t.backgroundBase;
    content_.setOpaque (true);
    content_.repaint();
    for (int i = 0; i < config_.numSlots; ++i)
        if (rows_[(size_t) i])
            rows_[(size_t) i]->applyThemeColors();

    rebuildLayout();
    repaint();
}

void MatrixViewBase::visibilityChanged()
{
    // F-ios-perf-3 (iOS hunt 2026-08-19): run the 30 Hz poll only while this
    // display is actually showing (its page is current / the editor is on a
    // desktop). visibilityChanged fires on tab-page unparent (the
    // TabbedComponent removes non-current content) and on the initial
    // add-to-parent; the constructor's startTimerHz stays for the
    // first-show case (stopTimer on an already-stopped timer is a no-op).
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}
