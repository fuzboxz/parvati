// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ui/ParamControl.h.

#include "ParamControl.h"

#include "ModMatrixHighlight.h"   // parvati::ModMatrixHighlight (dest-highlight bus)
#include "ModSourceCatalog.h"     // parvati::entryFor (tap-assign status text)
#include "ParamHelp.h"            // getParamHelp (per-parameter tooltip text)
#include "ParvatiLookAndFeel.h"   // widestComboTextWidth + ParvatiLookAndFeel::getTheme
#include "ParvatiTheme.h"         // ParvatiTheme category tokens

#include <algorithm>   // std::remove (instance registry teardown)

//==============================================================================
// ---- Map a parameter ID to one of the GUI sections --------------------------
// (Derived from the well-defined paramID prefixes in ParameterLayout.cpp, so the
//  checked APVTS byte-bridge stays untouched. The enum itself lives in
//  ui/ParamControl.h — it is shared with the editor's page generation.)
Section sectionForId (const juce::String& id)
{
    // Global synth options (no Patch/Part byte) live on the dedicated Global
    // tab. Check these BEFORE the prefix rules so e.g. "filter_card" (a global
    // filter voice-card selector) is not swept into the Filter page.
    if (id == "filter_card") return Section::Global;
    if (id == "vca_curve")    return Section::Global;
    if (id == "filter_drive") return Section::Global;   // Ladder drive: a global option

    // ---- Per-part FX params (isFx). Checked early so the generic synth prefixes
    // never sweep an FX id. fxmod{N}_* -> the FX mod matrix (FxMatrix, a clone of
    // the synth matrix, NOT a generated ParamPage). fx{1,2,3}_* per-slot params +
    // fx_topo / fx_order (fx_*) -> Section::Fx (generated into FX-slot pages).
    if (id.startsWith ("fxmod")) return Section::FxMatrix;
    if (id.startsWith ("fx1") || id.startsWith ("fx2") || id.startsWith ("fx3")
        || id.startsWith ("fx_"))   return Section::Fx;

    // Order matters: "modif" before "mod", "arp" before others.
    if (id.startsWith ("arp"))       return Section::Arp;
    if (id.startsWith ("seq"))       return Section::Sequencer;
    if (id.startsWith ("osc"))       return Section::Oscillators;
    if (id.startsWith ("mix"))       return Section::Mixer;
    if (id.startsWith ("filter"))    return Section::Filter;
    if (id.startsWith ("modif"))     return Section::Modifiers;
    if (id.startsWith ("mod"))       return Section::ModMatrix;
    // Each firmware env_lfo unit is BOTH an envelope (A/D/S/R) and an LFO
    // (shape/rate); they are independent modulation sources, so the two halves
    // route to separate Envelopes / LFOs tabs (the struct sharing is a firmware
    // memory-layout detail only). voice_lfo_* is the per-voice LFO (MOD_SRC_LFO_4).
    if (id.startsWith ("voice_lfo")) return Section::Lfos;
    if (id.startsWith ("env"))       return id.contains ("_lfo_") ? Section::Lfos : Section::Envelopes;
    // Part params stay on the Patch-hosted Global page (the per-part output
    // stage sits with the part table — measured: the Mixer page has only
    // ~41px of top-row slack at the default size, not enough for a 3-knob
    // panel without introducing a new scrollbar; see
    // audit/work_patch_page.md). The OTHER part_* knobs (octave / legato /
    // portamento / raga / polyphony) are absorbed into the Patch page's
    // per-part table and are SKIPPED from page generation in the editor's
    // bucket loop (their APVTS parameters remain: host automation, state and
    // files keep driving the bytes).
    if (id.startsWith ("part"))      return Section::Global;
    return Section::Global;
}

//==============================================================================
// Map a functional Section to its theme category-token colour. Oscillators /
// Mixer / Filter / ModMatrix / Modifiers / Global share the neutral
// "audio" brand accent; Envelopes/LFOs/Sequencer/Arp get their own hue. This is the
// ONLY place a Section resolves to a category token, so every arc / graph / tint
// shares one consistent mapping and a theme switch re-resolves automatically.
juce::Colour categoryColourForSection (const ParvatiTheme& theme, Section s)
{
    // Only Envelopes/LFOs/Sequencer/Arp carry a distinct hue; every other
    // section shares the theme's "audio" brand accent (catAudio). (if-chain, not switch, so the
    // remaining categories fall through to the neutral default without a
    // -Wswitch-enum warning.)
    if (s == Section::Envelopes) return theme.catEnv;
    if (s == Section::Lfos)      return theme.catLfo;
    if (s == Section::Sequencer) return theme.catSeq;
    if (s == Section::Arp)       return theme.catArp;
    return theme.catAudio;   // Osc/Mix/Filter/ModMatrix/Modifiers/Global
}


//==============================================================================
//==============================================================================
bool ParamControl::tooltipsEnabled_ = true;
bool ParamControl::modDragActive_    = false;
bool        ParamControl::tapAssignActive_      = false;
int         ParamControl::tapSelectedSource_    = -1;
juce::String ParamControl::transientStatusText_;
int         ParamControl::transientStatusFrames_ = 0;

namespace
{
// Live ParamControl instances (message-thread only: built/destroyed by the GUI
// component tree, toggled from the Settings panel). Function-local static avoids
// static-initialization-order issues across translation units.
std::vector<ParamControl*>& paramControlRegistry()
{
    static std::vector<ParamControl*> r;
    return r;
}

// Strip the redundant section prefix from a control's display label, driven by
// the parameter ID prefix (deterministic, easy to review). The full label is
// preserved elsewhere (tooltip / accessibility), so the section name stays
// discoverable. If the label does not start with the expected prefix, it is
// returned unchanged — never mangled. Display-only: ParameterLayout.cpp is NOT
// touched.
juce::String displayLabelFor (const juce::String& paramID, const juce::String& fullLabel)
{
    const auto stripPrefix = [&] (const juce::String& labelPrefix) -> juce::String
    {
        if (fullLabel.startsWith (labelPrefix))
            return fullLabel.substring (labelPrefix.length());
        return fullLabel;   // mismatch — never mangle
    };

    // env{1-3}_lfo_* — check BEFORE the generic envN_ ADSR rule
    for (int i = 1; i <= 3; ++i)
    {
        const auto n = juce::String (i);
        if (paramID.startsWith ("env" + n + "_lfo_"))
            return stripPrefix ("Env " + n + " LFO ");
    }

    // voice_lfo_*
    if (paramID.startsWith ("voice_lfo_"))
        return stripPrefix ("Voice LFO ");

    // osc1_ / osc2_
    for (int i = 1; i <= 2; ++i)
    {
        const auto n = juce::String (i);
        if (paramID.startsWith ("osc" + n + "_"))
            return stripPrefix ("Osc " + n + " ");
    }

    // filter1_
    if (paramID.startsWith ("filter1_"))
        return stripPrefix ("Filter 1 ");

    // env{1-3}_ (ADSR — LFO handled above)
    for (int i = 1; i <= 3; ++i)
    {
        const auto n = juce::String (i);
        if (paramID.startsWith ("env" + n + "_"))
            return stripPrefix ("Env " + n + " ");
    }

    // mod{N}_*  (N is the digit run after "mod")
    if (paramID.startsWith ("mod") && paramID.length() > 3
        && paramID[3] >= '0' && paramID[3] <= '9')
    {
        juce::String num;
        for (int i = 3; i < paramID.length() && paramID[i] >= '0' && paramID[i] <= '9'; ++i)
            num += paramID[i];
        return stripPrefix ("Mod " + num + " ");
    }

    // modif{N}_*
    if (paramID.startsWith ("modif") && paramID.length() > 5
        && paramID[5] >= '0' && paramID[5] <= '9')
    {
        juce::String num;
        for (int i = 5; i < paramID.length() && paramID[i] >= '0' && paramID[i] <= '9'; ++i)
            num += paramID[i];
        return stripPrefix ("Modifier " + num + " ");
    }

    // arp_*
    if (paramID.startsWith ("arp_"))
        return stripPrefix ("Arp ");

    // mix_*, part_*, filter_card, vca_curve, filter_drive, seq step/length —
    // already short: leave as-is.
    return fullLabel;
}
}  // namespace

//==============================================================================
// Tooltip helper for popup-menu items. See ParamControl::showContextMenu.
namespace
{
// A PopupMenu item rendered EXACTLY like a default item (via the active L&F
// drawPopupMenuItem — same geometry, colours and font as every other entry) but
// ALSO a juce::TooltipClient, so hovering it shows a short description.
// juce::PopupMenu items have no native tooltip field; a PopupMenu::CustomComponent
// carrying the tooltip is the JUCE idiom. Because it is "triggered automatically"
// the menu still fires the item action on a click — no manual mouse handling.
// getIdealSize() delegates to the L&F so the row matches the height/width of a
// normal item (no clipping, no layout change).
class TooltipMenuItemComponent final : public juce::PopupMenu::CustomComponent,
                                       public juce::TooltipClient
{
public:
    TooltipMenuItemComponent (juce::String itemText, juce::String tip)
        : text_ (std::move (itemText)), tip_ (std::move (tip)) {}

    void getIdealSize (int& idealWidth, int& idealHeight) override
    {
        getLookAndFeel().getIdealPopupMenuItemSize (text_, false, 0,
                                                    idealWidth, idealHeight);
    }

    void paint (juce::Graphics& g) override
    {
        getLookAndFeel().drawPopupMenuItem (g, getLocalBounds(),
                                            false,                 // isSeparator
                                            isEnabled(),           // isActive
                                            isItemHighlighted(),   // isHighlighted
                                            false,                 // isTicked
                                            false,                 // hasSubMenu
                                            text_,
                                            juce::String(),        // shortcutKeyText
                                            nullptr,               // icon
                                            nullptr);              // textColour -> L&F default
    }

    // Respect the global tooltips toggle (Settings panel): when tooltips are
    // disabled the context-menu items show nothing either, matching every other
    // ParamControl tooltip.
    juce::String getTooltip() override
    {
        return ParamControl::tooltipsEnabled() ? tip_ : juce::String();
    }

private:
    juce::String text_, tip_;
};

// (ChromeRule — the chrome separator rules — now lives in ui/ChromeRule.h,
// shared with the Synth/FxWorkspace mod-bar seams + the keyboard rule.)

}  // namespace

//==============================================================================
ParamControl::ParamControl (ParvatiAudioProcessor& processor, const PatchParamDescriptor& d)
    : desc_ (d), processor_ (processor), paramIDStr_ (d.paramID)
{
    // Viewport safety net (T4): a TOUCH drag that starts on a control cell must
    // not ALSO scroll an enclosing juce::Viewport (the viewport's
    // drag-to-scroll listener sees child events and the default nonHover mode
    // is touch-only). setViewportIgnoreDragFlag is juce's documented opt-out;
    // it is only consulted by a Viewport's drag listener, so cells in non-
    // viewport hosts (the main-row pages) are completely unaffected.
    setViewportIgnoreDragFlag (true);

    // Visible label uses the short (prefix-stripped) display form so the section
    // name (shown in the panel border) is not redundantly repeated in every knob
    // caption. The FULL label is preserved for tooltips / accessibility (see
    // getTooltip -> getParamHelp, which returns the full param help text).
    label_ = std::make_unique<juce::Label> (d.paramID + "_lbl",
                                            displayLabelFor (d.paramID, d.label));
    label_->setJustificationType (juce::Justification::centred);
    label_->setFont (juce::FontOptions (12.0f));
    // Label / combo / slider colours all come from the editor-wide
    // ParvatiLookAndFeel (inherited through the component tree).
    addAndMakeVisible (*label_);

    if (d.choices != nullptr)
    {
        comboBox_ = std::make_unique<juce::ComboBox> (d.paramID);
        comboBox_->addItemList (*d.choices, 1);
        // HIG touch target: the combo's BOUNDS are laid out 44pt tall (see
        // resized) but the DRAWN dropdown stays a compact 28pt strip centred
        // inside them — the L&F reads this "parvatiComboVisualH" property
        // (drawComboBox / positionComboBoxText), so the tap band is full-size
        // while the desktop look is pixel-identical.
        comboBox_->getProperties().set ("parvatiComboVisualH", 28);
        addAndMakeVisible (*comboBox_);
        comboAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.getApvts(), d.paramID, *comboBox_);
        // Catch right-clicks on the combo (it would otherwise consume the popup
        // click before this component sees it). `false` => events for the combo
        // only (no recursion into its popup children).
        comboBox_->addMouseListener (this, false);

        // A modulation-source combo (modN_source / modifN_in1|in2) listens to its
        // OWN value so it can re-tint its closed-box background to 15% alpha of
        // the SELECTED source's category colour as the routing changes.
        // Ordinary combos (osc shape / filter mode / mix op) are left untinted.
        isModSourceCombo_ = paramIDStr_.endsWith ("_source")
            || (paramIDStr_.startsWith ("modif")
                && (paramIDStr_.endsWith ("_in1") || paramIDStr_.endsWith ("_in2")));
        if (isModSourceCombo_)
        {
            processor_.getApvts().addParameterListener (paramIDStr_, this);
            applyModSourceTint();   // seed the tint for the current source
        }
    }
    else
    {
        slider_ = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag,
                                                   juce::Slider::NoTextBox);
        // Knobs adjust by DRAG only: disable the mouse wheel so an accidental
        // scroll over a knob never changes its value (the wheel is reserved for
        // scrolling lists / the Mod Matrix viewport). Drag + the right-click
        // context menu (Reset/Randomize) still adjust the value.
        slider_->setScrollWheelEnabled (false);
#if JUCE_IOS
        // Finer knob drag for touch: a larger pixelRange means a longer drag is
        // needed to sweep the full range, so fat-finger edits are more precise.
        // JUCE Slider drags are ALREADY screen-relative once initiated (the drag
        // accumulates cursor delta anywhere on screen), so HIG #2's "across the
        // entire screen" requirement is satisfied by default — this just scales
        // the precision for touch (the HIG #2 fine-control note).
        slider_->setMouseDragSensitivity (400);
#endif
        if (d.isSequencer && paramIDStr_.startsWith ("seq_length_"))
        {
            // The length control is marked ("Length" label) so it reads as the
            // sequence length, not just another step pot. Step cells keep their
            // label hidden (the group header "Sequencer n" identifies them) and
            // are dimmed when past the active length (refreshStepEnabled).
            label_->setText (TRANS ("Length"), juce::dontSendNotification);
            label_->setFont (juce::FontOptions (12.0f, juce::Font::bold));
        }
        else if (d.isSequencer)
        {
            label_->setVisible (false);   // plain step cells: no label
        }
        addAndMakeVisible (*slider_);
        sliderAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.getApvts(), d.paramID, *slider_);
        // Catch right-clicks on the knob (so the cell's popup menu shows).
        slider_->addMouseListener (this, false);
        // Colour the knob's fill ARC by functional category (theme-accent Osc/Filter/Mix,
        // cyan Env, magenta LFO, green Seq, purple Arp). Only the arc adopts the
        // category hue; the numeric value readout stays neutral. The LookAndFeel
        // drawRotarySlider reads this colour ID per-component, so this overrides
        // ONLY this knob (no L&F fork). Re-applied on theme change.
        applyCategoryArcColour();
    }

    // Cache the per-parameter help text and push it onto the interactive
    // children. The editor-wide TooltipWindow only queries the LEAF component
    // under the cursor, so the cell's own TooltipClient is insufficient — the
    // mouse actually hovers the Slider/ComboBox/Label child. Register the
    // instance so the global on/off toggle (setTooltipsEnabled) can re-apply
    // the text later without rebuilding the controls.
    helpText_ = getParamHelp (d.paramID);
    paramControlRegistry().push_back (this);
    applyTooltipState();

    // Sequencer steps subscribe to their sibling length param so they dim/enable
    // live as the length changes. Seeded once here from the current value.
    lengthParamID_ = siblingLengthParamFor (paramIDStr_);
    if (lengthParamID_.isNotEmpty())
    {
        processor_.getApvts().addParameterListener (lengthParamID_, this);
        refreshStepEnabled();
    }

    // Modulation-ring destination knobs: resolve the MOD_DST_* this knob is the
    // base value of (combos / sequencer steps are unaffected — slider_ is null).
    // A destination knob listens to all 14 mod{1..14}_amount params so ANY matrix
    // edit refreshes the outer aggregate-depth ring drawn by the LookAndFeel.
    // Seeded once here from the current APVTS values.
    modDest_ = parvati::ModDestMap::destForParamID (paramIDStr_);
    isModDestKnob_ = (modDest_ >= 0 && slider_ != nullptr);
    if (isModDestKnob_)
    {
        // A source change recolours an arc, a dest change adds/removes an arc,
        // and an amount change resizes one, so listen to ALL of this knob's
        // domain's matrix params (14 synth mod slots, or 16 FX fxmod slots).
        const bool        fx       = parvati::ModDestMap::isFxDest (modDest_);
        const int         numSlots = fx ? parvati::ModDestMap::kFxNumSlots : 14;
        const juce::String prefix  = fx ? "fxmod" : "mod";
        for (int slot = 1; slot <= numSlots; ++slot)
        {
            const auto s = juce::String (slot);
            processor_.getApvts().addParameterListener (prefix + s + "_source", this);
            processor_.getApvts().addParameterListener (prefix + s + "_dest", this);
            processor_.getApvts().addParameterListener (prefix + s + "_amount", this);
        }
        refreshModRing();

        // Receive hover/selection broadcasts from the Mod Matrix (and from other
        // knobs) so this knob's modulation ring glows when its dest is the
        // highlighted target. The callback is SafePointer-guarded AND explicitly
        // unsubscribed in the dtor, so a stale broadcast after teardown is a
        // safe no-op (see the multi-editor caveat in ModMatrixHighlight.h).
        juce::Component::SafePointer<ParamControl> safe (this);
        modHighlightSub_ = parvati::ModMatrixHighlight::instance().onDestHighlighted (
            [safe] (int modDst) { if (safe != nullptr) safe->applyModHighlight (modDst); });

        // Register this cell as a MouseListener on the label too so the hover
        // highlight fires across the whole cell, not just the dial (the slider
        // is already registered above). `false` => label events only.
        if (label_ != nullptr)
            label_->addMouseListener (this, false);
    }
}

ParamControl::~ParamControl()
{
    if (lengthParamID_.isNotEmpty())
        processor_.getApvts().removeParameterListener (lengthParamID_, this);
    if (isModSourceCombo_)
        processor_.getApvts().removeParameterListener (paramIDStr_, this);
    if (isModDestKnob_)
    {
        const bool        fx       = parvati::ModDestMap::isFxDest (modDest_);
        const int         numSlots = fx ? parvati::ModDestMap::kFxNumSlots : 14;
        const juce::String prefix  = fx ? "fxmod" : "mod";
        for (int slot = 1; slot <= numSlots; ++slot)
        {
            const auto s = juce::String (slot);
            processor_.getApvts().removeParameterListener (prefix + s + "_source", this);
            processor_.getApvts().removeParameterListener (prefix + s + "_dest", this);
            processor_.getApvts().removeParameterListener (prefix + s + "_amount", this);
        }
        if (label_ != nullptr)
            label_->removeMouseListener (this);
        if (modHighlightSub_ >= 0)
            parvati::ModMatrixHighlight::instance().unsubscribe (modHighlightSub_);
    }
    auto& r = paramControlRegistry();
    r.erase (std::remove (r.begin(), r.end(), this), r.end());
}

//==========================================================================
// Sequencer step dimming: map a step paramID to its sibling sequence length
// param, parse the step index, and enable/disable the step's slider so steps
// past the active length read as inactive (the LookAndFeel omits the knob's
// fill arc when a slider is disabled).
juce::String ParamControl::siblingLengthParamFor (const juce::String& id)
{
    if (id.startsWith ("seq1_step")) return "seq_length_1";
    if (id.startsWith ("seq2_step")) return "seq_length_2";
    if (id.startsWith ("seqnote_step") || id.startsWith ("seqnote_vel")) return "seq_length_3";
    return {};
}

int ParamControl::parseStepIndex (const juce::String& id)
{
    // Extract the trailing digit run (e.g. "seq1_step7" -> 7,
    // "seqnote_vel11" -> 11). Steps are the only params with a non-empty
    // lengthParamID_, so a stray number elsewhere is harmless.
    int lastDigit = id.length() - 1;
    while (lastDigit >= 0 && id[lastDigit] >= '0' && id[lastDigit] <= '9')
        --lastDigit;
    if (lastDigit + 1 < id.length())
        return id.substring (lastDigit + 1).getIntValue();
    return -1;
}

void ParamControl::parameterChanged (const juce::String& id, float)
{
    // F-ui-1 (bug hunt 2026-08-18): this listener ALSO fires on the audio
    // thread — the in-processBlock NRPN/CC map (PluginProcessor.cpp) and host
    // automation both call setValueNotifyingHost from the render thread.
    // Component mutation (setEnabled/setColour/properties/repaint) is
    // message-thread-only; defer to the coalesced async refresh.
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        triggerAsyncUpdate();
        return;
    }
    if (id == lengthParamID_)
        refreshStepEnabled();
    if (isModSourceCombo_ && id == paramIDStr_)   // selected mod source changed
        applyModSourceTint();
    // A mod-destination knob refreshes its per-source rings on ANY matrix edit:
    // a slot's SOURCE change recolours an arc, a DEST change adds/removes an
    // arc, an AMOUNT change resizes one (listeners cover all 42 synth mod params,
    // or 48 fxmod params for an FX knob).
    if (isModDestKnob_ && (id.startsWith ("mod") || id.startsWith ("fxmod")))
        refreshModRing();
}

void ParamControl::handleAsyncUpdate()
{
    // F-ui-1: the deferred half of parameterChanged — runs on the message
    // thread, from CURRENT state (the param id that triggered the deferral is
    // irrelevant: every refresh below is idempotent and cheap).
    refreshStepEnabled();   // self-guards on lengthParamID_ emptiness
    if (isModSourceCombo_)
        applyModSourceTint();
    if (isModDestKnob_)
        refreshModRing();
}

void ParamControl::refreshStepEnabled()
{
    if (lengthParamID_.isEmpty() || slider_ == nullptr)
        return;
    if (auto* raw = processor_.getApvts().getRawParameterValue (lengthParamID_))
    {
        // The length param's range is 1..16 (one cell per step); cap defensively.
        const int len = juce::jlimit (1, 16, juce::roundToInt (raw->load()));
        const int idx = parseStepIndex (paramIDStr_);
        const bool on = (idx < 0) || (idx < len);
        slider_->setEnabled (on);
        repaint();
    }
}

void ParamControl::applyTooltipState()
{
    const juce::String tip = tooltipsEnabled_ ? helpText_ : juce::String();
    if (label_    != nullptr) label_->setTooltip (tip);
    if (slider_   != nullptr) slider_->setTooltip (tip);
    if (comboBox_ != nullptr) comboBox_->setTooltip (tip);
}

void ParamControl::setDisplayLabel (const juce::String& label)
{
    // Override the visible caption (FxSlotCard relabels FX knobs to the active
    // algorithm's semantic name, e.g. "Time"/"Feedback", instead of the static
    // "FX1 Param 1" descriptor label). An EMPTY string reverts to the
    // descriptor-derived label (displayLabelFor). Harmless on controls with a
    // hidden label (sequencer step cells) — the text is set on the invisible
    // component; the override is stored so a re-show would honour it.
    displayLabelOverride_ = label;
    if (label_ != nullptr)
    {
        const auto text = displayLabelOverride_.isEmpty()
            ? displayLabelFor (desc_.paramID, desc_.label)
            : displayLabelOverride_;
        label_->setText (text, juce::dontSendNotification);
    }
}

void ParamControl::setDisplayValuePercent (bool percent)
{
    // Display-only: converts the stored 0..127 value to a 0..100% readout via the
    // slider's textFromValueFunction (the LookAndFeel renders it at
    // slider.getTextFromValue()). Stored value stays 0..127 (serialization
    // unchanged). FX slot knobs use this for a friendlier readout.
    if (slider_ == nullptr)
        return;
    if (percent)
    {
        slider_->textFromValueFunction = [] (double v)
        {
            return juce::String (juce::roundToInt (v / 127.0 * 100.0)) + "%";
        };
        slider_->valueFromTextFunction = [] (const juce::String& s)
        {
            return s.retainCharacters ("0123456789").getDoubleValue() / 100.0 * 127.0;
        };
    }
    else
    {
        slider_->textFromValueFunction = {};
        slider_->valueFromTextFunction = {};
    }
    slider_->repaint();
}

void ParamControl::setDisplayValueText (std::function<juce::String (double)> toText)
{
    // Display-only: installs a custom value->text formatter on the slider (note
    // names, +/-semitones, Hz, On/Off, ...). The stored value is unchanged. The
    // knob is drag-only (NoTextBox) so no valueFromTextFunction is wired (typed
    // entry would need a per-param inverter — omitted; falls back to raw drag).
    if (slider_ == nullptr)
        return;
    slider_->textFromValueFunction = std::move (toText);
    slider_->repaint();
}

int ParamControl::maxChoiceTextWidth() const
{
    // Measure every choice (plus the combo's current text) in the shared
    // combo-list font so the dropdown fits its longest entry (see
    // widestComboTextWidth). Used to size the fit-to-text combo width
    // (longest + 24px padding, capped at 140px).
    return widestComboTextWidth (*this, comboBox_.get(),
                                 desc_.choices != nullptr ? *desc_.choices : juce::StringArray{});
}

void ParamControl::setTooltipsEnabled (bool enabled)
{
    tooltipsEnabled_ = enabled;
    for (auto* c : paramControlRegistry())
        c->applyTooltipState();
}

void ParamControl::setModDragActive (bool active)
{
    modDragActive_ = active;
    for (auto* c : paramControlRegistry())
        c->applyModDragAffordance();
}

void ParamControl::setTapAssignActive (bool active)
{
    // [MOD] toggle entry. Reuses the existing drop-zone affordance (ring on
    // destination knobs, dim non-targets) so the tap mode gets the exact same
    // visual feedback as a desktop drag, with zero LookAndFeel changes.
    tapAssignActive_ = active;
    tapSelectedSource_ = -1;   // clear any pending source on entry/exit
    parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
    setModDragActive (active);
    if (active)
        postTransientStatus (TRANS ("Tap a mod source, then a knob"), 90);   // entry hint (~3s)
}

void ParamControl::setTapSelectedSource (int sourceEnum) noexcept
{
    tapSelectedSource_ = sourceEnum;
    // Touch has no hover/cursor, so surface the armed source by name in the
    // status strip (reuses the transient-status mechanism). Skipped on a reset
    // (sourceEnum < 0) so a consumed/cleared selection stays silent.
    // P3 lesson: while [MOD] is engaged a plain pill click arms the source
    // instead of browsing the generator — without a loud, self-explanatory
    // status this reads as "pill clicks no longer work". So the armed message
    // spells out BOTH halves of the gesture and lasts ~5s (150 frames @30 Hz),
    // long enough to be noticed mid-flow.
    if (sourceEnum >= 0)
        postTransientStatus (TRANS ("MOD assign armed: ") + parvati::entryFor (sourceEnum).fullName
                                 + TRANS (" — tap a destination knob ([MOD] to exit)"),
                             150);
}

void ParamControl::postTransientStatus (const juce::String& text, int frames)
{
    transientStatusText_   = text;
    transientStatusFrames_ = juce::jmax (1, frames);
}

juce::String ParamControl::tickTransientStatus()
{
    // Returns the armed text while the frame budget lasts (drained ~30 Hz by
    // the editor timer), or empty once expired so the normal hover tooltip
    // takes back over the status strip.
    if (transientStatusFrames_ > 0)
    {
        --transientStatusFrames_;
        return transientStatusText_;
    }
    return {};
}

void ParamControl::applyCategoryArcColour()
{
    if (slider_ == nullptr)
        return;
    // Resolve the category hue from the CURRENT theme via the component's L&F
    // (zero hardcoded hues; every value comes from a theme token).
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        if (const auto* theme = lnf->getTheme())
        {
            slider_->setColour (juce::Slider::rotarySliderFillColourId,
                                categoryColourForSection (*theme, sectionForId (paramIDStr_)));
            return;
        }
    // No theme yet (pre-L&F construction): leave the L&F default untouched.
}

void ParamControl::applyModSourceTint()
{
    if (comboBox_ == nullptr || ! isModSourceCombo_)
        return;
    // Guard against any re-entrant path (setColour does not itself change the
    // param, but the flag keeps the contract explicit, like the Patch page).
    if (refreshingModTint_)
        return;
    const juce::ScopedValueSetter<bool> guard (refreshingModTint_, true);

    // Read the SELECTED source's name from the APVTS raw value (the choice index
    // into kModSources). Reading the param directly avoids racing the combo
    // attachment's own listener update.
    juce::String sourceName;
    if (desc_.choices != nullptr && ! desc_.choices->isEmpty())
    {
        int idx = 0;
        if (auto* raw = processor_.getApvts().getRawParameterValue (paramIDStr_))
            idx = juce::jlimit (0, desc_.choices->size() - 1, juce::roundToInt (raw->load()));
        sourceName = (*desc_.choices) [idx];
    }

    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        if (const auto* theme = lnf->getTheme())
        {
            // The category colour is resolved by the shared helper. Neutral
            // sources (Op/Const/Velocity/etc) resolve to `accent`; only the
            // four functional categories carry a tint, so the tint-vs-revert
            // decision still keys on the category prefixes.
            const juce::Colour tintCol = categoryColourForSourceName (sourceName, *theme);
            const bool hasTint = sourceName.startsWith ("Env")
                              || sourceName.startsWith ("LFO") || sourceName == "Voice LFO"
                              || sourceName.startsWith ("Seq")
                              || sourceName.startsWith ("Arp");

            if (hasTint)
                comboBox_->setColour (juce::ComboBox::backgroundColourId, tintCol.withAlpha (0.15f));
            else
                comboBox_->removeColour (juce::ComboBox::backgroundColourId);   // revert to the L&F default
            comboBox_->repaint();
            return;
        }
}

juce::Colour ParamControl::categoryColourForSourceName (const juce::String& name,
                                                        const ParvatiTheme& theme)
{
    // One consistent source-name -> category-colour mapping (shared by the
    // mod-source combo tint and the per-source modulation ring). A source whose
    // name matches no category resolves to the neutral `accent`.
    if (name.startsWith ("Env"))                                return theme.catEnv;
    if (name.startsWith ("LFO") || name == "Voice LFO")         return theme.catLfo;
    if (name.startsWith ("Seq"))                                return theme.catSeq;
    if (name.startsWith ("Arp"))                                return theme.catArp;
    return theme.accentPrimary;   // Op/Const/Velocity/etc => neutral
}

void ParamControl::refreshModRing()
{
    if (! isModDestKnob_ || slider_ == nullptr)
        return;
    // Guard against any re-entrant repaint path (mirrors refreshingModTint_).
    if (refreshingModRing_)
        return;
    const juce::ScopedValueSetter<bool> guard (refreshingModRing_, true);

    // Bound the concentric stack (one arc per active source) so it never
    // overflows the cell.
    constexpr int kMaxModRings = 6;

    auto& props = slider_->getProperties();

    // Resolve the theme via the component's L&F (same path applyModSourceTint
    // uses). If the theme is not yet reachable (pre-reparent construction) defer
    // — the per-source props get pushed on reparent (parentHierarchyChanged /
    // lookAndFeelChanged) as today, so clear them here to avoid a stale render.
    const ParvatiTheme* theme = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        theme = lnf->getTheme();

    auto clearAll = [&]
    {
        props.remove ("parvatiModN");
        for (int i = 0; i < kMaxModRings; ++i)
        {
            props.remove ("parvatiModCol" + juce::String (i));
            props.remove ("parvatiModAmt" + juce::String (i));
        }
        // Legacy single-arc keys (no longer set / read).
        props.remove ("parvatiModDepth");
        props.remove ("parvatiModPosArgb");
        props.remove ("parvatiModNegArgb");
    };

    if (theme == nullptr)
    {
        clearAll();
        slider_->repaint();
        return;
    }

    // Build the per-source active list: one concentric arc per matrix slot
    // routed to this knob's destination (ascending slot order), each coloured by
    // that source's functional CATEGORY (Env=cyan, LFO=magenta, Seq=green,
    // Arp=purple; Velocity/Op/Const/etc = neutral). Zero-amount slots are
    // skipped; the list is capped at kMaxModRings.
    auto& apvts = processor_.getApvts();
    const auto slots = parvati::ModDestMap::slotsForDest (apvts, modDest_);

    clearAll();
    int n = 0;
    for (int slot : slots)
    {
        if (n >= kMaxModRings)
            break;

        const auto slotPrefix = (parvati::ModDestMap::isFxDest (modDest_) ? "fxmod" : "mod")
                                + juce::String (slot + 1);

        int amount = 0;
        if (auto* raw = apvts.getRawParameterValue (slotPrefix + "_amount"))
            amount = juce::roundToInt (raw->load());
        if (amount == 0)
            continue;   // an inactive slot draws no arc

        // Resolve the source's category colour from its (human) name. The source
        // raw value is the index into mod{N}_source's choices (kModSources).
        const auto sourceParamID = slotPrefix + "_source";
        int srcIdx = 0;
        if (auto* raw = apvts.getRawParameterValue (sourceParamID))
            srcIdx = juce::roundToInt (raw->load());

        juce::String sourceName;
        if (auto* param = apvts.getParameter (sourceParamID))
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
                if (srcIdx >= 0 && srcIdx < choice->choices.size())
                    sourceName = choice->choices[srcIdx];

        const juce::Colour col = categoryColourForSourceName (sourceName, *theme);
        props.set ("parvatiModCol" + juce::String (n), (int) col.getARGB());
        props.set ("parvatiModAmt" + juce::String (n), amount);
        ++n;
    }
    props.set ("parvatiModN", n);

    slider_->repaint();
}

void ParamControl::applyModHighlight (int modDst)
{
    if (! isModDestKnob_ || slider_ == nullptr)
        return;
    // Glow only when the broadcast dest matches this knob's destination; clear
    // otherwise (including the -1 clear). The LookAndFeel reads "parvatiModHi".
    slider_->getProperties().set ("parvatiModHi", modDst == modDest_);
    slider_->repaint();
}

void ParamControl::applyModDragAffordance()
{
    // A modulation-source drag is active: visually HIDE every control that
    // is NOT a valid drop target (dim to 0.3 alpha), and light up every
    // destination knob (full alpha + the drop-zone ring flag). setAlpha on the
    // ParamControl multiplies the whole cell — slider + label + combo — so
    // non-target combos/knobs visually recede. Idempotent: re-applied on both
    // drag-start and drag-end (modDragActive_ false restores full alpha +
    // clears the ring flag). The per-hover padlock (parvatiModLocked) is cleared
    // here too — locks are cursor-over only (set via itemDragEnter/Exit).
    const bool available = isModDestKnob_;
    setAlpha ((modDragActive_ && ! available) ? 0.3f : 1.0f);
    if (slider_ != nullptr)
    {
        slider_->getProperties().set ("parvatiModDrag", modDragActive_ && available);
        slider_->getProperties().set ("parvatiModLocked", false);
        slider_->repaint();
    }
    if (comboBox_ != nullptr)
    {
        comboBox_->getProperties().set ("parvatiModLocked", false);
        comboBox_->repaint();
    }
    repaint();
}

void ParamControl::setDropLocked (bool locked)
{
    // Bring the hovered NON-target back to full alpha while it shows the
    // padlock (otherwise the drag dim would make the lock illegible); clear =>
    // restore the drag dim/alpha state via applyModDragAffordance().
    if (locked)
    {
        setAlpha (1.0f);
        if (slider_  != nullptr) slider_->getProperties().set  ("parvatiModLocked", true);
        if (comboBox_ != nullptr) comboBox_->getProperties().set ("parvatiModLocked", true);
    }
    else
    {
        if (slider_  != nullptr) slider_->getProperties().set  ("parvatiModLocked", false);
        if (comboBox_ != nullptr) comboBox_->getProperties().set ("parvatiModLocked", false);
        applyModDragAffordance();   // restore dim/alpha for the current drag state
    }
    if (slider_  != nullptr) slider_->repaint();
    if (comboBox_ != nullptr) comboBox_->repaint();
    repaint();
}

void ParamControl::reapplyCategoryColours()
{
    // A theme switch changed the token VALUES, so re-resolve + re-push every
    // control's category colour from the new theme. Component-level setColour
    // overrides survive the switch but keep the OLD theme's value otherwise.
    // The mod ring's bipolar colours are re-resolved here too (each ParamControl
    // re-reads the APVTS depth + the new theme tokens).
    for (auto* c : paramControlRegistry())
    {
        c->applyCategoryArcColour();
        c->applyModSourceTint();
        c->refreshModRing();
    }
}

juce::String ParamControl::getTooltip()
{
    // When tooltips are disabled (Settings panel toggle), return an empty String
    // so the editor's TooltipWindow shows nothing. Cleaner than recreating the
    // window or toggling its visibility.
    return tooltipsEnabled_ ? getParamHelp (desc_.paramID) : juce::String();
}

void ParamControl::lookAndFeelChanged()
{
    // The ParvatiLookAndFeel (and its theme) is not attached during construction
    // (the control is reparented into the editor tree afterwards), so the
    // category arc / mod-source tint are applied once the L&F is reachable.
    // This also re-applies on a theme switch (sendLookAndFeelChange), making the
    // category hues follow the active theme. Both calls are idempotent.
    applyCategoryArcColour();
    applyModSourceTint();
    refreshModRing();
}

void ParamControl::parentHierarchyChanged()
{
    // On the initial reparent into the editor tree getLookAndFeel() finally
    // resolves to the editor's ParvatiLookAndFeel (lookAndFeelChanged does NOT
    // fire for inherited L&F, only for an explicit setLookAndFeel), so this is
    // where the category arc / mod tint first take effect. Idempotent.
    applyCategoryArcColour();
    applyModSourceTint();
    refreshModRing();
}

void ParamControl::resized()
{
    auto b = getLocalBounds().reduced (2);
    // Reserve the label band for non-sequencer controls AND for the marked
    // length control (which shows "Length"); plain step cells hide the label.
    if (! desc_.isSequencer || paramIDStr_.startsWith ("seq_length_"))
    {
        label_->setBounds (b.removeFromTop (15));
        b.removeFromTop (3);
    }

    if (slider_)
    {
        // CAP only — the real dial height is min(this, cellH-28), so raising
        // this constant alone is a no-op. To grow the dials, raise the group /
        // page cellH (configureGroupLayouts / PageInfo), NOT this cap.
        constexpr int kKnobDiameterCap = 52;
        // Diameter respects BOTH cell axes: a compacted column can make cells
        // narrower than the cap, and a fixed 52px dial in a narrower cell would
        // paint over the neighbouring control (the R3 overlap class).
        const int dial = juce::jmin (kKnobDiameterCap, b.getWidth(), b.getHeight());
        slider_->setBounds (b.withSizeKeepingCentre (dial, dial));
    }
    else if (comboBox_)
    {
        // Dropdown: the TAP band is 44pt tall (HIG touch minimum, clamped to
        // the cell), while the DRAWN dropdown stays a compact 28pt strip
        // centred inside it via the "parvatiComboVisualH" property set in the
        // constructor — so dense rows keep their exact look yet a finger gets
        // a full-size target. Width stays fit-to-text (longest choice + 34px
        // chrome: the L&F's positionComboBoxText insets the inline text by
        // 24px, so the text area gets longest-choice + 10px of room — the old
        // +26 chrome left longest + 2px, which clipped the trailing glyph of
        // short-but-full values like part_legato's "Off" the moment the
        // measure-time and draw-time font metrics drifted at all). There is
        // NO fixed width cap — each dropdown is exactly as wide as its longest
        // option (narrow lists get narrow dropdowns) — but it never exceeds
        // the cell width so dense rows (Mod / Modifier) stay compact. Centred
        // in the cell.
        const int comboH = juce::jmin (44, b.getHeight());
        const int textW = maxChoiceTextWidth() + 34;
        const int comboW = juce::jlimit (28, juce::jmax (28, b.getWidth()), textW);
        comboBox_->setBounds (b.withSizeKeepingCentre (comboW, comboH));
    }
}

//==========================================================================
// Hover highlight (mod-destination knobs <-> Mod Matrix rows).
void ParamControl::mouseEnter (const juce::MouseEvent&)
{
    if (isModDestKnob_)
        parvati::ModMatrixHighlight::instance().setHighlightedDest (modDest_);
}

void ParamControl::mouseExit (const juce::MouseEvent& e)
{
    if (! isModDestKnob_)
        return;
    // This cell is a MouseListener on its slider + label, so a mouseExit also
    // fires when moving BETWEEN those sub-components. Only clear when the mouse
    // has genuinely left the cell bounds (avoids a flicker / premature clear).
    const auto rel = e.getEventRelativeTo (this);
    if (! getLocalBounds().contains (rel.position.toInt()))
        parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
}

void ParamControl::mouseDoubleClick (const juce::MouseEvent&)
{
    if (! isModDestKnob_)
        return;
    auto& apvts = processor_.getApvts();
    // Only jump when the modulation ring is active (aggregate depth != 0): an
    // unmodulated knob has no matrix row to scroll to.
    if (parvati::ModDestMap::aggregateAmount (apvts, modDest_) == 0)
        return;
    // Jump to the first ACTIVE slot targeting this dest (its row is visible in
    // the matrix). slotsForDest lists every routed slot; pick the first whose
    // amount is non-zero so the scroll lands on a shown row.
    const auto slots = parvati::ModDestMap::slotsForDest (apvts, modDest_);
    // Domain-aware: an FX knob's slots are fxmod indices (0..15), read from the
    // "fxmod{N}_amount" param and encoded on the slot-select bus as slot +
    // kFxModDstOffset so only the FX matrix reacts; a synth knob's slots are mod
    // indices (0..13), read from "mod{N}_amount" and sent raw (synth-only). The
    // synth onSelectSlot guard (>= 14) rejects the FX-encoded 19..34 and vice-versa.
    const bool fx = parvati::ModDestMap::isFxDest (modDest_);
    const juce::String prefix = fx ? "fxmod" : "mod";
    for (int s : slots)
    {
        if (auto* raw = apvts.getRawParameterValue (prefix + juce::String (s + 1) + "_amount"))
            if (juce::roundToInt (raw->load()) != 0)
            {
                parvati::ModMatrixHighlight::instance().selectSlot (
                    fx ? s + parvati::ModDestMap::kFxModDstOffset : s);
                return;
            }
    }
}

//==========================================================================
// Drag-and-drop assignment: a mod source dragged from the Mod Matrix onto a
// destination knob. isInterested gates on mod-dest knobs + the payload prefix;
// drag-enter/exit drive the ring glow via the STEP-3 highlight bus; the drop
// requests the ModMatrixView to consume the next free slot for the source.
bool ParamControl::isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails)
{
    // Only mod-destination knobs accept the drag, and only for our payload.
    // Every ParamControl accepts the drag so hover/exit/drop fire on non-targets
    // too (non-targets show a padlock via itemDragEnter; their drop is a no-op).
    return dragSourceDetails.description.toString().startsWith ("parvatiModSrc:");
}

void ParamControl::itemDragEnter (const juce::DragAndDropTarget::SourceDetails&)
{
    if (isModDestKnob_)
        parvati::ModMatrixHighlight::instance().setHighlightedDest (modDest_);   // glow the ring
    else
        setDropLocked (true);   // non-target: show the "cannot drop here" padlock
}

void ParamControl::itemDragExit (const juce::DragAndDropTarget::SourceDetails&)
{
    if (isModDestKnob_)
        parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
    else
        setDropLocked (false);
}

void ParamControl::itemDropped (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails)
{
    if (! isModDestKnob_)
    {
        setDropLocked (false);   // non-target drop: no-op, just clear the padlock
        return;
    }

    // Clear the drop glow whether or not the assignment succeeds.
    parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);

    // Parse "parvatiModSrc:<enum>" -> the MOD_SRC_* source enum to assign.
    const juce::String desc = dragSourceDetails.description.toString();
    if (! desc.startsWith ("parvatiModSrc:"))
        return;
    const int sourceEnum = desc.fromFirstOccurrenceOf (":", false, false).getIntValue();
    if (sourceEnum < 0)
        return;

    // requestAssign fans out to every registered handler — the synth ModMatrixView
    // (synth dest) or the FX FxMatrixView (FX dest), each ignoring the other's
    // domain — which writes the source/dest/amount APVTS params (byte-bridged +
    // undoable). The knob's mod{1..14}_amount / fxmod{1..16}_amount listeners
    // (STEP 2) then refresh the ring on their own, so no manual repaint is needed
    // here. A full matrix is a silent no-op.
    parvati::ModMatrixHighlight::instance().requestAssign (sourceEnum, modDest_);
}

//==========================================================================
// Phase 4b: right-click context menu (Reset to default / Randomize).
void ParamControl::mouseDown (const juce::MouseEvent& e)
{
    // Desktop: only popup (right-click / Ctrl-click) triggers the menu; every
    // other click falls through to normal slider/combo interaction.
    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }
    if (e.source.isTouch())
    {
        // Touch: there is no right-click, so Reset/Randomize is reached via a
        // long-press. Start a ~450ms timer on press; if the finger stays put, the
        // timer ARMS the menu (longPressArmed_) but does NOT open it mid-drag — a
        // modal popup opened while the Slider is mid-drag would strand it (its
        // mouseUp would land on the popup). The menu opens on finger release
        // (mouseUp), after the Slider has cleanly ended its drag. A drag past a
        // small threshold cancels a pending/armed long-press.
        // Long-press Reset/Randomize is a KNOB feature. A ComboBox opens its
        // own popup on tap, so never arm the long-press there — it would fire
        // under/over the combo popup and make dropdown taps feel flaky.
        if (slider_ != nullptr)
        {
            longPressStart_ = e.getScreenPosition();
            longPressArmed_ = false;
            startTimer (450);
        }

        // Touch has no hover, so surface this parameter's help text inline in the
        // status strip on every touch (~3s). A value change needs a DRAG (not a
        // tap), so a user can tap-to-learn what a control does without altering it.
        // Respects the global tooltipsEnabled_ toggle; skips empty help.
        if (tooltipsEnabled_ && helpText_.isNotEmpty())
            postTransientStatus (helpText_, 90);
    }
}

void ParamControl::mouseDrag (const juce::MouseEvent& e)
{
    // An intentional knob drag (finger moved past kTouchSlop — the SAME slop
    // the tap-assign gate uses) cancels a pending OR already-armed long-press:
    // neither survives a real drag, and movement that fails a clean tap
    // must not leave the long-press armed (a 6-8px drift would otherwise open
    // the context menu from what felt like a small knob tweak).
    if (e.getScreenPosition().getDistanceFrom (longPressStart_) > kTouchSlop)
    {
        stopTimer();
        longPressArmed_ = false;
    }
}

void ParamControl::mouseUp (const juce::MouseEvent& e)
{
    stopTimer();
    // A long-press armed the menu (timer fired while the finger held still).
    // Open it NOW, on release — the Slider's own mouseUp has already run (these
    // handlers are listener-forwarded, non-consuming) so its drag ended cleanly
    // before the modal popup appears. Long-press takes priority over a tap-assign
    // (a held dest knob needs Reset/Randomize, not an assign).
    if (longPressArmed_)
    {
        longPressArmed_ = false;

        // Two-finger guard: finger 1 can hold a knob still (arming the
        // long-press) while finger 2 is mid-drag on ANOTHER control. Opening
        // the modal menu then would strand finger 2's drag — exactly the
        // scenario the arm-then-open-on-release design avoids for the
        // single-finger case. If any OTHER pointer is still dragging, silently
        // cancel the menu instead (the gesture simply does nothing).
        const auto mySource = e.source.getIndex();
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumMouseSources(); ++i)
            if (i != mySource && desktop.getMouseSource (i)->isDragging())
                return;

        showContextMenu();
        return;
    }
    // Tap-to-assign dest: in [MOD] mode, a CLEAN tap (<= kTouchSlop, mirroring
    // the 5px one-drag-per-press debounce in the source-side drag starts) on a
    // destination knob assigns the selected source to this
    // dest via the SAME seam itemDropped uses (no drag on touch). A value-drag
    // of the knob (> kTouchSlop) is NOT an assign — the slider just changes
    // its value. Stays in mode for more routings; clears the selected source
    // each time. (Long-press fires only on touch; tap-assign runs when [MOD]
    // is on.)
    if (tapAssignActive_ && isModDestKnob_ && e.getDistanceFromDragStart() <= kTouchSlop)
    {
        if (tapSelectedSource_ < 0)
        {
            postTransientStatus (TRANS ("Tap a mod source first"), 60);   // no source armed -> hint, not a silent no-op
            return;
        }
        const bool ok = parvati::ModMatrixHighlight::instance().requestAssign (tapSelectedSource_, modDest_);
        parvati::ModMatrixHighlight::instance().setHighlightedDest (-1);
        postTransientStatus (ok ? TRANS ("Assigned") : TRANS ("Mod Matrix full"), ok ? 45 : 90);
        tapSelectedSource_ = -1;   // consumed; stay in mode for another routing
    }
}

void ParamControl::timerCallback()
{
    // The finger held still long enough for a long-press. ARM the menu but do
    // NOT open it yet: opening a modal popup mid-drag would strand the Slider
    // (its mouseUp would land on the popup, not the knob). The menu opens on
    // finger release — see mouseUp.
    stopTimer();
    longPressArmed_ = true;
}

void ParamControl::showContextMenu()
{
    // Walk the FULL parent chain (ParamControls are parented to ParamPages,
    // then into workspaces — the immediate parent is never the editor; the
    // old getParentComponent() dynamic_cast here was a silent no-op). The
    // popup host is the editor, reached through the small interface so this
    // file does not include the editor header.
    auto* host = findParentComponentOfClass<ParamControlPopupHost>();

    // Suppress the host's (parented) TooltipWindow the instant the menu is
    // about to open: it would otherwise FREEZE this control's tip on screen
    // (the bleed-through). The host's 30 Hz timer also hides it while a modal
    // popup stays open; this call covers the very first frame.
    if (host != nullptr)
        host->hideHostedTooltip();

    juce::PopupMenu menu;
    // SafePointer guards against the control being deleted while the async
    // menu is still open (e.g. editor closed mid-menu).
    juce::Component::SafePointer<ParamControl> safe (this);

    // ---- Host entries (VST3 hosts: Cubase/Reaper-class automation
    // actions, e.g. "show automation lane"). getHostContext() is set by the
    // VST3 wrapper (AudioProcessorEditor::setHostContext) and is null in AU /
    // AUv3 / standalone — the null paths fall through to the local-only menu.
    // Pattern: take the host's PopupMenu as the BASE (so host entries stay on
    // top, where the user looks for them) and APPEND our Reset/Randomize below;
    // an empty host menu (host offers nothing for this parameter) is ignored.
    // The HostProvidedContextMenu unique_ptr is dropped after copying its
    // PopupMenu — the JUCE header documents the returned menu is safe to
    // modify and display.
    bool usedHostMenu = false;
    if (host != nullptr)
        if (auto* hostCtx = host->popupHostContext())
            if (auto* param = processor_.getApvts().getParameter (desc_.paramID))
                if (auto hostMenu = hostCtx->getContextMenuForParameter (param))
                    if (auto hostPopup = hostMenu->getEquivalentPopupMenu();
                        hostPopup.getNumItems() > 0)
                    {
                        menu = std::move (hostPopup);
                        usedHostMenu = true;
                    }

    // Each item is a TooltipMenuItemComponent: a menu entry rendered exactly
    // like a default item (via the L&F drawPopupMenuItem) that ALSO implements
    // juce::TooltipClient, so hovering it shows a short description. juce::PopupMenu
    // items have no native tooltip field, so a PopupMenu::CustomComponent is the
    // JUCE idiom. The item is "triggered automatically" so a click still fires
    // item.action (reset / randomize) — no manual mouse handling.
    auto addItemWithTooltip = [&menu] (const juce::String& text,
                                          const juce::String& tip,
                                          int itemID,
                                          std::function<void()> action)
    {
        juce::PopupMenu::Item item;
        item.text = text;
        item.itemID = itemID;   // non-zero: the menu fires item.action on click
        item.action = std::move (action);
        item.customComponent = new TooltipMenuItemComponent (text, tip);
        menu.addItem (std::move (item));
    };

    // A visual separator between host entries and ours (host menus already
    // carry their own internal structure; skipped for the local-only case,
    // which stays exactly as it was).
    if (usedHostMenu)
        menu.addSeparator();

    // itemIDs are deliberately far from the small integers a host menu may
    // already use (VST3 hosts number their own entries); nothing consumes the
    // ID (the async callback ignores it; each item carries its action), but a
    // collision-proof namespace costs nothing.
    addItemWithTooltip (TRANS ("Reset to default"),
                        TRANS ("Reset this parameter to its default value"),
                        1001, [safe] { if (safe != nullptr) safe->resetToDefault(); });
    addItemWithTooltip (TRANS ("Randomize"),
                        TRANS ("Set this parameter to a random value"),
                        1002, [safe] { if (safe != nullptr) safe->randomize(); });

    // The editor's TooltipWindow is parented to the editor (so it CANNOT render
    // above the popup window) and is suppressed while a popup is open. So the
    // context-menu item tooltips need their OWN window that floats above the
    // popup. A desktop (unparented) TooltipWindow tracks components in the
    // popup's peer and shows the hovered item's tooltip; themed via the editor's
    // L&F so it matches the normal tooltips. It lives only while this menu is
    // open and is destroyed in the close callback.
    popupTooltipWindow_ = std::make_unique<juce::TooltipWindow>();
    popupTooltipWindow_->setLookAndFeel (&getLookAndFeel());

    // F-ui-5 (bug hunt 2026-08-18): withTargetComponent only positions the
    // menu / watches the target — it does NOT theme it (PopupMenu L&F comes
    // solely from PopupMenu::setLookAndFeel in this JUCE; checked against
    // juce_PopupMenu.cpp findLookAndFeel). Set it explicitly like the zoom
    // overflow popup and FxTypeCombo do, so Reset/Randomize render themed.
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe] (int)
                        {
                            // Tear down the desktop tooltip window on close.
                            if (safe != nullptr)
                                safe->popupTooltipWindow_.reset();
                        });
}

void ParamControl::resetToDefault()
{
    // getParameterAsValue returns a Value bound to the APVTS parameter; assigning
    // the denormalized value drives the attachment (control moves) AND the
    // processor's APVTS::Listener (engine byte-bridge) — the same path patch
    // loading uses. defaultValue is the denormalized value (Int value or Choice
    // index), matching how the APVTS stores each type. beginNewTransaction()
    // first so this reset is its own discrete undo step (Phase 4c).
    processor_.getUndoManager().beginNewTransaction();
    processor_.getApvts().getParameterAsValue (desc_.paramID) =
        static_cast<float> (desc_.defaultValue);
}

void ParamControl::randomize()
{
    float value = 0.0f;
    if (comboBox_ != nullptr && desc_.choices != nullptr)
    {
        const int n = desc_.choices->size();
        if (n <= 0)
            return;
        // Random choice index.
        value = static_cast<float> (juce::Random::getSystemRandom().nextInt (n));
    }
    else
    {
        const int lo = desc_.minValue;
        const int hi = desc_.maxValue;
        if (hi < lo)
            return;
        // Random value in the inclusive [minValue, maxValue] range.
        value = static_cast<float> (lo + juce::Random::getSystemRandom().nextInt (hi - lo + 1));
    }

    // beginNewTransaction() so each Randomize is a discrete undo step (Phase 4c).
    processor_.getUndoManager().beginNewTransaction();
    processor_.getApvts().getParameterAsValue (desc_.paramID) = value;
}
