// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ModMatrixView.h.

#include "ModMatrixView.h"

#include "ModMatrixHighlight.h"
#include "ModDestMap.h"   // kFxModDstOffset (synth/FX dest-domain guard)
#include "PluginProcessor.h"   // ParvatiAudioProcessor (complete type)
#include "ThemeManager.h"
#include "ParvatiTheme.h"
#include "ParvatiLookAndFeel.h"   // appFont() via the inherited editor L&F
#include "IconButton.h"           // IconButton (row delete X)
#include "dsp/patch.h"            // ambika::dsp::MOD_SRC_*, MOD_DST_*, kNumModulations

#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachments + AudioParameterChoice

// The view assumes the engine's 14-slot mod matrix. If that ever changes, the
// 14-row UI must be revisited — this trips the build early.
static_assert (ambika::dsp::kNumModulations == 14, "ModMatrixView assumes a 14-slot mod matrix");

//==============================================================================
namespace
{
// Category colour for a mod-source display name, mirroring the STRICT family
// palette (and ModSourceCatalog::clusterAccent): Env=teal, LFO=magenta,
// Seq/Arp=mint, Op(modifier)=purple, Const=indigo, keyboard/Perf=amber,
// Gate/Noise/Random=orange. Returns a transparent Colour only for a source
// name with no known family.
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
// neutral accent. Every row therefore resolves to a concrete colour (never
// transparent), so the full-row tint + depth-slider fill + source-combo colour
// tag always carry the row's family hue.
juce::Colour rowCategoryColour (const ParvatiTheme& t, const juce::String& sourceName)
{
    const juce::Colour cat = sourceCategoryColour (t, sourceName);
    return cat.isTransparent() ? t.accentPrimary : cat;
}

// A signed amount (-63..+63) -> "+100%" / "0%" / "-50%". The slider/engine use
// ±63 as full-scale, so 63 maps to 100%.
juce::String formatPercent (int amount)
{
    const int pct = juce::roundToInt (static_cast<double> (amount) * 100.0 / 63.0);
    return (pct > 0 ? "+" : juce::String()) + juce::String (pct) + "%";
}

// Resolve the app font through the inherited editor L&F when present, else the
// JUCE default (keeps the view usable before it is reparented into the editor).
juce::Font appFontOr (const juce::Component& c, float height)
{
    if (const auto* lnf = dynamic_cast<const ParvatiLookAndFeel*> (&c.getLookAndFeel()))
        return lnf->appFont (height, juce::Font::plain);
    return juce::Font (juce::FontOptions (height));
}

// Row-index label width (user feedback 2026-08-20: the slot number '16'
// truncated to '...' — JUCE Label's default 5px-per-side border left an 18pt
// label an 8px text box). Measured from the 12pt app font's WIDEST index
// text ('16') + 6px slack, floored at the old 18. A compile-time constant
// (not per-row measurement) so every row is identical and the tests can pin
// it: 13px + 6 = 19pt for the default sans-serif at 12pt.
constexpr int kMatrixIndexLabelW = 20;
}  // namespace

//==============================================================================
// Local bipolar depth-slider LookAndFeel: a thin track, a fill drawn FROM the
// CENTRE to the value (accent right of centre / accent2 left of centre), a
// centre zero-detent tick and a thumb. Self-contained — never touches the shared
// ParvatiLookAndFeel.
class ModMatrixView::BipolarSliderLNF : public juce::LookAndFeel_V4
{
public:
    explicit BipolarSliderLNF (ThemeManager& tm) : themeManager_ (tm) {}

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           juce::Slider::SliderStyle /*style*/, juce::Slider& slider) override
    {
        const auto& t = themeManager_.getCurrentTheme();
        // Flat bipolar depth slider: a clean DARK track (the rotary knob-track
        // token so the slider echoes the knob tracks), a solid fill from the
        // CENTRE to the value in this ROW's category colour (the row's
        // routing-source category — same tokens the knob rings use; neutral
        // sources -> accent; the NEGATIVE side is a dimmed same-hue version for
        // sign readability), a centre zero-detent tick and a flat solid CIRCLE
        // thumb (no 3D/gradient). Geometry/positions are unchanged.
        const juce::Colour trackCol = t.trackEmpty;
        // Per-row category fill (pushed onto the slider as "parvatiRowFill" by
        // ModMatrixRow::applyThemeColors); falls back to the accent when unset.
        const juce::Colour rowFill = [&]
        {
            const auto* v = slider.getProperties().getVarPointer ("parvatiRowFill");
            return (v != nullptr && v->isInt()) ? juce::Colour ((uint32_t) (int) *v) : t.accentPrimary;
        }();
        const juce::Colour posFill  = rowFill;
        const juce::Colour negFill  = rowFill.darker (0.40f);
        const juce::Colour thumbCol = t.textPrimary;

        const float cy      = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        // BIGGER visual slider, SMALLER thumb (2026-08-20): track 4 -> 7pt so
        // the depth control reads at a glance, thumb fixed at 15pt — the same
        // diameter as the enable/disable lamp — instead of 0.30*height (~13-30pt
        // depending on the band).
        const float trackH  = 7.0f;
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

        // Flat solid circle thumb (no 3D/gradient / no outline ring), fixed
        // at the lamp diameter (15pt) regardless of the band height.
        const float tr = 7.5f;
        const auto  thumbRect = juce::Rectangle<float> (tr * 2.0f, tr * 2.0f).withCentre (juce::Point<float> (sp, cy));
        g.setColour (slider.isEnabled() ? thumbCol : thumbCol.withAlpha (0.4f));
        g.fillEllipse (thumbRect);
    }

private:
    ThemeManager& themeManager_;
};

//==============================================================================
// MuteLamp — the per-row mute/bypass toggle (user feedback 2026-08-20): the
// SAME widget style as the FX slot module-disable toggle (FxSlotCard's
// PowerToggle — a compact bordered indicator dot with a full-bounds hit area),
// in the mod matrix's accent family. Dot = accentPrimary while the routing is
// ACTIVE, the theme's inactive grey (textDisabled) while MUTED (accent = "on"
// parity with the FX power lamps). The toggle STATE is driven by the row
// (setMutedLook); a click routes through the view's toggleMute() — the SAME
// stash-amount/restore seam the old text "M" button used (editor-only mute,
// never persisted).
class MuteLamp final : public ParvatiModuleLamp {};

//==============================================================================
// One matrix row. Owns its source/dest combos + bipolar depth slider, each bound
// to its slot's APVTS params via attachments that live for the view's lifetime
// (visibility toggles, attachments never churn). Buttons call back into the view.
struct ModMatrixRow : public juce::Component,
                      private juce::Slider::Listener,
                      private juce::ComboBox::Listener
{
    ModMatrixRow (ModMatrixView& owner, int slot, juce::LookAndFeel& sliderLnf)
        : owner_ (owner), slot_ (slot)
    {
        auto& apvts = owner_.processor().getApvts();
        const juce::String srcId = ModMatrixView::slotParam (slot, "_source");
        const juce::String dstId = ModMatrixView::slotParam (slot, "_dest");
        const juce::String amtId = ModMatrixView::slotParam (slot, "_amount");

        // Populate the combos from the (already-registered) choice params so the
        // row stays in lock-step with kModSources / kModDests without duplicating
        // the string lists.
        if (auto* sp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (srcId)))
            sourceCombo_.addItemList (sp->choices, 1);
        if (auto* dp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (dstId)))
            destCombo_.addItemList (dp->choices, 1);

        indexLabel_.setText (juce::String (slot + 1), juce::dontSendNotification);
        indexLabel_.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (indexLabel_);

        // Mute/bypass LAMP (module-disable widget parity; see MuteLamp) — the
        // LEFTMOST control of the row. Click routes through the view's
        // toggleMute() (the same stash/restore seam the old "M" text button
        // used; editor-only mute, never persisted).
        muteLamp_ = std::make_unique<MuteLamp>();
        muteLamp_->setTitle (TRANS ("Mute / bypass this modulation"));
        muteLamp_->setTooltip (TRANS ("Mute / bypass this modulation"));
        muteLamp_->onClick = [this] { owner_.toggleMute (slot_); };
        // FX-module enable/disable SIZE (2026-08-20): pin the drawn dot to the
        // FX card's 15pt diameter instead of the proportional ~30pt the tall
        // 44pt row band renders — same visual size as the FX modules' toggle.
        muteLamp_->setLampDiameter (15.0f);
        addAndMakeVisible (*muteLamp_);

        addAndMakeVisible (sourceCombo_);
        addAndMakeVisible (destCombo_);

        depthSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
        depthSlider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        depthSlider_.setDoubleClickReturnValue (true, 0.0);   // double-click = reset to 0 (centre)
        depthSlider_.setLookAndFeel (&sliderLnf);
        addAndMakeVisible (depthSlider_);

        valueLabel_.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (valueLabel_);

        // Delete X (IconButton, path-drawn glyph) — the RIGHTMOST control of
        // the row, replacing the old "Clear" text button. 44pt HIG hit target
        // with a visually compact glyph (setGlyphInset). Same action as Clear:
        // free the slot through the view's clearSlot().
        clearButton_ = std::make_unique<IconButton> (IconButton::Icon::Close);
        clearButton_->setTooltip (TRANS ("Delete modulation"));
        clearButton_->setGlyphInset (11.0f);
        clearButton_->onClick = [this] { owner_.clearSlot (slot_); };
        addAndMakeVisible (*clearButton_);

        // Bind to the APVTS AFTER the widgets are populated.
        srcAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, srcId, sourceCombo_);
        dstAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, dstId, destCombo_);
        depthAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>  (apvts, amtId, depthSlider_);

        depthSlider_.addListener (this);
        // Re-resolve the row's category colour (row tint + slider fill + combo
        // tint) whenever a new source is picked so it follows live.
        sourceCombo_.addListener (this);
        refreshValueDisplay();

        // Register this row as a MouseListener on every child so a hover anywhere
        // over the row (combos / slider / buttons / labels) drives the dest
        // highlight bus, not just the few-pixel bare gaps between widgets.
        // `false` => child events only (no recursion into popup children).
        indexLabel_.addMouseListener (this, false);
        muteLamp_->addMouseListener (this, false);
        sourceCombo_.addMouseListener (this, false);
        destCombo_.addMouseListener (this, false);
        depthSlider_.addMouseListener (this, false);
        valueLabel_.addMouseListener (this, false);
        clearButton_->addMouseListener (this, false);

        // Accessibility-only: name the row after its slot (matches the visible
        // index label; "Mod " + N follows the suffix-key i18n idiom).
        setTitle (TRANS ("Mod ") + juce::String (slot_ + 1));
    }

    ~ModMatrixRow() override
    {
        depthSlider_.removeListener (this);
        sourceCombo_.removeListener (this);
        indexLabel_.removeMouseListener (this);
        muteLamp_->removeMouseListener (this);
        sourceCombo_.removeMouseListener (this);
        destCombo_.removeMouseListener (this);
        depthSlider_.removeMouseListener (this);
        valueLabel_.removeMouseListener (this);
        clearButton_->removeMouseListener (this);
        // Drop the custom L&F before the slider is destroyed (the L&F is owned by
        // the view and outlives this row, but unsetting keeps the contract clean).
        depthSlider_.setLookAndFeel (nullptr);
    }

    // Accessibility-only: a labelled `group` container for this row's widgets
    // (source/dest combos, depth slider, M/Clear), so screen readers announce
    // the row as a structured unit ("Mod 1, group") instead of an unnamed
    // stream of controls. Title is set in the ctor; children keep their own
    // built-in handlers. Follows the EnvelopeDisplay/FxRoutingBar pattern.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this,
                juce::AccessibilityRole::group);
    }

    void sliderValueChanged (juce::Slider*) override { refreshValueDisplay(); }

    void comboBoxChanged (juce::ComboBox*) override
    {
        // A new source was picked: re-resolve this row's category colour (the
        // full-row tint, the depth-slider fill and the source-combo tint all
        // live in applyThemeColors).
        applyThemeColors();
        repaint();
    }

    // ---- Hover / selection emphasis (driven via the ModMatrixHighlight bus) ----
    void mouseEnter (const juce::MouseEvent&) override
    {
        parvati::ModMatrixHighlight::instance().setHighlightedDest (owner_.destForSlot (slot_));
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        // Only clear when the mouse has actually left the row: moving between the
        // row's child widgets also fires mouseExit (this row is their MouseListener).
        const auto rel = e.getEventRelativeTo (this);
        if (! getLocalBounds().contains (rel.position.toInt()))
            parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
    }

    // Hovered: this row's dest is the highlighted target.
    void setHighlighted (bool on) { if (highlighted_ != on) { highlighted_ = on; repaint(); } }
    // Flashed: a knob's double-click jumped the matrix to this slot.
    void setFlashed (bool on) { if (flashed_ != on) { flashed_ = on; repaint(); } }
    bool highlighted_ = false;
    bool flashed_ = false;

    void refreshValueDisplay()
    {
        const int amt = owner_.isSlotMuted (slot_) ? owner_.stashedAmount (slot_)
                                                    : owner_.amountForSlot (slot_);
        valueLabel_.setText (formatPercent (amt), juce::dontSendNotification);
    }

    void setMutedLook (bool muted)
    {
        sourceCombo_.setEnabled (! muted);
        destCombo_.setEnabled (! muted);
        depthSlider_.setEnabled (! muted);
        // Lamp semantics = module-disable parity: accent (toggle ON) while the
        // routing is ACTIVE, grey while muted.
        muteLamp_->setToggleState (! muted, juce::dontSendNotification);
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

        // SOURCE combo: uniformly DARK dropdown (the fill is drawn by the shared
        // drawComboBox, which ignores the per-combo background colour) tagged with
        // a 4px family-colour STRIP on its far-left edge. The strip colour is this
        // row's routing-source FAMILY (the same cat*/accent token the knob rings +
        // row tint resolve to) — it tags the family WITHOUT colouring the dropdown
        // fill. The DEST combo gets NO tag (just dark + white).
        const juce::Colour famCol = rowCategoryColour (t, owner_.sourceNameForSlot (slot_));
        // Lamp ON colour = the row's modulator category colour (2026-08-20:
        // the user asked the enable/disable button to carry the modulator's
        // colour, matching the row tint / slider fill / combo tag).
        muteLamp_->setOnColour (famCol);
        sourceCombo_.getProperties().set ("parvatiComboTag", (int) famCol.getARGB());
        sourceCombo_.removeColour (juce::ComboBox::backgroundColourId);
        destCombo_.getProperties().remove ("parvatiComboTag");
        destCombo_.removeColour (juce::ComboBox::backgroundColourId);

        // Per-row depth-slider fill colour: this row's routing-source CATEGORY
        // colour (the same tokens the knob modulation rings use; neutral sources
        // -> accent). BipolarSliderLNF reads "parvatiRowFill" so each slider
        // matches its own row. Re-applied here on every build / source change /
        // theme switch (the negative side is a dimmed same-hue version, derived
        // in the LNF).
        depthSlider_.getProperties().set ("parvatiRowFill",
            (int) rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).getARGB());

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = owner_.themeManager().getCurrentTheme();

        // FULL-ROW CATEGORY TINT: a very low-opacity (0.08) fill of this row's
        // routing-source category colour — the same cat*/accent tokens the knob
        // modulation rings resolve to — so the WHOLE row reads in its source's
        // hue at a glance (in addition to the thin left stripe below). Flat;
        // painted first so a stronger flash/highlight overlay still wins.
        g.setColour (rowCategoryColour (t, owner_.sourceNameForSlot (slot_)).withAlpha (0.08f));
        g.fillAll();

        // Emphasis when this row is the highlighted modulation target (hovered
        // knob or matching row) or the transient selection (knob double-click).
        // A faint accent background makes the row read as the target; the flash
        // (double-click jump) is the stronger of the two.
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

        // F-ios-touch (bug hunt 2026-08-19): the right-column ACTION targets
        // and the value readout are FIXED 44pt-floor widgets — reserve them
        // FIRST so a narrow row squeezes the proportional COMBOS (their choice
        // text scrolls), never the buttons. The delete X is the RIGHTMOST
        // control (user feedback 2026-08-20); the value readout sits left of it.
        clearButton_->setBounds (b.removeFromRight (44));   // delete X hit target (unified)
        b.removeFromRight (8);
        valueLabel_.setBounds (b.removeFromRight (46));
        b.removeFromRight (8);

        // LEFT cluster: the mute/bypass lamp (module-disable widget parity,
        // user feedback 2026-08-20), then the row index. The former drag-grip
        // slot is gone — rows are NOT drag sources; modulators are dragged
        // only from the CentralModBar pills.
        muteLamp_->setBounds (b.removeFromLeft (44));   // mute lamp hit target (unified)
        b.removeFromLeft (4);
        // INDEX LABEL (user feedback 2026-08-20: '16' showed as "..."): JUCE
        // Label's DEFAULT border is 5px per side, so an 18pt-wide label leaves
        // an 8px text box for "16" (13px at the 12pt app font) -> ellipsis.
        // Border zeroed (the text is centred anyway) and the width measured
        // from the font: text + 6px slack, floored at 18.
        indexLabel_.setBorderSize (juce::BorderSize<int> (0));
        indexLabel_.setBounds (b.removeFromLeft (kMatrixIndexLabelW));
        b.removeFromLeft (4);

        // Source + dest combos share the REST with the depth slider. The
        // SLIDER leads (user feedback 2026-08-20: sliders much wider, combos
        // leaner): it takes the width remaining after the combos' measured
        // floors — the combo floors are fit-to-widest-item, capped so a long
        // choice list can never push the slider under ~35% of the row.
        const int rowW = b.getWidth();
        const int sliderFloor = juce::jmax (96, rowW * 35 / 100);
        const int comboBudget = juce::jmax (0, rowW - sliderFloor - 14 - 8);
        int srcW = juce::jmax (56, juce::jmin (comboBudget * 5 / 9, 130));
        int dstW = juce::jmax (60, juce::jmin (comboBudget - 14 - srcW, 150));
        // When even the floors cannot fit, shrink the SOURCE first (the
        // narrower semantic: the fixed source list), then hard-floor both at
        // 44 — the HIG minimum for a functional combo on touch.
        if (srcW + 14 + dstW > comboBudget)
            srcW = juce::jmax (44, comboBudget - 14 - dstW);
        if (srcW + 14 + dstW > comboBudget)
            dstW = juce::jmax (44, comboBudget - 14 - srcW);
        sourceCombo_.setBounds (b.removeFromLeft (srcW));
        b.removeFromLeft (14);   // arrow gap
        destCombo_.setBounds (b.removeFromLeft (dstW));
        b.removeFromLeft (8);

        // iOS HIG: the depth slider fills the remaining row area, so on the 48pt
        // row it is ~44pt tall -> a large invisible hit zone while the visual
        // thumb (BipolarSliderLNF) stays small. No custom hitTest needed.
        depthSlider_.setBounds (b);
    }

    ModMatrixView& owner_;
    const int slot_;

    juce::Label    indexLabel_;
    std::unique_ptr<MuteLamp>  muteLamp_;    // mute/bypass lamp (module-disable parity)
    juce::ComboBox sourceCombo_;
    juce::ComboBox destCombo_;
    juce::Slider   depthSlider_;
    juce::Label    valueLabel_;
    std::unique_ptr<IconButton> clearButton_;   // delete X (far right)

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> srcAttach_, dstAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   depthAttach_;
};

//==============================================================================
ModMatrixView::ModMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : processor_ (processor), themeManager_ (themeManager)
{
    bipolarLnf_ = std::make_unique<BipolarSliderLNF> (themeManager_);

    headerLabel_.setText (TRANS ("0 of 14 Used"), juce::dontSendNotification);
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

    for (int i = 0; i < 14; ++i)
    {
        rows_[(size_t) i] = std::make_unique<ModMatrixRow> (*this, i, *bipolarLnf_);
        content_.addAndMakeVisible (*rows_[(size_t) i]);
    }

    applyThemeColors();
    startTimerHz (30);   // stay live across preset load / undo / automation (message thread)
    refresh();

    // Subscribe to the highlight bus so (a) a hovered knob / matching row glows
    // every row routed to the same dest, and (b) double-clicking a knob's ring
    // scrolls + flashes the first active row here. The callbacks are
    // SafePointer-guarded AND unsubscribed in the dtor.
    juce::Component::SafePointer<ModMatrixView> safe (this);
    destHighlightSub_ = parvati::ModMatrixHighlight::instance().onDestHighlighted (
        [safe] (int modDst) { if (safe != nullptr) safe->onHighlightDest (modDst); });
    slotSelectSub_ = parvati::ModMatrixHighlight::instance().onSlotSelected (
        [safe] (int slot) { if (safe != nullptr) safe->onSelectSlot (slot); });
    assignSub_ = parvati::ModMatrixHighlight::instance().onAssignRequest (
        [safe] (int source, int dest) -> bool { return safe != nullptr && safe->assignNextFreeSlot (source, dest); });
}

ModMatrixView::~ModMatrixView()
{
    stopTimer();

    // Mute is SESSION-ONLY: restore every stashed amount so the persisted APVTS
    // state (and thus host/standalone state + presets) never contains the
    // mute-induced zeros that would silently DELETE routings across a restart
    // (e.g. muting ENV->VCA then quitting used to leave the amp envelope at 0
    // forever). During the session the engine was truly bypassed (amount==0);
    // here we write the real amount back. The editor/view is destroyed BEFORE
    // the processor serializes state, so the saved state reflects the restored
    // (un-muted) values. This is the convention-A mute contract: bypass is real
    // but never persists. (Edge case: a host that saves project state WHILE the
    // editor is open + a slot muted still captures the 0 — accepted; mute is
    // documented as not preset-safe.)
    for (int i = 0; i < 14; ++i)
        if (muted_[(size_t) i])
            setAmountForSlot (i, stashedAmount_[(size_t) i]);

    if (destHighlightSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (destHighlightSub_);
    if (slotSelectSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (slotSelectSub_);
    if (assignSub_ >= 0)
        parvati::ModMatrixHighlight::instance().unsubscribe (assignSub_);
}

//==============================================================================
juce::String ModMatrixView::slotParam (int slot, const char* suffix)
{
    return "mod" + juce::String (slot + 1) + suffix;
}

juce::Component* ModMatrixView::rowForSlotForTest (int slot) const
{
    slot = juce::jlimit (0, 13, slot);
    return rows_[(size_t) slot].get();
}

int ModMatrixView::amountForSlot (int slot) const
{
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_amount")))
        return juce::jlimit (-63, 63, juce::roundToInt (raw->load()));
    return 0;
}

void ModMatrixView::setAmountForSlot (int slot, int amount)
{
    amount = juce::jlimit (-63, 63, amount);
    if (auto* p = processor_.getApvts().getParameter (slotParam (slot, "_amount")))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (amount)));
}

bool ModMatrixView::isSlotActive (int slot) const
{
    return amountForSlot (slot) != 0 || muted_[(size_t) juce::jlimit (0, 13, slot)];
}

int ModMatrixView::firstFreeSlot() const
{
    for (int i = 0; i < 14; ++i)
        if (amountForSlot (i) == 0 && ! muted_[(size_t) i])
            return i;
    return -1;
}

juce::Colour ModMatrixView::rowCategoryColourForTest (int slot) const
{
    return rowCategoryColour (themeManager_.getCurrentTheme(), sourceNameForSlot (slot));
}

juce::String ModMatrixView::sourceNameForSlot (int slot) const
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

int ModMatrixView::sourceForSlot (int slot) const
{
    slot = juce::jlimit (0, 13, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_source")))
        return juce::roundToInt (raw->load());
    return -1;
}

int ModMatrixView::destForSlot (int slot) const
{
    slot = juce::jlimit (0, 13, slot);
    if (auto* raw = processor_.getApvts().getRawParameterValue (slotParam (slot, "_dest")))
        return juce::roundToInt (raw->load());
    return -1;
}

void ModMatrixView::onHighlightDest (int modDst)
{
    // Emphasise every row currently routed to the highlighted dest (read live so
    // a row whose dest combo was just edited follows immediately). -1 clears all.
    for (const auto& r : rows_)
        if (r)
            r->setHighlighted (modDst >= 0 && destForSlot (r->slot_) == modDst);
}

void ModMatrixView::onSelectSlot (int slotIndex)
{
    // Clear any prior flash, then (for a valid slot) flash + scroll that row in.
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    if (slotIndex < 0 || slotIndex >= 14)
        return;

    flashSlots_.add (slotIndex);
    flashStartMs_ = juce::Time::getMillisecondCounter();

    if (rows_[(size_t) slotIndex] != nullptr)
    {
        rows_[(size_t) slotIndex]->setFlashed (true);
        if (rows_[(size_t) slotIndex]->isVisible())
            ensureRowVisible (slotIndex);
    }
}

void ModMatrixView::ensureRowVisible (int slot)
{
    if (slot < 0 || slot >= 14 || rows_[(size_t) slot] == nullptr)
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

void ModMatrixView::flashTick()
{
    // Auto-expire the transient selection flash (single-slot jump or the
    // multi-row source-flash share this one timed expiry).
    if (! flashSlots_.isEmpty()
        && (juce::Time::getMillisecondCounter() - flashStartMs_) > (juce::uint32) kFlashMs)
    {
        for (const int s : flashSlots_)
            if (s >= 0 && s < 14 && rows_[(size_t) s] != nullptr)
                rows_[(size_t) s]->setFlashed (false);
        flashSlots_.clearQuick();
    }
}

void ModMatrixView::flashRowsForSource (int sourceEnum)
{
    // Clear any prior flash, then flash every ACTIVE row currently routed FROM
    // @p sourceEnum (read live so a freshly-edited source combo follows). The
    // first matching row is scrolled into view; the flash auto-expires via
    // flashTick() on the timer (same kFlashMs as the knob-double-click jump).
    for (const auto& r : rows_)
        if (r)
            r->setFlashed (false);
    flashSlots_.clearQuick();

    int firstVisible = -1;
    for (int i = 0; i < 14; ++i)
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

void ModMatrixView::toggleMute (int slot)
{
    slot = juce::jlimit (0, 13, slot);
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

void ModMatrixView::clearSlot (int slot)
{
    slot = juce::jlimit (0, 13, slot);
    // Zero the amount (frees the slot by convention) and drop any mute. The
    // source/dest selections are left intact (non-destructive); a later Add
    // overwrites the chosen free slot's source/dest/amount, so nothing leaks
    // back into the active list.
    muted_[(size_t) slot] = false;
    setAmountForSlot (slot, 0);
    refresh();
}

void ModMatrixView::addSlot()
{
    // Defaults: ENV 1 -> Filter Cutoff at +50% (a visible, classic routing).
    assignNextFreeSlot (ambika::dsp::MOD_SRC_ENV_1, ambika::dsp::MOD_DST_FILTER_CUTOFF, 32);
}

bool ModMatrixView::assignNextFreeSlot (int sourceEnum, int destEnum, int amount)
{
    // Defensive: a dest in the FX domain (offset >= kFxModDstOffset) is never a
    // synth target — reject it rather than let convertTo0to1 clamp it into a
    // bogus synth slot. The FX handler owns FX-dest drops.
    if (destEnum >= parvati::ModDestMap::kFxModDstOffset)
        return false;

    const int s = firstFreeSlot();
    if (s < 0)
        return false;   // matrix full — caller (button / drop) is a no-op

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
juce::String ModMatrixView::buildSignature() const
{
    juce::String s;
    s.preallocateBytes (16);
    for (int i = 0; i < 14; ++i)
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

void ModMatrixView::refresh()
{
    // Reconcile transient mute with external amount changes: if a muted slot's
    // amount was changed outside (preset load / undo / automation to non-zero),
    // the mute is stale — drop it so the row reflects reality.
    for (int i = 0; i < 14; ++i)
        if (muted_[(size_t) i] && amountForSlot (i) != 0)
            muted_[(size_t) i] = false;

    const auto sig = buildSignature();
    if (sig == lastSignature_)
        return;
    lastSignature_ = sig;
    rebuildLayout();
    repaint();
}

void ModMatrixView::rebuildLayout()
{
    const int w = juce::jmax (0, content_.getWidth());

    int y = 4;
    int activeCount = 0;
    for (int i = 0; i < 14; ++i)
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
    const bool full = (activeCount >= 14);
    addButton_->setButtonText (full ? TRANS ("Matrix Full (14/14)") : TRANS ("+ Add Modulation"));
    addButton_->setEnabled (! full);
    addButton_->setBounds (4, y + 4, juce::jmax (0, w - 8), kAddButtonH);
    y += kAddButtonH + 8;

    headerLabel_.setText (juce::String (activeCount) + " " + TRANS ("of 14 Used"),
                          juce::dontSendNotification);

    content_.setSize (w, y);
}

//==============================================================================
void ModMatrixView::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

void ModMatrixView::resized()
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

void ModMatrixView::applyThemeColors()
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

void ModMatrixView::visibilityChanged()
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
