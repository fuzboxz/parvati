// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See PluginEditor.h.

#include "PluginEditor.h"
#include "ParvatiPreset.h"
#include "ui/EnvelopeDisplay.h"
#include "ui/FilterResponseDisplay.h"
#include "ui/ModDestMap.h"
#include "ui/ModMatrixHighlight.h"
#include "ui/OscPreviewDisplay.h"
#include "ui/PatchPage.h"
#include "ui/ParamHelp.h"
#include "ui/SynthParamLabels.h"
#include "ui/SynthWorkspace.h"
#include "ui/FxWorkspace.h"
#include "ui/FxRoutingBar.h"
#include "ui/FxSlotCard.h"
#include "ui/NoteStepControl.h"
#include "ui/SeqLengthStepper.h"
#include "ui/FxMatrixView.h"
#include "ui/ModSourceCatalog.h"   // parvati::kNoteSeqSentinel (bar-only NOTE pill)
#include "ui/WheelsComponent.h"
#include "ui/Translations.h"
#include "dsp/patch.h"            // ambika::dsp::MOD_SRC_* (generator-tab drag payloads)

#include <algorithm>   // std::remove for the ParamControl instance registry

// Version string from CMake (Parvati target compile def). Fallback for any
// translation unit that does not get the define.
#ifndef PARVATI_VERSION
#define PARVATI_VERSION "0.0.0"
#endif

// parvati_logo.svg is embedded via a dedicated juce_add_binary_data target
// (NAMESPACE ParvatiLogo, see CMakeLists.txt). We resolve its bytes through
// getNamedResource() rather than #include "BinaryData.h": the project already
// links a second binary-data target (parvati_factory_presets, namespace
// FactoryPresets) whose generated header shares the filename "BinaryData.h",
// so an #include here would be ambiguous across the two JuceLibraryCode include
// dirs. getNamedResource() is emitted in the generated BinaryData.cpp (external
// linkage); "parvati_logo_svg" is the resource name derived from the filename
// parvati_logo.svg.
namespace ParvatiLogo {
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);
}

// ---- Header logo: [brand icon] + white "Parvati" wordmark -----------------
// The embedded parvati_logo.svg is true vector art (outlined <path>/<g>, no
// raster); it is parsed once into logoDrawable_ via JUCE's SVG renderer and
// drawn as-is (brand colours, NOT theme-tinted). The "Parvati" wordmark is
// painted in the theme `text` token (theme-aware near-white; dark on Paper)
// so it re-colours on theme switch. The logo block width is measured in
// resized() with the SAME font paint() uses so the logo/version/centre/right
// header cluster stays byte-stable.
constexpr const char* kLogoText       = "Parvati";
constexpr float       kLogoTextHeight = 22.0f;   // bold sans-serif cap height in the header bar

// Re-apply each Label's font in the component tree (same height/style, default
// family) so each re-resolves its typeface through the active L&F after a
// font-mode switch (juce::Label caches its font, so a plain repaint would NOT
// pick up the new family).
void refreshFontsIn (juce::Component* c, const ParvatiLookAndFeel& lnf)
{
    if (c == nullptr)
        return;
    if (auto* l = dynamic_cast<juce::Label*> (c))
    {
        const auto f = l->getFont();
        l->setFont (lnf.appFont (f.getHeight(), f.getStyleFlags()));
    }
    for (auto* child : c->getChildren())
        refreshFontsIn (child, lnf);
}

namespace
{
// ---- Map a parameter ID to one of the GUI sections --------------------------
// (Derived from the well-defined paramID prefixes in ParameterLayout.cpp, so the
//  verified APVTS byte-bridge stays untouched.)
enum class Section { Oscillators, Mixer, Filter, Envelopes, Lfos, ModMatrix, Modifiers, Arp, Sequencer, Global, Multi, Fx, FxMatrix };

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
    if (id.startsWith ("part"))      return Section::Global;   // part volume/legato/portamento
    return Section::Global;
}

// Map a functional Section to its theme category-token colour. Oscillators /
// Mixer / Filter / ModMatrix / Modifiers / Global / Multi share the neutral
// "audio" amber; Envelopes/LFOs/Sequencer/Arp get their own hue. This is the
// ONLY place a Section resolves to a category token, so every arc / graph / tint
// shares one consistent mapping and a theme switch re-resolves automatically.
juce::Colour categoryColourForSection (const ParvatiTheme& theme, Section s)
{
    // Only Envelopes/LFOs/Sequencer/Arp carry a distinct hue; every other
    // section shares the neutral "audio" amber. (if-chain, not switch, so the
    // remaining categories fall through to the neutral default without a
    // -Wswitch-enum warning.)
    if (s == Section::Envelopes) return theme.catEnv;
    if (s == Section::Lfos)      return theme.catLfo;
    if (s == Section::Sequencer) return theme.catSeq;
    if (s == Section::Arp)       return theme.catArp;
    return theme.catAudio;   // Osc/Mix/Filter/ModMatrix/Modifiers/Global/Multi
}
}  // namespace

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
// discoverable. If the label doesn't start with the expected prefix, it is
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
    // name (shown in the panel border) isn't redundantly repeated in every knob
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
        // Catch right-clicks on the combo (it would otherwise swallow the popup
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
        // Colour the knob's fill ARC by functional category (amber Osc/Filter/Mix,
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
    // Measure every choice (plus the combo's current text) in the active L&F
    // combo font so the dropdown fits its longest entry. Used to size the
    // fit-to-text combo width (longest + 24px padding, capped at 140px).
    const auto f = [this]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            return lnf->appFont (14.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (14.0f));
    }();

    int widest = 0;
    if (desc_.choices != nullptr)
        for (const auto& c : *desc_.choices)
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, c));
    if (comboBox_ != nullptr)
        widest = juce::jmax (widest,
                             juce::GlyphArrangement::getStringWidthInt (f, comboBox_->getText()));
    return widest;
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
    if (sourceEnum >= 0)
        postTransientStatus (TRANS ("Mod source: ") + parvati::entryFor (sourceEnum).fullName, 45);
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
    // A modulation-source drag is in flight: visually HIDE every control that
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
        slider_->setBounds (b.withSizeKeepingCentre (kKnobDiameterCap,
                                                     juce::jmin (kKnobDiameterCap, b.getHeight())));
    }
    else if (comboBox_)
    {
        // Dropdown: the TAP band is 44pt tall (HIG touch minimum, clamped to
        // the cell), while the DRAWN dropdown stays a compact 28pt strip
        // centred inside it via the "parvatiComboVisualH" property set in the
        // constructor — so dense rows keep their exact look yet a finger gets
        // a full-size target. Width stays fit-to-text (longest choice + 26px
        // chrome: 6px left pad + amber chevron + slack). There is NO fixed
        // width cap — each dropdown is exactly as wide as its longest option
        // (narrow lists get narrow dropdowns) — but it never exceeds the cell
        // width so dense rows (Mod / Modifier) stay compact. Centred in the
        // cell.
        const int comboH = juce::jmin (44, b.getHeight());
        const int textW = maxChoiceTextWidth() + 26;
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
        setDropLocked (true);   // non-target: show the "can't drop here" padlock
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
    // neither should survive a real drag, and movement that fails a clean tap
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
    // (a held dest knob wants Reset/Randomize, not an assign).
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
    if (tapAssignActive_ && isModDestKnob_ && e.getDistanceFromDragStart() <= 5)
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
    // Suppress the editor's (parented) TooltipWindow the instant the menu is
    // about to open: it would otherwise FREEZE this control's tip on screen
    // (the bleed-through). The editor's 30 Hz timer also hides it while a modal
    // popup stays open; this call covers the very first frame.
    if (auto* ed = dynamic_cast<ParvatiEditor*> (getParentComponent()))
        if (auto* tw = ed->getTooltipWindow())
            tw->hideTip();

    juce::PopupMenu menu;
    // SafePointer guards against the control being deleted while the async
    // menu is still open (e.g. editor closed mid-menu).
    juce::Component::SafePointer<ParamControl> safe (this);

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

    addItemWithTooltip (TRANS ("Reset to default"),
                        TRANS ("Reset this parameter to its default value"),
                        1, [safe] { if (safe != nullptr) safe->resetToDefault(); });
    addItemWithTooltip (TRANS ("Randomize"),
                        TRANS ("Set this parameter to a random value"),
                        2, [safe] { if (safe != nullptr) safe->randomize(); });

    // The editor's TooltipWindow is parented to the editor (so it CANNOT render
    // above the popup window) and is suppressed while a popup is open. So the
    // context-menu item tooltips need their OWN window that floats above the
    // popup. A desktop (unparented) TooltipWindow tracks components in the
    // popup's peer and shows the hovered item's tooltip; themed via the editor's
    // L&F so it matches the normal tooltips. It lives only while this menu is
    // open and is destroyed in the close callback.
    popupTooltipWindow_ = std::make_unique<juce::TooltipWindow>();
    popupTooltipWindow_->setLookAndFeel (&getLookAndFeel());

    // withTargetComponent(this) so the menu inherits the editor's
    // ParvatiLookAndFeel (themed colours + font) instead of the default L&F.
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

// Sub-section key for the merged Mixer panel: the three logical
// groups (Mixer / Sub Oscillator / Noise) used only for internal layout +
// divider placement. (groupForId now returns ONE merged name for all mix IDs.)
static juce::String mixerSubSectionForId (const juce::String& id)
{
    if (id == "mix_balance" || id == "mix_op" || id == "mix_param")
        return "Mixer";
    if (id == "mix_sub_shape" || id == "mix_sub")
        return "Sub Oscillator";
    return "Noise / Waveshaper";   // mix_noise / mix_fuzz / mix_crush
}

// Partition a group's control indices into consecutive mixer sub-sections
// (preserving descriptor order): { name, [controlIndices...] }. Used by the
// sectioned layout so each sub-section occupies its own row-band inside the
// single Mixer panel.
static std::vector<std::pair<juce::String, std::vector<int>>>
mixerSubSectionsOf (const std::vector<std::unique_ptr<ParamControl>>& controls,
                    const std::vector<int>& controlIndices)
{
    std::vector<std::pair<juce::String, std::vector<int>>> out;
    for (int ci : controlIndices)
    {
        if (ci < 0 || ci >= (int) controls.size()) continue;
        const auto key = mixerSubSectionForId (controls[(size_t) ci]->getParamID());
        if (out.empty() || out.back().first != key)
            out.emplace_back (key, std::vector<int>{});
        out.back().second.push_back (ci);
    }
    return out;
}

// Per-control column span inside a mixer sub-section (the sectioned panel). A
// span > 1 lets a wider control occupy multiple cells in its row. Keyed on
// paramID so it travels with the descriptor (no hardcoded position): today
// only mix_sub_shape ("Sub Shape") spans 2 cells (cols 1-2) with mix_sub
// ("Sub Level") on col 3.
static int mixerControlSpan (const juce::String& id)
{
    if (id == "mix_sub_shape") return 2;
    return 1;
}

// Number of row-bands a mixer sub-section occupies, honouring per-control
// column spans. Mirrors applyLayout's column-cursor placement exactly so the
// panel height (layoutGroups) + themed divider gaps (paint) stay in sync with
// the actual cell positions (a ceil(size/cols) formula would break the moment
// a span changes the row count).
static int mixerSectionRowCount (const std::vector<std::unique_ptr<ParamControl>>& controls,
                                const std::vector<int>& controlIndices, int cols)
{
    int col = 0, row = 0, rows = 1;
    for (int ci : controlIndices)
    {
        if (ci < 0 || ci >= (int) controls.size())
            continue;
        const int span = mixerControlSpan (controls[(size_t) ci]->getParamID());
        if (col + span > cols && col > 0)   // wrap to a new row
        {
            col = 0;
            ++row;
        }
        rows = juce::jmax (rows, row + 1);
        col += span;
    }
    return rows;
}

//==============================================================================
juce::String ParamPage::groupForId (const juce::String& id)
{
    // ---- Mixer: ONE merged panel ("Mixer") holding all 8 mix
    // controls. The three logical sub-sections (Mixer / Sub Oscillator / Noise)
    // are separated inside the panel by themed dividers (see ParamPage::paint +
    // the sectioned layout path), not by separate bordered boxes. ----
    if (id == "mix_balance" || id == "mix_op"     || id == "mix_param" ||
        id == "mix_sub_shape" || id == "mix_sub" ||
        id == "mix_noise"   || id == "mix_fuzz"  || id == "mix_crush")
        return "Mixer";

    // ---- The two filter modulation amounts share one panel ----
    if (id == "filter_env" || id == "filter_lfo")
        return "Filter Mod";

    // ---- Sequencer length slots (exact ids) belong to their step group ----
    if (id == "seq_length_1") return "Sequencer 1";
    if (id == "seq_length_2") return "Sequencer 2";
    if (id == "seq_length_3") return "Note Pitch";

    // ---- Synth options with no patch byte (Global page) ----
    if (id == "vca_curve" || id == "filter_card" || id == "filter_drive")
        return "Global";

    // ---- Sequencer step grids (prefixes) ----
    if (id.startsWith ("seq1_step")) return "Sequencer 1";
    if (id.startsWith ("seq2_step")) return "Sequencer 2";
    if (id.startsWith ("seqnote_step")) return "Note Pitch";    // note byte (gate in bit 7)
    if (id.startsWith ("seqnote_vel"))  return "Note Velocity"; // vel byte (legato in bit 7)

    // ---- Oscillators / filters ----
    if (id.startsWith ("osc1_"))    return "Osc 1";
    if (id.startsWith ("osc2_"))    return "Osc 2";
    if (id.startsWith ("filter1_")) return "Filter";
    if (id.startsWith ("filter2_")) return "Filter 2";

    // ---- FX slots (fx{N}_*) — one panel per slot. Each FX-slot ParamPage holds
    // a single slot's type/enabled/drywet/param1-4; fx_topo / fx_order (the FX
    // chain topology + slot order) group as "FX Chain" on the FX1 page. ----
    if (id.startsWith ("fx1_")) return "FX1";
    if (id.startsWith ("fx2_")) return "FX2";
    if (id.startsWith ("fx3_")) return "FX3";
    if (id == "fx_topo" || id == "fx_order") return "FX Chain";

    // ---- Envelopes / LFOs (now on separate tabs) ----
    // env{N}_attack/decay/sustain/release -> "Env N (role)"; env{N}_lfo_* -> "LFO N".
    // Role verified from the dsp routing: ENV3 -> VCA (mod-matrix default,
    // amount 63) = Amp; ENV2 -> filter cutoff (hardcoded filter_env) = Filter;
    // ENV1 -> free mod parameters = Mod.
    if (id.startsWith ("env") && id.length() > 3 && id[3] >= '1' && id[3] <= '3')
    {
        const juce::String n = juce::String::charToString (id[3]);
        if (id.contains ("_lfo_"))
            return "LFO " + n;
        if (n == "1") return "Env 1 (Mod)";
        if (n == "2") return "Env 2 (Filter)";
        return "Env 3 (Amp)";
    }
    if (id.startsWith ("voice_lfo_")) return "Voice LFO";

    // ---- Modifiers (modifN_*) — checked before the "mod" rule ----
    if (id.startsWith ("modif") && id.length() > 5 && id[5] >= '0' && id[5] <= '9')
    {
        juce::String num;
        for (int i = 5; i < id.length() && id[i] >= '0' && id[i] <= '9'; ++i) num += id[i];
        return "Modifier " + num;
    }

    // ---- Mod matrix (modN_*) ----
    if (id.startsWith ("mod") && id.length() > 3 && id[3] >= '0' && id[3] <= '9')
    {
        juce::String num;
        for (int i = 3; i < id.length() && id[i] >= '0' && id[i] <= '9'; ++i) num += id[i];
        return "Mod " + num;
    }

    // ---- Part / Play + Arp ----
    if (id.startsWith ("part_")) return "Part / Play";
    if (id.startsWith ("arp_"))  return "Arp";

    return "Other";
}

void ParamPage::buildGroups (const std::vector<const PatchParamDescriptor*>& descriptors)
{
    // Partition descriptors into named groups, preserving first-appearance
    // order of the groups and the descriptor order within each group.
    for (int i = 0; i < (int) descriptors.size(); ++i)
    {
        const juce::String gname = groupForId (descriptors[(size_t) i]->paramID);
        GroupLayout* g = nullptr;
        for (auto& existing : groups_)
            if (existing.name == gname) { g = &existing; break; }
        if (g == nullptr)
        {
            groups_.emplace_back();
            g = &groups_.back();
            g->name = gname;
        }
        g->controlIndices.push_back (i);
    }

    // For sequencer step-grid groups, the Length control should be the LAST cell
    // (user edits steps first, then sets the length). The descriptor order has
    // seq_length_* before the step params, so reorder via stable_partition.
    for (auto& g : groups_)
    {
        if (! (g.name == "Sequencer 1" || g.name == "Sequencer 2" || g.name == "Note Pitch"))
            continue;
        std::stable_partition (g.controlIndices.begin(), g.controlIndices.end(),
            [&descriptors] (int idx)
            { return descriptors[(size_t) idx]->paramID.find ("seq_length") == std::string::npos; });
    }
}

void ParamPage::configureGroupLayouts()
{
    // Internal column count for a generic panel, chosen so the cells stay
    // roughly square-ish (1->1, 2->2, 3->3, 4->2x2, 5/6->3, 7/8->4).
    auto generalCols = [] (int n) -> int {
        if (n <= 1) return 1;
        if (n == 2) return 2;
        if (n == 3) return 3;
        if (n == 4) return 2;
        if (n <= 6) return 3;
        return 4;
    };

    for (auto& g : groups_)
    {
        const int n = (int) g.controlIndices.size();
        g.cellW = cellWidth_;
        g.cellH = cellHeight_;

        // Dense step grids (Seq1/2 + the Note Pitch/Velocity splits). 8 columns
        // so 16 steps wrap to 2 rows (17 cells -> 3 rows); cell sizes fit the
        // narrow GroupPager content area (no horizontal scrollbar) while keeping
        // the 44px step knob legible.
        if (g.name == "Sequencer 1" || g.name == "Sequencer 2"
            || g.name == "Note Pitch" || g.name == "Note Velocity")
        {
            g.stepGrid = true;
            g.internalCols = 8;
            g.cellW = 72;
            g.cellH = 64;   // was 56: step dial 28px -> 36px (note names fit; 2 rows still <= 262 budget)
        }
        // Mod-matrix slots: source / dest / amount, one row each. Two slots fit
        // side-by-side in the 50% right-mod column; a GroupPager shows 4 per page.
        else if (g.name.startsWith ("Mod "))
        {
            g.singleRow = true;
            g.internalCols = juce::jmax (1, n);
            g.cellW = 96;
            g.cellH = 56;
        }
        // Modifier strips: in1 / in2 / op combos; 2 per GroupPager page.
        else if (g.name.startsWith ("Modifier "))
        {
            g.singleRow = true;
            g.internalCols = juce::jmax (1, n);
            g.cellW = 96;
            g.cellH = 64;
        }
        // Env / LFO generators: one row of knobs + an ADSR/LFO preview graph.
        // Sized for the 50% left-mod column width + the GroupPager content height.
        else if (g.name.startsWith ("Env ") || g.name.startsWith ("LFO ") || g.name == "Voice LFO")
        {
            g.internalCols = juce::jmax (1, n);
            g.cellW = 150;
            g.cellH = 76;   // was 64: dial 36px -> 48px (matches ARP/Global; ~68px slack vs 262 budget)
        }
        // Mixer column (narrow 20%): ONE merged "Mixer" panel holds
        // all 8 mix controls, laid out in 3 logical sub-sections (one row each)
        // separated by themed dividers. cellH matches Filter (full-arc knobs);
        // cellW is a floor that the row-fill grows to the column width. One
        // panel of 3 rows = 232px <= 279px main-row half at 1280x620.
        else if (g.name == "Mixer")
        {
            g.sectioned    = true;
            g.internalCols = 3;   // widest sub-section (Mixer/Noise = 3 knobs)
            g.cellW = 60;         // floor: 3-col natural = 196px <= 200px avail at min 1100
            g.cellH = 64;         // full-arc knobs (matches Filter)
        }
        // Filter column (40%): Filter (3 knobs) + Filter Mod (2 amounts) + a
        // magnitude-response curve decoration under Filter. cellW sized so the
        // 3-knob group (3*cellW+16) fits the 420px content width at the 1100px
        // minimum (no horizontal clipping of the knobs OR the curve); the row
        // grows to fill at 1280.
        else if (g.name == "Filter" || g.name == "Filter Mod")
        {
            g.internalCols = generalCols (n);
            g.cellW = 130;  // 3-col group = 406px <= 420px avail at min 1100
            g.cellH = 64;   // 2 stacked single-row groups + curve fit <= 269px main-row half at min 1100
        }
        // Oscillators (40% column): Shape combo + INLINE waveform preview + the
        // other 3 knobs (param/range/detune), all in ONE row so both "Osc 1" +
        // "Osc 2" stack and fit the ~440px-wide (min 1100) / 512px (1280) OSC
        // column with BOTH visible at once (no [OSC1][OSC2] pager). The row is
        // modelled as 5 columns: col0=Shape, col1=reserved for the INLINE
        // preview (set via setGroupInlinePreview), col2..4=param/range/detune.
        // cellW=80 is a floor sized so 5 columns (5*80+16=416px) fit the 420px
        // content width at the 1100px minimum (no horizontal clipping); the row
        // grows to fill at 1280.
        else if (g.name == "Osc 1" || g.name == "Osc 2")
        {
            g.internalCols = 5;
            g.cellW = 80;
            g.cellH = 64;
        }
        else
        {
            g.internalCols = generalCols (n);
        }
    }
}

void ParamPage::layoutGroups (int targetWidth)
{
    const int topY = kMargin;   // no page-heading row: the tab bar already names the page
    const int availW = targetWidth - 2 * kMargin;

    // Natural panel size for each group (independent of placement).
    for (auto& g : groups_)
    {
        if (! groupVisible (g))   // a GroupPager subset hides the other groups
            continue;
        const int cols = juce::jmax (1, g.internalCols);
        {
            const int n = (int) g.controlIndices.size();
            if (g.sectioned)
            {
                // A sectioned panel (merged Mixer) stacks its sub-sections, each
                // on its own row-band, separated by kSectionGap dividers. The
                // total height must match applyLayout's placement exactly so no
                // control lands outside its group rect (layoutIsSane check c).
                const auto sections = mixerSubSectionsOf (controls_, g.controlIndices);
                int rows = 0;
                for (const auto& s : sections)
                    rows += mixerSectionRowCount (controls_, s.second, cols);
                const int gaps = juce::jmax (0, (int) sections.size() - 1);
                g.naturalWidth  = cols * g.cellW + 2 * kGroupPad;
                g.naturalHeight = kGroupTitleH + rows * g.cellH + gaps * kSectionGap + 2 * kGroupPad;
            }
            else
            {
                const int rows = (n + cols - 1) / cols;
                g.naturalWidth  = cols * g.cellW + 2 * kGroupPad;
                g.naturalHeight = kGroupTitleH + rows * g.cellH + 2 * kGroupPad;
            }
        }
        // A group with a decoration (e.g. an ADSR/LFO preview) reserves room
        // below its control cells so the panel height includes it.
        if (g.decoration != nullptr)
            g.naturalHeight += g.decorationH + kDecorationGap;
    }

    // Greedy left-to-right flow. A row wraps when the next panel would overflow
    // the available width OR when the row already holds pageCols_ panels
    // (PageInfo::cols: a tunable cap on panels-per-row; pageCols_ <= 0 =>
    // width-only wrap). rowOf[gi] tags each group with its row for the fill pass.
    int x = kMargin, y = topY, rowH = 0;
    const int rowStartX = kMargin;
    const int maxRight = kMargin + juce::jmax (0, availW);

    // Visible group indices only (a GroupPager subset hides the rest). Placement,
    // the pageCols_ count, and the row-fill pass all operate on JUST these, so a
    // hidden group neither occupies space nor overlaps a visible one, and the
    // page reflows to the subset's natural size.
    std::vector<int> vis;
    vis.reserve (groups_.size());
    for (int gi = 0; gi < (int) groups_.size(); ++gi)
        if (groupVisible (groups_[(size_t) gi]))
            vis.push_back (gi);

    std::vector<int> rowOf (groups_.size(), -1);   // -1 = hidden (never placed)
    int currentRow = 0;

    for (size_t vi = 0; vi < vis.size(); ++vi)
    {
        const int gi = vis[vi];
        auto& g = groups_[(size_t) gi];

        // Panels already placed on THIS row (for the pageCols_ cap). Walk back
        // over the VISIBLE groups only (hidden ones keep rowOf == -1).
        int panelsThisRow = 0;
        for (int k = (int) vi - 1; k >= 0 && rowOf[(size_t) vis[(size_t) k]] == currentRow; --k)
            ++panelsThisRow;

        if ((x != rowStartX && (x + g.naturalWidth > maxRight))
            || (pageCols_ > 0 && panelsThisRow >= pageCols_))
        {
            ++currentRow;
            x = rowStartX;
            y += rowH + kGroupGap;
            rowH = 0;
        }
        rowOf[(size_t) gi] = currentRow;
        g.rect.setBounds (x, y, g.naturalWidth, g.naturalHeight);
        x += g.naturalWidth + kGroupGap;
        rowH = juce::jmax (rowH, g.naturalHeight);
    }
    const int lastRow = vis.empty() ? -1 : currentRow;

    // ---- Row-fill justification (flexible-width grid). For each row, grow the
    // NON-dense panels (stepGrid / singleRow are excluded) so the row fills up
    // to maxRight, eliminating the ragged right edge. Only the panel WIDTH grows;
    // height and decoration sizing are untouched, so contentHeight_ (computed
    // below from the row geometry above) is unchanged. All-dense rows are left
    // as-is (ragged is fine for sequencer / modifier strips). ----
    for (int r = 0; r <= lastRow; ++r)
    {
        // Gather this row's panel indices in left-to-right (gi) order.
        std::vector<int> rowPanels;
        int nonDense = 0;
        int rowRight = rowStartX;
        for (int gi = 0; gi < (int) groups_.size(); ++gi)
            if (rowOf[(size_t) gi] == r)
            {
                rowPanels.push_back (gi);
                rowRight = juce::jmax (rowRight, groups_[(size_t) gi].rect.getRight());
                if (! (groups_[(size_t) gi].stepGrid || groups_[(size_t) gi].singleRow))
                    ++nonDense;
            }
        if (rowPanels.empty() || nonDense == 0)
            continue;   // all-dense row: leave ragged (no grow, no re-tile)

        const int slack = maxRight - rowRight;
        if (slack > 0)
        {
            const int grow = slack / nonDense;
            if (grow > 0)
                for (int gi : rowPanels)
                {
                    auto& g = groups_[(size_t) gi];
                    if (! (g.stepGrid || g.singleRow))
                        g.rect.setWidth (g.rect.getWidth() + grow);
                }
        }

        // RE-TILE the row left-to-right from the (now-grown) widths so panels
        // never overlap — the in-place width grow above left each panel's X at
        // its natural position, which overlaps its neighbour when 2+ non-dense
        // panels share a row (e.g. Osc 1/2, Mixer + Sub Oscillator). Y is kept.
        int tileX = rowStartX;
        for (int gi : rowPanels)
        {
            auto& g = groups_[(size_t) gi];
            g.rect.setX (tileX);
            tileX = g.rect.getRight() + kGroupGap;
        }
    }

    contentWidth_  = juce::jmax (targetWidth, 2 * kMargin + 40);
    const int naturalH = groups_.empty() ? (topY + kMargin)
                                          : (y + rowH + kMargin);

    // Vertically centre a short page inside its viewport so sparse pages (Arp /
    // Global) do not leave a large empty void below the controls: shift the
    // whole grid down by half the slack and grow the page to fill the viewport
    // (no vertical scroll when it fits). Pages taller than the viewport keep
    // their natural height and scroll as before. The target height comes from
    // centerHeight_ (set by the editor for every tab) — NOT getViewHeight(),
    // which tracks the content size and so can never exceed the natural height.
    // Top-align the page content (no vertical centring). Previously short
    // pages (single-row group subsets like Mod Matrix [13-14] or Modifiers
    // [3-4]) floated in the vertical middle of the viewport, looking
    // inconsistent next to denser multi-row pages. Content now starts at the
    // top; the page still grows to fill the viewport (contentHeight_ >= viewH)
    // so there is no vertical scrollbar. The target height comes from
    // centerHeight_ (set by the editor for every tab) — NOT getViewHeight(),
    // which tracks the content size and so can never exceed the natural height.
    yOffset_ = 0;
    // Prefer the editor-supplied tab height (reliable for every tab); fall back
    // to the parent Viewport's physical height for standalone / headless use.
    int viewH = centerHeight_;
    if (viewH <= 0)
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            viewH = vp->getHeight();
    contentHeight_ = juce::jmax (naturalH, viewH);
}

void ParamPage::applyLayout()
{
    for (auto& g : groups_)
    {
        const bool visible = groupVisible (g);
        if (g.groupComp != nullptr)
            g.groupComp->setVisible (visible);
        if (g.decoration != nullptr)
            g.decoration->setVisible (visible);
        if (g.inlinePreview != nullptr)
            g.inlinePreview->setVisible (visible);
        if (! visible)
        {
            // A hidden group's controls are never positioned here; hide them so
            // they do not paint over the active subset.
            for (int ci : g.controlIndices)
                if (ci >= 0 && ci < (int) controls_.size())
                    controls_[(size_t) ci]->setVisible (false);
            continue;
        }

        if (g.groupComp != nullptr)
            g.groupComp->setBounds (g.rect);

        auto inner = g.rect.reduced (kGroupPad);
        inner.removeFromTop (kGroupTitleH);   // room for the panel title text

        const int cols = juce::jmax (1, g.internalCols);
        // Flexible-width cells: a NON-dense panel distributes its cells evenly
        // across the actual (possibly row-filled) inner width; a DENSE panel
        // (sequencer step grid / mod-modifier strip) keeps its fixed cell size
        // and is left-aligned. Column width never shrinks below the natural
        // cellW, only grows to fill. Row height is always g.cellH.
        const bool dense = g.stepGrid || g.singleRow;
        const int colStep = dense ? g.cellW
                                  : juce::jmax (g.cellW, inner.getWidth() / cols);

        if (g.sectioned)
        {
            // Sectioned panel (merged Mixer): each sub-section occupies its own
            // row-band; Y advances by the section's row count + kSectionGap so
            // the themed dividers (drawn in paint) sit in the gaps. This mirrors
            // layoutGroups' naturalHeight exactly so no control lands outside
            // its group rect.
            const auto sections = mixerSubSectionsOf (controls_, g.controlIndices);
            int y = inner.getY();
            for (const auto& s : sections)
            {
                // Per-control column span: a column cursor advances by each
                // control's span (mix_sub_shape = 2), wrapping to the next row
                // when it would overflow cols. The sub-section's row count is
                // precomputed by the SAME cursor logic (mixerSectionRowCount) so
                // the themed divider gap sits at the right Y regardless of spans.
                const int sRows = mixerSectionRowCount (controls_, s.second, cols);
                int col = 0, row = 0;
                for (int i = 0; i < (int) s.second.size(); ++i)
                {
                    const int ci = s.second[(size_t) i];
                    if (ci < 0 || ci >= (int) controls_.size())
                        continue;
                    const int span = mixerControlSpan (controls_[(size_t) ci]->getParamID());
                    if (col + span > cols && col > 0)   // wrap to a new row
                    {
                        col = 0;
                        ++row;
                    }
                    const juce::Rectangle<int> cell (inner.getX() + col * colStep,
                                                     y + row * g.cellH,
                                                     span * colStep, g.cellH);
                    auto* ctrl = controls_[(size_t) ci].get();
                    ctrl->setVisible (true);
                    ctrl->setBounds (cell.reduced (3));
                    col += span;
                }
                y += sRows * g.cellH + kSectionGap;
            }
            continue;   // sectioned: skip the standard grid + decoration
        }

        // OSC groups reserve column 1 for an INLINE waveform preview (col0=Shape,
        // col1=preview, col2..4=param/range/detune), so the shape combo + preview
        // sit side by side and the other 3 knobs shift right by one column.
        const bool hasInlinePreview = (g.inlinePreview != nullptr);

        for (int idx = 0; idx < (int) g.controlIndices.size(); ++idx)
        {
            const int ci = g.controlIndices[(size_t) idx];
            if (ci < 0 || ci >= (int) controls_.size()) continue;
            const int col = hasInlinePreview ? (idx == 0 ? 0 : idx + 1) : (idx % cols);
            const int row = hasInlinePreview ? 0 : (idx / cols);
            const juce::Rectangle<int> cell (inner.getX() + col * colStep,
                                             inner.getY() + row * g.cellH,
                                             colStep, g.cellH);
            auto* ctrl = controls_[(size_t) ci].get();
            ctrl->setVisible (true);
            ctrl->setBounds (cell.reduced (3));
        }

        // The inline preview occupies its reserved column (col 1) for the row height.
        if (hasInlinePreview)
        {
            g.inlinePreview->setVisible (true);
            const juce::Rectangle<int> pc (inner.getX() + 1 * colStep,
                                           inner.getY(),
                                           colStep, g.cellH);
            g.inlinePreview->setBounds (pc.reduced (3));
        }

        // A group's decoration (if any) spans the panel width below the cells.
        const int rows = ((int) g.controlIndices.size() + cols - 1) / cols;
        if (g.decoration != nullptr)
        {
            const int decY = inner.getY() + rows * g.cellH + kDecorationGap;
            g.decoration->setBounds (
                juce::Rectangle<int> (inner.getX(), decY, inner.getWidth(), g.decorationH));
        }
    }
}

bool ParamPage::layoutIsSane() const
{
    // (a) every VISIBLE group panel has positive size. (Hidden groups in an
    // active setVisibleGroups subset have no rect and are skipped.)
    for (const auto& g : groups_)
    {
        if (! groupVisible (g))
            continue;
        if (g.rect.getWidth() <= 0 || g.rect.getHeight() <= 0)
            return false;
    }

    // (b) no two VISIBLE group panels overlap (siblings in page coordinate space).
    for (size_t i = 0; i < groups_.size(); ++i)
    {
        if (! groupVisible (groups_[i])) continue;
        for (size_t j = i + 1; j < groups_.size(); ++j)
        {
            if (! groupVisible (groups_[j])) continue;
            if (groups_[i].rect.intersects (groups_[j].rect))
                return false;
        }
    }

    // (c) every control has positive size and sits inside its group's rect.
    // (ParamControl is a direct child of ParamPage, so getBoundsInParent() is in
    // the same page-space coordinates as the group rects.) Only VISIBLE groups
    // are validated (a GroupPager subset hides the rest).
    for (const auto& g : groups_)
    {
        if (! groupVisible (g))
            continue;
        for (int ci : g.controlIndices)
        {
            if (ci < 0 || ci >= (int) controls_.size())
                return false;
            const auto b = controls_[(size_t) ci]->getBoundsInParent();
            if (b.getWidth() <= 0 || b.getHeight() <= 0)
                return false;
            if (! g.rect.contains (b))
                return false;
        }
    }

    // (d) the page fills its width: at least one NON-dense row reaches the right
    // margin (within a 2*kGroupGap tolerance for integer rounding), proving the
    // grid fills the row rather than leaving a ragged edge. Dense-only pages
    // are exempt.
    const int maxRight = juce::jmax (0, contentWidth_ - kMargin);
    bool anyNonDense = false, fillsWidth = false;
    for (const auto& g : groups_)
    {
        if (! groupVisible (g))
            continue;
        if (! (g.stepGrid || g.singleRow))
        {
            anyNonDense = true;
            if (g.rect.getRight() >= maxRight - 2 * kGroupGap)
                fillsWidth = true;
        }
    }
    return ! anyNonDense || fillsWidth;
}

void ParamPage::setGroupDecoration (const juce::String& groupName,
                                    std::unique_ptr<juce::Component> decoration)
{
    // ParamPage always owns the component (so it never leaks / dangles), even
    // if @p groupName does not match an existing group.
    auto* raw = decoration.get();
    decorations_.push_back (std::move (decoration));
    addAndMakeVisible (raw);
    for (auto& g : groups_)
        if (g.name == groupName)
            g.decoration = raw;

    // Recompute the layout so contentHeight_ already accounts for the new
    // decoration when the editor sizes this page immediately afterwards.
    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

void ParamPage::setGroupInlinePreview (const juce::String& groupName,
                                       std::unique_ptr<juce::Component> preview)
{
    // ParamPage always owns the component (reuse the decorations_ ownership
    // vector), even if @p groupName does not match an existing group.
    auto* raw = preview.get();
    decorations_.push_back (std::move (preview));
    addAndMakeVisible (raw);
    for (auto& g : groups_)
        if (g.name == groupName)
            g.inlinePreview = raw;

    // Re-lay out so the reserved column + remapped knobs take effect immediately.
    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

void ParamPage::setGroupDecorationHeight (const juce::String& groupName, int height)
{
    // Override the reserved room for the named group's decoration (below its
    // control cells). Used for the compact Global voice strip (smaller than the
    // 80px reserved for the Env/LFO ADSR/LFO previews). Re-lays out so the new
    // height takes effect immediately.
    for (auto& g : groups_)
        if (g.name == groupName)
            g.decorationH = juce::jmax (0, height);

    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

void ParamPage::setVisibleGroups (const juce::StringArray& groupNames)
{
    visibleGroups_ = groupNames;

    // Not yet sized (construction / pre-layout): defer entirely. The owning
    // GroupPager::resized() performs the first real layout at the true content
    // width; laying out here at a guessed width would be wasted and wrong.
    if (getWidth() <= 0)
        return;

    // Re-lay-out at the CURRENT (real) width — never a 940px floor. The 940 floor
    // made the row-fill justification grow non-dense groups (OSC/ENV/LFO) far
    // wider than the narrow GroupPager content area, clipping their right column
    // of knobs on a runtime sub-tab switch (reviewer blocker B1). Dense groups
    // (stepGrid/singleRow) are unaffected by the fill pass either way.
    layoutGroups (getWidth());
    applyLayout();
    setSize (getWidth(), contentHeight_);
}

ParamPage::ParamPage (ParvatiAudioProcessor& processor,
                      ThemeManager& themeManager,
                      const std::vector<const PatchParamDescriptor*>& descriptors,
                      int columns, int cellWidth, int cellHeight)
    : themeManager_ (themeManager),
      cellWidth_ (cellWidth), cellHeight_ (cellHeight)
{
    // Honour the page's declared column count (PageInfo::cols) as a cap on the
    // number of group panels per row (whichever wraps first: width overflow or
    // the cap). 0 => width-only wrap (Patch page is not a ParamPage).
    pageCols_ = juce::jmax (0, columns);

    buildGroups (descriptors);
    configureGroupLayouts();

    // Bordered panels first (so they sit behind the control cells), one per group.
    for (auto& g : groups_)
    {
        // The component NAME keeps the English key (stable identity for
        // setGroupDecoration matching); only the displayed TITLE is translated.
        auto gc = std::make_unique<juce::GroupComponent> (g.name, TRANS (g.name));
        gc->setTextLabelPosition (juce::Justification::top | juce::Justification::left);
        // Outline + title-text colours come from the editor-wide L&F, so a theme
        // switch refreshes them automatically.
        addAndMakeVisible (*gc);
        g.groupComp = gc.get();
        groupComponents_.push_back (std::move (gc));
    }

    // Control cells on top of the panel borders.
    for (const auto* d : descriptors)
    {
        const juce::String pid = d->paramID;
        // Sequencer step cells use purpose-built controls instead of a plain
        // knob: the note byte (0..255, half-dead) -> a remapped rotary (one
        // Rest stop + a full note range); the length (1..16) -> a − [n] +
        // stepper. Both subclass ParamControl so they live in controls_ and
        // inherit the mod ring / right-click menu / step-dimming / label.
        if (pid.startsWith ("seqnote_step"))
            controls_.emplace_back (std::make_unique<NoteStepControl> (processor, *d));
        else if (pid.startsWith ("seq_length_"))
            controls_.emplace_back (std::make_unique<SeqLengthStepper> (processor, *d));
        else
            controls_.emplace_back (std::make_unique<ParamControl> (processor, *d));
        addAndMakeVisible (*controls_.back());
        // User-friendly readout (Hz / ms / semitones / % / note names / ...) on
        // raw-numeric SYNTH knobs only. Choice params already show their text;
        // FX params use their own formatter (FxSlotCard). The note-step rotary
        // owns its own Rest/note readout (remapped range) and the length stepper
        // shows its own number, so both are skipped here. Display-only.
        if (! d->isFx && d->choices == nullptr
            && ! pid.startsWith ("seqnote_step")
            && ! pid.startsWith ("seq_length_"))
            controls_.back()->setDisplayValueText (
                [id = juce::String (d->paramID)] (double v) {
                    return paramValueTextSynth (id, v);
                });
    }

    // Seed the content size at a sensible default width; the editor reflows to
    // the real tab width on the first resized().
    layoutGroups (940);
}

void ParamPage::applyThemeColors()
{
    // Group borders / titles are themed via the L&F; force a repaint so a theme
    // switch refreshes them (and the control cells) immediately.
    for (auto& gc : groupComponents_) gc->repaint();
    for (auto& c : controls_)         c->repaint();
    for (auto& d : decorations_)      d->repaint();   // e.g. ADSR previews read the theme live
    repaint();
}

void ParamPage::refreshLanguage()
{
    // The group-component NAME is the stable English key (used for
    // setGroupDecoration matching); only the displayed TITLE is re-translated.
    for (auto& g : groups_)
        if (g.groupComp != nullptr)
            g.groupComp->setText (TRANS (g.name));
    repaint();
}

void ParamPage::paint (juce::Graphics& g)
{
    const auto& theme = themeManager_.getCurrentTheme();
    g.fillAll (theme.backgroundBase);

    // Sub-section dividers inside a sectioned panel (merged Mixer):
    // a 1px muted line in each inter-section gap, drawn from the theme divider
    // token (never a literal colour). The gap positions mirror applyLayout's
    // section walk so each line sits cleanly between knob rows.
    for (const auto& grp : groups_)
    {
        if (! grp.sectioned || ! groupVisible (grp))
            continue;
        const auto sections = mixerSubSectionsOf (controls_, grp.controlIndices);
        if (sections.size() < 2)
            continue;
        auto inner = grp.rect.reduced (kGroupPad);
        inner.removeFromTop (kGroupTitleH);
        const int cols = juce::jmax (1, grp.internalCols);
        int y = inner.getY();
        g.setColour (theme.divider);
        for (size_t si = 0; si + 1 < sections.size(); ++si)
        {
            const int sRows = mixerSectionRowCount (controls_, sections[si].second, cols);
            y += sRows * grp.cellH;                 // top of the gap
            const int dy = y + kSectionGap / 2;      // centre of the gap
            g.drawHorizontalLine (dy, (float) inner.getX(), (float) inner.getRight());
            y += kSectionGap;
        }
    }
}

void ParamPage::resized()
{
    layoutGroups (getWidth());
    applyLayout();
}

void ParamPage::reflowToWidth (int targetWidth, int viewportHeight)
{
    if (targetWidth <= 0)
        return;
    // Record the tab content height so layoutGroups can vertically centre short
    // pages consistently across ALL tabs (not just the current one).
    centerHeight_ = juce::jmax (0, viewportHeight);
    // Lay out for the requested width, then adopt the resulting height so the
    // parent Viewport scrolls vertically only. setSize() re-triggers resized()
    // which re-lays-out to the same width (cheap rectangle math).
    layoutGroups (targetWidth);
    applyLayout();
    if (getWidth() != targetWidth || getHeight() != contentHeight_)
        setSize (targetWidth, contentHeight_);
}

//==============================================================================
ParvatiEditor::ParvatiEditor (ParvatiAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef_ (p), loadMouseListener_ (p)
{
    // The UI is landscape-only (no portrait layout exists). Lock the device to
    // the two landscape orientations. The iOS/Android peer consults this live in
    // its supportedInterfaceOrientations (see juce_UIViewComponentPeer_ios.mm);
    // on desktop it is a harmless no-op (no device rotation).
    juce::Desktop::getInstance().setOrientationsEnabled (
        juce::Desktop::rotatedClockwise | juce::Desktop::rotatedAntiClockwise);

    // Install the persisted chrome language BEFORE building the UI, so every
    // TRANS() below resolves to the right language at construction. English (and
    // "auto" on an English locale) clears the mappings => TRANS() is the
    // identity => the UI is byte-identical to the un-localised build.
    installLanguage (processorRef_.getUiLanguage());

    // Theme + LookAndFeel: one L&F on the editor, inherited by the whole control
    // tree, so no per-component palette is needed.
    lnf_.setTheme (themeManager_.getCurrentTheme());
    setLookAndFeel (&lnf_);
    themeManager_.addChangeListener (this);

    // Tooltips: one TooltipWindow parented to (and deleted with) the editor.
    // ParamControl is a TooltipClient returning its parameter's help text.
    tooltipWindow_ = std::make_unique<juce::TooltipWindow> (this);

    // Phase 4a: apply persisted UI preferences. The theme selection may differ
    // from the ThemeManager default (Carbon); selectByName broadcasts a change
    // (caught by changeListenerCallback) if the selection actually moves.
    themeManager_.selectByName (processorRef_.getUiTheme());
    lnf_.setTheme (themeManager_.getCurrentTheme());
    ParamControl::setTooltipsEnabled (processorRef_.getUiTooltips());

    // Apply the persisted parameter-smoothing preference to the engine (the
    // SettingsPanel toggle is seeded from getUiSmoothing() when it is built
    // below; this covers the audio side for hosts that show the editor).
    processorRef_.setParameterSmoothing (processorRef_.getUiSmoothing());

    // Group every descriptor into its section bucket; Part params (volume,
    // legato, portamento) and synth options (VCA curve) ride on the Oscillators
    // page as the "global" footer. `part_select` is intentionally skipped here:
    // it has a dedicated top-bar ComboBox (partCombo_) bound to the same APVTS
    // param, so generating a second control for it on a page would be redundant.
    std::vector<const PatchParamDescriptor*> sec[10];
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.paramID == "part_select")
            continue;
        // FX params (fx* / fxmod*) are per-part Parvati-exclusive params routed
        // via applyFxParameter; they are NEVER bucketed into the synth pages.
        // The FX-slot pages are generated separately below, and the FX mod matrix
        // is the editor-owned FxMatrixView (no ParamPage at all).
        if (d.isFx)
            continue;
        sec[(int) sectionForId (d.paramID)].push_back (&d);
    }

    // Integrated workspace hosts the 9 synth ParamPages (built + routed below).
    // Created early so the page-build loop can reparent each page into it.
    synthWorkspace_ = std::make_unique<SynthWorkspace> (themeManager_);

    const ParvatiTheme& theme = themeManager_.getCurrentTheme();

    // ---- Top patch bar: factory patch list + Load .PRO... + name ----
    patchCaption_.setText (TRANS ("Patch:"), juce::dontSendNotification);
    // Caption text colour from the L&F (dim).
    patchCaption_.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (patchCaption_);

    // Cascading patch menu (Templates / User / Factory banks / Multi). The
    // browser scans the dirs live on each open, so there is no pre-populate.
    presetBrowser_ = std::make_unique<PresetBrowser> (
        processorRef_.getTemplatesDir(), processorRef_.getUserPatchDir(),
        processorRef_.getFactoryPatchDir(), processorRef_.getFactoryMultiDir(),
        [this] (const juce::File& f) { applyPatchFile (f); });
    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
    addAndMakeVisible (*presetBrowser_);

    loadButton_.setButtonText (TRANS ("Load"));
    // Button colours from the L&F.
    loadButton_.onClick = [this] { openLoadDialog(); };
    addAndMakeVisible (loadButton_);

    // Save: a single button whose popup menu picks the format. "Ambika Patch
    // (.PRO)" writes the byte-faithful hardware-shareable patch; "Parvati Patch
    // (.parvati)" writes the full-fidelity YAML (carries vca_curve / filter_card
    // / arp that the .PRO byte format drops).
    saveButton_.setButtonText (TRANS ("Save"));
    saveButton_.onClick = [this] {
        juce::PopupMenu m;
        m.addItem (1, TRANS ("Ambika Patch (.PRO)"));
        m.addItem (2, TRANS ("Parvati Patch (.parvati)"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int result) {
            if (result == 1)      openSaveDialog();
            else if (result == 2) openSaveParvatiDialog();
        });
    };
    addAndMakeVisible (saveButton_);

    // Phase 4c: Undo / Redo are Path-drawn IconButtons (curved arrows) — no
    // unicode glyph (the font stack renders U+21B6/21B7 as "..."). The APVTS
    // UndoManager records every parameter change; enable/disable is mirrored on
    // the editor timer.
    undoButton_.setTooltip (TRANS ("Undo"));
    undoButton_.onClick = [this] { processorRef_.getUndoManager().undo(); };
    addAndMakeVisible (undoButton_);
    redoButton_.setTooltip (TRANS ("Redo"));
    redoButton_.onClick = [this] { processorRef_.getUndoManager().redo(); };
    addAndMakeVisible (redoButton_);

    // On-screen zoom +/-/0 (visible on every platform; iPad has no keyboard).
    // Mirror the Cmd/Ctrl +/-/0 shortcuts via the shared applyZoom() helper.
    zoomInButton_.setTooltip (TRANS ("Zoom in"));
    zoomInButton_.onClick = [this] { applyZoom (zoom_ + 0.1); };
    addAndMakeVisible (zoomInButton_);
    zoomOutButton_.setTooltip (TRANS ("Zoom out"));
    zoomOutButton_.onClick = [this] { applyZoom (zoom_ - 0.1); };
    addAndMakeVisible (zoomOutButton_);
    zoomResetButton_.setTooltip (TRANS ("Reset zoom"));
    zoomResetButton_.onClick = [this] { applyZoom (1.0); };
    addAndMakeVisible (zoomResetButton_);
    // Zoom overflow: one "..." button opens a 44pt-row popup holding the three
    // zoom actions, so the grown (44pt) icon cluster still fits the 1280pt
    // editor width. The three zoom buttons above stay constructed (their logic
    // is reused here) but are not placed on iOS (see resized()).
    zoomOverflowButton_.setTooltip (TRANS ("Zoom"));
    zoomOverflowButton_.onClick = [this]
    {
        juce::PopupMenu m;
        m.setLookAndFeel (&lnf_);   // app-themed popup (amber accent, dark fill)
        m.addItem (juce::PopupMenu::Item (TRANS ("Zoom In")).setAction  ([this] { applyZoom (zoom_ + 0.1); }));
        m.addItem (juce::PopupMenu::Item (TRANS ("Zoom Out")).setAction ([this] { applyZoom (zoom_ - 0.1); }));
        m.addItem (juce::PopupMenu::Item (TRANS ("Reset Zoom")).setAction ([this] { applyZoom (1.0); }));
        m.showMenuAsync (juce::PopupMenu::Options()
                             .withTargetComponent (&zoomOverflowButton_)
                             .withStandardItemHeight (ParvatiLookAndFeel::kPopupRowHeight),
                         nullptr);
    };
    addAndMakeVisible (zoomOverflowButton_);

    // ---- Top bar: Part selector (bound to the `part_select` APVTS param) ----
    partCaption_.setText (TRANS ("Part:"), juce::dontSendNotification);
    // Caption text colour from the L&F (dim).
    partCaption_.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (partCaption_);

    for (int i = 1; i <= SynthEngine::getNumParts(); ++i)
        partCombo_.addItem (TRANS ("Part") + " " + juce::String (i), i);
    // Combo colours from the L&F.
    addAndMakeVisible (partCombo_);
    partComboAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef_.getApvts(), "part_select", partCombo_);

    // pageSelector_ ([SYNTH | GLOBAL]) tab-bar depth + outline are set after its
    // tabs are populated (below). TabbedComponent / TabbedButtonBar colours come
    // from the inherited editor L&F.

    struct PageInfo { const char* name; const char* shortName; Section s; int cols, cellW, cellH; };
    // Cell heights are kept tight (a 44px knob + its label fits in ~76px) so
    // every page matches the dense SEQ reference instead of the sparse look the
    // 106px rows produced. Mod/Modifier/Seq groups override these in
    // configureGroupLayouts() — their entries here are kept for reference only.
    const PageInfo pages[] = {
        { "Oscillators", "OSC",        Section::Oscillators, 4, 214, 76 },
        { "Mixer",       "MIX",        Section::Mixer,       4, 214, 76 },
        { "Filter",      "FILTER",     Section::Filter,      4, 214, 76 },
        { "Envelopes",   "ENV",        Section::Envelopes,   3, 198, 76 },
        { "LFOs",        "LFO",        Section::Lfos,        4, 198, 76 },
        { "Mod Matrix",  "MOD MATRIX", Section::ModMatrix,   2, 164, 72 },
        { "Modifiers",   "MODIFIERS",  Section::Modifiers,   3, 300, 64 },   // cellH overridden per-group (configureGroupLayouts); ref only
        { "Sequencer",   "SEQ",        Section::Sequencer,   6, 150, 80 },
        { "Arp",         "ARP",        Section::Arp,         3, 214, 76 },
        { "Patch",       "PATCH",      Section::Global,      3, 214, 76 },
    };

    // Generator pages captured by section during the loop, then registered with
    // the CentralModBar's active-generator editor (bottom-left host). Each is an
    // editor-owned ParamPage (reparented, never regenerated); ARP shows all its
    // groups (empty setVisibleGroups set).
    ParamPage* envPage = nullptr;
    ParamPage* lfoPage = nullptr;
    ParamPage* modifierPage = nullptr;
    ParamPage* arpPage = nullptr;
    ParamPage* seqPage = nullptr;

    for (const auto& pg : pages)
    {
        // MOD MATRIX is now the editor-owned ModMatrixView (Wave 1), NOT a
        // ParamPage: build + host it here and skip page generation entirely.
        // The mod1_*/mod14_* APVTS params are created independently in
        // createParameterLayout (ParameterLayout.cpp), so removing this ParamPage
        // does NOT touch the byte-bridge / patch / DSP. The view is hosted
        // NON-owned by the MOD MATRIX tab (editor-owned via modMatrixView_).
        if (pg.s == Section::ModMatrix)
        {
            modMatrixView_ = std::make_unique<ModMatrixView> (processorRef_, themeManager_);
            synthWorkspace_->setModMatrixView (modMatrixView_.get());
            continue;
        }

        auto page = std::make_unique<ParamPage> (processorRef_, themeManager_, sec[(int) pg.s],
                                                 pg.cols, pg.cellW, pg.cellH);

        // Live previews: an ADSR curve under each Env group (Envelopes tab) and
        // an LFO waveform under each LFO group (LFOs tab). The getters read the
        // APVTS parameter's NORMALIZED value (getValue() returns 0..1) so the
        // preview tracks the knobs live. (Each env_lfo unit runs BOTH its
        // envelope and its LFO; splitting the halves onto two tabs matches that.)
        auto norm = [this] (const juce::String& id) -> float {
            auto* param = processorRef_.getApvts().getParameter (id);
            return param ? param->getValue() : 0.0f;
        };
        // Register a graph preview for live category re-tinting on theme change:
        // each entry stores a closure calling the concrete component's
        // setCategoryColour + a theme-token pointer so reapplyGraphCategoryColours
        // can re-resolve the NEW theme's value and re-push it.
        auto bindGraph = [this] (GraphTintFn fn, ThemeColourField field) {
            graphCategoryBindings_.emplace_back (std::move (fn), field);
        };
        if (pg.s == Section::Envelopes)
        {
            const juce::String envs[3] = { "env1", "env2", "env3" };
            const juce::String envLabels[3] = { "Env 1 (Mod)", "Env 2 (Filter)", "Env 3 (Amp)" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = envs[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    envLabels[i],
                    [norm, e] { return norm (e + "_attack");  },
                    [norm, e] { return norm (e + "_decay");   },
                    [norm, e] { return norm (e + "_sustain"); },
                    [norm, e] { return norm (e + "_release"); });
                disp->setPreviewMode (0);   // ADSR curve
                // Cyan trace from the Envelopes category token (re-resolved live
                // on theme change via the binding registered below).
                disp->setCategoryColour (theme.catEnv);
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catEnv);
                page->setGroupDecoration (envLabels[i], std::move (disp));
            }
        }
        else if (pg.s == Section::Lfos)
        {
            // LFO 1/2/3: the LFO half of env_lfo[0..2] (shape drives the preview).
            const juce::String lfos[3] = { "env1", "env2", "env3" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = lfos[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    "LFO " + juce::String (i + 1),
                    std::function<float()> {}, std::function<float()> {},
                    std::function<float()> {}, std::function<float()> {},
                    [norm, e] { return norm (e + "_lfo_shape"); });
                disp->setPreviewMode (1);   // LFO waveform
                disp->setCategoryColour (theme.catLfo);   // magenta trace
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catLfo);
                page->setGroupDecoration ("LFO " + juce::String (i + 1), std::move (disp));
            }
            // Voice LFO (MOD_SRC_LFO_4).
            auto vdisp = std::make_unique<EnvelopeDisplay> (
                "Voice LFO",
                std::function<float()> {}, std::function<float()> {},
                std::function<float()> {}, std::function<float()> {},
                [norm] { return norm ("voice_lfo_shape"); });
            vdisp->setPreviewMode (1);
            vdisp->setCategoryColour (theme.catLfo);   // magenta trace
            bindGraph ([gp = vdisp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                       &ParvatiTheme::catLfo);
            page->setGroupDecoration ("Voice LFO", std::move (vdisp));
        }
        else if (pg.s == Section::Oscillators)
        {
            // INLINE waveform preview beside each OSC Shape dropdown: one
            // OscPreviewDisplay per oscillator, laid out in the reserved column
            // 1 (configureGroupLayouts gives each OSC group 5 columns). Amber
            // (catAudio) trace, re-tinted live on theme change.
            const juce::String oscs[2]  = { "osc1", "osc2" };
            const juce::String labels[2] = { "Osc 1", "Osc 2" };
            for (int i = 0; i < 2; ++i)
            {
                const juce::String o = oscs[i];
                auto disp = std::make_unique<OscPreviewDisplay> (
                    labels[i] + " Wave",
                    [norm, o] { return norm (o + "_shape"); },
                    [norm, o] { return norm (o + "_param"); });
                disp->setCategoryColour (theme.catAudio);   // amber trace
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catAudio);
                page->setGroupInlinePreview (labels[i], std::move (disp));
            }
        }
        else if (pg.s == Section::Filter)
        {
            // Magnitude-response curve under the "Filter" group (decoration).
            // Compact height so the Filter column (3 knobs + filter-env/lfo
            // amounts + the curve) fits the main-row half-height at the 1100
            // minimum. Amber (catAudio) trace, re-tinted live on theme change.
            auto disp = std::make_unique<FilterResponseDisplay> (
                "Filter Response",
                [norm] { return norm ("filter1_cutoff"); },
                [norm] { return norm ("filter1_reso"); },
                [norm] { return norm ("filter1_mode"); });
            disp->setCategoryColour (theme.catAudio);   // amber trace
            bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                       &ParvatiTheme::catAudio);
            page->setGroupDecoration ("Filter", std::move (disp));
            page->setGroupDecorationHeight ("Filter", 42);
        }

        if (pg.s == Section::Global)
            globalPage_ = page.get();   // voice-activity cells attach here as a decoration

        page->setSize (page->getContentWidth(), page->getContentHeight());
        ParamPage* rawPage = page.get();
        generatedPages_.push_back (std::move (page));

        // Route the editor-owned page into the integrated workspace by section.
        // The Global page is hosted inside the Patch page after the loop.
        // Pages are reparented — NOT regenerated — so every APVTS attachment and
        // the verified byte-bridge survive the reorganization unchanged. Dense
        // sections paginate by group via a GroupPager (one sub-tab = one group
        // subset) so each visible slice fits its cell with NO scrollbar. (Patch
        // is never a generated page; if/else avoids switch/enum + branch-clone
        // Route the editor-owned page by section. Main-row pages (MIX/OSC/
        // FILTER) are hosted directly. Generator pages (ENV/LFO/MODIFIERS/ARP/
        // SEQ) are captured here and registered with the CentralModBar's
        // active-generator editor AFTER the loop (one pill -> one page+group).
        // Pages are reparented — NOT regenerated — so every APVTS attachment and
        // the verified byte-bridge survive unchanged. (Patch/ModMatrix never
        // reach here; the if/else avoids switch/enum + branch-clone warnings.)
        if (pg.s == Section::Mixer)
            synthWorkspace_->setMainLeft (rawPage);
        else if (pg.s == Section::Oscillators)
            synthWorkspace_->setOscillators (rawPage);   // both osc panels visible directly
        else if (pg.s == Section::Filter)
            synthWorkspace_->setMainRight (rawPage);
        else if (pg.s == Section::Envelopes)
            envPage = rawPage;
        else if (pg.s == Section::Lfos)
            lfoPage = rawPage;
        else if (pg.s == Section::Modifiers)
            modifierPage = rawPage;
        else if (pg.s == Section::Arp)
            arpPage = rawPage;
        else if (pg.s == Section::Sequencer)
            seqPage = rawPage;
        // Section::ModMatrix is handled by the early-continue above (ModMatrixView).
        // Section::Global (-> hosted inside the Patch page after the loop)
        // intentionally falls through here.
    }

    // ---- Central Modulation Bar wiring (Phase 2) ----
    // Register every GENERATOR pill -> { owning ParamPage, groups-to-show } so the
    // bar's bottom-left active-editor host can reparent + setVisibleGroups the
    // right slice per pill (pages are never regenerated). Group names match the
    // ParamPage groupForId() keys (verified in groupForId). VLFO == per-voice
    // LFO (MOD_SRC_LFO_4, verified in voice.cpp). ARP shows ALL its groups
    // (EMPTY array). The Note Sequencer pill is the bar-only sentinel
    // (parvati::kNoteSeqSentinel == -1, NOT a real MOD_SRC_*): it reveals its
    // "Note Pitch" group from the Sequencer page (Option A: only the note
    // pitch + gate control; velocity stays in the full Sequencer TAB), and is
    // click-only (the bar skips its drag because enumValue < 0). The drag
    // payload ("parvatiModSrc:<enum>") is emitted by the bar itself, so the
    // destination-side rings / padlock / ModMatrixHighlight need ZERO changes.
    using namespace ambika::dsp;
    synthWorkspace_->registerGeneratorPage (MOD_SRC_ENV_1, envPage,        juce::StringArray{ "Env 1 (Mod)" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_ENV_2, envPage,        juce::StringArray{ "Env 2 (Filter)" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_ENV_3, envPage,        juce::StringArray{ "Env 3 (Amp)" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_LFO_1, lfoPage,        juce::StringArray{ "LFO 1" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_LFO_2, lfoPage,        juce::StringArray{ "LFO 2" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_LFO_3, lfoPage,        juce::StringArray{ "LFO 3" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_LFO_4, lfoPage,        juce::StringArray{ "Voice LFO" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_SEQ_1, seqPage,        juce::StringArray{ "Sequencer 1" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_SEQ_2, seqPage,        juce::StringArray{ "Sequencer 2" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_ARP_STEP, arpPage,     juce::StringArray{});   // empty => all groups
    // Note Sequencer pill (bar-only sentinel): click-only, NOT draggable; opens
    // the Sequencer page showing ONLY Note Pitch (the remapped note rotary +
    // gate-at-rest). Note Velocity is NOT shown in the generator host — the
    // ~290px non-viewport band cannot fit both 16-step groups, so stacking them
    // clipped velocity ~75%. Velocity stays reachable in the full Sequencer TAB
    // (its knob is unchanged). One group => no clip.
    synthWorkspace_->registerGeneratorPage (parvati::kNoteSeqSentinel, seqPage,
                                            juce::StringArray{ "Note Pitch" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_OP_1, modifierPage,    juce::StringArray{ "Modifier 1" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_OP_2, modifierPage,    juce::StringArray{ "Modifier 2" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_OP_3, modifierPage,    juce::StringArray{ "Modifier 3" });
    synthWorkspace_->registerGeneratorPage (MOD_SRC_OP_4, modifierPage,    juce::StringArray{ "Modifier 4" });
    // Drag-only (Perf/Util/Const) pill click: briefly flash the mod-matrix rows
    // routed FROM that source, reusing the existing timed flash.
    synthWorkspace_->setOnDragOnlyPillClicked ([this] (int src)
    {
        if (modMatrixView_ != nullptr)
            modMatrixView_->flashRowsForSource (src);
    });
    // Default to Env 1 visible on startup.
    synthWorkspace_->setActiveGenerator (MOD_SRC_ENV_1);


    // ---- FX workspace construction (Phase 4) ----
    // FxWorkspace is a structural clone of SynthWorkspace (TOP = 3 FX-slot
    // ParamPages, MIDDLE = its own CentralModBar, BOTTOM-LEFT = the SHARED
    // active-generator host, BOTTOM-RIGHT = the editor-owned FxMatrixView). It
    // reuses the synth's generator ParamPages (shared, never duplicated) so a
    // mode toggle reparents a single active selection between the two
    // workspaces. The FX-slot pages + FxMatrixView are editor-owned
    // (generatedPages_ / fxMatrixView_) and hosted NON-owned by the workspace.
    fxWorkspace_ = std::make_unique<FxWorkspace> (themeManager_);

    // Generate the 3 FX-slot CARDS (FX1/FX2/FX3) — self-contained modular cards
    // (power/bypass toggle + type combo + visualizer + a param knob grid with the
    // dry/wet anchored bottom-right). Each card CREATES + OWNS its 6 full
    // ParamControls from the fx{N}_param1..5 + fx{N}_drywet descriptors (so they
    // keep EVERY modulation behaviour: FX-mod-matrix drag-drop + mod rings +
    // tooltips + category arc). fx_topo / fx_order now ride on the full-width
    // FxRoutingBar (set below), NOT on a slot page. Cards are editor-owned
    // (fxSlotCards_) and hosted NON-owned via setFxSlotCard.
    for (int slot = 0; slot < 3; ++slot)
    {
        const juce::String prefix = "fx" + juce::String (slot + 1) + "_";
        const PatchParamDescriptor *p1 = nullptr, *p2 = nullptr, *p3 = nullptr,
                                   *p4 = nullptr, *p5 = nullptr, *dw = nullptr;
        for (const auto& d : getPatchParamDescriptors())
        {
            if (! (d.isFx && juce::String (d.paramID).startsWith (prefix)))
                continue;
            if      (d.paramID == prefix + "param1") p1 = &d;
            else if (d.paramID == prefix + "param2") p2 = &d;
            else if (d.paramID == prefix + "param3") p3 = &d;
            else if (d.paramID == prefix + "param4") p4 = &d;
            else if (d.paramID == prefix + "param5") p5 = &d;
            else if (d.paramID == prefix + "drywet") dw = &d;
        }
        jassert (p1 != nullptr && p2 != nullptr && p3 != nullptr
                 && p4 != nullptr && p5 != nullptr && dw != nullptr);
        auto card = std::make_unique<FxSlotCard> (processorRef_, slot,
                                                  p1, p2, p3, p4, p5, dw);
        FxSlotCard* raw = card.get();
        fxSlotCards_[slot] = std::move (card);
        fxWorkspace_->setFxSlotCard (slot, raw);
    }

    // The FX routing header bar (topology dropdown + drag-reorderable chain).
    // Editor-owned, hosted NON-owned above the three cards.
    fxRoutingBar_ = std::make_unique<FxRoutingBar> (processorRef_, themeManager_);
    fxWorkspace_->setFxRoutingBar (fxRoutingBar_.get());

    // The FX mod matrix (editor-owned, NON-owned host of the FX workspace).
    fxMatrixView_ = std::make_unique<FxMatrixView> (processorRef_, themeManager_);
    fxWorkspace_->setFxMatrixView (fxMatrixView_.get());

    // Register the SAME generator pages into the FX workspace (SHARED editor —
    // the pages are NOT duplicated, only reparented between workspaces on a
    // mode toggle). ARP shows all its groups (empty array); the Note Sequencer
    // pill reveals Note Pitch (only) from the Sequencer page (Option A: one
    // group fits the non-viewport host without clipping; velocity stays in the
    // full Sequencer TAB).
    using namespace ambika::dsp;
    fxWorkspace_->registerGeneratorPage (MOD_SRC_ENV_1, envPage,        juce::StringArray{ "Env 1 (Mod)" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_ENV_2, envPage,        juce::StringArray{ "Env 2 (Filter)" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_ENV_3, envPage,        juce::StringArray{ "Env 3 (Amp)" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_LFO_1, lfoPage,        juce::StringArray{ "LFO 1" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_LFO_2, lfoPage,        juce::StringArray{ "LFO 2" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_LFO_3, lfoPage,        juce::StringArray{ "LFO 3" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_LFO_4, lfoPage,        juce::StringArray{ "Voice LFO" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_SEQ_1, seqPage,        juce::StringArray{ "Sequencer 1" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_SEQ_2, seqPage,        juce::StringArray{ "Sequencer 2" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_ARP_STEP, arpPage,     juce::StringArray{});   // empty => all groups
    fxWorkspace_->registerGeneratorPage (parvati::kNoteSeqSentinel, seqPage,
                                         juce::StringArray{ "Note Pitch" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_OP_1, modifierPage,    juce::StringArray{ "Modifier 1" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_OP_2, modifierPage,    juce::StringArray{ "Modifier 2" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_OP_3, modifierPage,    juce::StringArray{ "Modifier 3" });
    fxWorkspace_->registerGeneratorPage (MOD_SRC_OP_4, modifierPage,    juce::StringArray{ "Modifier 4" });
    // Drag-only pill click: flash the FX-matrix rows routed FROM that source.
    fxWorkspace_->setOnDragOnlyPillClicked ([this] (int src)
    {
        if (fxMatrixView_ != nullptr)
            fxMatrixView_->flashRowsForSource (src);
    });
    // Track the SHARED active generator selection from BOTH workspaces so a mode
    // toggle reparents the right page into the newly-visible workspace.
    synthWorkspace_->setOnActiveGeneratorChanged ([this] (int src) { activeGeneratorModSrc_ = src; });
    fxWorkspace_->setOnActiveGeneratorChanged    ([this] (int src) { activeGeneratorModSrc_ = src; });
    // NOTE: the FX workspace does NOT host the active generator at startup — the
    // shared page can only have ONE parent, and the VISIBLE workspace (SYNTH,
    // default) owns it (set above). setFxMode(true) releases it from SYNTH and
    // reparents it into FX on demand (via activeGeneratorModSrc_).


    // ---- Top-level page selector [SYNTH | FX] ----
    // Two NON-owned tab contents (synthWorkspace_ at index 0, fxWorkspace_ at
    // index 1). The tab bar is HIDDEN (depth 0) — the header [Synth]/[FX]
    // buttons are the UI (setFxMode swaps the current tab). PATCH is a header
    // overlay (patchPage_, which hosts globalPage_), not a tab. Both tab
    // contents are editor-owned (synthWorkspace_ / fxWorkspace_), so the teardown
    // order stays deterministic.
    pageSelector_.setTabBarDepth (0);          // hide the tab bar — [Synth]/[FX] header buttons are the UI
    pageSelector_.setOutline (0);
    pageSelector_.addTab (TRANS ("SYNTH"), theme.backgroundBase, synthWorkspace_.get(), false);
    pageSelector_.addTab (TRANS ("FX"),    theme.backgroundBase, fxWorkspace_.get(),     false);
    pageSelector_.setCurrentTabIndex (0, false);   // SYNTH shown first
    addAndMakeVisible (pageSelector_);

    // ---- Patch page overlay (custom component, not descriptor-generated) ----
    // The Patch page replaces the old separate Multi/Setup + Global pages: it
    // hosts the editor-owned Section::Global ParamPage (patch-wide knobs + the
    // voice-activity meter decoration) below its 6 part rows. A header "Patch"
    // button (next to the Part dropdown) toggles this page as an overlay over
    // the tab area. globalPage_ ownership stays in generatedPages_; hostParamPage
    // only reparents it into the Patch page.
    patchPage_ = std::make_unique<PatchPage> (processorRef_, themeManager_);
    addChildComponent (patchPage_.get());   // owned here; invisible until toggled
    patchPage_->setVisible (false);
    if (globalPage_ != nullptr)
        patchPage_->hostParamPage (globalPage_);   // reparents the Section::Global ParamPage into the Patch page

    // ---- Unified 3-way top-level page selector: [Synth][FX][Patch] ----
    // All three header buttons are radio-group peers; each selects its PAGE via
    // showTopPage(idx), which sets EXCLUSIVE visibility (Patch is now a FULL
    // page — pageSelector_ is hidden while it is active, so it is the sole
    // content, not a floating overlay) and syncs every button. Synth/FX
    // additionally reparent the shared generator (only on a real Synth<->FX
    // change). NOT APVTS params — view-state only.
    synthModeButton_.setTooltip (TRANS ("Synth page"));
    fxModeButton_.setTooltip    (TRANS ("FX page"));
    globalButton_.setTooltip    (TRANS ("Patch / arrangement page"));
    for (auto* b : { &synthModeButton_, &fxModeButton_, &globalButton_ })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1, juce::dontSendNotification);
        addAndMakeVisible (*b);
    }
    synthModeButton_.onClick = [this] { showTopPage (0); };
    fxModeButton_.onClick    = [this] { showTopPage (1); };
    globalButton_.onClick    = [this] { showTopPage (2); };
    showTopPage (0);   // SYNTH is the default page (sets visibility + button states)

    // ---- [KBD] header toggle: show/hide the bottom virtual keyboard ----
    // The keyboard floats as an OVERLAY over the bottom of the workspace:
    // toggling only shows/hides it. The content area keeps its FULL height
    // whether or not the keyboard is visible, so the synth controls never move
    // (the keyboard bounds are positioned once in resized() and only its
    // visibility toggles here). See resized() for the overlay placement + z-order.
    kbdToggleButton_.setTooltip (TRANS ("Toggle virtual keyboard"));
    kbdToggleButton_.setClickingTogglesState (true);
    kbdToggleButton_.setToggleState (false, juce::dontSendNotification);   // hidden by default: the dense no-scrollbar workspace fits 1280x620 without reserving the 76px keyboard strip (toggle [KBD] to float it on top)
    kbdToggleButton_.onClick = [this] {
        const bool on = kbdToggleButton_.getToggleState();
        if (keyboardView_ != nullptr) keyboardView_->setVisible (on);
        if (wheels_       != nullptr) wheels_->setVisible (on);
    };
    addAndMakeVisible (kbdToggleButton_);

    // ---- [MOD] header toggle: tap-to-assign modulation ----
    // Where there is no drag-and-drop (touch), modulation routing is reached by
    // toggling [MOD] ON, tapping a mod source, then tapping a destination knob —
    // which calls the same requestAssign seam itemDropped uses. ON reuses the
    // drop-zone affordance (ring on dest knobs, dim non-targets) so the tap
    // mode mirrors a drag visually. The toggled button is the "still in assign
    // mode" indicator.
    modAssignButton_.setTooltip (TRANS ("Tap-to-assign modulation"));
    modAssignButton_.setClickingTogglesState (true);
    modAssignButton_.setToggleState (false, juce::dontSendNotification);
    modAssignButton_.onClick = [this] {
        ParamControl::setTapAssignActive (modAssignButton_.getToggleState());
    };
    addAndMakeVisible (modAssignButton_);

    // ---- Header: brand icon + white "Parvati" wordmark (painted, left) + version (inline, right of logo) ----
    versionLabel_.setText ("by 805Labs - v" PARVATI_VERSION, juce::dontSendNotification);
    versionLabel_.setFont (juce::FontOptions (10.0f));
    versionLabel_.setColour (juce::Label::textColourId, theme.textSecondary);
    versionLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (versionLabel_);

    // ---- Phase 4a: settings button + side panel ----
    // Click-toggle feedback reflects whether the Settings panel is open (the
    // "on" colour is the theme accent, via TextButton::buttonOnColourId). The
    // authoritative sync is the panel's onPanelShowHide callback below, which
    // fires on any show/hide (button click, dismiss glyph, click-outside).
    settingsButton_.setClickingTogglesState (true);
    settingsButton_.setTooltip (TRANS ("Settings"));
    settingsButton_.onClick = [this] {
        settingsPanelHost_->showOrHide (! settingsPanelHost_->isPanelShowing());
        settingsButton_.setToggleState (settingsPanelHost_->isPanelShowing(),
                                        juce::dontSendNotification);
    };
    addAndMakeVisible (settingsButton_);

    // ---- Phase 4a: virtual keyboard (bottom strip) ----
    // Click-to-play routes MIDI into the processor's MidiMessageCollector
    // (thread-safe); the timer mirrors sounding notes back as latch highlights.
    keyboardView_ = std::make_unique<KeyboardView>();
    keyboardView_->setNoteCallback ([this] (int note, bool on, float vel) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int status   = on ? (0x90 | ((ch - 1) & 0xf)) : (0x80 | ((ch - 1) & 0xf));
        const int velocity = on ? juce::jlimit (0, 127, juce::roundToInt (vel * 127.0f)) : 0;
        processorRef_.addMidiEvent (juce::MidiMessage (status, note, velocity));
    });
    addAndMakeVisible (*keyboardView_);
    keyboardView_->refresh();

    // ---- Pitch + Mod wheels (left of the keyboard) ----
    wheels_ = std::make_unique<WheelsComponent>();
    wheels_->onPitch = [this] (float v) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int pv = juce::jlimit (0, 16383, juce::roundToInt ((v * 0.5f + 0.5f) * 16383.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::pitchWheel (ch, pv));
    };
    wheels_->onMod = [this] (float v) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int mv = juce::jlimit (0, 127, juce::roundToInt (v * 127.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::controllerEvent (ch, 1, mv));   // CC1 = mod wheel
    };
    addAndMakeVisible (*wheels_);
    // Computer-keyboard (musical-typing) play is a STANDALONE-only affordance.
    // In a plugin host the DAW owns the computer keyboard (e.g. Ableton's
    // "Computer MIDI Keyboard") and routes it as normal MIDI, so capturing keys
    // here would double-trigger and steal keystrokes from the host.
    keyboardView_->setComputerKeyboardEnabled (
        processorRef_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);

    // ---- Voice activity cells live on the Global page; the bottom strip shows
    // only the active-count + a hover-tooltip bar (cells + "Voices" word were
    // removed per request). Build the cells meter, wire it, and attach it to the
    // Global page's "Global" group as a decoration (owned by the page). ----
    {
        auto vm = std::make_unique<VoiceMeter>();
        vm->setStateProvider ([this]() {
            std::vector<VoiceActivity> v;
            auto& e = processorRef_.getEngine();
            v.reserve (static_cast<size_t> (e.getNumVoices()));
            for (int i = 0; i < e.getNumVoices(); ++i)
            {
                // SF-1: read the lock-free atomic snapshot instead of the
                // non-atomic SynthesiserVoice::currentlyPlayingNote.
                auto* av = e.getAmbikaVoice (i);
                v.push_back ({ av != nullptr && av->isDisplayedActive(),
                               av != nullptr ? av->getDisplayedNote() : -1 });
            }
            return v;
        });
        globalVoiceMeter_ = vm.get();
        if (globalPage_ != nullptr)
        {
            globalPage_->setGroupDecoration ("Global", std::move (vm));
            // Compact the voice strip: ~32px instead of the 80px reserved for the
            // Env/LFO ADSR/LFO previews (those keep kDecorationH via the default).
            globalPage_->setGroupDecorationHeight ("Global", 32);
        }
    }

    // ---- Bottom status strip: compact active-voice count + tooltip bar ----
    statusCountLabel_.setJustificationType (juce::Justification::centred);
    statusCountLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    statusCountLabel_.setColour (juce::Label::textColourId, theme.accentPrimary);
    statusCountLabel_.setText ("0/" + juce::String (
        processorRef_.getEngine()
            .getPart (processorRef_.getEngine().getCurrentPart()).voiceCount_.load()),
                               juce::dontSendNotification);
    addAndMakeVisible (statusCountLabel_);
    // Realtime audio-load / overrun probe readout (see ParvatiAudioProcessor::
    // getAudioLoadCurrent/Peak/getAudioOverrunCount). Shows "CPU 42%" in green,
    // amber near the limit, red on an overrun ("CPU 98% !3"). Updated at 30 Hz
    // in timerCallback(). Pure read of the processor's atomics (message thread).
    statusLoadLabel_.setJustificationType (juce::Justification::centred);
    statusLoadLabel_.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    statusLoadLabel_.setColour (juce::Label::textColourId, theme.textSecondary);
    statusLoadLabel_.setText ("CPU 0%", juce::dontSendNotification);
    statusLoadLabel_.setTooltip ("Audio-thread realtime load (peak since reset) + overrun count. "
                                 "Approaching 100% means xruns/crackle. Right-click (or tap on touch) to reset the peak.");
    addAndMakeVisible (statusLoadLabel_);
    statusLoadLabel_.addMouseListener (&loadMouseListener_, false);   // right-click resets the probe
    statusTooltipLabel_.setJustificationType (juce::Justification::centredLeft);
    statusTooltipLabel_.setFont (juce::FontOptions (12.0f));
    statusTooltipLabel_.setColour (juce::Label::textColourId, theme.textSecondary);
    addAndMakeVisible (statusTooltipLabel_);

    // ---- Phase 4a: settings side panel (right side, always-on-top) ----
    // The SettingsPanel is owned + deleted by the SidePanel.
    // RIGHT-docked so the panel never covers the left-side Settings button
    // (which the user re-clicks to dismiss it). Was left-docked (true).
    settingsPanelHost_ = std::make_unique<juce::SidePanel> (TRANS ("Settings"), 300, false);
    settingsPanel_ = new SettingsPanel (processorRef_, themeManager_,
        [this] (double z) { setZoom (z); repaint(); },
        [] (bool b)         { ParamControl::setTooltipsEnabled (b); },
        [this] (bool b)     { processorRef_.setParameterSmoothing (b); },
        [] (int)            {},   // processor.setOversamplingFactor already applied in the panel
        [this] (const juce::String& code) {
            // Language changed: persist it, install the LocalisedStrings, then
            // re-translate every chrome string live.
            processorRef_.setUiLanguage (code);
            installLanguage (code);
            applyChromeTranslations();
        });
    settingsPanelHost_->setContent (settingsPanel_, true);
    // Keep the Settings button's toggle state in sync when the panel is
    // dismissed by other means (the dismiss glyph / clicking outside / ESC) —
    // onPanelShowHide fires after the slide animation on any show/hide.
    settingsPanelHost_->onPanelShowHide = [this] (bool isShown) {
        settingsButton_.setToggleState (isShown, juce::dontSendNotification);
    };
    addAndMakeVisible (*settingsPanelHost_);

    // Refresh the Patch page (~30 Hz) so it tracks the edited part.
    startTimerHz (30);

    // Re-apply the UI font family (system default sans-serif) to every cached
    // Label now that all widgets exist (juce::Label caches its font, so the
    // family from getLabelFont needs to be pushed here too). Combos, buttons,
    // tabs, popups and group titles follow via the L&F font getters.
    refreshFontsIn (this, lnf_);

    // Dense integrated layout: header(40) + page tabs(28) + content + status(22).
    // The CentralModBar spans the full content width (== editor width — no
    // horizontal chrome), so the MINIMUM width is its no-clipping preferredWidth()
    // plus a small safety margin: NO pill ever compresses. The DEFAULT size is
    // raised to at least that minimum so the bar is uncompressed at startup too.
    // Min height 600 keeps the 3 rows (top | bar | bottom) usable. (Headless tests
    // call setSize() below the min, which bypasses setResizeLimits.)
    // The CentralModBar scrolls internally (Viewport), so it never widens the
    // editor — the width floor can sit BELOW the old 1280pt so the editor FILLS
    // the screen at 100% zoom on tablets narrower than 1280pt (iPad Pro 11"
    // landscape is 1194pt): no manual zoom-out is needed. 1024 covers every
    // current iPad; min height 500 keeps the 3 rows usable. (Headless tests call
    // setSize() below the min, which bypasses setResizeLimits.)
    setSize (1280, 634);
    setResizable (true, true);
    setResizeLimits (1024, 500, 1800, 1100);

    // Apply persisted zoom (global scale; only if non-default to avoid an
    // unnecessary rescale at startup). iOS fullscreen always starts at 100%,
    // ignoring any persisted value.
#if JUCE_IOS
    setZoom (1.0);
#else
    if (processorRef_.getUiZoom() != 1.0)
        setZoom (processorRef_.getUiZoom());
#endif

#if JUCE_IOS
    // T14 (iPadOS audit): keep the display awake while an editor exists — a
    // patch tweak mid-performance must not lock the screen (audio keeps going
    // via UIBackgroundModes, but the UI would vanish mid-drag). Restored in the
    // destructor next to the zoom reset, mirroring that pattern. iOS-only seam
    // (same gate as the zoom default above): desktop screensaver policy is not
    // ours to change.
    juce::Desktop::getInstance().setScreenSaverEnabled (false);
#endif

    // Guarantee the full theme-derived colour re-apply runs on first build in
    // EVERY context (standalone, headless screen tool, editor tests).
    // changeListenerCallback is only the theme-CHANGE path: it is NOT invoked at
    // initial construction unless selectByName actually moves the selection
    // (default Carbon => no broadcast), so without this explicit call the
    // category knob arcs / ENV-LFO graph traces / mod-source tints could stay on
    // the L&F default gold until the first manual theme switch. The helper runs
    // the SAME sequence as changeListenerCallback after the whole tree is built
    // + parented, so every control resolves its category colour from the active
    // theme on the very first paint.
    applyAllColoursFromTheme();
}

ParvatiEditor::~ParvatiEditor()
{
    stopTimer();
    // Clear callbacks that capture `this` before the owning components are
    // destroyed during the reverse-order member teardown (defensive: the
    // components stop their own timers in their destructors, but nulling the
    // providers avoids any lingering reference).
    if (globalVoiceMeter_ != nullptr)
        globalVoiceMeter_->setStateProvider (nullptr);
    if (keyboardView_ != nullptr)
        keyboardView_->setNoteCallback (nullptr);
    // Detach from the theme broadcaster and release the L&F BEFORE the member
    // objects (themeManager_, lnf_) and the base Component are destroyed, so the
    // ChangeBroadcaster never calls back into a half-dead editor and no child
    // component references a destroyed L&F during teardown.
    themeManager_.removeChangeListener (this);
    setLookAndFeel (nullptr);

    // SF-2: reset the process-wide global scale factor so a non-default zoom
    // does not leak to other JUCE windows / plugin instances after this editor
    // closes. (Global scale is the only zoom path today; per-editor transform
    // zoom is a documented future enhancement — see the setZoom() comment.)
    if (zoom_ != 1.0)
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);

#if JUCE_IOS
    // T14: re-allow screen sleep (pairs with the constructor's disable — see
    // the matching seam there).
    juce::Desktop::getInstance().setScreenSaverEnabled (true);
#endif
}

void ParvatiEditor::dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& details)
{
    // Only a modulation-source drag (payload "parvatiModSrc:<enum>") triggers
    // the drop-zone affordance; any other (defensive — none exist today) leaves
    // the controls untouched. dragOperationStarted fires at the end of
    // startDragging() for the drag that THIS container owns.
    if (details.description.toString().startsWith ("parvatiModSrc"))
        ParamControl::setModDragActive (true);
}

void ParvatiEditor::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&)
{
    // Unconditional restore: dragOperationEnded fires from the DragImageComponent
    // destructor on BOTH a successful drop and a cancel, so the affordance state
    // always clears — no knob is left dimmed or ring-flagged.
    ParamControl::setModDragActive (false);
}

void ParvatiEditor::timerCallback()
{
    // ---- Tooltip bleed-through fix (~30 Hz) ----
    // ROOT CAUSE: the editor's TooltipWindow is parented to the editor (so it
    // inherits the ParvatiLookAndFeel and scales with the editor / DAW), but a
    // popup menu (a ComboBox drop-down OR a right-click context menu) lives in
    // its OWN top-level window with a different ComponentPeer. The base
    // juce::TooltipWindow::timerCallback only processes components that share
    // ITS peer, so while a popup is open it SKIPS its show/hide logic and
    // FREEZES the underlying control's tip on screen (the reported bleed).
    // A popup always enters the modal state (juce::PopupMenu::showMenuAsync ->
    // MenuWindow::enterModalState), so while any modal component is active we
    // hide the editor tooltip. (tooltipWindow_'s own timer then does nothing —
    // it skips its block while a different-peer popup is open — so the hide
    // sticks until the popup closes.) The context-menu items show their OWN
    // tooltips via the desktop TooltipWindow created in
    // ParamControl::showContextMenu; ComboBox drop-down items simply show
    // nothing. No-op when no popup is open.
    const bool popupOpen = juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    if (popupOpen && tooltipWindow_ != nullptr)
        tooltipWindow_->hideTip();

    // Mirror the UndoManager's undo/redo availability onto the top-bar buttons
    // (~30 Hz, same cadence as the Patch-page refresh below). Cheap O(1)
    // canUndo/canRedo checks; setEnabled() is a no-op when unchanged.
    undoButton_.setEnabled (processorRef_.getUndoManager().canUndo());
    redoButton_.setEnabled (processorRef_.getUndoManager().canRedo());

    // ---- Bottom status strip: active-voice count + hover tooltip (~30 Hz) ----
    {
        auto& engine = processorRef_.getEngine();
        int active = 0;
        for (int i = 0; i < engine.getNumVoices(); ++i)
            if (auto* av = engine.getAmbikaVoice (i); av != nullptr && av->isDisplayedActive())
                ++active;
        const int denom = processorRef_.getEngine()
            .getPart (processorRef_.getEngine().getCurrentPart()).voiceCount_.load();
        const juce::String countText = juce::String (active) + "/" + juce::String (denom);
        if (statusCountLabel_.getText() != countText)
            statusCountLabel_.setText (countText, juce::dontSendNotification);

        // ---- Realtime audio-load probe readout (overrun diagnosis) ----
        // Shows the current block's CPU% (render-time / real-time-budget) and,
        // if any block overran its budget, a "!N" overrun count. Colour flips to
        // amber above 70%, red above 90% or on any overrun — so a glance at the
        // strip tells you whether audible crackle coincides with the audio
        // thread being starved (e.g. by GUI render load on a shared core).
        {
            const double cur = processorRef_.getAudioLoadCurrent();
            const double peak = processorRef_.getAudioLoadPeak();
            const uint64_t over = processorRef_.getAudioOverrunCount();
            const int curPct = juce::jlimit (0, 999, juce::roundToInt (cur * 100.0));
            const int peakPct = juce::jlimit (0, 999, juce::roundToInt (peak * 100.0));
            juce::String loadText = "CPU " + juce::String (curPct) + "%";
            if (over > 0) loadText += " !" + juce::String ((int) over);   // overrun count
            if (statusLoadLabel_.getText() != loadText)
                statusLoadLabel_.setText (loadText, juce::dontSendNotification);
            // Colour by headroom (peak drives the colour; overruns force red).
            auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
            const ParvatiTheme* th = lnf ? lnf->getTheme() : nullptr;
            const juce::Colour ok     = th ? th->textSecondary : juce::Colour (0xff9a9aa8);
            const juce::Colour warn   = th ? th->accentPrimary  : juce::Colour (0xffe0b341);
            const juce::Colour danger = juce::Colour (0xffe0584a);
            const juce::Colour c = (over > 0 || peak >= 0.90) ? danger
                                  : peak >= 0.70                ? warn
                                                                : ok;
            statusLoadLabel_.setColour (juce::Label::textColourId, c);
            // Keep the tooltip current with the peak (so the hovered help shows
            // the worst-case seen, not just the live value).
            statusLoadLabel_.setTooltip ("Audio-thread realtime load: now " + juce::String (curPct)
                + "%, peak " + juce::String (peakPct) + "%" + (over > 0 ? (", " + juce::String ((int) over) + " overruns") : juce::String())
                + ". Near/over 100% = xruns/crackle. Right-click to reset the peak.");
        }

        // Tooltip bar: the help text of the control under the mouse (walks up
        // to the first ancestor carrying a tooltip). Empty when tooltips are
        // disabled in Settings or the mouse is over dead space. Suppressed while
        // a modal popup is open too: getComponentAt() would otherwise return the
        // editor control physically under the popup and leak its help text
        // (same bleed-through class of bug, in the status strip).
        juce::String tip;
        if (ParamControl::tooltipsEnabled() && ! popupOpen)
        {
            const auto rel = getMouseXYRelative();
            if (getLocalBounds().contains (rel))
                for (auto* c = getComponentAt (rel); c != nullptr; c = c->getParentComponent())
                {
                    const juce::String t = (dynamic_cast<juce::TooltipClient*> (c) != nullptr)
                        ? dynamic_cast<juce::TooltipClient*> (c)->getTooltip() : juce::String();
                    if (t.isNotEmpty()) { tip = t; break; }
                }
        }
        // Tap-to-assign transient status (e.g. "Mod Matrix full") takes priority
        // over the hover tooltip for a short time after requestAssign returns
        // false (full matrix). Drains back to the normal hover tip afterwards.
        if (const auto ts = ParamControl::tickTransientStatus(); ts.isNotEmpty())
            tip = ts;
        if (statusTooltipLabel_.getText() != tip)
            statusTooltipLabel_.setText (tip, juce::dontSendNotification);
    }

    // NOTE: the Patch page shows ALL 6 parts (it is not part-relative), so there
    // is nothing to re-sync on a part switch here. External state changes (a
    // .MUL load) are covered by the forced refresh in applyPatchFile.

    // ---- Keyboard latching: mirror sounding notes across all voices ----
    if (keyboardView_ == nullptr)
        return;

    const int curPart = processorRef_.getEngine().getCurrentPart();
    if (curPart != lastLatchPart_)
    {
        // Edited part changed: clear all latched notes to avoid stuck lamps.
        for (int n = 0; n < 128; ++n)
            keyboardView_->latchNoteOff (n);
        latchedNotes_.clear();
        lastLatchPart_ = curPart;
        return;
    }

    // Collect the set of currently-active notes across ALL voices.
    juce::Array<int> activeNotes;
    auto& engine = processorRef_.getEngine();
    for (int i = 0; i < engine.getNumVoices(); ++i)
    {
        // SF-1: read the lock-free atomic snapshot instead of the
        // non-atomic SynthesiserVoice::currentlyPlayingNote.
        auto* voice = engine.getAmbikaVoice (i);
        if (voice != nullptr && voice->isDisplayedActive())
        {
            const int note = voice->getDisplayedNote();
            if (note >= 0 && ! activeNotes.contains (note))
                activeNotes.add (note);
        }
    }

    // New notes: latch on.
    for (int note : activeNotes)
    {
        if (! latchedNotes_.contains (note))
        {
            keyboardView_->latchNoteOn (note, 1.0f);
            latchedNotes_.add (note);
        }
    }

    // Released notes: latch off.
    for (int i = latchedNotes_.size() - 1; i >= 0; --i)
    {
        if (! activeNotes.contains (latchedNotes_[i]))
        {
            keyboardView_->latchNoteOff (latchedNotes_[i]);
            latchedNotes_.remove (i);
        }
    }
}

void ParvatiEditor::reparentGeneratorTo (bool toFx)
{
    // The generator ParamPages are SHARED (editor-owned, registered into BOTH
    // workspaces). Only the VISIBLE workspace may host the active page: release
    // it from the outgoing workspace first (detach + forget), then reparent it
    // into the destination workspace. This guarantees a single parent — no
    // double-parent / dangling (a JUCE Component can only have one parent, and
    // addAndMakeVisible re-parents cleanly once the outgoing host has released
    // its stale activePage_ reference).
    if (toFx)
    {
        if (synthWorkspace_ != nullptr) synthWorkspace_->releaseActiveEditor();
        if (fxWorkspace_    != nullptr) fxWorkspace_->setActiveGenerator (activeGeneratorModSrc_);
    }
    else
    {
        if (fxWorkspace_    != nullptr) fxWorkspace_->releaseActiveEditor();
        if (synthWorkspace_ != nullptr) synthWorkspace_->setActiveGenerator (activeGeneratorModSrc_);
    }
}

void ParvatiEditor::setFxMode (bool fx)
{
    // Public entry for the screen-shot tool / tests: select SYNTH (false) or FX
    // (true) via the unified page selector.
    showTopPage (fx ? 1 : 0);
}

void ParvatiEditor::showTopPage (int idx)
{
    // idx: 0=Synth 1=FX 2=Patch — three PEER top-level pages. Patch is a FULL
    // page (pageSelector_ is hidden while it is active so it is the sole
    // content), not a floating overlay.
    currentTopPage_ = idx;

    // Reparent the shared generator only when landing on Synth/FX and it is
    // currently hosted by the OTHER workspace. Going to/from Patch leaves the
    // generator where it was (its workspace is just hidden, not torn down).
    if (idx == 0 && fxModeActive_)        { fxModeActive_ = false; reparentGeneratorTo (false); }
    else if (idx == 1 && ! fxModeActive_) { fxModeActive_ = true;  reparentGeneratorTo (true); }

    // Exclusive page visibility: exactly one of the synth/fx tabbed selector or
    // the Patch full-page child is shown.
    pageSelector_.setVisible (idx == 0 || idx == 1);
    if (idx == 0 || idx == 1)
        pageSelector_.setCurrentTabIndex (idx, false);
    if (patchPage_ != nullptr) patchPage_->setVisible (idx == 2);

    // The full-page child covers the content area; bring it to the front (above
    // the keyboard overlay). PatchPage::resized lays out its rows + hosts /
    // reflows the globalPage_, so no reflow is needed here.
    if (idx == 2 && patchPage_ != nullptr)
        patchPage_->toFront (true);

    // Sync all three header page buttons to the active page.
    synthModeButton_.setToggleState (idx == 0, juce::dontSendNotification);
    fxModeButton_.setToggleState    (idx == 1, juce::dontSendNotification);
    globalButton_.setToggleState    (idx == 2, juce::dontSendNotification);
}

void ParvatiEditor::applyAllColoursFromTheme()
{
    // Force every descendant to re-run lookAndFeelChanged(): ComboBox only
    // re-syncs its internal label's text colour (ComboBox::textColourId) in
    // colourChanged()/lookAndFeelChanged(), which a plain L&F colour change
    // does NOT trigger — so a combo themed under a dark theme would otherwise
    // keep near-white label text after switching to the light Paper theme.
    // This also re-applies the per-widget fonts (combo/button/tab/popup) and
    // (crucially) makes each ParamControl::lookAndFeelChanged() re-push its
    // category arc / mod tint once the editor's ParvatiLookAndFeel is attached.
    sendLookAndFeelChange();
    for (auto& page : generatedPages_)
        page->applyThemeColors();
    if (synthWorkspace_ != nullptr)
        synthWorkspace_->applyThemeColors();   // 3-row workspace: top pages + bar + active editor + matrix
    if (modMatrixView_ != nullptr)
        modMatrixView_->applyThemeColors();    // bottom-right ModMatrixView (direct child of the workspace)
    if (fxWorkspace_ != nullptr)
        fxWorkspace_->applyThemeColors();      // FX workspace: slot pages + bar + active editor + FxMatrixView
    if (fxMatrixView_ != nullptr)
        fxMatrixView_->applyThemeColors();     // bottom-right FxMatrixView (direct child of the FX workspace)
    if (patchPage_ != nullptr)
        patchPage_->applyThemeColors();
    // Re-resolve + re-push every control's category arc colour / mod-source tint
    // and the ENV/LFO graph trace from the active theme. Component-level
    // setColour overrides survive a theme switch but keep the OLD theme's value
    // otherwise, so they must be re-resolved (sliders, source combos, graph
    // traces). sendLookAndFeelChange() above also re-applies the arcs, but this
    // explicit pass guarantees them regardless of any L&F-resolution timing.
    ParamControl::reapplyCategoryColours();
    reapplyGraphCategoryColours();
    statusCountLabel_.setColour (juce::Label::textColourId,
                                 themeManager_.getCurrentTheme().accentPrimary);
    statusTooltipLabel_.setColour (juce::Label::textColourId,
                                   themeManager_.getCurrentTheme().textSecondary);
    // Phase 4a: refresh visualization components so they pick up the new colours.
    if (keyboardView_ != nullptr)
        keyboardView_->refresh();
    if (globalVoiceMeter_ != nullptr)
        globalVoiceMeter_->refresh();
    repaint();
}

void ParvatiEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A new theme was selected: install it on the L&F, then re-apply every
    // theme-derived colour across the whole tree (shared helper, also used at
    // ctor end for the first-paint guarantee).
    lnf_.setTheme (themeManager_.getCurrentTheme());
    applyAllColoursFromTheme();
}

void ParvatiEditor::reapplyGraphCategoryColours()
{
    // Re-resolve each graph preview's category token from the current theme and
    // re-push it (a snapshot Colour would otherwise freeze on the old theme).
    const auto& theme = themeManager_.getCurrentTheme();
    for (auto& binding : graphCategoryBindings_)
        binding.first (theme.*binding.second);
}

void ParvatiEditor::setZoom (double zoom)
{
    zoom_ = juce::jlimit (0.75, 2.0, zoom);
    juce::Desktop::getInstance().setGlobalScaleFactor (static_cast<float> (zoom_));
}

void ParvatiEditor::applyZoom (double zoom)
{
    // Shared by the Cmd/Ctrl +/-/0 shortcuts and the on-screen zoom buttons so
    // both use one clamping + persist + Settings-mirror path.
    setZoom (zoom);                               // clamps to [0.75, 2.0] + applies global scale
    processorRef_.setUiZoom (zoom_);              // persist the clamped value
    if (settingsPanel_ != nullptr)
        settingsPanel_->setZoomValue (zoom_);     // mirror into the slider (no re-fire)
}

std::vector<ParamPage*> ParvatiEditor::allGeneratedPages() const
{
    std::vector<ParamPage*> out;
    out.reserve (generatedPages_.size());
    for (const auto& p : generatedPages_)
        out.push_back (p.get());
    return out;
}

bool ParvatiEditor::keyPressed (const juce::KeyPress& key)
{
    // Only Cmd/Ctrl + +/-/0 are zoom shortcuts; everything else passes through
    // so typing in combos / text boxes is never swallowed.
    if (! (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()))
        return false;

    // Accept both '=' (un-shifted) and '+' for zoom-in across keyboard layouts.
    if (key.getKeyCode() == '+' || key.getKeyCode() == '=')
    {
        applyZoom (zoom_ + 0.1);
        return true;
    }
    if (key.getKeyCode() == '-')
    {
        applyZoom (zoom_ - 0.1);
        return true;
    }
    if (key.getKeyCode() == '0')
    {
        applyZoom (1.0);
        return true;
    }

    // Phase 4c: Undo / Redo. Cmd/Ctrl+Z = undo; Cmd/Ctrl+Shift+Z or
    // Cmd/Ctrl+Y = redo. These carry the Cmd/Ctrl modifier (already required to
    // reach here), so they never collide with KeyboardView's plain-key musical
    // typing — that view returns false for modifier-key combos, letting these
    // keypresses bubble up to the editor. The keyCode is the bare letter on
    // both shifted and un-shifted presses (JUCE tracks shift in the modifiers),
    // so check 'z'/'Z' both and decide undo-vs-redo from isShiftDown().
    const int code = key.getKeyCode();
    if (code == 'z' || code == 'Z')
    {
        if (key.getModifiers().isShiftDown())
            processorRef_.getUndoManager().redo();
        else
            processorRef_.getUndoManager().undo();
        return true;
    }
    if (code == 'y' || code == 'Y')
    {
        processorRef_.getUndoManager().redo();
        return true;
    }

    return false;
}

void ParvatiEditor::applyChromeTranslations()
{
    // Re-translate every editor-chrome string through the active LocalisedStrings
    // so a live language switch updates immediately. The top-level page-selector
    // labels (SYNTH/GLOBAL) and the chrome strings below are translated; the
    // CentralModBar pill/cluster labels are short fixed codes (E1/L1/ARP/...),
    // so they need no translation. With no mappings installed (English) TRANS()
    // is the identity, so this is a no-op for the default.
    patchCaption_.setText (TRANS ("Patch:"), juce::dontSendNotification);
    partCaption_.setText (TRANS ("Part:"), juce::dontSendNotification);
    loadButton_.setButtonText (TRANS ("Load"));
    saveButton_.setButtonText (TRANS ("Save"));
    undoButton_.setTooltip (TRANS ("Undo"));
    redoButton_.setTooltip (TRANS ("Redo"));
    zoomInButton_.setTooltip (TRANS ("Zoom in"));
    zoomOutButton_.setTooltip (TRANS ("Zoom out"));
    zoomResetButton_.setTooltip (TRANS ("Reset zoom"));
    settingsButton_.setTooltip (TRANS ("Settings"));
    globalButton_.setButtonText (TRANS ("Patch"));
    globalButton_.setTooltip (TRANS ("Patch / arrangement"));
    synthModeButton_.setButtonText (TRANS ("Synth"));
    fxModeButton_.setButtonText (TRANS ("FX"));

    pageSelector_.setTabName (0, TRANS ("SYNTH"));
    if (pageSelector_.getNumTabs() > 1)
        pageSelector_.setTabName (1, TRANS ("FX"));
    // The CentralModBar pill/cluster labels are language-neutral short codes
    // (E1/L1/ARP/ENV...), so there are no tab labels to re-apply on a language
    // switch (the old nested ENV/LFO/MOD-MATRIX tab strip is gone).

    for (auto& page : generatedPages_)
        page->refreshLanguage();

    if (patchPage_ != nullptr)
        patchPage_->refreshLanguage();
    if (settingsPanel_ != nullptr)
        settingsPanel_->refreshLanguage();
    // NOTE: the SidePanel's own title-bar text ("Settings") has no public setter,
    // so it updates on the next editor open (set via TRANS at construction) but
    // not live. The in-panel chrome (Language combo etc.) DOES update live.

    repaint();
}

void ParvatiEditor::loadLogoIcon()
{
    if (logoDrawable_ != nullptr)
        return;

    int svgBytes = 0;
    const char* const svgData = ParvatiLogo::getNamedResource ("parvati_logo_svg", svgBytes);
    if (svgData == nullptr || svgBytes <= 0)
        return;

    // parvati_logo.svg is now TRUE vector art (outlined <path>/<g>, no raster),
    // so parse it with JUCE's SVG renderer and cache the resulting Drawable.
    // No PNG/base64/<image> decode anywhere. (JUCE 9 exposes the string-based
    // parser createFromSVGString; the older createFromSVG(XmlElement) is gone.)
    logoDrawable_ = juce::Drawable::createFromSVGString (
        juce::String (svgData, (size_t) svgBytes));
}

void ParvatiEditor::paint (juce::Graphics& g)
{
    const auto& theme = themeManager_.getCurrentTheme();
    // The whole UI (header included) is one flat windowBackground — no tinted
    // band, no grey divider lines (borderless aesthetic).
    g.fillAll (theme.backgroundBase);

    // Header logo cluster: [brand icon] [gap] [white "Parvati" text], painted
    // into the reserved left logo block (the version label sits inline to its
    // right). The icon is a fixed brand asset drawn as-is (own colours); the
    // "Parvati" text uses the theme `text` token so it re-colours each paint().
    if (! logoArea_.isEmpty())
    {
        // Two-line brand: "Parvati" (bold) over the subtitle (10px, dim).
        // brand — "Parvati" (bold) over "by 805Labs \xc2\xb7 v<ver>" (10px, dim).
        auto block = logoArea_;
        g.setFont (lnf_.appFont (kLogoTextHeight, juce::Font::bold));
        g.setColour (theme.textPrimary);
        g.drawText (kLogoText, block.removeFromTop (juce::roundToInt (static_cast<float> (block.getHeight()) * 0.62f)),
                    juce::Justification::centredLeft, false);
        g.setFont (lnf_.appFont (10.0f, juce::Font::plain));
        g.setColour (theme.textSecondary);
        g.drawText ("by 805Labs \xc2\xb7 v" PARVATI_VERSION, block,
                    juce::Justification::centredLeft, false);

    }
}

void ParvatiEditor::resized()
{
    auto area = getLocalBounds();

    // Keep the UI out of the OS safe area (iOS: status bar / home indicator /
    // landscape camera stub; macOS: the MacBook notch if reported). Without the
    // TOP inset the header is laid out at y=0 directly under the iOS status bar,
    // where iOS swallows/defers the first touch — the reported double-tap /
    // non-recognized-tap issue on the top row. This is platform-COMMON: on
    // platforms with no reserved area (windowed macOS/Win/Linux) safeAreaInsets
    // is a 0-border, so this is a harmless no-op there.
    // safeAreaInsets are in DISPLAY (physical) points, but `area` is in the
    // editor's LOCAL logical coords. The global scale factor maps local->physical,
    // so divide each inset by it: at any zoom the rendered inset then lands on the
    // real safe-area edge (without this, zooming out shrank the inset and the UI
    // slid back under the status bar).
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const double z = juce::jmax (0.1, (double) juce::Desktop::getInstance().getGlobalScaleFactor());
        const auto& s = d->safeAreaInsets;
        area = area.withTrimmedTop    (juce::roundToInt (s.getTop()    / z))
                   .withTrimmedBottom (juce::roundToInt (s.getBottom() / z))
                   .withTrimmedLeft   (juce::roundToInt (s.getLeft()   / z))
                   .withTrimmedRight  (juce::roundToInt (s.getRight()  / z));
    }

    // ---- Bottom status strip = LOWEST band: [n/denom] + tooltip bar ----
    {
        auto strip = area.removeFromBottom (kVoiceStripH).reduced (6, 1);
        statusCountLabel_.setBounds (strip.removeFromLeft (48));
        statusLoadLabel_.setBounds (strip.removeFromLeft (96));
        statusTooltipLabel_.setBounds (strip);
    }

    // NOTE: the virtual keyboard is NO LONGER part of the layout flow. It floats
    // as an OVERLAY over the bottom of the content area (positioned at the end of
    // resized()), so the workspace + overlays keep the FULL content height and
    // toggling [KBD] never moves the controls.

    // ---- Header (36px row): [logo+version] (left) | Patch/Part menu (centre) | icons+[KBD] (right) ----
    auto header = area.removeFromTop (kHeaderH);
    // A kBarHeight-tall strip vertically centred in the 36px header holds every
    // header control (the logo block uses the same strip height).
    auto bar = header.withTrimmedTop ((kHeaderH - kBarHeight) / 2)
                     .withTrimmedBottom ((kHeaderH - kBarHeight) / 2)
                     .reduced (6, 0);

    // Right cluster (removeFromRight => first item ends up rightmost): system
    // icons, then the [KBD] toggle at the far right.
    // Right cluster = a coherent toolbar grouped [Load][Save] | [Undo][Redo] |
    // [Zoom +/0/-] | [Gear] | [KBD]. The icon/zoom buttons share a uniform 4px
    // gap; an 8px gap separates the history/zoom/view icons from the file group.
    // Save/Load are trimmed (100/80 -> 84/70) so the cluster stays compact and
    // never collides with the centred Patch/Part cluster at the default width.
    // Every icon is a 44x44 touch target with >=8pt gaps, and the three
    // zoom buttons (+/-/0) are folded into one "..." overflow popup so the grown
    // cluster still fits the 1280pt editor width. [KBD] is already 44pt wide.
    kbdToggleButton_.setBounds (bar.removeFromRight (44));     // [KBD] (already 44pt wide)
    bar.removeFromRight (8);
    modAssignButton_.setBounds (bar.removeFromRight (44));     // [MOD] tap-to-assign toggle
    bar.removeFromRight (8);
    settingsButton_.setBounds (bar.removeFromRight (44));      // gear
    bar.removeFromRight (8);
    zoomOverflowButton_.setBounds (bar.removeFromRight (44));  // "..." zoom overflow (popup)
    bar.removeFromRight (8);
    redoButton_.setBounds (bar.removeFromRight (44));          // redo
    bar.removeFromRight (8);
    undoButton_.setBounds (bar.removeFromRight (44));          // undo
    bar.removeFromRight (6);   // separates the history/view icons from the file group
    saveButton_.setBounds (bar.removeFromRight (64));          // Save (carries the format popup menu)
    bar.removeFromRight (6);
    loadButton_.setBounds (bar.removeFromRight (54));          // Load


    // Left: brand icon + white "Parvati" wordmark (painted) + version label
    // inline to its right. Layout: [~14px edge] "Parvati" [6px] [icon] [6px] [version]
    // (equal 6px gaps; text width measured with the SAME font paint() uses).
    {
        bar.removeFromLeft (8);   // extra left edge whitespace (6 from bar.reduced + 8 = ~14px)
        const juce::Font textFont = lnf_.appFont (kLogoTextHeight, juce::Font::bold);
        juce::GlyphArrangement ga;
        ga.addLineOfText (textFont, kLogoText, 0.0f, 0.0f);
        const int textW = juce::roundToInt (ga.getBoundingBox (0, ga.getNumGlyphs(), true).getWidth());
        // logoArea_ shrinks to the wordmark width (the subtitle
        // "by 805Labs \xc2\xb7 v<ver>" rides beneath it in paint()). The inline
        // version label is replaced by the painted subtitle, so it is hidden
        // here (it stays wired on desktop).
        // +16px slack so the wordmark breathes and the (now left-aligned)
        // preset dropdown can sit close to it without jamming against the text.
        logoArea_ = bar.removeFromLeft (textW + 16);
        versionLabel_.setVisible (false);

    }

    // Patch/Part menu cluster. The "Patch:" caption is removed and the preset
    // dropdown is LEFT-aligned right after the logo block (logoArea_ carries
    // breathing-room slack) so it sits close to the wordmark; the preset browser
    // is narrowed. The toolbar hugs the right edge, so the menus pack from the
    // left of the remaining bar. Layout: [preset][gap][Patch][Part n][Synth][FX]
    {
        patchCaption_.setVisible (false);   // "Patch:" label removed
        partCaption_.setVisible (false);    // "Part:" label removed (dropdown only)
        const int presetW   = (presetBrowser_ != nullptr) ? 168 : 0;   // narrower patch dropdown
        const int partComboW = 88;
        const int gapW = 6;
        const int globalW = 64;
        const int modeW = 50;   // [Synth]/[FX] toggle buttons (radio group)

        auto cluster = bar;   // left-aligned: follows the logo block directly
        if (presetBrowser_ != nullptr)
            presetBrowser_->setBounds (cluster.removeFromLeft (presetW));
        cluster.removeFromLeft (gapW);   // small gap between the Patch dropdown and the Patch button
        globalButton_.setBounds (cluster.removeFromLeft (globalW));   // Patch page overlay toggle (between Patch dropdown and Part)
        partCombo_.setBounds (cluster.removeFromLeft (partComboW));
        // Synth/FX mode toggle (radio group) after Part.
        synthModeButton_.setBounds (cluster.removeFromLeft (modeW));
        fxModeButton_.setBounds (cluster.removeFromLeft (modeW));
    }

    // ---- Page selector [SYNTH] + integrated content (no void) ----
    // pageSelector_ (a single-tab TabbedComponent, bar hidden via depth 0) fills
    // the remaining area and sizes SYNTH (SynthWorkspace) into all of it — butted
    // directly under the header. SynthWorkspace lays out its 3 columns + nested
    // tab groups in its own resized().
    pageSelector_.setBounds (area);

    // The Patch page overlay covers exactly the content area when toggled on.
    // PatchPage::resized lays out its rows + hosts / reflows the globalPage_, so
    // only its bounds are set here (no direct globalPage_ setBounds / reflow).
    if (patchPage_ != nullptr)
        patchPage_->setBounds (area);

    // ---- Keyboard OVERLAY: floats over the bottom of the content area ----
    // `area` is the full content rect (status strip + header already trimmed);
    // the workspace + overlays above were given ALL of it, so they keep their
    // full height. The keyboard (incl. the pitch/mod wheels to its left) is now
    // positioned absolutely over the bottom kKeyboardH pixels of that rect and
    // shown/hidden purely via setVisible() — toggling [KBD] never resizes the
    // content above. Z-order: keyboardView_ is added AFTER pageSelector_ so it
    // already paints above the workspace; the Patch overlay calls
    // toFront() when shown so they cover the keyboard, and the Settings side
    // panel (added last) stays above it too. No toFront() is called here so a
    // resize while a modal is open never lifts the keyboard above it.
    if (keyboardView_ != nullptr)
    {
        constexpr int kWheelsW = 76;
        const bool kbdVisible = kbdToggleButton_.getToggleState();
        auto bottomStrip = area.withHeight (juce::jmin (kKeyboardH, area.getHeight()))
                               .withY (area.getBottom() - juce::jmin (kKeyboardH, area.getHeight()));
        if (wheels_ != nullptr)
            wheels_->setBounds (bottomStrip.removeFromLeft (kWheelsW));
        keyboardView_->setBounds (bottomStrip);
        keyboardView_->setVisible (kbdVisible);
        if (wheels_ != nullptr)
            wheels_->setVisible (kbdVisible);
    }
}

//==========================================================================
#if JUCE_IOS
// iOS: make a successful user-area save visible in the Files app. The
// Standalone plist advertises UIFileSharingEnabled, which browses
// <sandbox>/Documents — but the USER patch area lives in the shared App-Group
// container (Source/ui/SharedContainer.h), which Files cannot browse at all.
// COPY, not move: the group container stays the single source of truth for
// the PresetBrowser and the AUv3 extension (Standalone + AUv3 keep one tree);
// the Documents copy is a plain export. Only saves that land INSIDE the USER
// area are mirrored — a picker navigated elsewhere is an explicit export
// already — and the USER/ sub-path is preserved (Documents/Parvati/USER/...)
// so bank folders survive. A failed copy is non-fatal (the save itself
// already succeeded) and the next successful save re-mirrors; stale mirrors
// are intentionally never deleted (silently removing user-visible files
// would be surprising, and the group tree remains authoritative).
// Note: when this editor runs inside the AUv3 extension in a host, Documents
// is the EXTENSION's sandbox, which Files does not browse — the copy is
// harmless there; the Standalone app's own saves are the visible ones.
static void mirrorUserSaveToDocumentsIOS (const juce::File& saved)
{
    const auto userDir = ParvatiAudioProcessor::getUserPatchDir();
    if (! saved.isAChildOf (userDir))
        return;
    const auto dest = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile ("Parvati/USER")
                          .getChildFile (saved.getRelativePathFrom (userDir));
    dest.getParentDirectory().createDirectory();
    saved.copyFileTo (dest);   // overwrite-in-place; a torn copy self-heals next save
}
#endif

void ParvatiEditor::openLoadDialog()
{
    // The load picker starts nowhere in particular (empty start file): on iOS
    // the document picker opens at its browse root, from which the mirrored
    // Documents/Parvati/USER saves are reachable (On My iPad > Parvati); on
    // desktop the browser starts at the OS default. The PresetBrowser keeps
    // reading the SHARED tree either way (one tree for Standalone + AUv3).
    fileChooser_ = std::make_unique<juce::FileChooser> (TRANS ("Load Patch / Multi (.PRO / .MUL / .parvati)"),
                                                       juce::File(), "*.PRO;*.MUL;*.parvati");
    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
            applyPatchFile (fc.getResult());
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::openSaveDialog()
{
    // Save the CURRENT part as an Ambika .PRO (byte-faithful; shareable with
    // Ambika hardware). For a full-fidelity Parvati patch (incl. vca_curve /
    // filter_card), use "Save Parvati". Defaults to the user's preset area.
    auto defaultName = processorRef_.getLoadedProgramName();
    if (defaultName.isEmpty())
        defaultName = "Parvati";
    const juce::File defaultDir = processorRef_.getUserPatchDir();
    defaultDir.createDirectory();   // ensure USER/ exists
    const juce::File defaultFile (defaultDir.getChildFile (defaultName + ".PRO"));
    fileChooser_ = std::make_unique<juce::FileChooser> (TRANS ("Save Ambika Patch (.PRO)"),
                                                       defaultFile, "*.PRO");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
        {
            const auto f = fc.getResult().withFileExtension (".PRO");
            if (processorRef_.saveProgramFile (f))
            {
#if JUCE_IOS
                mirrorUserSaveToDocumentsIOS (f);   // Files-app export (see helper)
#endif
                if (presetBrowser_ != nullptr)
                    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
            }
        }
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::openSaveParvatiDialog()
{
    // Save a full-fidelity Parvati-native patch (.parvati, YAML) that carries
    // EVERYTHING — including vca_curve / filter_card / arp, which the Ambika
    // .PRO byte format drops. Defaults to the user's preset area.
    auto defaultName = processorRef_.getLoadedProgramName();
    if (defaultName.isEmpty())
        defaultName = "Parvati";
    const juce::File defaultDir = processorRef_.getUserPatchDir();
    defaultDir.createDirectory();
    const juce::File defaultFile (defaultDir.getChildFile (defaultName + ".parvati"));
    fileChooser_ = std::make_unique<juce::FileChooser> (TRANS ("Save Parvati Patch (.parvati)"),
                                                       defaultFile, "*.parvati");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
        {
            const auto f = fc.getResult().withFileExtension (".parvati");
            if (processorRef_.saveParvatiPatchFile (f))
            {
#if JUCE_IOS
                mirrorUserSaveToDocumentsIOS (f);   // Files-app export (see helper)
#endif
                if (presetBrowser_ != nullptr)
                    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
            }
        }
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::applyPatchFile (const juce::File& f)
{
    // .MUL -> multitimbral multi (all 6 Parts); .PRO -> single program;
    // .parvati -> Parvati-native YAML (patch or multi, sniffed by format:).
    bool ok = false;
    bool isMulti = false;

    if (f.hasFileExtension (".parvati"))
    {
        juce::String text;
        if (juce::FileInputStream in (f); in.openedOk())
            text = in.readEntireStreamAsString();
        const juce::String fmt = parvati::preset::detectParvatiFormat (text);
        if (fmt == parvati::preset::kFormatMulti)
        {
            isMulti = true;
            ok = processorRef_.loadParvatiMultiFile (f);
        }
        else
        {
            ok = processorRef_.loadParvatiPatchFile (f);
        }
    }
    else
    {
        isMulti = f.hasFileExtension (".mul");
        ok = isMulti ? processorRef_.loadMultiFile (f)
                     : processorRef_.loadProgramFile (f);
    }

    if (ok)
    {
        if (presetBrowser_ != nullptr)
            presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
        // A multi rewrites every part's channel / key zone / voice allocation /
        // polyphony, so force the Patch page to re-read (and re-infer the
        // arrangement) even though the edited part is unchanged.
        if (isMulti && patchPage_ != nullptr)
            patchPage_->refresh();
    }
}

bool ParvatiEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return std::any_of (files.begin(), files.end(), [] (const juce::String& fn) {
        return fn.endsWithIgnoreCase (".pro") || fn.endsWithIgnoreCase (".mul")
               || fn.endsWithIgnoreCase (".parvati");
    });
}

void ParvatiEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& fn : files)
    {
        juce::File f (fn);
        if (f.hasFileExtension (".pro") || f.hasFileExtension (".mul") || f.hasFileExtension (".parvati"))
        {
            applyPatchFile (f);
            break;
        }
    }
}
