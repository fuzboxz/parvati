// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotCard.h.

#include "FxSlotCard.h"
#include "FxSlotLabels.h"   // declarations for the activeParamCount/paramLabel defined below

#include "FxSlotVisualizer.h"
#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"
#include "PluginEditor.h"          // ParamControl complete type
#include "PluginProcessor.h"       // ParvatiAudioProcessor::getApvts()
#include "ParameterLayout.h"       // PatchParamDescriptor
#include "dsp/fx/FxTypes.h"        // FxType

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
//==============================================================================
// PowerToggle — the Enable/Bypass button. Draws the IEC 5009 power glyph (a
// vertical bar rising into an open-topped arc) with juce::Path, so there is NO
// unicode/font dependency (mirrors IconButton's glyph recipe). The glyph reads
// accentSecondary (the orange FX/bypass accent) when the slot is ENABLED and
// reads dimmed (textDisabled) when bypassed. Its toggle STATE is driven from the
// fx{N}_enable APVTS Value by FxSlotCard; clicking flips that Value (handled by
// the card's onClick), so this button does NOT toggle its own state on click.
class PowerToggle : public juce::Button
{
public:
    PowerToggle() : juce::Button ({}) { setClickingTogglesState (false); }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        const ParvatiTheme* t = nullptr;
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            t = lnf->getTheme();

        const juce::Colour accent = t ? t->accentSecondary : juce::Colour (0xffe8b84b);
        const juce::Colour text   = t ? t->textPrimary     : juce::Colour (0xffe8e8ee);
        const juce::Colour dim    = t ? t->textDisabled    : text.withAlpha (0.35f);

        juce::Colour c = (getToggleState() || isButtonDown) ? accent : dim;
        if (! getToggleState() && isMouseOverButton)
            c = text.brighter (0.20f);
        if (! isEnabled())
            c = text.withAlpha (0.25f);

        g.setColour (c);

        const auto r = getLocalBounds().toFloat().reduced (3.0f);
        const auto c2 = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;

        // Open-topped arc (~270 deg, gap at the top): two quadratic beziers that
        // leave a vertical slit through which the central bar rises.
        const float leftX  = c2.x - rad * 0.92f;
        const float rightX = c2.x + rad * 0.92f;
        const float topY   = c2.y - rad * 1.15f;
        const float botY   = c2.y + rad * 1.05f;

        juce::Path arc;
        arc.startNewSubPath (leftX, botY);
        arc.quadraticTo (c2.x - rad * 1.30f, c2.y, leftX, topY);          // lower-left -> upper-left
        arc.startNewSubPath (rightX, botY);
        arc.quadraticTo (c2.x + rad * 1.30f, c2.y, rightX, topY);         // lower-right -> upper-right
        g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));

        // Central vertical bar from the centre up to the top of the arc slit.
        g.drawLine (juce::Line<float> (c2.x, c2.y + rad * 0.35f, c2.x, topY), 2.2f);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerToggle)
};

// A small themed chevron button ("<" / ">") that steps the FX slot's effect
// TYPE selection prev/next — a shortcut alongside the algorithm ComboBox. Draws
// a juce::Path chevron (no font/unicode dependency) in the editor-wide theme
// colours (text dim at rest, accent on hover/press), mirroring IconButton.
class TypeStepButton : public juce::Button
{
public:
    explicit TypeStepButton (bool pointsRight) : juce::Button ({}), right_ (pointsRight)
    {
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        const ParvatiTheme* t = nullptr;
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            t = lnf->getTheme();
        const juce::Colour text   = t ? t->textPrimary   : juce::Colour (0xffe8e8ee);
        const juce::Colour dim    = t ? t->textDisabled   : text.withAlpha (0.45f);
        const juce::Colour accent = t ? t->accentPrimary : juce::Colour (0xffe8b84b);

        juce::Colour c = dim;
        if (isButtonDown)           c = accent;
        else if (isMouseOverButton) c = text.brighter (0.20f);
        if (! isEnabled())          c = text.withAlpha (0.25f);

        g.setColour (c);
        const auto r = getLocalBounds().toFloat().reduced (3.0f);
        const auto ctr = r.getCentre();
        const float h = juce::jmin (r.getWidth(), r.getHeight()) * 0.30f;   // chevron half-height
        const float dx = (right_ ? 1.0f : -1.0f) * h;                       // apex x-offset: ">" apex right, "<" apex left
        // A "<" (left) or ">" (right) chevron: two strokes meeting at the apex.
        juce::Path chev;
        chev.startNewSubPath (ctr.x + dx, ctr.y);          // apex
        chev.lineTo (ctr.x - dx, ctr.y - h);               // upper tail
        chev.startNewSubPath (ctr.x + dx, ctr.y);          // apex
        chev.lineTo (ctr.x - dx, ctr.y + h);               // lower tail
        g.strokePath (chev, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
    }

private:
    bool right_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TypeStepButton)
};

#if JUCE_IOS
// FxTypeCombo — the iOS effect picker. The whole "< None >" selector is one
// 44pt-tall tap target (HIG #4); the prev/next chevron step buttons are removed
// on iOS. JUCE's default ComboBox popup item rows (~24pt) are below the HIG
// touch minimum, so showPopup() rebuilds the popup from the combo's OWN items
// at a 44pt standard item height. Selecting an item writes through
// setSelectedItemIndex, which the ComboBoxAttachment syncs to the APVTS — so the
// byte-bridge + per-type knob refresh are unchanged from the desktop combo.
class FxTypeCombo : public juce::ComboBox
{
public:
    FxTypeCombo() : juce::ComboBox ({}) {}

    void showPopup() override
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        const int current = getSelectedItemIndex();
        for (int i = 0; i < getNumItems(); ++i)
        {
            juce::PopupMenu::Item item;
            item.text     = getItemText (i);
            item.itemID   = getItemId (i);
            item.isTicked = (i == current);
            item.action   = [this, i] { setSelectedItemIndex (i, juce::sendNotificationSync); };
            menu.addItem (std::move (item));
        }
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withStandardItemHeight (44),
                            nullptr);
    }
};
#endif

// Per-type ENGAGEMENT defaults applied the moment a user selects an effect
// type. The generic slot params all default to 0 — silent (Delay time=0,
// drywet=0 = fully dry, enabled=0 = bypassed) — so picking a type otherwise
// sounds like "nothing happened". These seed each effect with an audible,
// characteristic starting point. Applied from parameterChanged() ONLY on a real
// fx{N}_type change (never at construction). Because APVTS listener dispatch is
// synchronous on the message thread AND the descriptor order is type, enabled,
// drywet, param1..5, a preset/part load that sets type THEN params overrides
// these — so saved patches keep their own values.
struct FxTypeDefaults { uint8_t enabled; uint8_t drywet; uint8_t p[5]; };
FxTypeDefaults fxTypeDefaults (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:     return { 1,  40, {  0,  0,  0,  0,  0 } }; // (amount fixed 1.0; chain Dry/Wet is the mix)
        case FxType::PitchShifter: return { 1, 100, { 50, 50,  0,  0,  0 } }; // Pitch(unison) / Size / Spread(none)
        case FxType::Reverb: return { 1,  50, {  0, 64, 60, 70,  0 } }; // Predelay(none) / Diffusion / Time / Tone / Low-Cut(off) (amount fixed 1.0; chain Dry/Wet is the mix)
        case FxType::LoopingDelay:  return { 1,  80, { 50, 50, 50,  0,  0 } }; // Position / Size / Pitch(unison) / Freeze(off)
        case FxType::WSOLAStretch:  return { 1,  80, { 50, 50, 50,  0,127 } }; // Pitch(unison) / Position / Size / Freeze(off) / Tone(bright)
        case FxType::Spectral:      return { 1,  80, { 50, 50, 50, 50,  0 } }; // Pitch(unison) / Warp / Position / Blur / Freeze(off)
        case FxType::Wavefolder:    return { 1,  80, {  0, 50, 50,127,  0 } }; // Drive(unity) / Fold(mid) / Bias(centre) / Tone(bright)
        case FxType::FrequencyShifter: return { 1, 80, { 50,  0, 30,  0,  0 } }; // Shift(0 Hz) / Shape(sine) / Feedback(low) / Spread(none)
        case FxType::RingModulator:  return { 1,  80, { 30,  0, 50,  0,  0 } }; // Carrier(low) / Shape(sine) / Amount(mid)
        case FxType::Resonator:    return { 1,  80, { 50, 30, 50, 25, 32 } }; // Pitch(C4) / Decay / Bright / Position(0.25) / Structure(0.25 = Rings default)
        // FV-1 hardware-emulation family (Source/dsp/fx/fv1/). enabled=1 + an
        // audible Dry/Wet so selecting one is immediately hearable; characteristic
        // mid params. param[4] unused (Mix is the chain Dry/Wet).
        case FxType::ClockedDelay:    return { 1, 80, { 54, 38, 25,  0, 0 } }; // Sync(1/4) / Feedback(0.3) / TapeAge(0.2) / Grit(off=24-bit)
        case FxType::Ensemble:        return { 1, 70, { 40, 60, 30, 50, 0 } }; // Rate(~1 Hz) / Depth(7 ms) / Center(7 ms) / Feedback(gentle)
        case FxType::PlateReverb:     return { 1, 60, { 25, 62, 70, 30, 0 } }; // Predelay(20 ms) / Decay(~2 s) / Damping(~5 kHz) / Mod(light)
        case FxType::VinylCompressor: return { 1, 80, { 51, 38, 25, 57, 0 } }; // Compress(0.4) / Pitch(0.3) / Crackle(0.2) / Age(6 kHz)
        case FxType::Phaser:          return { 1, 60, { 47, 89, 85, 76, 0 } }; // Rate(~0.5 Hz) / Depth(0.7) / Feedback(0.3) / Center(800 Hz)
        case FxType::None:
        case FxType::Count:   break;
    }
    return { 0, 0, { 0, 0, 0, 0, 0 } };
}

// Layout constants (px). The card sits in the FX top row (~261..271px tall at
// the 600..620px editor height). The layout mirrors the synth OSC/Mixer/Filter
// sections: a header band, a STYLED algorithm dropdown (Shape/Mode parity), a
// COMPACT visualizer band (synth decoration parity, <= kVisMax), and a
// Mixer-style 3-column knob GRID (kCellH = the synth cell height). Fixed
// header / dropdown / visualizer heights keep the three cards' baselines
// aligned; the grid centres vertically in its remainder so a short row-set
// reads balanced + spacious.
constexpr int kPad         = 6;      // card edge inset
constexpr int kHeaderH     = 16;     // header row (title + power toggle; synth kGroupTitleH parity)
constexpr int kHalfGap     = 2;      // gap below the header + between bands
#if JUCE_IOS
constexpr int kTypeRowH    = 44;     // HIG: the whole algorithm selector is one 44pt tap target
constexpr int kComboH      = 44;     // HIG: 44pt-tall combo (picker tap target)
#else
constexpr int kTypeRowH    = 28;     // algorithm dropdown row (styled combo height, Shape/Mode parity)
constexpr int kComboH      = 28;     // dropdown height (ParamControl combo parity)
#endif
constexpr int kComboChrome = 26;     // fit-to-text chrome: pad + amber chevron + slack
constexpr int kComboMinW   = 80;     // dropdown floor width
constexpr int kGridCols    = 3;      // knob grid column count (Mixer parity)
#if JUCE_IOS
constexpr int kCellH       = 58;     // knob cell height (denser on iOS: label band + dial)
constexpr int kVisMin      = 32;     // visualizer band floor (smaller FX illustrations on iOS)
constexpr int kVisMax      = 56;     // visualizer band cap (smaller FX illustrations on iOS)
#else
constexpr int kCellH       = 64;     // knob cell height (synth cellH parity: label band + 52px dial)
constexpr int kVisMin      = 40;     // visualizer band floor (legible at the min size)
constexpr int kVisMax      = 80;     // visualizer band cap (synth kDecorationH parity)
#endif
// Bypass affordance: a bypassed slot's live controls (knobs + visualizer + type
// combo) are recessed to this alpha so the slot reads as inactive at a glance
// (0.5 matches the synth GroupComponent / knob disabled alpha).
constexpr float kBypassedAlpha = 0.5f;
constexpr float kCorner        = 7.0f; // card panel corner radius (synth GroupComponent parity)

// Fit-to-text width of a ComboBox's longest item, measured in the active
// LookAndFeel combo font (mirrors ParamControl::maxChoiceTextWidth) so the FX
// type dropdown sizes itself exactly like the Osc "Shape" / Filter "Mode"
// selectors (widest choice + kComboChrome, centred in its row).
int maxComboItemWidth (const juce::ComboBox& combo)
{
    const auto f = [&]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&combo.getLookAndFeel()))
            return lnf->appFont (14.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (14.0f));
    }();

    int widest = 0;
    for (int i = 0; i < combo.getNumItems(); ++i)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo.getItemText (i)));
    widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo.getText()));
    return widest;
}

// (The knob grid is a FIXED 3-column x 2-row layout in layoutParamGrid(); no
// column-count adaptation is needed, so the former knobGridCols() helper is gone.)
} // namespace

//==============================================================================
FxSlotCard::FxSlotCard (ParvatiAudioProcessor& processor, int slot,
                        const PatchParamDescriptor* p1Desc, const PatchParamDescriptor* p2Desc,
                        const PatchParamDescriptor* p3Desc, const PatchParamDescriptor* p4Desc,
                        const PatchParamDescriptor* p5Desc,
                        const PatchParamDescriptor* drywetDesc)
    : processor_ (processor),
      slot_ (juce::jlimit (0, 2, slot)),
      prefix_ ("fx" + juce::String (slot_ + 1) + "_")
{
    // ---- Six full ParamControl knobs (modulation-destination parity) ----
    if (p1Desc     != nullptr) p1_     = std::make_unique<ParamControl> (processor_, *p1Desc);
    if (p2Desc     != nullptr) p2_     = std::make_unique<ParamControl> (processor_, *p2Desc);
    if (p3Desc     != nullptr) p3_     = std::make_unique<ParamControl> (processor_, *p3Desc);
    if (p4Desc     != nullptr) p4_     = std::make_unique<ParamControl> (processor_, *p4Desc);
    if (p5Desc     != nullptr) p5_     = std::make_unique<ParamControl> (processor_, *p5Desc);
    if (drywetDesc != nullptr) drywet_ = std::make_unique<ParamControl> (processor_, *drywetDesc);

    for (auto* pc : { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get(), drywet_.get() })
        if (pc != nullptr)
            addAndMakeVisible (*pc);

    // FX slot param knobs: per-param meaningful-unit readout (note names /
    // +/-semitones / Hz / On-Off / %) via paramValueText. dry/wet stays %.
    // Display-only (stored 0..127 unchanged). Re-installed on type change in
    // refreshFromType() (the formatter depends on the live type).
    const auto t0 = static_cast<FxType> (currentTypeIndex());
    ParamControl* initParams[5] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get() };
    for (int i = 0; i < 5; ++i)
        if (initParams[i] != nullptr)
            initParams[i]->setDisplayValueText ([t0, i] (double v) { return paramValueText (t0, i, v); });
    if (drywet_ != nullptr)
        drywet_->setDisplayValuePercent (true);

    // ---- Type combo (ComboBoxAttachment auto-uses the param's choices) ----
#if JUCE_IOS
    // iOS: FxTypeCombo rebuilds the popup at a 44pt item height (HIG picker).
    // It IS-A juce::ComboBox, so the ComboBoxAttachment + addItemList below are
    // unchanged. The prev/next chevrons are not placed on iOS (see resized()).
    typeCombo_ = std::make_unique<FxTypeCombo> ();
#else
    typeCombo_ = std::make_unique<juce::ComboBox> ();
#endif
    typeCombo_->setTooltip ("FX " + juce::String (slot_ + 1) + " algorithm");
    // Populate from the param's OWN choice list BEFORE the attachment:
    // AudioProcessorValueTreeState::ComboBoxAttachment does NOT add items itself,
    // so without this the dropdown is empty and nothing is selectable.
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (processor_.getApvts ().getParameter (prefix_ + "type")))
        typeCombo_->addItemList (p->choices, 1);
    addAndMakeVisible (*typeCombo_);
    typeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor_.getApvts (), prefix_ + "type", *typeCombo_);

    // ---- Prev/next ("<" ">") step buttons flanking the type combo ----
    // A keyboard/mouse shortcut to cycle the effect TYPE without opening the
    // dropdown. Writes through the APVTS type param (the ComboBoxAttachment
    // syncs the combo selection; the APVTS::Listener refreshes the knob set).
    typePrev_ = std::make_unique<TypeStepButton> (false);   // points left (prev)
    typeNext_ = std::make_unique<TypeStepButton> (true);    // points right (next)
    typePrev_->setTooltip ("FX " + juce::String (slot_ + 1) + " previous algorithm");
    typeNext_->setTooltip ("FX " + juce::String (slot_ + 1) + " next algorithm");
    addAndMakeVisible (*typePrev_);
    addAndMakeVisible (*typeNext_);
    typePrev_->onClick = [this] { stepType (-1); };
    typeNext_->onClick = [this] { stepType (+1); };

    // ---- Power/bypass toggle (bound to the 0..1 enable Int via a Value) ----
    powerToggle_ = std::make_unique<PowerToggle> ();
    powerToggle_->setTooltip ("FX " + juce::String (slot_ + 1) + " enable / bypass");
    addAndMakeVisible (*powerToggle_);
    powerToggle_->onClick = [this]
    {
        // Flip the enable param: 0 <-> 1. The Value listener writes the new
        // toggle state back onto the button for display.
        const bool on = ! powerToggle_->getToggleState();
        enabledValue_ = on ? 1.0f : 0.0f;
    };

    // ---- APVTS parameter listeners (type + enable) ----
    // addParameterListener fires on ANY change (combo edit, host automation,
    // preset load) — a Value::Listener on a separate getParameterAsValue instance
    // does NOT, so without this the knob visible-set + power state would freeze
    // at construction. type is read LIVE in currentTypeIndex() via getParameter.
    enabledValue_ = processor.getApvts ().getParameterAsValue (prefix_ + "enabled");
    processor.getApvts ().addParameterListener (prefix_ + "type", this);
    processor.getApvts ().addParameterListener (prefix_ + "enabled", this);

    // ---- Visualizer (normalized APVTS getters) ----
    const auto prefixStr = prefix_;
    auto norm = [&processor, prefixStr] (const char* tail) -> float
    {
        auto* p = processor.getApvts ().getParameter (prefixStr + tail);
        return p != nullptr ? p->getValue () : 0.0f;
    };
    visualizer_ = std::make_unique<FxSlotVisualizer> (
        [norm] { return norm ("type"); },
        [norm] { return norm ("param1"); },
        [norm] { return norm ("param2"); },
        [norm] { return norm ("param3"); },
        [norm] { return norm ("param4"); },
        [norm] { return norm ("param5"); },
        [norm] { return norm ("drywet"); });
    addAndMakeVisible (*visualizer_);

    // Initial knob visible set + semantic labels + power state.
    refreshFromType();
    refreshEnabled();
}

FxSlotCard::~FxSlotCard()
{
    // Detach BEFORE the AsyncUpdater base cancels its pending update, so no
    // in-flight parameterChanged can reach a half-destroyed card.
    processor_.getApvts ().removeParameterListener (prefix_ + "type", this);
    processor_.getApvts ().removeParameterListener (prefix_ + "enabled", this);
}

//==============================================================================
int FxSlotCard::currentTypeIndex() const
{
    // Read the LIVE parameter value (a cached Value would not track external
    // changes such as preset load / host automation).
    auto* p = processor_.getApvts ().getParameter (prefix_ + "type");
    const float v = p != nullptr ? p->getValue () : 0.0f;
    // fx{N}_type is an AudioParameterChoice: getValue() is NORMALIZED 0..1, so
    // scale it back to the choice index (0..Count-1). Without this, e.g. Reverb
    // (idx 3, normalized 0.75) would read as idx 1 (GainPan) -> wrong knobs.
    // Mirrors FxSlotVisualizer::typeIndex().
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    return juce::jlimit (0, kLast, juce::roundToInt (v * static_cast<float> (kLast)));
}

void FxSlotCard::stepType (int delta)
{
    // Clamp the new choice index to the FxType range and write it through the
    // APVTS type param (Choice: getParameterAsValue holds the denormalized
    // index). The ComboBoxAttachment syncs the combo selection; the APVTS::
    // Listener (parameterChanged) refreshes the visible knob set + labels.
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    const int cur = currentTypeIndex();
    const int nxt = juce::jlimit (0, kLast, cur + delta);
    if (nxt == cur)
        return;
    processor_.getApvts().getParameterAsValue (prefix_ + "type") = nxt;
}

void FxSlotCard::refreshFromType()
{
    const auto t = static_cast<FxType> (currentTypeIndex());
    const int active = activeParamCount (t);

    ParamControl* params[5] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get() };
    for (int i = 0; i < 5; ++i)
    {
        auto* pc = params[i];
        if (pc == nullptr)
            continue;
        // Relabel to the active algorithm's semantic name (or revert to the
        // descriptor label for an inactive param so a future show is correct).
        pc->setDisplayLabel (i < active ? juce::String (paramLabel (t, i)) : juce::String());
        // Re-install the per-param value formatter for the NEW type (the param
        // index i is fixed, but the unit depends on the type).
        pc->setDisplayValueText ([t, i] (double v) { return paramValueText (t, i, v); });
    }
    if (drywet_ != nullptr)
        drywet_->setDisplayLabel ("Dry/Wet");

    resized();   // reflow the visible-set + anchored dry/wet
}

void FxSlotCard::refreshEnabled()
{
    // Read the LIVE enable value (0..1 Int param).
    auto* p = processor_.getApvts ().getParameter (prefix_ + "enabled");
    const bool on = (p != nullptr ? juce::roundToInt (p->getValue ()) : 0) != 0;
    if (powerToggle_ != nullptr)
        powerToggle_->setToggleState (on, juce::dontSendNotification);

    // Bypass affordance: recess the LIVE controls (knobs + visualizer + type
    // combo) when the slot is bypassed, so a disabled slot reads as inactive at a
    // glance — without it a bypassed slot's knobs stay full-brightness and look
    // live. NON-colour (alpha only): the panel / title / power glyph keep full
    // alpha so the bypass state + slot identity stay legible. setAlpha is
    // compositing-only (it does NOT disable interaction), so the values remain
    // editable even while bypassed.
    const float contentAlpha = on ? 1.0f : kBypassedAlpha;
    juce::Component* content[] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get(),
                                   drywet_.get(), visualizer_.get(), typeCombo_.get(),
                                   typePrev_.get(), typeNext_.get() };
    for (auto* c : content)
        if (c != nullptr)
            c->setAlpha (contentAlpha);
}

void FxSlotCard::parameterChanged (const juce::String& id, float /*newValue*/)
{
    if (id != prefix_ + "type" && id != prefix_ + "enabled")
        return;

    // The per-type ENGAGEMENT defaults must be seeded SYNCHRONOUSLY on the
    // message thread for a TYPE change, BEFORE a preset/part load's subsequent
    // param writes (descriptor order: type, enabled, drywet, param1..5) override
    // them — so saved patches keep their own values. (Seeding writes 'enabled',
    // which re-fires parameterChanged("enabled") re-entrantly; that just re-
    // requests the deferred refresh below — harmless.)
    if (id == prefix_ + "type")
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm != nullptr && mm->isThisTheMessageThread())
        {
            const auto t = static_cast<FxType> (currentTypeIndex());
            if (t != FxType::None)
            {
                const auto d = fxTypeDefaults (t);
                auto& apvts  = processor_.getApvts();
                apvts.getParameterAsValue (prefix_ + "enabled") = (float) d.enabled;
                apvts.getParameterAsValue (prefix_ + "drywet")  = (float) d.drywet;
                for (int k = 0; k < 5; ++k)
                    apvts.getParameterAsValue (prefix_ + "param" + juce::String (k + 1)) = (float) d.p[k];
            }
        }
    }

    // The visual REFRESH (refreshFromType / refreshEnabled -> resized() ->
    // setBounds on the type combo) is ALWAYS DEFERRED to handleAsyncUpdate.
    // Running it synchronously would execute resized() re-entrantly INSIDE the
    // ComboBox's own onChange (combo edit -> ComboBoxAttachment writes the type
    // param -> this listener fires), which disrupts the ComboBoxAttachment's
    // selection-display sync (the combo ends up showing nothing selected) and
    // re-lays-out the combo mid-event. Deferring lets the combo's change-
    // handling + the attachment's self-sync complete first; the knob layout then
    // refreshes cleanly on the next message loop. (The ~1-frame delay is
    // imperceptible; preset loads are still reflected promptly.)
    triggerAsyncUpdate();
}

void FxSlotCard::handleAsyncUpdate()
{
    refreshFromType();
    refreshEnabled();
}

//==============================================================================
void FxSlotCard::layoutParamGrid (const juce::Rectangle<int>& gridArea)
{
    const auto t = static_cast<FxType> (currentTypeIndex());
    ParamControl* params[5] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get() };

    // Hide every owned knob first; only the placed ones are re-shown below.
    for (auto* pc : params)
        if (pc != nullptr)
            pc->setVisible (false);
    if (drywet_ != nullptr)
        drywet_->setVisible (false);

    // None => NO Dry/Wet, NO params (the grid collapses; the card shows just the
    // header + type combo + visualizer). This is the explicit "Dry/Wet hidden
    // when None" rule.
    if (t == FxType::None || gridArea.isEmpty())
        return;

    const int active = activeParamCount (t);

    // FIXED 3-column x 2-row grid (6 cells = up to 5 params + Dry/Wet). The
    // ACTIVE params (param1..N) fill row-major from the top-left (cells
    // 0..active-1); the fx{N}_drywet knob is FIXED in the BOTTOM-RIGHT cell
    // (cell index 5 of the 3x2 grid) so it never moves when the effect type
    // changes — consistent placement across all three cards. Cells between the
    // last active param and the Dry/Wet are simply left empty. Equal-width
    // columns; the rightmost column absorbs the integer width remainder so the
    // grid fills the full width. Cell height is the synth kCellH parity (label
    // band + 52px dial), shrunk only if the grid region is shorter than 2*kCellH.
    // The block centres VERTICALLY in the region.
    constexpr int cols        = kGridCols;   // 3
    constexpr int rows        = 2;
    constexpr int kDryWetCell = 5;           // bottom-right of the 3x2 grid

    const int cellH  = juce::jmin (kCellH, gridArea.getHeight() / rows);
    const int cellW  = gridArea.getWidth() / cols;
    const int blockH = rows * cellH;
    const int y0     = gridArea.getY() + (gridArea.getHeight() - blockH) / 2;

    auto placeCell = [&] (ParamControl* pc, int cell)
    {
        if (pc == nullptr)
            return;
        const int col = cell % cols;
        const int row = cell / cols;
        const int x = gridArea.getX() + col * cellW;
        const int w = (col == cols - 1) ? (gridArea.getRight() - x) : cellW;
        const int y = y0 + row * cellH;
        pc->setVisible (true);
        pc->setBounds (juce::Rectangle<int> (x, y, w, cellH));
    };

    for (int i = 0; i < active; ++i)
        placeCell (params[i], i);
    if (drywet_ != nullptr)
        placeCell (drywet_.get(), kDryWetCell);
}

//==============================================================================
void FxSlotCard::resized()
{
    auto area = getLocalBounds ().reduced (kPad);
    if (area.isEmpty())
        return;

    // ---- Header row: title (upper-left, painted) + power toggle (top-right) ----
    auto header = area.removeFromTop (kHeaderH);
    if (powerToggle_ != nullptr)
        powerToggle_->setBounds (header.removeFromRight (kHeaderH - 2).reduced (2));
    // The remaining header band is the title (drawn in paint(), no child).

    if (area.isEmpty())
        return;
    area.removeFromTop (kHalfGap);   // gap below the header divider

    // ---- Type row: the algorithm dropdown as a STYLED combo (Osc "Shape" /
    //      Filter "Mode" parity) — 28px tall, fit-to-text width, centred. The
    //      combo already inherits the editor-wide ComboBox theme colours
    //      (backgroundInput fill, amber accentPrimary chevron, borderless) via
    //      the LookAndFeel — identical to the synth selectors — so only the SIZE
    //      is set here (was previously full-width / 20px). ----
    if (typeCombo_ != nullptr && area.getHeight() > kTypeRowH)
    {
        auto typeRow = area.removeFromTop (kTypeRowH);
        const int textW  = maxComboItemWidth (*typeCombo_) + kComboChrome;
        const int comboW = juce::jlimit (kComboMinW, juce::jmax (kComboMinW, typeRow.getWidth()), textW);
#if JUCE_IOS
        // iOS HIG: the whole algorithm selector is ONE 44pt-tall tap target that
        // opens the effect picker (the prev/next chevrons are removed on iOS).
        // Centred, fit-to-text width; the 44pt combo fills the 44pt row.
        const int comboX = typeRow.getX() + (typeRow.getWidth() - comboW) / 2;
        typeCombo_->setBounds (comboX, typeRow.getY(), comboW, kComboH);
#else
        // The prev/next ("<" ">") step buttons flank the combo as a centred
        // cluster: [ < ][ combo ][ > ]. Small square chevrons, combo height.
        constexpr int kStepW   = 16;                 // chevron button width
        constexpr int kStepGap = 3;                  // gap between a chevron and the combo
        const int clusterW = comboW + 2 * (kStepW + kStepGap);
        const int clusterX = typeRow.getX() + (typeRow.getWidth() - clusterW) / 2;
        const int comboX   = clusterX + kStepW + kStepGap;
        typeCombo_->setBounds (comboX, typeRow.getY(), comboW, kComboH);
        const int stepY = typeRow.getY() + (kTypeRowH - kComboH) / 2;
        const int stepH = kComboH;
        if (typePrev_ != nullptr)
            typePrev_->setBounds (clusterX, stepY, kStepW, stepH);
        if (typeNext_ != nullptr)
            typeNext_->setBounds (comboX + comboW + kStepGap, stepY, kStepW, stepH);
#endif
        area.removeFromTop (kHalfGap);
    }

    // ---- Body: a COMPACT visualizer band (<= kVisMax, synth decoration parity)
    //      on top + a Mixer-style 3-column knob GRID below. The grid is the
    //      primary control surface, so it claims its ideal height (rows *
    //      kCellH) first; the band fills the rest down to kVisMin. If even the
    //      band floor cannot fit alongside the ideal grid, the band holds at
    //      kVisMin and the grid shrinks. (Was: a large ~2/3-body band + a single
    //      knob row.) ----
    const auto t = static_cast<FxType> (currentTypeIndex());
    // The knob grid is a FIXED 3x2 layout (up to 5 params + Dry/Wet) whenever the
    // slot has an effect; for None the grid collapses entirely (Dry/Wet hidden).
    const bool hasGrid = (t != FxType::None);
    const int gridIdealH = hasGrid ? (2 * kCellH) : 0;

    const int bodyH = area.getHeight();
    int visH  = kVisMax;
    int gridH = gridIdealH;
    if (visH + gridH + kHalfGap > bodyH)          // ideal grid + max band does not fit
    {
        if (gridIdealH + kVisMin + kHalfGap <= bodyH)   // band can shrink to its floor
        {
            visH  = juce::jmax (0, bodyH - gridIdealH - kHalfGap);
            gridH = gridIdealH;
        }
        else                                            // grid must shrink; band holds kVisMin
        {
            visH  = juce::jmin (kVisMin, juce::jmax (0, bodyH - kHalfGap));
            gridH = juce::jmax (0, bodyH - visH - kHalfGap);
        }
    }

    if (visualizer_ != nullptr && visH > 0)
    {
        visualizer_->setBounds (area.removeFromTop (visH));
        if (! area.isEmpty())
            area.removeFromTop (kHalfGap);
    }

    layoutParamGrid (area);
}

//==============================================================================
void FxSlotCard::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel ());
    const ParvatiTheme* t = lnf != nullptr ? lnf->getTheme () : nullptr;

    const juce::Colour panel   = t ? t->containerFill   : juce::Colour (0xff202028);
    const juce::Colour title   = t ? t->textSecondary   : juce::Colour (0xffb0b0bc);
    const juce::Colour accent  = t ? t->accentSecondary : juce::Colour (0xffe8b84b);

    const auto r = getLocalBounds ().toFloat ();

    // ---- Card panel: BORDERLESS (synth GroupComponent parity). Depth comes from
    //      the tonal lift of containerFill over the page backgroundBase — no
    //      outline, no under-header divider (the title band alone separates). ----
    g.setColour (panel);
    g.fillRoundedRectangle (r, kCorner);

    // ---- Title "FX N" upper-left: BOLD + UPPERCASE (14px), mirroring the synth
    //      card GroupComponent header. An accent tick sits just left of the text
    //      (the FX-slot accent marker); the power toggle lives top-right. ----
    juce::Font font = lnf != nullptr ? lnf->appFont (14.0f, juce::Font::bold)
                                    : juce::Font (juce::FontOptions (14.0f, juce::Font::bold));
    const juce::String name = "FX" + juce::String (slot_ + 1);   // "FX1" (uppercase)
    const int tickX  = kPad + 2;
    const int titleX = tickX + 6;
    const int titleW = juce::jmax (0, getWidth() - titleX - kPad);
    const juce::Rectangle<int> titleRect (titleX, kPad, titleW, kHeaderH);

    g.setColour (accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (static_cast<float> (tickX),
                                                    static_cast<float> (kPad + 6),
                                                    2.5f, static_cast<float> (kHeaderH - 12)),
                            1.2f);
    g.setColour (title);
    g.setFont (font);
    g.drawText (name, titleRect, juce::Justification::centredLeft, true);
}

//==============================================================================
void FxSlotCard::applyThemeColors()
{
    // Push the live theme token onto the visualizer trace so a theme switch
    // re-tints it immediately (the visualizer otherwise reads accentSecondary
    // live each paint).
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel ()))
    {
        if (const auto* th = lnf->getTheme ())
        {
            if (visualizer_ != nullptr)
                visualizer_->setCategoryColour (th->accentSecondary);
        }
    }

    repaint();
}
