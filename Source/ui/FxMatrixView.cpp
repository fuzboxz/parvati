// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxMatrixView.h.

#include "FxMatrixView.h"
#include "FxSlotLabels.h"   // activeParamCount / paramLabel (dynamic FX-dest labels)
#include "ModDestMap.h"        // isFxDest / kFxModDstOffset (FX-dest domain)
#include "ModMatrixHighlight.h" // onAssignRequest bus (drag-and-drop -> fxmod slot)

#include "PluginProcessor.h"   // ParvatiAudioProcessor (complete type)
#include "ThemeManager.h"
#include "ParvatiTheme.h"
#include "ParvatiLookAndFeel.h"   // appFont() via the inherited editor L&F
#include "dsp/patch.h"            // ambika::dsp::MOD_SRC_*
#include "PluginEditor.h"   // ParamControl (tap-to-assign state)

#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachments + AudioParameterChoice

// The view assumes the FX matrix's 16-slot capacity (kNumFxMatrixSlots). Unlike
// the synth matrix this count is not tied to a patch-byte stride, so there is no
// firmware static_assert — only this capacity check.
static_assert (kNumFxMatrixSlots == 16, "FxMatrixView assumes a 16-slot FX mod matrix");

//==============================================================================
namespace
{
// Per-slot Dry/Wet FX_DST_* index (slot 0/1/2 -> FX_DST_FX{1,2,3}_DRYWET). Used
// by FxMatrixRow::rebuildDestItems to build the dynamic FX-dest combo with a
// stable itemId == FX_DST_* index + 1 (the stored fxmod{N}_dest value is the
// index itself, so presets/serialization are unaffected by the relabelling).
constexpr int kDryWetDestIdx[3] = { FX_DST_FX1_DRYWET, FX_DST_FX2_DRYWET, FX_DST_FX3_DRYWET };

// Category colour for a mod-source display name, mirroring the STRICT family
// palette (and ModSourceCatalog::clusterAccent): Env=teal, LFO=magenta,
// Seq/Arp=mint, Op(modifier)=purple, Const=indigo, keyboard/Perf=amber,
// Gate/Noise/Random=orange. Returns a transparent Colour only for a source
// name with no known family. (Verbatim from ModMatrixView — sources are shared.)
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

// Row category colour matching the knob MODULATION RINGS + the STRICT family
// palette: every source resolves to its family cat* token (Env/LFO/Seq/Arp/Op/
// Const/Perf/Util -> their cat* token); an unknown name falls back to the
// neutral accent. (Verbatim from ModMatrixView.)
juce::Colour rowCategoryColour (const ParvatiTheme& t, const juce::String& sourceName)
{
    const juce::Colour cat = sourceCategoryColour (t, sourceName);
    return cat.isTransparent() ? t.accentPrimary : cat;
}

// A signed amount (-63..+63) -> "+100%" / "0%" / "-50%". The slider/engine use
// ±63 as full-scale, so 63 maps to 100%. (Verbatim from ModMatrixView.)
juce::String formatPercent (int amount)
{
    const int pct = juce::roundToInt (static_cast<double> (amount) * 100.0 / 63.0);
    return (pct > 0 ? "+" : juce::String()) + juce::String (pct) + "%";
}

// Resolve the app font through the inherited editor L&F when present, else the
// JUCE default (keeps the view usable before it is reparented into the editor).
// (Verbatim from ModMatrixView.)
juce::Font appFontOr (const juce::Component& c, float height)
{
    if (const auto* lnf = dynamic_cast<const ParvatiLookAndFeel*> (&c.getLookAndFeel()))
        return lnf->appFont (height, juce::Font::plain);
    return juce::Font (juce::FontOptions (height));
}
}  // namespace

//==============================================================================
// Local bipolar depth-slider LookAndFeel (verbatim from ModMatrixView): a thin
// track, a fill drawn FROM the CENTRE to the value (accent right of centre /
// accent2 left of centre), a centre zero-detent tick and a thumb. Self-contained.
class FxMatrixView::BipolarSliderLNF : public juce::LookAndFeel_V4
{
public:
    explicit BipolarSliderLNF (ThemeManager& tm) : themeManager_ (tm) {}

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           juce::Slider::SliderStyle /*style*/, juce::Slider& slider) override
    {
        const auto& t = themeManager_.getCurrentTheme();
        const juce::Colour trackCol = t.trackEmpty;
        // Per-row category fill (pushed onto the slider as "parvatiRowFill"); falls
        // back to the accent when unset.
        const juce::Colour rowFill = [&]
        {
            const auto* v = slider.getProperties().getVarPointer ("parvatiRowFill");
            return (v != nullptr && v->isInt()) ? juce::Colour ((uint32_t) (int) *v) : t.accentPrimary;
        }();
        const juce::Colour posFill  = rowFill;
        const juce::Colour negFill  = rowFill.darker (0.40f);
        const juce::Colour thumbCol = t.textPrimary;

        const float cy      = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        const float trackH  = 4.0f;
        const float radius  = trackH * 0.5f;
        const float left    = static_cast<float> (x);
        const float right   = static_cast<float> (x + width);
        const float centreX = left + static_cast<float> (width) * 0.5f;
        const float sp      = juce::jlimit (left, right, sliderPos);

        // Empty track.
        g.setColour (trackCol);
        g.fillRoundedRectangle (juce::Rectangle<float> (left, cy - radius, right - left, trackH), radius);

        // Fill from centre to value: accent (positive) / accent2 (negative).
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
        const float tr = juce::jmax (3.0f, static_cast<float> (height) * 0.30f);
        const auto  thumbRect = juce::Rectangle<float> (tr * 2.0f, tr * 2.0f).withCentre (juce::Point<float> (sp, cy));
        g.setColour (slider.isEnabled() ? thumbCol : thumbCol.withAlpha (0.4f));
        g.fillEllipse (thumbRect);
    }

private:
    ThemeManager& themeManager_;
};

//==============================================================================
// A small drag-grip (six-dot handle) rendered left of a row's source combo.
// mouseDrag (past a small threshold) starts an INTERNAL DragAndDropContainer
// drag carrying "parvatiModSrc:<sourceEnum>" and a themed drag image — the SAME
// payload the synth matrix grip / CentralModBar emit. Dropping it on a
// destination knob assigns the next free slot for that source on the matching
// matrix — an FX knob (offset-encoded FX dest) takes an FX-mod slot, a synth
// knob takes a synth slot; each matrix's handler ignores the other's domain.
// Matrix rows themselves are not drop targets. Clicking (no drag) is a no-op so
// the grip never competes with the adjacent source combo.
struct FxSourceDragGrip : public juce::Component,
                              public juce::SettableTooltipClient
{
    FxSourceDragGrip (FxMatrixView& owner, int slot) : owner_ (owner), slot_ (slot)
    {
        setTooltip (TRANS ("Drag onto a knob to assign this modulation"));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = owner_.themeManager().getCurrentTheme();
        g.setColour (t.textSecondary);
        const float r  = 1.4f;
        const int   w  = getWidth();
        const int   h  = getHeight();
        const float x0 = static_cast<float> (w) * 0.35f;
        const float x1 = static_cast<float> (w) * 0.65f;
        const float ys[3] = { h * 0.35f, h * 0.5f, h * 0.65f };
        for (const float y : ys)
        {
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f)
                               .withCentre (juce::Point<float> (x0, y)));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f)
                               .withCentre (juce::Point<float> (x1, y)));
        }
    }

    void mouseDown (const juce::MouseEvent&) override { dragStarted_ = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Debounce: start exactly one drag per press, and only past a small
        // threshold so a stray jitter never fires a phantom drag.
        if (dragStarted_ || e.getDistanceFromDragStart() < 5)
            return;
        dragStarted_ = true;

        const int src = owner_.sourceForSlot (slot_);
        if (src < 0)
            return;

        auto* ddc = findParentComponentOfClass<juce::DragAndDropContainer>();
        if (ddc == nullptr)
            return;   // no DragAndDropContainer ancestor (e.g. a headless test)

        ddc->startDragging ("parvatiModSrc:" + juce::String (src), this, buildDragImage(), true);
    }

    // Tap-to-assign: a clean tap (no drag) selects this row's mod source for
    // the next dest tap. Mirrors the CentralModBar pill's clean-tap detection
    // (! dragStarted_ + small movement). Available on all platforms; inert
    // unless [MOD] tap-to-assign is toggled on (tapAssignActive()).
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (ParamControl::tapAssignActive() && ! dragStarted_ && e.getDistanceFromDragStart() <= 5)
        {
            const int src = owner_.sourceForSlot (slot_);
            if (src >= 0)
                ParamControl::setTapSelectedSource (src);
        }
    }

private:
    // A small themed drag image composited under the cursor: the source's
    // category colour chip + its short name, on a container-fill rounded tile.
    juce::Image buildDragImage() const
    {
        const auto&      t    = owner_.themeManager().getCurrentTheme();
        const juce::String name = owner_.sourceNameForSlot (slot_);
        const juce::Font  f    = appFontOr (owner_, 13.0f);
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, name);
        const int w = juce::jmax (48, 12 + 8 + textW + 10);
        const int h = 22;

        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        g.setColour (t.containerFill);
        g.fillRoundedRectangle (img.getBounds().toFloat(), 5.0f);

        const juce::Colour cat = sourceCategoryColour (t, name);
        g.setColour (cat.isTransparent() ? t.accentPrimary : cat);
        g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 5.0f, 7.0f, static_cast<float> (h) - 10.0f), 2.0f);

        g.setColour (t.textPrimary);
        g.setFont (f);
        g.drawText (name, juce::Rectangle<int> (17, 0, w - 17, h), juce::Justification::centredLeft, true);

        g.setColour (t.accentPrimary.withAlpha (0.6f));
        g.drawRoundedRectangle (img.getBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        return img;
    }

    FxMatrixView& owner_;
    int slot_;
    bool dragStarted_ = false;
};

//==============================================================================
// One FX matrix row. Owns its source/dest combos + bipolar depth slider, each
// bound to its slot's APVTS params (fxmod{N}_source/_dest/_amount) via
// attachments that live for the view's lifetime. The dest combo is populated
// from the fxmod{N}_dest APVTS choice param, so it shows the FX destinations
// (makeFxDests()) without any MOD_DST_* coupling. Hover highlights THIS row
// locally (no broadcast to the shared ModMatrixHighlight bus — see the header).
struct FxMatrixRow : public juce::Component,
                     private juce::Slider::Listener,
                     private juce::ComboBox::Listener
{
    FxMatrixRow (FxMatrixView& owner, int slot, juce::LookAndFeel& sliderLnf)
        : owner_ (owner), slot_ (slot)
    {
        auto& apvts = owner_.processor().getApvts();
        const juce::String srcId = FxMatrixView::slotParam (slot, "_source");
        const juce::String amtId = FxMatrixView::slotParam (slot, "_amount");

        // The SOURCE combo is populated from its (static) APVTS choice param so it
        // stays in lock-step with makeModSources. The DEST combo is NOT: its labels
        // are DYNAMIC (each slot's actual parameter names from paramLabel()) and it
        // is index-bound manually instead of via a ComboBoxAttachment (the stored
        // fxmod{N}_dest value stays the stable FX_DST_* index 0..17). Its items are
        // first built from the live slot types in FxMatrixView::refresh().
        if (auto* sp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (srcId)))
            sourceCombo_.addItemList (sp->choices, 1);

        indexLabel_.setText (juce::String (slot + 1), juce::dontSendNotification);
        indexLabel_.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (indexLabel_);

        dragGrip_ = std::make_unique<FxSourceDragGrip> (owner_, slot);
        addAndMakeVisible (*dragGrip_);

        addAndMakeVisible (sourceCombo_);
        addAndMakeVisible (destCombo_);

        depthSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
        depthSlider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        depthSlider_.setDoubleClickReturnValue (true, 0.0);   // double-click = reset to 0 (centre)
        depthSlider_.setLookAndFeel (&sliderLnf);
        addAndMakeVisible (depthSlider_);

        valueLabel_.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (valueLabel_);

        muteButton_.setButtonText (TRANS ("M"));
        muteButton_.setClickingTogglesState (true);
        muteButton_.setTooltip (TRANS ("Mute / bypass this modulation"));
        muteButton_.onClick = [this] { owner_.toggleMute (slot_); };
        addAndMakeVisible (muteButton_);

        clearButton_.setButtonText (TRANS ("Clear"));
        clearButton_.setTooltip (TRANS ("Clear this modulation (free the slot)"));
        clearButton_.onClick = [this] { owner_.clearSlot (slot_); };
        addAndMakeVisible (clearButton_);

        // Bind to the APVTS AFTER the widgets are populated. Only the SOURCE combo
        // uses a ComboBoxAttachment; the DEST combo is manually index-bound (see
        // comboBoxChanged / syncDestFromParam).
        srcAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, srcId, sourceCombo_);
        depthAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>  (apvts, amtId, depthSlider_);

        depthSlider_.addListener (this);
        // Re-resolve the row's category colour (row tint + slider fill + combo
        // tint) whenever a new source is picked so it follows live.
        sourceCombo_.addListener (this);
        // The dest combo is manually index-bound (no ComboBoxAttachment): listen
        // here so a user pick writes the FX_DST_* index back to fxmod{N}_dest.
        destCombo_.addListener (this);
        refreshValueDisplay();

        // Register this row as a MouseListener on every child so a hover anywhere
        // over the row (combos / slider / buttons / labels) drives the highlight
        // (local + the ModMatrixHighlight broadcast), not just the bare gaps.
        // `false` => child events only (no recursion into popup children).
        indexLabel_.addMouseListener (this, false);
        dragGrip_->addMouseListener (this, false);
        sourceCombo_.addMouseListener (this, false);
        destCombo_.addMouseListener (this, false);
        depthSlider_.addMouseListener (this, false);
        valueLabel_.addMouseListener (this, false);
        muteButton_.addMouseListener (this, false);
        clearButton_.addMouseListener (this, false);
    }

    ~FxMatrixRow() override
    {
        depthSlider_.removeListener (this);
        sourceCombo_.removeListener (this);
        destCombo_.removeListener (this);
        indexLabel_.removeMouseListener (this);
        dragGrip_->removeMouseListener (this);
        sourceCombo_.removeMouseListener (this);
        destCombo_.removeMouseListener (this);
        depthSlider_.removeMouseListener (this);
        valueLabel_.removeMouseListener (this);
        muteButton_.removeMouseListener (this);
        clearButton_.removeMouseListener (this);
        // Drop the custom L&F before the slider is destroyed (the L&F is owned by
        // the view and outlives this row, but unsetting keeps the contract clean).
        depthSlider_.setLookAndFeel (nullptr);
    }

    void sliderValueChanged (juce::Slider*) override { refreshValueDisplay(); }

    void comboBoxChanged (juce::ComboBox* c) override
    {
        if (c == &destCombo_)
        {
            // Manual index-binding for the (dynamically-labelled) dest combo: a
            // user pick writes the FX_DST_* index (itemId-1) back to fxmod{N}_dest.
            // Mirrors how assignNextFreeSlot writes the dest choice (convertTo0to1
            // + setValueNotifyingHost). The STORED value stays the index, so
            // presets/serialization are unaffected by the relabelling.
            const int id = destCombo_.getSelectedId();
            if (id > 0)
            {
                auto& apvts = owner_.processor().getApvts();
                if (auto* p = apvts.getParameter (FxMatrixView::slotParam (slot_, "_dest")))
                    p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (id - 1)));
            }
            return;
        }
        // A new source was picked: re-resolve this row's category colour (the
        // full-row tint, the depth-slider fill and the source-combo tint all
        // live in applyThemeColors).
        applyThemeColors();
        repaint();
    }

    // ---- Hover emphasis (local + shared ModMatrixHighlight bus) ----
    // A hovered FX row highlights itself AND broadcasts its dest (offset-encoded
    // as FX_DST_* + kFxModDstOffset, disjoint from synth MOD_DST_* 0..18) so
    // every OTHER FX row routed to the same dest highlights and the matching FX
    // knobs glow. The offset keeps synth knobs/rows from ever matching an FX
    // broadcast (applyModHighlight compares the broadcast to each knob's modDest_).
    void mouseEnter (const juce::MouseEvent&) override
    {
        setHighlighted (true);
        const int d = owner_.destForSlot (slot_);
        parvati::ModMatrixHighlight::instance().setHighlightedDest (
            d < 0 ? -1 : d + parvati::ModDestMap::kFxModDstOffset);
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        // Only clear when the mouse has actually left the row: moving between the
        // row's child widgets also fires mouseExit (this row is their MouseListener).
        const auto rel = e.getEventRelativeTo (this);
        if (! getLocalBounds().contains (rel.position.toInt()))
        {
            setHighlighted (false);
            parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
        }
    }

    // Hovered: this row's dest is the highlighted target.
    void setHighlighted (bool on) { if (highlighted_ != on) { highlighted_ = on; repaint(); } }
    // Flashed: a source-pill click jumped the matrix to this slot.
    void setFlashed (bool on) { if (flashed_ != on) { flashed_ = on; repaint(); } }
    bool highlighted_ = false;
    bool flashed_ = false;

    void refreshValueDisplay()
    {
        const int amt = owner_.isSlotMuted (slot_) ? owner_.stashedAmount (slot_)
                                                    : owner_.amountForSlot (slot_);
        valueLabel_.setText (formatPercent (amt), juce::dontSendNotification);
    }

    // (Re)build the dest combo's items from the three slots' current FX types so
    // it shows each slot's ACTUAL parameter names ("FX1 Position" ...) instead of
    // the static "FX1 Param K". itemId == FX_DST_* index + 1 keeps the stored
    // fxmod{N}_dest value (the index) stable. Inactive params are omitted for a
    // clean list (a dest routed to a now-absent param is handled by sync below).
    void rebuildDestItems (FxType t0, FxType t1, FxType t2)
    {
        const FxType types[3] = { t0, t1, t2 };
        destCombo_.clear (juce::dontSendNotification);
        for (int s = 0; s < 3; ++s)
        {
            const int dryWet = kDryWetDestIdx[s];
            destCombo_.addItem ("FX" + juce::String (s + 1) + " Dry/Wet", dryWet + 1);
            const FxType t = types[s];
            const int active = (t != FxType::None && t < FxType::Count)
                                   ? activeParamCount (t) : 0;
            for (int idx = 0; idx < kNumFxSlotParams; ++idx)
            {
                if (idx < active)
                {
                    const int pIdx = dryWet + 1 + idx;
                    destCombo_.addItem ("FX" + juce::String (s + 1) + " "
                                         + juce::String (paramLabel (t, idx)), pIdx + 1);
                }
            }
        }
    }

    // Sync the dest combo's selection from the live fxmod{N}_dest value (the
    // FX_DST_* index). Called every refresh() tick (preset load / undo /
    // assignNextFreeSlot / automation can change the value without the combo). If
    // the index is absent from the current list — e.g. the dest points at a param
    // the slot's current type no longer exposes — the stored value is LEFT INTACT
    // (the effect ignores inactive params) and the combo is cleared.
    void syncDestFromParam()
    {
        const int destIdx = owner_.destForSlot (slot_);
        const int wantId = destIdx + 1;
        if (destIdx >= 0 && destCombo_.indexOfItemId (wantId) >= 0)
        {
            if (destCombo_.getSelectedId() != wantId)
                destCombo_.setSelectedId (wantId, juce::dontSendNotification);
        }
        else if (destCombo_.getSelectedId() != 0)
        {
            destCombo_.setSelectedId (0, juce::dontSendNotification);
        }
    }

    void setMutedLook (bool muted)
    {
        sourceCombo_.setEnabled (! muted);
        destCombo_.setEnabled (! muted);
        depthSlider_.setEnabled (! muted);
        muteButton_.setToggleState (muted, juce::dontSendNotification);
        const auto& t = owner_.themeManager().getCurrentTheme();
        valueLabel_.setColour (juce::Label::textColourId, muted ? t.textSecondary : t.textPrimary);
        refreshValueDisplay();
        repaint();
    }

    void applyThemeColors()
    {
        const auto& t = owner_.themeManager().getCurrentTheme();
        const juce::Font f = appFontOr (*this, 12.0f);
        indexLabel_.setFont (f);
        valueLabel_.setFont (f);
        indexLabel_.setColour (juce::Label::textColourId, t.textSecondary);
        valueLabel_.setColour (juce::Label::textColourId,
                               owner_.isSlotMuted (slot_) ? t.textSecondary : t.textPrimary);

        // SOURCE combo: uniformly DARK dropdown tagged with a 4px family-colour
        // STRIP on its far-left edge. The DEST combo gets NO tag (just dark +
        // white). (Verbatim from ModMatrixView.)
        const juce::Colour famCol = rowCategoryColour (t, owner_.sourceNameForSlot (slot_));
        sourceCombo_.getProperties().set ("parvatiComboTag", (int) famCol.getARGB());
        sourceCombo_.removeColour (juce::ComboBox::backgroundColourId);
        destCombo_.getProperties().remove ("parvatiComboTag");
        destCombo_.removeColour (juce::ComboBox::backgroundColourId);

        // Per-row depth-slider fill colour: this row's routing-source CATEGORY
        // colour. BipolarSliderLNF reads "parvatiRowFill" so each slider matches
        // its own row.
        depthSlider_.getProperties().set ("parvatiRowFill",
            (int) rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).getARGB());

        muteButton_.setColour (juce::TextButton::buttonOnColourId, t.accentPrimary);
        muteButton_.setColour (juce::TextButton::textColourOnId, t.backgroundBase);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = owner_.themeManager().getCurrentTheme();

        // FULL-ROW CATEGORY TINT: a very low-opacity (0.08) fill of this row's
        // routing-source category colour. (Verbatim from ModMatrixView.)
        g.setColour (rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).withAlpha (0.08f));
        g.fillAll();

        // Emphasis when this row is the hovered/selected target. The flash
        // (source-pill jump) is the stronger of the two.
        if (flashed_ || highlighted_)
        {
            g.setColour (t.accentPrimary.withAlpha (flashed_ ? 0.18f : 0.10f));
            g.fillAll();
        }

        // Left accent bar in the source's functional-category colour (brighter /
        // thicker while this row is emphasised).
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

        indexLabel_.setBounds (b.removeFromLeft (18));
        b.removeFromLeft (4);
        dragGrip_->setBounds (b.removeFromLeft (44));   // drag-grip touch target (unified)
        b.removeFromLeft (8);

        // Source + dest combos: proportional, floored so the choice text stays legible.
        const int comboW = juce::jmax (70, b.getWidth() / 5);
        sourceCombo_.setBounds (b.removeFromLeft (comboW));
        b.removeFromLeft (14);   // arrow gap
        destCombo_.setBounds (b.removeFromLeft (juce::jmax (70, b.getWidth() / 4)));
        b.removeFromLeft (8);

        // Right-aligned controls: Mute, Clear, value, then slider fills the rest.
        muteButton_.setBounds (b.removeFromRight (44));   // mute touch target (unified)
        b.removeFromRight (8);
        clearButton_.setBounds (b.removeFromRight (juce::jmax (44, b.getWidth() / 8)));
        b.removeFromRight (8);
        valueLabel_.setBounds (b.removeFromRight (46));
        b.removeFromRight (8);

        // iOS HIG: the depth slider fills the remaining row area, so on the 48pt
        // row it is ~44pt tall -> a large invisible hit zone while the visual
        // thumb (BipolarSliderLNF) stays small. No custom hitTest needed.
        depthSlider_.setBounds (b);
    }

    FxMatrixView& owner_;
    const int slot_;

    juce::Label    indexLabel_;
    std::unique_ptr<FxSourceDragGrip> dragGrip_;   // drag source (parvatiModSrc payload)
    juce::ComboBox sourceCombo_;
    juce::ComboBox destCombo_;
    juce::Slider   depthSlider_;
    juce::Label    valueLabel_;
    juce::TextButton muteButton_;
    juce::TextButton clearButton_;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> srcAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   depthAttach_;
};

//==============================================================================
FxMatrixView::FxMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : processor_ (processor), themeManager_ (themeManager)
{
    bipolarLnf_ = std::make_unique<BipolarSliderLNF> (themeManager_);

    headerLabel_.setText (TRANS ("0 of 16 Used"), juce::dontSendNotification);
    headerLabel_.setJustificationType (juce::Justification::centredLeft);
    headerLabel_.setFont (appFontOr (*this, 13.0f));
    addAndMakeVisible (headerLabel_);

    addButton_ = std::make_unique<juce::TextButton> (TRANS ("+ Add Modulation"));
    addButton_->setTooltip (TRANS ("Assign the next free slot"));
    addButton_->onClick = [this] { addSlot(); };
    content_.addAndMakeVisible (*addButton_);

    viewport_.setScrollBarsShown (true, false, false, false);   // vertical scroll only (shown when content overflows)
    viewport_.setViewedComponent (&content_, false);            // view owns content_, not its deletion
    addAndMakeVisible (viewport_);

    for (int i = 0; i < kNumFxMatrixSlots; ++i)
    {
        rows_[(size_t) i] = std::make_unique<FxMatrixRow> (*this, i, *bipolarLnf_);
        content_.addAndMakeVisible (*rows_[(size_t) i]);
    }

    applyThemeColors();
    startTimerHz (30);   // stay live across preset load / undo / automation (message thread)
    refresh();

    // Register the FX-domain assign handler on the ModMatrixHighlight bus so a
    // drag-and-drop onto an FX parameter knob (which calls requestAssign with an
    // FX-offset dest) assigns the next free FX mod slot. The dest arrives encoded
    // (FX_DST_* + kFxModDstOffset); decode + guard here so synth dests
    // (< kFxModDstOffset) are ignored (the synth handler owns those). The guard
    // also keeps a hovered-synth-knob drop from grabbing an FX slot.
    // assignNextFreeSlot keeps its RAW FX_DST index contract (0..17).
    juce::Component::SafePointer<FxMatrixView> safe (this);
    assignSub_ = parvati::ModMatrixHighlight::instance().onAssignRequest (
        [safe] (int source, int dest) -> bool
        {
            if (safe == nullptr || ! parvati::ModDestMap::isFxDest (dest))
                return false;   // ignore synth-dest drops
            const int raw = dest - parvati::ModDestMap::kFxModDstOffset;   // FX_DST_* index
            if (raw >= FX_DST_LAST)
                return false;   // defensive: out-of-range FX dest (unreachable via modDest_)
            return safe->assignNextFreeSlot (source, raw);
        });

    // GAP 1: react to a dest-highlight broadcast (hover/drag over an FX knob or
    // another FX row) by highlighting every FX row routed to that dest. The
    // broadcast carries an FX-offset dest (FX_DST_* + kFxModDstOffset); synth
    // broadcasts (0..18) are rejected by onHighlightDest's isFxDest guard.
    destHighlightSub_ = parvati::ModMatrixHighlight::instance().onDestHighlighted (
        [safe] (int modDst) { if (safe != nullptr) safe->onHighlightDest (modDst); });
    // GAP 2: react to a knob double-click (an offset-encoded slot index) by
    // scrolling the row into view and flashing it. Synth-encoded slots (0..13)
    // are rejected by onSelectSlot's isFxDest guard.
    slotSelectSub_ = parvati::ModMatrixHighlight::instance().onSlotSelected (
        [safe] (int slot) { if (safe != nullptr) safe->onSelectSlot (slot); });
}

FxMatrixView::~FxMatrixView()
{
    stopTimer();

    if (destHighlightSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (destHighlightSub_);
    if (slotSelectSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (slotSelectSub_);
    if (assignSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (assignSub_);

    // Mute is SESSION-ONLY: restore every stashed amount so the persisted APVTS
    // state never contains the mute-induced zeros (mirrors ModMatrixView's
    // convention-A mute contract). The editor/view is destroyed BEFORE the
    // processor serializes state, so the saved state reflects the restored
    // (un-muted) values.
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
        if (muted_[(size_t) i])
            setAmountForSlot (i, stashedAmount_[(size_t) i]);
}

//==============================================================================
juce::String FxMatrixView::slotParam (int slot, const char* suffix)
{
    return "fxmod" + juce::String (slot + 1) + suffix;
}

int FxMatrixView::amountForSlot (int slot) const
{
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_amount")))
        return juce::jlimit (-63, 63, juce::roundToInt (raw->load()));
    return 0;
}

void FxMatrixView::setAmountForSlot (int slot, int amount)
{
    amount = juce::jlimit (-63, 63, amount);
    if (auto* p = processor_.getApvts().getParameter (slotParam (slot, "_amount")))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (amount)));
}

bool FxMatrixView::isSlotActive (int slot) const
{
    return amountForSlot (slot) != 0 || muted_[(size_t) juce::jlimit (0, 15, slot)];
}

int FxMatrixView::firstFreeSlot() const
{
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
        if (amountForSlot (i) == 0 && ! muted_[(size_t) i])
            return i;
    return -1;
}

juce::String FxMatrixView::sourceNameForSlot (int slot) const
{
    const juce::String id = slotParam (slot, "_source");
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

int FxMatrixView::sourceForSlot (int slot) const
{
    slot = juce::jlimit (0, 15, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_source")))
        return juce::roundToInt (raw->load());
    return -1;
}

int FxMatrixView::destForSlot (int slot) const
{
    slot = juce::jlimit (0, 15, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_dest")))
        return juce::roundToInt (raw->load());
    return -1;
}

void FxMatrixView::onHighlightDest (int modDst)
{
    // FX-domain guard: synth broadcasts (0..18) and the -1 clear never match an
    // FX dest. Decode the offset to a raw FX_DST_* index and highlight every row
    // whose current dest matches (read live so a just-edited dest combo follows).
    const bool active = modDst >= 0 && parvati::ModDestMap::isFxDest (modDst);
    const int raw = active ? modDst - parvati::ModDestMap::kFxModDstOffset : -1;
    for (const auto& r : rows_)
        if (r)
            r->setHighlighted (active && destForSlot (r->slot_) == raw);
}

void FxMatrixView::onSelectSlot (int slotIndex)
{
    // Clear any prior flash, then (for an FX-encoded slot) flash + scroll that
    // row in. Synth-encoded slots (0..13) are rejected by the isFxDest guard so
    // a synth knob double-click never flashes an FX row.
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    if (slotIndex < 0 || ! parvati::ModDestMap::isFxDest (slotIndex))
        return;

    const int s = slotIndex - parvati::ModDestMap::kFxModDstOffset;
    if (s < 0 || s >= kNumFxMatrixSlots)
        return;

    flashSlots_.add (s);
    flashStartMs_ = juce::Time::getMillisecondCounter();

    if (rows_[(size_t) s] != nullptr)
    {
        rows_[(size_t) s]->setFlashed (true);
        ensureRowVisible (s);
    }
}

std::array<FxType, 3> FxMatrixView::currentSlotTypes() const
{
    // The per-slot FX types (fx1/2/3_type), read live so a type edit / preset
    // load / part switch is picked up on the next refresh() tick. getRawParameterValue
    // on an AudioParameterChoice returns the choice index directly (= the FxType).
    std::array<FxType, 3> types { FxType::None, FxType::None, FxType::None };
    auto& apvts = processor_.getApvts();
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    for (int s = 0; s < 3; ++s)
    {
        const juce::String id = "fx" + juce::String (s + 1) + "_type";
        if (auto* raw = apvts.getRawParameterValue (id))
            types[(size_t) s] = static_cast<FxType> (
                juce::jlimit (0, kLast, juce::roundToInt (raw->load())));
    }
    return types;
}

void FxMatrixView::ensureRowVisible (int slot)
{
    if (slot < 0 || slot >= kNumFxMatrixSlots || rows_[(size_t) slot] == nullptr)
        return;
    // Scroll the Viewport the minimal amount so the row is fully on screen.
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

void FxMatrixView::flashTick()
{
    // Auto-expire the transient source-flash (driven from the FX workspace's
    // CentralModBar drag-only pill click).
    if (! flashSlots_.isEmpty()
        && (juce::Time::getMillisecondCounter() - flashStartMs_) > (juce::uint32) kFlashMs)
    {
        for (const int s : flashSlots_)
            if (s >= 0 && s < kNumFxMatrixSlots && rows_[(size_t) s] != nullptr)
                rows_[(size_t) s]->setFlashed (false);
        flashSlots_.clearQuick();
    }
}

void FxMatrixView::flashRowsForSource (int sourceEnum)
{
    // Clear any prior flash, then flash every ACTIVE row currently routed FROM
    // @p sourceEnum (read live so a freshly-edited source combo follows). The
    // first matching row is scrolled into view; the flash auto-expires via
    // flashTick() on the timer.
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    int firstVisible = -1;
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
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

void FxMatrixView::toggleMute (int slot)
{
    slot = juce::jlimit (0, 15, slot);
    if (muted_[(size_t) slot])
    {
        // Unmute: restore the stashed amount (true modulation resumes).
        muted_[(size_t) slot] = false;
        setAmountForSlot (slot, stashedAmount_[(size_t) slot]);
    }
    else
    {
        // Mute: stash the live amount, write 0 (true engine bypass), keep visible.
        stashedAmount_[(size_t) slot] = amountForSlot (slot);
        muted_[(size_t) slot] = true;
        setAmountForSlot (slot, 0);
    }
    refresh();
}

void FxMatrixView::clearSlot (int slot)
{
    slot = juce::jlimit (0, 15, slot);
    // Zero the amount (frees the slot by convention) and drop any mute. The
    // source/dest selections are left intact (non-destructive).
    muted_[(size_t) slot] = false;
    setAmountForSlot (slot, 0);
    refresh();
}

void FxMatrixView::addSlot()
{
    // Defaults: ENV 1 -> FX1 Dry/Wet at +50% (a visible, classic routing). The
    // default FX dest index 0 == FX_DST_FX1_DRYWET (the first makeFxDests() entry).
    assignNextFreeSlot (ambika::dsp::MOD_SRC_ENV_1, 0, 32);
}

bool FxMatrixView::assignNextFreeSlot (int sourceEnum, int destEnum, int amount)
{
    const int s = firstFreeSlot();
    if (s < 0)
        return false;   // matrix full — caller (button) is a no-op

    muted_[(size_t) s] = false;
    auto setChoice = [this] (int slot, const char* suffix, int choiceIndex)
    {
        if (auto* p = processor_.getApvts().getParameter (slotParam (slot, suffix)))
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (choiceIndex)));
    };
    setChoice (s, "_source", sourceEnum);
    setChoice (s, "_dest",   destEnum);
    setAmountForSlot (s, amount);   // a visible non-zero depth so the row appears
    refresh();
    return true;
}

//==============================================================================
juce::String FxMatrixView::buildSignature() const
{
    juce::String s;
    s.preallocateBytes (16);
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
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

void FxMatrixView::refresh()
{
    // Reconcile transient mute with external amount changes: if a muted slot's
    // amount was changed outside (preset load / undo / automation to non-zero),
    // the mute is stale — drop it so the row reflects reality.
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
        if (muted_[(size_t) i] && amountForSlot (i) != 0)
            muted_[(size_t) i] = false;

    // Dynamic FX-dest labels: when a slot's FX type changes (a type edit or a
    // part switch, which reloads fx{N}_type) rebuild every row's dest combo to
    // the slots' actual parameter names. The dest combo has NO APVTS attachment
    // (it is index-bound manually), so its item list + selection are reconciled
    // here every tick regardless of the active-set signature below.
    const auto types = currentSlotTypes();
    if (types != lastSlotTypes_)
    {
        lastSlotTypes_ = types;
        for (const auto& r : rows_)
            if (r)
                r->rebuildDestItems (types[0], types[1], types[2]);
    }
    for (const auto& r : rows_)
        if (r)
            r->syncDestFromParam();

    const auto sig = buildSignature();
    if (sig == lastSignature_)
        return;
    lastSignature_ = sig;
    rebuildLayout();
    repaint();
}

void FxMatrixView::rebuildLayout()
{
    const int w = juce::jmax (0, content_.getWidth());

    int y = 4;
    int activeCount = 0;
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
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

    // "+ Add Modulation" row at the bottom of the active list (or "Matrix Full").
    // Full row height (44pt HIG target): the rows above scroll inside the
    // Viewport, so the taller button costs nothing but scroll length.
    const bool full = (activeCount >= kNumFxMatrixSlots);
    addButton_->setButtonText (full ? TRANS ("Matrix Full (16/16)") : TRANS ("+ Add Modulation"));
    addButton_->setEnabled (! full);
    addButton_->setBounds (4, y + 4, juce::jmax (0, w - 8), kAddButtonH);
    y += kAddButtonH + 8;

    headerLabel_.setText (juce::String (activeCount) + " " + TRANS ("of 16 Used"),
                          juce::dontSendNotification);

    content_.setSize (w, y);
}

//==============================================================================
void FxMatrixView::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

void FxMatrixView::resized()
{
    auto area = getLocalBounds().reduced (4);

    headerLabel_.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);
    viewport_.setBounds (area);

    // Size the scrolled content to the viewport width minus the (always-
    // reserved) vertical-scrollbar thickness, so the rows are never clipped
    // behind the scrollbar. Height is recomputed in rebuildLayout().
    const int cw = juce::jmax (0, viewport_.getWidth() - viewport_.getScrollBarThickness());
    content_.setSize (cw, content_.getHeight());
    rebuildLayout();
}

void FxMatrixView::applyThemeColors()
{
    const auto& t = themeManager_.getCurrentTheme();
    headerLabel_.setColour (juce::Label::textColourId, t.textPrimary);
    headerLabel_.setFont (appFontOr (*this, 13.0f));

    addButton_->setColour (juce::TextButton::buttonColourId, t.containerFill);
    addButton_->setColour (juce::TextButton::buttonOnColourId, t.accentPrimary);
    addButton_->setColour (juce::TextButton::textColourOffId, t.textPrimary);
    addButton_->setColour (juce::TextButton::textColourOnId, t.backgroundBase);

    content_.bg = t.backgroundBase;
    content_.setOpaque (true);
    content_.repaint();
    for (const auto& r : rows_)
        if (r) r->applyThemeColors();

    rebuildLayout();
    repaint();
}

void FxMatrixView::visibilityChanged()
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
