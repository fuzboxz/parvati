// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotCard.h.

#include "FxSlotCard.h"
#include "FxSlotLabels.h"   // activeParamCount/paramLabel — defined in FxSlotLabels.cpp

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
// PowerToggle — the Enable/Bypass button. THE SHARED ParvatiModuleLAMP
// (2026-08-20 unification): the FX slot power toggle, the synth mod-matrix
// bypass lamp and the FX mod-matrix lamp are the SAME widget — accentPrimary
// fill when enabled (the former accentSecondary fill was the style mismatch
// the user reported), textDisabled grey when bypassed, outline ring that
// brightens on hover. Pure juce primitives; the dot scales with the band.
// Its toggle STATE is driven from the fx{N}_enable APVTS Value by FxSlotCard;
// clicking flips that Value (handled by the card's onClick), so this button
// does NOT toggle its own state on click.
class PowerToggle final : public ParvatiModuleLamp {};

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
        const juce::Colour accent = t ? t->accentPrimary : parvati::parvatiFallbackAccent;

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

// FxTypeCombo — the effect picker. The whole "< None >" selector is one
// 44pt-tall tap target (HIG #4); the prev/next chevron step buttons are not
// placed. JUCE's default ComboBox popup item rows (~24pt) are below the HIG
// touch minimum, so showPopup() rebuilds the popup at a 44pt standard item
// height. 16 flat entries overflow short panes, so the popup is CATEGORIZED
// SUBMENUS: "None" plus one submenu per category (alphabetical — see
// fxTypeDisplayOrder()/fxCategoryOf() in FxTypes.h), effects alphabetical by
// display name inside. The combo's INTERNAL items stay in ENUM order
// (ComboBoxAttachment syncs by item INDEX == choice index), so each popup
// item writes setSelectedItemIndex(enumValue) and the tick reads
// getSelectedItemIndex() (== enum value). The collapsed combo keeps showing
// the PLAIN effect name (its internal item text). The APVTS choice list (host
// surface) stays flat — categories are Parvati-UI only.
class FxTypeCombo : public juce::ComboBox
{
public:
    // W10 (lane-A finding 1b) + W10b (F-w10-1/-2): onUserPick is invoked by
    // EVERY real user pick — the popup item actions AND keyboard arrow
    // navigation — just before the selection write. The owning FxSlotCard
    // installs the guarded seeding seam (pickTypeUserAction →
    // seedEngagementDefaultsForType) — the APVTS listener cannot distinguish a
    // user pick from host automation / NRPN / undo replay (all fire
    // parameterChanged), so the seed must originate at the UI seams. NOT fired
    // for the attachment's external sync path.
    using OnUserPick = std::function<void (FxType)>;

    explicit FxTypeCombo (OnUserPick onUserPick) : juce::ComboBox ({}), onUserPick_ (std::move (onUserPick)) {}

    void showPopup() override
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        // The combo's internal items are enum-ordered, so the selected index
        // IS the FxType enum value, and getItemText(enum) is the plain label.
        const int current = getSelectedItemIndex();
        auto addItem = [this, current] (juce::PopupMenu& m, FxType t)
        {
            const auto ev = static_cast<int> (t);
            juce::PopupMenu::Item item;
            item.text     = getItemText (ev);   // plain name inside the submenu
            item.itemID   = getItemId (ev);
            item.isTicked = (ev == current);
            juce::Component::SafePointer<FxTypeCombo> safe { this };
            item.action   = [safe, ev] { if (safe != nullptr) safe->pickItemIndex (ev); };
            m.addItem (std::move (item));
        };

        addItem (menu, FxType::None);   // uncategorized: always first, plain item

        // One submenu per category, in the (alphabetical) FxCategory order.
        // Header item IDs must merely be unique vs the selectable IDs above,
        // hence the offset above the choice-ID range.
        for (int ci = 1; ci < static_cast<int> (FxCategory::Reverb) + 1; ++ci)
        {
            const auto cat = static_cast<FxCategory> (ci);
            juce::PopupMenu sub;
            sub.setLookAndFeel (&getLookAndFeel());
            for (FxType t : fxTypeDisplayOrder())
                if (fxCategoryOf (t) == cat)
                    addItem (sub, t);
            if (sub.getNumItems() == 0)
                continue;
            // addSubMenu: non-selectable category header with an arrow
            menu.addSubMenu (fxCategoryName (cat), std::move (sub));
        }
        // The completion callback is NOT optional: stock ComboBox::showPopup
        // finishes with comboBoxPopupMenuFinishedCallback -> hidePopup(), which
        // resets the private `menuActive` flag. Passing nullptr (the old code)
        // left menuActive latched TRUE after dismissal, so every later click
        // bailed inside showPopupIfNotActive() and the dropdown could never be
        // reopened after the first selection.
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withStandardItemHeight (ParvatiLookAndFeel::kPopupRowHeight),
                            juce::ModalCallbackFunction::create (
                                [safe = juce::Component::SafePointer<FxTypeCombo> { this }] (int)
                                {
                                    if (safe != nullptr)
                                        safe->hidePopup();
                                }));
    }

    // The guarded user-pick action SHARED by the popup item actions and
    // keyboard navigation (W10b, lane F-w10-2). @p ev is the item INDEX == the
    // FxType enum value (the combo's internal items are enum-ordered). A
    // re-pick of the CURRENT selection is a NO-OP: the seam guard skips the
    // seeding (which would clobber the user's knob tweaks) and JUCE's
    // setSelectedItemIndex would early-out on the unchanged selection anyway —
    // this mirrors stepType's `if (nxt == cur) return;`.
    void pickItemIndex (int ev)
    {
        if (ev == getSelectedItemIndex())
            return;
        if (onUserPick_)
            onUserPick_ (static_cast<FxType> (ev));   // guarded pick at the card seam (seed + write)
        setSelectedItemIndex (ev, juce::sendNotificationSync);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // W10b (lane F-w10-1): the base class handles the arrow keys by nudging
        // the selection DIRECTLY (ComboBox::keyPressed → nudgeSelectedItem — no
        // popup, so the showPopup override never ran and onUserPick_ never
        // fired): keyboard navigation changed the type with NO engagement
        // seeding — a silent effect. Route keyboard changes through the same
        // guarded pickItemIndex seam as a popup pick.
        const int delta = (key == juce::KeyPress::upKey || key == juce::KeyPress::leftKey) ? -1
                        : (key == juce::KeyPress::downKey || key == juce::KeyPress::rightKey) ? +1
                        : 0;
        if (delta != 0)
        {
            const int cur = getSelectedItemIndex();
            const int nxt = juce::jlimit (0, getNumItems() - 1, cur + delta);
            if (nxt != cur)   // at either list end: consumed, no change (stepType's guard shape)
                pickItemIndex (nxt);
            return true;
        }
        return juce::ComboBox::keyPressed (key);
    }

private:
    OnUserPick onUserPick_;
};

// Per-type ENGAGEMENT defaults applied the moment a user selects an effect
// type. The generic slot params all default to 0 — silent (Delay time=0,
// drywet=0 = fully dry, enabled=0 = bypassed) — so picking a type otherwise
// sounds like "nothing happened". These seed each effect with an audible,
// characteristic starting point. Applied ONLY from the UI seams (the type
// combo's popup pick and stepType, via seedEngagementDefaultsForType — W10),
// never from parameterChanged (that listener also fires for host automation /
// NRPN / undo replay / part loads, where seeding would clobber live values).
// For the GUI/message-thread part load
// the descriptor order is type, enabled, drywet, param1..5, so a load that sets
// type THEN params overrides these — saved patches keep their own values.
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
        case FxType::VinylCompressor: return { 1, 80, { 51, 38, 18, 57, 0 } }; // Compress(0.4) / Wow(0.3) / Crackle(0.14, subtle) / Age(6 kHz)
        case FxType::Phaser:          return { 1, 60, { 47, 89, 85, 76, 0 } }; // Rate(~0.5 Hz) / Depth(0.7) / Feedback(0.3) / Center(800 Hz)
        // FV-1 second wave (2026-08-17).
        case FxType::Overdrive:       return { 1, 80, { 50, 50, 80, 64, 0 } }; // Drive(4x) / Bias(centre) / Tone(bright) / Level(1.0)
        case FxType::LutDistortion:   return { 1, 80, { 50,  0, 25, 80, 0 } }; // Drive(4x) / Shape(0=Clip) / Jitter(subtle) / Tone(bright)
        case FxType::Compressor:      return { 1, 90, { 50, 40, 50, 64, 0 } }; // Amount(mid) / Attack(5 ms) / Release(~110 ms) / Level(1.0)
        case FxType::Gate:            return { 1, 90, { 50, 50, 40, 50, 0 } }; // Threshold(mid; 0=off) / Attack(~1 ms) / Hold(60 ms) / Release(~70 ms)
        case FxType::Chorus:          return { 1, 70, { 45, 50, 55, 10, 0 } }; // Rate(~0.9 Hz) / Depth(3 ms) / Center(~13 ms) / Feedback(light)
        case FxType::Flanger:         return { 1, 70, { 40, 60, 50, 60, 0 } }; // Rate(~0.3 Hz) / Depth(2.7 ms) / Manual(~3 ms) / Feedback(0.55)
        case FxType::Echo:            return { 1, 80, { 55, 50, 70, 30, 0 } }; // Time(~250 ms) / Feedback(0.48) / Tone(~4 kHz) / Spread(1.3x)
        case FxType::Room:            return { 1, 60, { 55, 60, 80, 80, 0 } }; // Decay(~1.6 s) / Damp(~3 kHz) / Width(stereo) / Tone(bright)
        case FxType::Spring:          return { 1, 60, { 45, 55, 65, 90, 0 } }; // Decay(~1.3 s) / Damp(~2.4 kHz) / Chirp(~0.74) / Width(stereo)
        case FxType::None:
        case FxType::Count:   break;
    }
    return { 0, 0, { 0, 0, 0, 0, 0 } };
}

// Layout constants (px). The card sits in the FX top row (~228px tall at
// the 600..620px editor height; the visualizer band was REMOVED 2026-08-20
// — the knob grid now owns the body). The layout mirrors the synth
// OSC/Mixer/Filter sections: a header band, a STYLED algorithm dropdown
// (Shape/Mode parity), and a Mixer-style 3-column knob GRID (kCellH = the
// synth cell height). Fixed header / dropdown heights keep the three cards'
// baselines aligned; the grid centres vertically in its remainder so a
// short row-set reads balanced + spacious.
constexpr int kPad         = 8;      // card edge inset (2026-08-23 harmonization: matches
                                     // the synth ParamPage's kGroupPad so the module
                                     // containers read identically on both pages)
constexpr int kHeaderH     = 16;     // header row (title + power toggle; synth kGroupTitleH parity)
constexpr int kHalfGap     = 2;      // gap below the header + between bands
constexpr int kTypeRowH    = 44;     // the whole algorithm selector is one 44pt tap target
constexpr int kComboH      = 44;     // 44pt-tall combo (picker tap target)
constexpr int kComboChrome = 26;     // fit-to-text chrome: pad + amber chevron + slack
constexpr int kComboMinW   = 80;     // dropdown floor width
constexpr int kGridCols    = 3;      // knob grid column count (Mixer parity)
constexpr int kCellH       = 70;     // knob cell height (bigger, more visible dials + tighter spacing)
// Bypass affordance: a bypassed slot's live controls (knobs + type
// combo) are recessed to this alpha so the slot reads as inactive at a glance
// (0.5 matches the synth GroupComponent / knob disabled alpha).
constexpr float kBypassedAlpha = 0.5f;
constexpr float kCorner        = 7.0f; // card panel corner radius (synth GroupComponent parity)

// Header-left LAMP geometry (the enable indicator dot beside the "FX N"
// title): the SHARED ParvatiModuleLamp's dot for the header hit band
// (44 x (kPad + kHeaderH) = 44x22) resolves to jmin(44,22)*0.68 ≈ 15pt
// (kPad + kHeaderH/2 vertically) and kPad in from the left edge horizontally.
// The toggle's hit band is the 44pt-wide HEADER band (44 x kPad+kHeaderH) —
// see FxSlotCard::resized().
constexpr float kLampDotW  = 14.0f;                              // lamp diameter (PINNED via setLampDiameter — see resized(); 14pt keeps the 2.5pt ring inside the header band)
constexpr float kLampCx    = static_cast<float> (kPad) + kLampDotW * 0.5f;   // lamp centre x (optical)
constexpr float kLampCy    = static_cast<float> (kPad) + static_cast<float> (kHeaderH) * 0.5f;   // title optical middle
constexpr int   kLampTitleGap = 8;   // lamp -> title gap (2026-08-23: "a tiny bit more space between the text and the button")

// (The knob grid is a FIXED 3-column x 2-row layout in layoutParamGrid(); no
// column-count adaptation is needed, so the former knobGridCols() helper is gone.)
// (The fit-to-text combo measurement is the SHARED widestComboTextWidth in
// ParvatiLookAndFeel.h — the former local maxComboItemWidth copy is gone.)
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
    // Accessibility-only: name the card after its painted "FX N" header
    // ("FX" is a proper noun — matches FxRoutingBar's untranslated slot ids).
    setTitle ("FX" + juce::String (slot_ + 1));

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
    // FxTypeCombo rebuilds the popup at a 44pt item height (HIG picker). It IS-A
    // juce::ComboBox, so the ComboBoxAttachment + addItemList below are unchanged.
    // The prev/next chevrons are not placed (see resized()).
    typeCombo_ = std::make_unique<FxTypeCombo> (
        // W10: the combo's USER PICK drives the seeding seam explicitly (the
        // popup item action calls it before writing the param). Nothing else
        // seeds — see seedEngagementDefaultsForType.
        [this] (FxType t) { seedEngagementDefaultsForType (static_cast<int> (t)); });
    // The FX number stays OUTSIDE the TRANS'd fragments (suffix-key pattern,
    // same idiom as FxRoutingBar's "FX master EQ " + band name) so FR/DE can
    // translate the tail ("FX 2 algorithme" / "FX 2 Algorithmus").
    typeCombo_->setTooltip (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" algorithm"));
    // Accessibility-only: ComboBox's built-in handler reads Component::getTitle()
    // (the collapsed box paints its own text, so the name is the only label a
    // screen reader gets). Same suffix-key chain as the tooltip.
    typeCombo_->setTitle (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" algorithm"));
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
    typePrev_->setTooltip (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" previous algorithm"));
    typeNext_->setTooltip (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" next algorithm"));
    // Accessibility-only: the chevrons are Path-drawn Buttons with no text;
    // name them so the default Button handler announces a purpose (title,
    // then the tooltip as help).
    typePrev_->setTitle (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" previous algorithm"));
    typeNext_->setTitle (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" next algorithm"));
    addAndMakeVisible (*typePrev_);
    addAndMakeVisible (*typeNext_);
    typePrev_->onClick = [this] { stepType (-1); };
    typeNext_->onClick = [this] { stepType (+1); };

    // ---- Power/bypass toggle (bound to the 0..1 enable Int via a Value) ----
    // Drawn as the header-left LAMP beside the "FX N" title (see resized());
    // disabled wholesale for a None slot (refreshEnabled) — None IS the
    // disabled state.
    powerToggle_ = std::make_unique<PowerToggle> ();
    powerToggle_->setTooltip (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" enable / bypass"));
    // Accessibility-only: the power lamp is a glyph-drawn Button with no text.
    // The default Button handler reads Component::getTitle() first; its
    // toggle-state + On/Off value interface come free from the base handler
    // (the card drives the toggle state from the fx{N}_enabled Value).
    powerToggle_->setTitle (TRANS ("FX ") + juce::String (slot_ + 1) + TRANS (" enable / bypass"));
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

std::unique_ptr<juce::AccessibilityHandler> FxSlotCard::createAccessibilityHandler()
{
    // Role `group` + the ctor's "FX N" title (see the header declaration).
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
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
    // (Mirrors the removed visualizer's typeIndex() convention.)
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    return juce::jlimit (0, kLast, juce::roundToInt (v * static_cast<float> (kLast)));
}

void FxSlotCard::simulateUserTypePickForTest (int typeIndex)
{
    // Test-only bridge to the combo's guarded pick seam (W10b) — the exact
    // path a popup item action or a keyboard arrow drives. The member is the
    // type-erased unique_ptr<juce::ComboBox>, but the concrete object is the
    // file-local FxTypeCombo by construction (see the ctor).
    if (typeCombo_ != nullptr)
        static_cast<FxTypeCombo*> (typeCombo_.get())
            ->pickItemIndex (juce::jlimit (0, static_cast<int> (FxType::Count) - 1, typeIndex));
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
    seedEngagementDefaultsForType (nxt);   // W10: UI-originated -> seed BEFORE the write
    processor_.getApvts().getParameterAsValue (prefix_ + "type") = nxt;
}

void FxSlotCard::seedEngagementDefaultsForType (int newTypeIndex)
{
    // W10 (lane-A finding 1b): the ONLY seeding path. Called by the UI seams
    // (type-combo popup pick via FxTypeCombo::onUserPick, and stepType) before
    // they write the type param. The per-type ENGAGEMENT defaults give a newly
    // picked effect an audible, characteristic starting point (generic slot
    // params all default to 0 = silent). parameterChanged() must NOT seed: the
    // same listener fires for host automation / NRPN writes of fx{N}_type
    // (which must preserve the current params) and for undo replay / part
    // loads (which write their own full param set around the type write).
    // Undo transaction shape (W7): the seed writes land BEFORE the type write,
    // so a replay restores the type FIRST — with no listener-side seeding the
    // restored params survive untouched either way.
    const auto t = static_cast<FxType> (juce::jlimit (0, static_cast<int> (FxType::Count) - 1,
                                                       newTypeIndex));
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || ! mm->isThisTheMessageThread())
        return;
    if (t == FxType::None)
        return;
    const auto d = fxTypeDefaults (t);
    auto& apvts  = processor_.getApvts();
    apvts.getParameterAsValue (prefix_ + "enabled") = (float) d.enabled;
    apvts.getParameterAsValue (prefix_ + "drywet")  = (float) d.drywet;
    for (int k = 0; k < 5; ++k)
        apvts.getParameterAsValue (prefix_ + "param" + juce::String (k + 1)) = (float) d.p[k];
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
    const bool none = static_cast<FxType> (currentTypeIndex ()) == FxType::None;
    if (powerToggle_ != nullptr)
    {
        powerToggle_->setToggleState (on, juce::dontSendNotification);
        // NONE IS the disabled state — the slot processes nothing by
        // definition, so "bypassing" it is a no-op that would only dirty the
        // enabled param in the patch. The toggle therefore goes
        // NON-INTERACTIVE for a None slot (juce disabled buttons receive no
        // mouse events, so onClick never fires) and its dot paints the
        // disabled look (grey + alpha — see paintButton). Nothing is WRITTEN
        // to the enabled param here: a None slot's enabled byte is already
        // semantically moot. Re-enabled the moment a real type is selected
        // (handleAsyncUpdate runs this on every type change).
        powerToggle_->setEnabled (! none);
    }

    // Bypass affordance: recess the LIVE controls (knobs + type
    // combo) when the slot is bypassed, so a disabled slot reads as inactive at a
    // glance — without it a bypassed slot's knobs stay full-brightness and look
    // live. NON-colour (alpha only): the panel / title / power indicator dot keep
    // full alpha so the bypass state + slot identity stay legible. setAlpha is
    // compositing-only (it does NOT disable interaction), so the values remain
    // editable even while bypassed.
    const float contentAlpha = on ? 1.0f : kBypassedAlpha;
    juce::Component* content[] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(), p5_.get(),
                                   drywet_.get(), typeCombo_.get(),
                                   typePrev_.get(), typeNext_.get() };
    for (auto* c : content)
        if (c != nullptr)
            c->setAlpha (contentAlpha);
}

void FxSlotCard::parameterChanged (const juce::String& id, float /*newValue*/)
{
    if (id != prefix_ + "type" && id != prefix_ + "enabled")
        return;

    if (id == prefix_ + "type")
    {
        // W10 (lane-A finding 1b): NO SEEDING HERE ANYMORE. This listener fires
        // for host automation / NRPN writes of fx{N}_type and for undo replay /
        // part loads just like a UI pick — seeding on any of those would
        // CLOBBER the current enabled/drywet/param1..5 with the incoming
        // type's engagement defaults (an automation lane moving fx1_type used
        // to reset the user's knob values on every step). The engagement
        // defaults are now seeded ONLY at the UI seams
        // (FxTypeCombo::onUserPick + stepType -> seedEngagementDefaultsForType)
        // BEFORE they write the type param. Loads/undo still land their own
        // full param sets in descriptor order (type, enabled, drywet,
        // param1..5), exactly as before — the seed was transient there.
        // (The old W7 isPerformingUndoRedo guard is subsumed: nothing seeds
        // here at all.)
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
    // header + type combo). This is the explicit "Dry/Wet hidden
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

    // FIXED cell geometry: the cell height is the CONSTANT kCellH — it is NOT
    // derived from gridArea (the pre-2026-08 behaviour shrank the cells — and
    // with them the dials — whenever the window was resized shorter). Knob
    // size stability parity with the synth pages: ParamPage lays its groups out
    // at FIXED cell sizes and lets the parent Viewport scroll; the FX top row
    // does the same (FxWorkspace::kTopRowNaturalH floors the top-row host at
    // the cards' full-grid natural height, so a shorter frame scrolls instead
    // of starving the grid). If the region is somehow shorter than the block
    // (host floor bypassed), the fixed block is still centred and clipped
    // symmetrically — the dials never scale.
    const int cellH  = kCellH;
    const int cellW  = gridArea.getWidth() / cols;
    const int blockH = rows * cellH;
    // TOP-PINNED grid (2026-08-23 user follow-up — "work just like the synth
    // page's controls"): the grid flows from the TOP of the card body
    // (header -> type row -> grid), exactly how a synth ParamPage group lays
    // its control grid under its title; the card's fixed +20px spaciousness
    // (kCardModuleH) shows as panel space at the BOTTOM of the card instead
    // of padding the grid from both sides. (The old vertical centring was the
    // pre-fixed-height stretch compensation — superseded.)
    const int y0     = gridArea.getY();

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

    // ---- Header row: power lamp (top-left) + title (painted, follows it).
    //      The toggle is the ONLY per-slot enable/bypass control, and the old
    //      16px header strip left it a ~10x12pt hit rect — reliably missed by a
    //      fingertip. Its HIT area is the card's full header band
    //      (44 x (kPad + kHeaderH) = 44x22 at the card's TOP-LEFT; full 44pt
    //      HIG WIDTH, height clamped to the header so it never laps into the
    //      type row) so the lamp it draws sits NEXT TO the "FX N" title
    //      (header reads [lamp][FX1]); the ~12pt dot is pinned
    //      onto the title band's optical middle via setLampCentreOffset, so
    //      the tappable region never moves the lamp away from the title.
    //      (The earlier 44x44 corner-lap trick reached down over the type
    //      row's left edge — with the combo now HORIZONTALLY CENTRED that lap
    //      overlaps it at the narrow floor (layout-overlap gate at 800x400);
    //      the band is header-only now, which removes the overlap at every
    //      width by construction while the combo stays exactly centred.) ----
    area.removeFromTop (kHeaderH);
    if (powerToggle_ != nullptr)
    {
        // 44pt-wide HIG target, height = the header band + 1pt of ring room
        // (kPad + kHeaderH + 1; the lamp's 2.5pt border ring is centred on
        // the dot edge so it extends ~1.25pt past it — the exact +1 band
        // height is what stops the ring's BOTTOM being clipped by the
        // button bounds, the 2026-08-23 "on/off button has its bottom cut
        // off" report. The band still ends 1pt ABOVE the type row (which
        // starts at kPad + kHeaderH + kHalfGap), so the two never meet.)
        const int hitW = juce::jmin (kPowerHitSize, getWidth ());
        const int hitH = juce::jmin (kPad + kHeaderH + 1, getHeight ());
        powerToggle_->setBounds (getLocalBounds ().removeFromTop (hitH).removeFromLeft (hitW));
        // The lamp is pinned to the title band's optical middle
        // (kLampCx/kLampCy are relative to the card's top-left == the band's
        // top-left). powerToggle_ is declared as its juce::Button base in the
        // header (PowerToggle is file-local), so downcast to the concrete
        // type we constructed (now the SHARED ParvatiModuleLamp subclass).
        if (auto* lamp = static_cast<PowerToggle*> (powerToggle_.get ()))
        {
            lamp->setLampCentreOffset ({ kLampCx, kLampCy });
            // PIN the dot at 14pt (2026-08-23): the proportional default
            // (jmin(44, kPad+kHeaderH)*0.68 ~= 16.3 after the kPad 6->8
            // harmonization) pushed the ring's bottom past the band edge —
            // the clipped-bottom look. 14pt keeps ring-outer bottom at
            // kLampCy + 7 + 1.25 = 24.25 < 25 (the +1 band).
            lamp->setLampDiameter (kLampDotW);
        }
    }

    if (area.isEmpty())
        return;
    area.removeFromTop (kHalfGap);   // gap below the header divider

    // ---- Type row: the algorithm dropdown as a STYLED combo (Osc "Shape" /
    //      Filter "Mode" parity) — fit-to-text width, HORIZONTALLY CENTRED in
    //      the card width. The combo already inherits the editor-wide ComboBox
    //      theme colours (backgroundInput fill, amber accentPrimary chevron,
    //      borderless) via the LookAndFeel — identical to the synth selectors
    //      — so only the SIZE + position are set here. ----
    if (typeCombo_ != nullptr && area.getHeight() > kTypeRowH)
    {
        auto typeRow = area.removeFromTop (kTypeRowH);
        // CENTRED IN THE FULL ROW: the old code trimmed the power toggle's
        // corner band off the RIGHT before centring, which shifted the combo
        // left of centre by half the band — the "dropdown looks off-centre"
        // report. The toggle band is HEADER-ONLY now (44x22, ends at the
        // header divider ~2px above this row), so a centred fit-to-text combo
        // can never intersect it at any card width.
        const int textW  = widestComboTextWidth (*typeCombo_, typeCombo_.get()) + kComboChrome;
        const int comboW = juce::jmin (typeRow.getWidth (),
                                       juce::jlimit (kComboMinW, juce::jmax (kComboMinW, typeRow.getWidth ()), textW));
        // The whole algorithm selector is ONE 44pt-tall tap target that opens
        // the effect picker. Centred in the card width, fit-to-text width;
        // the 44pt combo fills the 44pt row. (The prev/next chevrons are not
        // placed.)
        const int comboX = typeRow.getX () + (typeRow.getWidth () - comboW) / 2;
        typeCombo_->setBounds (comboX, typeRow.getY (), comboW, kComboH);
        // Defensive z-order guard: hit-testing walks siblings front-first, so
        // the combo wins any future overlap (none exists with the
        // header-only band above).
        typeCombo_->toFront (false);
        area.removeFromTop (kHalfGap);
    }

    // ---- Body: the Mixer-style 3-column knob GRID owns the whole body
    //      (the visualizer band was REMOVED 2026-08-20 — the user asked for
    //      the FX graphic illustrations gone; the grid centres vertically in
    //      the freed height, so the card has no hole where the band was). ----
    const auto t = static_cast<FxType> (currentTypeIndex());
    // The knob grid is a FIXED 3x2 layout (up to 5 params + Dry/Wet) whenever
    // the slot has an effect; for None the grid collapses entirely (Dry/Wet
    // hidden). The grid height is FIXED at rows*kCellH (knob size stability —
    // see layoutParamGrid); if the body is shorter, the grid keeps its fixed
    // height and the card relies on the workspace top-row scroll floor
    // (kTopRowNaturalH) — the grid is never squeezed.
    static_cast<void> (t);

    layoutParamGrid (area);
}

//==============================================================================
void FxSlotCard::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel ());
    const ParvatiTheme* t = lnf != nullptr ? lnf->getTheme () : nullptr;

    const juce::Colour panel   = t ? t->containerFill   : juce::Colour (0xff202028);
    // HEADER COLOUR PARITY (user feedback 2026-08-20): the synth page's
    // module headers are GroupComponent titles painted by the L&F in
    // textPrimary (GroupComponent::textColourId == theme.textPrimary after
    // setTheme). The FX card title previously used textSecondary — the
    // mismatch the user reported. Same token now; re-resolved per paint so a
    // theme switch re-colours both paths together.
    const juce::Colour title   = t ? t->textPrimary     : juce::Colour (0xffe8e8ee);

    const auto r = getLocalBounds ().toFloat ();

    // ---- Card panel: BORDERLESS (synth GroupComponent parity). Depth comes from
    //      the tonal lift of containerFill over the page backgroundBase — no
    //      outline, no under-header divider (the title band alone separates). ----
    g.setColour (panel);
    g.fillRoundedRectangle (r, kCorner);

    // ---- Title "FX N" header: LAMP (top-left, drawn by the PowerToggle
    //      child) + title text following it — the header reads [lamp][FX1]
    //      left-to-right. BOLD + UPPERCASE (14px), mirroring the synth card
    //      GroupComponent header. ----
    juce::Font font = lnf != nullptr ? lnf->appFont (14.0f, juce::Font::bold)
                                    : juce::Font (juce::FontOptions (14.0f, juce::Font::bold));
    const juce::String name = "FX" + juce::String (slot_ + 1);   // "FX1" (uppercase)
    // kLampTitleGap right of the 15pt lamp sits the title text. (The old
    // accent tick between the lamp and the title went away with the lamp
    // redesign — one indicator glyph per header is enough.)
    const int titleX = kPad + static_cast<int> (kLampDotW) + kLampTitleGap;
    const int titleW = juce::jmax (0, getWidth() - titleX - kPad);
    const juce::Rectangle<int> titleRect (titleX, kPad, titleW, kHeaderH);

    g.setColour (title);
    g.setFont (font);
    g.drawText (name, titleRect, juce::Justification::centredLeft, true);
}

//==============================================================================
void FxSlotCard::applyThemeColors()
{
    // (The per-slot visualizer was REMOVED 2026-08-20 — nothing to re-tint;
    // the owned ParamControl knobs are re-themed by the editor's global
    // ParamControl::reapplyCategoryColours() pass on a theme switch.)
    repaint();
}

juce::Colour FxSlotCard::headerTitleColourForTest() const
{
    // (getLookAndFeel() returns LookAndFeel& even on a const Component — JUCE
    // semantics — so the cast targets the non-const type.)
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        if (const ParvatiTheme* t = lnf->getTheme())
            return t->textPrimary;
    return juce::Colour (0xffe8e8ee);   // paint()'s no-L&F fallback
}

ParvatiModuleLamp* FxSlotCard::powerLampForTest() const
{
    return static_cast<ParvatiModuleLamp*> (powerToggle_.get());
}
