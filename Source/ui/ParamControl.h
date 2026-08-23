// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParamControl — one descriptor-driven control cell (a rotary Slider or a
// ComboBox plus a label) bound to one APVTS parameter. Extracted unchanged
// from PluginEditor.h so the ui/ layer no longer includes the editor header
// for this type. The paramID-to-GUI-section mapping (Section, sectionForId,
// categoryColourForSection) is shared with the editor page generation, so it
// lives here and the editor includes this header.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

#include "ParameterLayout.h"   // PatchParamDescriptor
#include "PluginProcessor.h"   // ParvatiAudioProcessor (complete type: subclass headers rely on it)
#include "ModDestMap.h"        // parvati::ModDestMap::ModDst (modDest_ member)

struct ParvatiTheme;

// ---- Map a parameter ID to one of the GUI sections --------------------------
// (Derived from the well-defined paramID prefixes in ParameterLayout.cpp, so the
//  checked APVTS byte-bridge stays untouched.)
enum class Section { Oscillators, Mixer, Filter, Envelopes, Lfos, ModMatrix, Modifiers, Arp, Sequencer, Global, Fx, FxMatrix };

Section sectionForId (const juce::String& id);

// Map a functional Section to its theme category-token colour. Oscillators /
// Mixer / Filter / ModMatrix / Modifiers / Global share the neutral
// "audio" brand accent; Envelopes/LFOs/Sequencer/Arp get their own hue. This is the
// ONLY place a Section resolves to a category token, so every arc / graph / tint
// shares one consistent mapping and a theme switch re-resolves automatically.
juce::Colour categoryColourForSection (const ParvatiTheme& theme, Section s);

// Popup host protocol: the ancestor that owns the editor-wide TooltipWindow
// and the host-context plumbing. ParamControl::showContextMenu uses both when
// it opens a context menu: it hides the tooltip window for the first popup
// frame and merges the host's parameter menu below Reset/Randomize.
// ParvatiEditor implements this, so the control never depends on the editor
// type itself.
class ParamControlPopupHost
{
public:
    virtual ~ParamControlPopupHost() = default;
    virtual void hideHostedTooltip() = 0;
    virtual juce::AudioProcessorEditorHostContext* popupHostContext() const = 0;
};

//==============================================================================
// One control cell: a rotary Slider (numeric params) or a ComboBox (choice
// params), plus a label, all bound to one APVTS parameter. Colours are taken
// from the editor-wide ParvatiLookAndFeel (inherited via the component tree),
// and the cell exposes its parameter's help text as a tooltip.
class ParamControl : public juce::Component,
                     public juce::TooltipClient,
                     public juce::AudioProcessorValueTreeState::Listener,
                     public juce::DragAndDropTarget,
                     public juce::AsyncUpdater   // F-ui-1: audio-thread param writes defer GUI refresh here
                   , private juce::Timer   // long-press -> context menu (armed on touch only)
{
public:
    ParamControl (ParvatiAudioProcessor& processor, const PatchParamDescriptor& descriptor);

    void resized() override;

    ~ParamControl() override;

    // Fires when the component inherits/changes its LookAndFeel (initial
    // reparent into the editor tree, and on a theme switch via
    // sendLookAndFeelChange). At construction the ParvatiLookAndFeel is not yet
    // attached, so the category arc / mod tint are applied here once the theme
    // is reachable. Idempotent.
    void lookAndFeelChanged() override;
    // Reparenting into the editor tree: getLookAndFeel() then walks up to the
    // editor's ParvatiLookAndFeel, so this is where the initial category arc /
    // mod tint are first applied (lookAndFeelChanged only fires on an explicit
    // setLookAndFeel, not on inheritance via reparent).
    void parentHierarchyChanged() override;

    // juce::TooltipClient — per-parameter help text (ParamHelp.h). Queried only
    // for the bare ~2px cell border; the interactive children carry their own
    // setTooltip() (see applyTooltipState) because JUCE's TooltipWindow only
    // queries the leaf component under the cursor.
    juce::String getTooltip() override;

    // Suppress/enable tooltips globally (Phase 4a settings panel). Flips the
    // flag and re-applies the enabled/disabled tooltip text to every live
    // ParamControl's children via the instance registry. getTooltip() also
    // honours the flag for the bare-cell hover.
    static void   setTooltipsEnabled (bool enabled);
    static bool   tooltipsEnabled() noexcept { return tooltipsEnabled_; }  // for the status-bar tooltip gate

    // Re-apply the per-control category arc colour (sliders) and mod-source tint
    // (source combos) to EVERY live ParamControl, resolving the current theme.
    // Called on a theme switch: component-level setColour overrides survive a
    // theme change, but the token VALUE differs per theme, so they must be
    // re-resolved + re-pushed (the L&F default alone would not recolour them).
    static void   reapplyCategoryColours();

    // While a modulation source is being dragged onto a destination knob, dim
    // every control that is NOT a valid drop target (alpha 0.3) and light up
    // every valid target with a drop-zone ring (parvatiModDrag). Toggled from
    // ParvatiEditor's dragOperationStarted/Ended; iterates the live registry
    // (mirrors setTooltipsEnabled / reapplyCategoryColours). Restored (full
    // alpha, ring cleared) the instant the drag ends.
    static void   setModDragActive (bool active);
    // ---- Tap-to-assign modulation ----
    // iPad has no drag-and-drop, so modulation routing is reached by toggling
    // [MOD] ON, tapping a mod source, then tapping a destination knob. The dest
    // tap calls the SAME assign seam itemDropped uses
    // (ModMatrixHighlight::requestAssign(source, modDest_)) — no drag path on
    // touch. ON reuses the existing drop-zone affordance (setModDragActive) so
    // destination knobs show the ring + non-targets dim. The selected source is
    // carried in a static int (mirrors the static modDragActive_ pattern).
    static void setTapAssignActive (bool active);   // [MOD] toggle entry (sets flag + affordance)
    static bool tapAssignActive() noexcept { return tapAssignActive_; }
    static void setTapSelectedSource (int sourceEnum) noexcept;   // .cpp: sets + posts a selected-source status (touch has no hover)
    static int  tapSelectedSource() noexcept { return tapSelectedSource_; }
    // Transient status shown in the editor's status strip (e.g. "Mod Matrix
    // full" when requestAssign finds no free slot). postTransientStatus arms a
    // short frame budget; tickTransientStatus (drained ~30 Hz by the editor
    // timer) returns the text while the budget lasts, or empty once expired.
    static void        postTransientStatus (const juce::String& text, int frames);
    static juce::String tickTransientStatus();

    // Right-click (popup) on this cell — or on its child Slider/ComboBox, which
    // registers `this` as a MouseListener (Component is already a MouseListener,
    // so no extra base is needed) — shows a context menu (Reset to default /
    // Randomize). Non-popup clicks fall through to normal interaction.
    void mouseDown (const juce::MouseEvent& e) override;
    // Touch long-press -> context menu. iPad has no right-click, so Reset /
    // Randomize (the desktop right-click menu) is reached by holding a knob for
    // ~450ms. Dragging past a small threshold or releasing cancels the pending
    // menu; the timer is a ParamControl member (private juce::Timer base).
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    // Hover highlight: mousing over a mod-destination knob publishes its dest on
    // the editor-scoped ModMatrixHighlight bus so every matching matrix row
    // emphasises itself (and this knob's own ring glows — see applyModHighlight).
    // Also routed here from the child slider/label because this cell is registered
    // as their MouseListener.
    void mouseEnter (const juce::MouseEvent& e) override;
    void mouseExit  (const juce::MouseEvent& e) override;
    // Double-click on a mod-destination knob whose modulation ring is active
    // (aggregate depth != 0) selects the first ACTIVE slot targeting this dest
    // on the bus, so the Mod Matrix scrolls to + emphasises that row.
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    // ---- Drag-and-drop assignment (drag a mod source onto a dest knob) ----
    // An internal "parvatiModSrc:<enum>" drag is accepted by EVERY ParamControl
    // (so drag hover/exit/drop fire on non-targets too, not just destination
    // knobs). Over a destination knob the modulation ring glows (STEP-3
    // highlight bus) and the drop consumes the next free slot for
    // (source -> this knob's dest). Over a NON-destination control a small
    // padlock is shown (setDropLocked) to signal "cannot drop here", and the
    // drop is a no-op.
    bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails) override;
    void itemDragEnter (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails) override;
    void itemDragExit  (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails) override;
    void itemDropped   (const juce::DragAndDropTarget::SourceDetails& dragSourceDetails) override;
    // Show/clear the "locked" padlock on a non-target control while a mod-source
    // drag is hovered over it. Locked => full alpha + the padlock flag the L&F
    // renders; cleared => restore the drag dim/alpha state.
    void setDropLocked (bool locked);

    // ---- Sequencer step-grid introspection (UI + the editor_test) ----
    // paramID of the bound APVTS parameter (e.g. "seq1_step7", "seq_length_2").
    const juce::String& getParamID() const noexcept { return paramIDStr_; }

    // Override the displayed knob label (used by FxSlotCard to show the active
    // algorithm's semantic name, e.g. "Time"/"Feedback", instead of the static
    // descriptor label "FX1 Param 1"). An EMPTY string reverts to the
    // descriptor-derived label (displayLabelFor). Stored even when the control
    // has no visible label so a later re-show honours it.
    void setDisplayLabel (const juce::String& label);
    // Display the knob value as 0..100% (from stored 0..127). FX slot knobs use
    // this for a friendlier readout. Display-only; stored value unchanged.
    void setDisplayValuePercent (bool percent);
    // Display the knob value via a custom text formatter (e.g. note names,
    // +/-semitones, Hz, On/Off). Used by FX slot params for meaningful-unit
    // readouts. Display-only; stored value unchanged. The knob is drag-only
    // (NoTextBox), so no valueFromTextFunction is installed.
    void setDisplayValueText (std::function<juce::String (double)> toText);
    // True for the Seq1/2/3 length controls (marked "Length").
    bool isLengthControl() const noexcept { return paramIDStr_.startsWith ("seq_length_"); }
    // 0-based step index for a seq*_step* / seqnote_* control, else -1.
    int  stepIndex() const noexcept { return parseStepIndex (paramIDStr_); }
    // Whether the step's slider is now interactive (false => dimmed: step
    // index >= its sequence length). True for non-step controls.
    bool isStepEnabled() const noexcept { return slider_ ? slider_->isEnabled() : true; }
    // Whether this control shows a visible (Length) label — used by the test.
    // Reflects the ACTUAL label component so the editor_test gets an independent
    // signal that the Length label renders (not just the paramID prefix).
    bool isLengthLabelVisible() const noexcept { return label_ != nullptr && label_->isVisible(); }

private:
    void showContextMenu();
    // juce::Timer: fires ~450ms into an unmoving touch to ARM the long-press
    // menu. The menu itself opens on finger release (mouseUp) so a modal popup
    // never strands a mid-drag Slider. Desktop never starts the timer.
    void timerCallback() override;
    void resetToDefault();
    void randomize();

    // Push the current help text (or empty, when tooltips are disabled) onto
    // every interactive child (label / slider / combo). Called at construction
    // and whenever the global toggle flips.
    void applyTooltipState();

    // ---- Category colour-coding ----
    // Resolve this control's functional Section and push its theme category token
    // onto the slider's fill arc (rotarySliderFillColourId). No-op for combos /
    // when there is no theme yet. The numeric value readout stays neutral
    // (textBoxTextColourId is never touched).
    void applyCategoryArcColour();
    // Resolve a mod SOURCE's functional CATEGORY colour from its (human) name:
    // Env -> catEnv, LFO / "Voice LFO" -> catLfo, Seq -> catSeq, Arp -> catArp;
    // every other source (Op/Const/Velocity/etc) resolves to the neutral `accent`.
    // Shared by the mod-source combo tint AND the per-source modulation ring so
    // both use one consistent name->colour mapping. Pure / null-safe.
    static juce::Colour categoryColourForSourceName (const juce::String& name,
                                                     const ParvatiTheme& theme);
    // Tint a mod-source combo's background (modN_source / modifN_in1|in2) to 15%
    // alpha of the SELECTED source's category colour (Env=cyan, LFO=magenta,
    // Seq=green, Arp=purple; Op/Const/Velocity/etc => neutral / no tint).
    void applyModSourceTint();
    // Whether this combo takes a modulation source (and so is eligible for the
    // category tint). Detected from the paramID at construction.
    bool isModSourceCombo() const noexcept { return isModSourceCombo_; }

    // ---- Modulation ring (per-source concentric arcs) ----
    // Recompute the ACTIVE mod slots routed to this knob's ModulationDestination
    // (parvati::ModDestMap::slotsForDest) and push ONE concentric arc PER active
    // source onto the slider's getProperties(): "parvatiModN" = count, and for
    // each i: "parvatiModCol"+i = the source's CATEGORY colour (via
    // categoryColourForSourceName), "parvatiModAmt"+i = the signed amount
    // (-63..63). Each arc is later anchored at the knob's CURRENT value angle by
    // the LookAndFeel (NOT the centre). Capped at 6 arcs. A no-op for
    // non-destination knobs. Re-entrant-safe (refreshingModRing_).
    void refreshModRing();
    // Push the highlight flag onto the slider's getProperties() ("parvatiModHi")
    // so the LookAndFeel renders the modulation ring brighter/thicker when this
    // knob is the hovered/selected modulation target (@p modDst == modDest_),
    // and clears it otherwise. A no-op for non-destination knobs.
    void applyModHighlight (int modDst);
    // Re-style this cell for the active drag affordance: dim non-targets
    // (alpha 0.3) and push the drop-zone flag onto a destination knob's slider
    // so the LookAndFeel renders the drop-zone ring. Idempotent / null-safe.
    void applyModDragAffordance();

    // Pixel width of the widest choice string (or the current text) measured in
    // the active L&F combo font — drives the fit-to-text dropdown width.
    int maxChoiceTextWidth() const;

protected:
    // ---- Sequencer step dimming + subclass access (NoteStepControl /
    // SeqLengthStepper) ----
    // APVTS::Listener callback: the sibling seq_length_* param changed, so
    // re-evaluate whether to enable/dim this step's slider.
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    // Re-enable/dim this step based on its sibling sequence length (steps at
    // index >= length are disabled => the LookAndFeel omits the fill arc).
    void refreshStepEnabled();
    // F-ui-1 (bug hunt 2026-08-18): APVTS listeners fire synchronously on the
    // WRITING thread, and two real paths write from the audio/render thread
    // (the NRPN/CC map inside processBlock, and host automation —
    // setValueNotifyingHost from the host's process call). parameterChanged
    // therefore DEFERS here when off the message thread (coalesced async
    // refresh from CURRENT state — the FxSlotCard pattern). Idempotent full
    // refresh; safe to run redundantly.
    void handleAsyncUpdate() override;
    // Map a step paramID (seq1_step* / seq2_step* / seqnote_step*|vel*) to its
    // sibling length param (seq_length_1/2/3); empty for non-steps.
    static juce::String siblingLengthParamFor (const juce::String& stepID);
    // Parse the trailing integer of a step paramID ("seq1_step7" -> 7); -1 if not
    // a step.
    static int parseStepIndex (const juce::String& id);

    const PatchParamDescriptor& desc_;
    ParvatiAudioProcessor& processor_;   // APVTS access for reset/randomize
    // NoteStepControl re-ranges slider_ after tearing down the byte-range
    // sliderAttachment_; SeqLengthStepper hides slider_ and overlays -/number/+.
    std::unique_ptr<juce::Slider>    slider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sliderAttachment_;
private:
    std::unique_ptr<juce::ComboBox>  comboBox_;
    std::unique_ptr<juce::Label>     label_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment_;

    // Desktop TooltipWindow active ONLY while this control's right-click context
    // menu is open: it shows the custom TooltipClient menu-item tooltips above
    // the popup (the editor's parented TooltipWindow cannot render above the
    // popup and is suppressed while a popup is open). Created in
    // showContextMenu, destroyed in the menu's async close callback.
    std::unique_ptr<juce::TooltipWindow> popupTooltipWindow_;

    juce::String helpText_;         // cached getParamHelp(paramID); set in ctor
    static bool tooltipsEnabled_;   // toggled from the Settings panel
    static bool modDragActive_;     // true while a parvatiModSrc drag is active
    static bool tapAssignActive_;       // [MOD] toggle ON -> reuse the drop-zone affordance
    static int  tapSelectedSource_;     // MOD_SRC_* tapped on a source; -1 = none yet
    static juce::String transientStatusText_;   // transient status-strip text (e.g. "Mod Matrix full")
    static int  transientStatusFrames_;         // frame budget for the transient status

    // Touch slop: the px of finger drift a "clean tap" tolerates. SHARED by the
    // tap-assign gate (mouseUp) and the long-press cancel (mouseDrag) so any
    // movement that fails clean-tap also cancels a pending/armed long-press —
    // a 6-8px drift must never BOTH arm the context menu AND fail the tap.
    // Matches the 5px one-drag-per-press debounce in the DnD drag sources.
    static constexpr int kTouchSlop = 5;

    juce::String paramIDStr_;        // cached juce::String (desc_.paramID)
    juce::Point<int> longPressStart_;     // screen pos where the touch began (matches MouseEvent::getScreenPosition)
    bool longPressArmed_ = false;         // set in timerCallback; menu opens on mouseUp (see .cpp)
    juce::String displayLabelOverride_;   // empty => use displayLabelFor (desc_.paramID, desc_.label)
    juce::String lengthParamID_;     // sibling length param; empty for non-steps

    // Mod-source combo tint state: a source/modifyer-input combo listens to its
    // OWN value and re-tints when the selected source changes. refreshingModTint_
    // guards against any re-entrant setColour path.
    bool isModSourceCombo_ = false;
    bool refreshingModTint_ = false;

    // Modulation-ring state: a knob whose paramID maps to a MOD_DST listens to
    // all 42 mod{1..14}_source/_dest/_amount params so any matrix edit refreshes
    // the per-source concentric rings (a source change recolours an arc, a dest
    // change adds/removes an arc, an amount change resizes one). modDest_ is -1
    // for non-destination controls. refreshingModRing_ guards against re-entrant
    // repaints (mirrors the refreshingModTint_ pattern).
    parvati::ModDestMap::ModDst modDest_ = -1;
    bool isModDestKnob_ = false;
    bool refreshingModRing_ = false;
    // ModMatrixHighlight bus subscription id for the dest-highlight observer
    // (so the knob's ring follows a hover on a matching matrix row). -1 = none.
    int modHighlightSub_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamControl)
};
