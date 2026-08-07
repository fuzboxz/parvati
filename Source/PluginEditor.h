// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiEditor — the full Ambika GUI. An integrated (Serum-style dense)
// editor whose controls are generated entirely from the PatchParamDescriptor
// table (ParameterLayout.h), so the GUI and the APVTS byte-bridge can never
// drift apart. Every one of
// the 104 patch/part parameters gets a control (rotary Slider or ComboBox)
// plus an APVTS attachment.
//
// Colours come from the ParvatiTheme via a single ParvatiLookAndFeel set on the
// editor and inherited by the whole component tree — no per-control palette.
// Phase 2a of docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ui/KeyboardView.h"
#include "ui/ModDestMap.h"
#include "ui/ModMatrixView.h"
#include "ui/WheelsComponent.h"
#include "ui/ParvatiLookAndFeel.h"

class ParvatiAudioProcessor;

// Re-apply every Label font in the component tree via the active L&F's
// appFont() (the system default sans-serif), preserving each label's
// height/style. juce::Label caches its font, so it must be re-pushed when the
// UI is (re)built.
void refreshFontsIn (juce::Component* root, const ParvatiLookAndFeel& lnf);
#include "ui/SettingsPanel.h"
#include "ui/ThemeManager.h"
#include "ui/VoiceMeter.h"
#include "ui/IconButton.h"
#include "ui/PresetBrowser.h"

class MultiPage;
class SynthWorkspace;
class EnvelopeDisplay;

//==============================================================================
// One control cell: a rotary Slider (numeric params) or a ComboBox (choice
// params), plus a label, all bound to one APVTS parameter. Colours are taken
// from the editor-wide ParvatiLookAndFeel (inherited via the component tree),
// and the cell exposes its parameter's help text as a tooltip.
class ParamControl : public juce::Component,
                     public juce::TooltipClient,
                     public juce::AudioProcessorValueTreeState::Listener,
                     public juce::DragAndDropTarget
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

    // Right-click (popup) on this cell — or on its child Slider/ComboBox, which
    // registers `this` as a MouseListener (Component is already a MouseListener,
    // so no extra base is needed) — shows a context menu (Reset to default /
    // Randomize). Non-popup clicks fall through to normal interaction.
    void mouseDown (const juce::MouseEvent& e) override;

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
    // padlock is shown (setDropLocked) to signal "can't drop here", and the
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
    // True for the Seq1/2/3 length controls (marked "Length").
    bool isLengthControl() const noexcept { return paramIDStr_.startsWith ("seq_length_"); }
    // 0-based step index for a seq*_step* / seqnote_* control, else -1.
    int  stepIndex() const noexcept { return parseStepIndex (paramIDStr_); }
    // Whether the step's slider is currently interactive (false => dimmed: step
    // index >= its sequence length). True for non-step controls.
    bool isStepEnabled() const noexcept { return slider_ ? slider_->isEnabled() : true; }
    // Whether this control shows a visible (Length) label — used by the test.
    // Reflects the ACTUAL label component so the editor_test gets an independent
    // signal that the Length label renders (not just the paramID prefix).
    bool isLengthLabelVisible() const noexcept { return label_ != nullptr && label_->isVisible(); }

private:
    void showContextMenu();
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

    // ---- Sequencer step dimming ----
    // APVTS::Listener callback: the sibling seq_length_* param changed, so
    // re-evaluate whether this step's slider should be enabled/dimmed.
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    // Re-enable/dim this step based on its sibling sequence length (steps at
    // index >= length are disabled => the LookAndFeel omits the fill arc).
    void refreshStepEnabled();
    // Map a step paramID (seq1_step* / seq2_step* / seqnote_step*|vel*) to its
    // sibling length param (seq_length_1/2/3); empty for non-steps.
    static juce::String siblingLengthParamFor (const juce::String& stepID);
    // Parse the trailing integer of a step paramID ("seq1_step7" -> 7); -1 if not
    // a step.
    static int parseStepIndex (const juce::String& id);

    const PatchParamDescriptor& desc_;
    ParvatiAudioProcessor& processor_;   // APVTS access for reset/randomize
    std::unique_ptr<juce::Slider>    slider_;
    std::unique_ptr<juce::ComboBox>  comboBox_;
    std::unique_ptr<juce::Label>     label_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sliderAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment_;

    // Desktop TooltipWindow active ONLY while this control's right-click context
    // menu is open: it shows the custom TooltipClient menu-item tooltips above
    // the popup (the editor's parented TooltipWindow cannot render above the
    // popup and is suppressed while a popup is open). Created in
    // showContextMenu, destroyed in the menu's async close callback.
    std::unique_ptr<juce::TooltipWindow> popupTooltipWindow_;

    juce::String helpText_;         // cached getParamHelp(paramID); set in ctor
    static bool tooltipsEnabled_;   // toggled from the Settings panel
    static bool modDragActive_;     // true while a parvatiModSrc drag is in flight

    juce::String paramIDStr_;        // cached juce::String (desc_.paramID)
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

//==============================================================================
// One page (tab) of controls for a logical section. The controls are generated
// from the descriptor table (one ParamControl per descriptor) and partitioned
// into bordered GroupComponents derived from the param-ID prefixes. The groups
// flow left-to-right and wrap to the page width, so the layout reflows when the
// window / tab resizes (Phase 2b of docs/UI_MODERNIZATION_PLAN.md).
class ParamPage : public juce::Component
{
public:
    ParamPage (ParvatiAudioProcessor& processor,
               ThemeManager& themeManager,
               const std::vector<const PatchParamDescriptor*>& descriptors,
               int columns, int cellWidth, int cellHeight);

    void paint (juce::Graphics&) override;
    void resized() override;

    int getContentWidth()  const noexcept { return contentWidth_; }
    int getContentHeight() const noexcept { return contentHeight_; }

    // Re-apply the theme-derived colours and repaint. Called by the editor when
    // the active theme changes (page fill is read at paint time; group borders /
    // titles are themed via the LookAndFeel).
    void applyThemeColors();

    // Re-apply the translated group-panel titles through the active
    // LocalisedStrings (called by the editor after a live language switch). The
    // component name stays the English key; only the displayed text changes.
    void refreshLanguage();

    // Re-flow the grouped layout to @p targetWidth (called by the editor when
    // the tab / window resizes, so the group panels wrap to the available
    // width). Lays out, then sizes the page to (targetWidth, contentHeight_) so
    // the parent Viewport scrolls vertically only. @p viewportHeight, when > 0,
    // is the tab content area's height: short pages are vertically centred
    // within it (see layoutGroups) so a sparse page does not leave a large void
    // below the controls. The editor passes this for EVERY page (only the
    // current tab's Viewport is sized by JUCE, so reading the viewport at layout
    // time would mis-centre non-current tabs).
    void reflowToWidth (int targetWidth, int viewportHeight = 0);

    // Attach an auxiliary "decoration" component (e.g. an EnvelopeDisplay ADSR
    // preview) to a named group panel. ParamPage owns the component and lays it
    // out below the group's control cells, spanning the panel width (its height
    // is reserved in the group's computed size). Used on the Env/LFO page so the
    // envelope shape is visible while editing. No-op (besides owning the
    // pointer) if @p groupName does not match an existing group.
    void setGroupDecoration (const juce::String& groupName,
                             std::unique_ptr<juce::Component> decoration);

    // Attach an INLINE preview component to a named OSC group: it is laid out
    // INLINE in the knob row, beside the Shape combo (column 1, with the shape
    // combo at column 0 and the other 3 knobs shifted to columns 2..4). ParamPage
    // owns the component. Used on the Oscillators page so the waveform is visible
    // next to the Shape dropdown. No-op (besides owning the pointer) if
    // @p groupName does not match an existing group.
    void setGroupInlinePreview (const juce::String& groupName,
                                std::unique_ptr<juce::Component> preview);

    // Override a named group's decoration height (reserved room below the
    // control cells). Defaults to kDecorationH; smaller values make a compact
    // decoration (e.g. the Global voice strip uses ~32px instead of the 80px
    // reserved for the Env/LFO ADSR/LFO previews). Re-lays out the page so the
    // new height takes effect immediately. No-op if @p groupName is unknown.
    void setGroupDecorationHeight (const juce::String& groupName, int height);

    // Show only the named group panels (hide the rest) and re-layout so the page
    // contains just that subset. Used by GroupPager sub-tabs to paginate a dense
    // section (e.g. OSC1/OSC2, MOD MATRIX slots 1-4/5-8) WITHOUT regenerating a
    // single control or APVTS attachment. An empty array shows ALL groups.
    void setVisibleGroups (const juce::StringArray& groupNames);

    // Headless layout sanity check (called by parvati_editor_test): every group
    // panel has positive size, no two panels overlap, every (active) control
    // sits inside its group, and at least one non-dense row fills the page width.
    // Returns true when the flexible-width grid is well-formed.
    bool layoutIsSane() const;

private:
    // Maps a paramID to its bordered-group display name (e.g. "osc1_*"->"Osc 1",
    // "mod3_*"->"Mod 3"). Derived purely from the param-ID prefixes so the
    // verified APVTS byte-bridge is untouched.
    static juce::String groupForId (const juce::String& id);

    // One logical group of controls sharing a bordered panel.
    struct GroupLayout
    {
        juce::String name;
        std::vector<int> controlIndices;   // indices into controls_ (descriptor order)
        juce::GroupComponent* groupComp = nullptr;
        juce::Component* decoration = nullptr;  // optional aux component laid out below the cells
        juce::Component* inlinePreview = nullptr;  // optional preview laid out INLINE (OSC: beside the shape combo)
        int internalCols = 1;             // cell columns inside the panel
        int cellW = 0, cellH = 0;         // per-control cell size for this group
        bool singleRow = false;           // mod/modifier 3-wide horizontal strip
        bool stepGrid  = false;           // sequencer step grid
        bool sectioned = false;          // one panel holding labelled sub-sections (merged Mixer panel)
        int decorationH = kDecorationH;   // reserved height for this group's decoration (overridable per group, e.g. the compact Global voice strip)
        int naturalWidth = 0, naturalHeight = 0;
        juce::Rectangle<int> rect;
    };

    void buildGroups (const std::vector<const PatchParamDescriptor*>& descriptors);
    void configureGroupLayouts();        // internal cols + per-group cell sizes
    void layoutGroups (int targetWidth); // compute group rects + content size
    void applyLayout();                  // push computed rects to the components

    ThemeManager& themeManager_;
    int cellWidth_, cellHeight_;
    int pageCols_ = 0;              // PageInfo::cols: cap on group panels per row (0 => width-only wrap)
    int contentWidth_ = 0, contentHeight_ = 0;

    std::vector<std::unique_ptr<ParamControl>> controls_;
    std::vector<std::unique_ptr<juce::GroupComponent>> groupComponents_;
    std::vector<GroupLayout> groups_;
    std::vector<std::unique_ptr<juce::Component>> decorations_;   // owned group decorations

    // Layout constants (pixels). Tight margins / gaps / insets keep every page
    // dense (high component density, minimal whitespace), matching the compact
    // SEQ page rather than the sparse look the wider values produced.
    static constexpr int kMargin      = 10;  // page edge padding
    static constexpr int kGroupGap    = 8;   // gap between group panels (h + v)
    static constexpr int kGroupPad    = 8;   // inset inside a group border
    static constexpr int kGroupTitleH = 16;  // room reserved for the group title
    static constexpr int kDecorationH   = 80;  // reserved height for a group decoration (graphs)
    static constexpr int kDecorationGap = 6;   // gap between control cells and a decoration
    static constexpr int kSectionGap    = 6;   // vertical gap (with a themed divider) between sub-sections of a sectioned panel

    // Vertical centre offset applied to short pages (see layoutGroups): when a
    // page's natural content is shorter than its viewport, the whole grid is
    // shifted down so the empty space splits evenly instead of leaving a large
    // void below the controls.
    int yOffset_ = 0;

    // Tab content height passed by the editor (reliable for all tabs); drives
    // the vertical centring of short pages. 0 => fall back to the parent
    // Viewport's height (standalone / headless test use).
    int centerHeight_ = 0;

    // Active group-subset filter (empty => ALL groups visible). Set by
    // setVisibleGroups; honoured by layoutGroups/applyLayout/layoutIsSane so a
    // GroupPager can show one slice of a page at a time (hidden groups neither
    // occupy space, overlap, nor fail the layout-sanity check).
    juce::StringArray visibleGroups_;
    bool groupVisible (const GroupLayout& g) const noexcept
    { return visibleGroups_.isEmpty() || visibleGroups_.contains (g.name); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamPage)
};

//==============================================================================
// Multi / Setup page: edits the CURRENT part's MIDI channel + key zone directly
// on the engine (these are Part routing fields, not APVTS patch params). Refreshed
// by the editor's timer whenever the edited part changes.
class MultiPage : public juce::Component
{
public:
    MultiPage (ParvatiAudioProcessor& processor, ThemeManager& themeManager);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-read the current part's channel/keyzone from the engine and update the
    // controls (without firing onChange).
    void refresh();
    // Cheap timer hook: no-op unless the edited part changed since the last refresh.
    void refreshIfPartChanged();
    // Force a full refresh on the next call (e.g. after a .MUL load rewrites
    // every part's routing while the edited part stays the same).
    void forceRefresh() { lastPart_ = -1; }

    // Re-apply theme-derived colours and repaint (page fill is read at paint
    // time, the heading accent is explicit).
    void applyThemeColors();

    // Re-apply every chrome string through TRANS() (called by the editor after
    // a live language switch). The dynamic "Editing Part X of Y" line is
    // rebuilt by refresh().
    void refreshLanguage();

private:
    ParvatiAudioProcessor& proc_;
    ThemeManager& themeManager_;
    juce::Label heading_, partLabel_, chLabel_, loLabel_, hiLabel_, allocLabel_;
    juce::ComboBox channelCombo_;
    juce::Slider   loSlider_, hiSlider_;
    juce::ToggleButton allocBits_[6];   // one per firmware voicecard (vc1..6)
    bool refreshing_ = false;
    int  lastPart_ = -1;   // last part shown; -1 forces the first refresh

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultiPage)
};

//==============================================================================
class ParvatiEditor : public juce::AudioProcessorEditor,
                     public juce::DragAndDropContainer,
                     private juce::FileDragAndDropTarget,
                     private juce::Timer,
                     private juce::ChangeListener
{
public:
    explicit ParvatiEditor (ParvatiAudioProcessor&);
    ~ParvatiEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Parses the embedded parvati_logo.svg (a true vector: outlined <path>/<g>
    // art, no raster) into a juce::Drawable once (idempotent — no-op once the
    // drawable exists). The drawable carries its OWN brand colours and is drawn
    // as-is (NOT theme-tinted). Called lazily from paint().
    void loadLogoIcon();

    // Zoom keyboard shortcuts: Cmd/Ctrl + +/=/-/0 (Phase 4b). Returns true only
    // for handled keys, so typing in combos / text boxes is never swallowed.
    bool keyPressed (const juce::KeyPress& key) override;

    // User zoom, clamped to [0.75, 2.0] (also reachable via Cmd/Ctrl + +/=/-/0).
    // Applies juce::Desktop::setGlobalScaleFactor(), which is PROCESS-WIDE in
    // JUCE: every JUCE window / plugin instance in the host shares one zoom,
    // and the last editor to set it wins (a documented limitation of
    // multi-instance use). ~ParvatiEditor resets it to 1.0 so a non-default zoom
    // does not leak after close. Per-editor, transform-based zoom (no global
    // side-effect) is a documented future enhancement, deferred to avoid
    // destabilizing the reflow layout. Default 1.0.
    void   setZoom (double zoom);
    double getZoom() const noexcept { return zoom_; }

    // The editor-wide TooltipWindow. Exposed so ParamControl::showContextMenu can
    // hide it the instant a right-click menu opens (the 30 Hz timer in
    // timerCallback also hides it while any modal popup stays open).
    juce::TooltipWindow* getTooltipWindow() const noexcept { return tooltipWindow_.get(); }

private:
    // juce::FileDragAndDropTarget — accept dropped Ambika .PRO/.MUL files.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // juce::DragAndDropContainer — detect the start/end of an internal
    // mod-source drag (payload "parvatiModSrc:<enum>") to toggle the drag-drop
    // affordance: valid destination knobs light up as drop zones and every
    // other control dims. dragOperationEnded fires on BOTH drop and cancel, so
    // the state always clears. (ParvatiEditor IS a DragAndDropContainer, so it
    // overrides these two protected virtuals directly — this JUCE version has
    // no separate DragAndDropContainer::Listener / addListener API.)
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& details) override;
    void dragOperationEnded   (const juce::DragAndDropTarget::SourceDetails&) override;

    // juce::Timer — keep the Multi page in sync with the edited part (~30 Hz).
    void timerCallback() override;

    // juce::ChangeListener — re-apply the L&F theme + repaint when the
    // ThemeManager selection moves.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void openLoadDialog();
    void openSaveDialog();
    void openSaveParvatiDialog();
    void applyPatchFile (const juce::File&);

    // Re-apply every editor-chrome string through TRANS() (buttons, captions,
    // tab names, page headings) and refresh the settings panel + multi page,
    // so a live language switch updates immediately. Called once after the UI
    // is built and again on every language change.
    void applyChromeTranslations();

    // The Multi page is owned here and shown as an overlay over the content area.
    std::unique_ptr<MultiPage> multiPage_;
    // Generated ParamPages — EDITOR-OWNED. Each is reparented (NOT regenerated)
    // into the integrated SynthWorkspace (the 9 synth pages) or the top-level
    // GLOBAL tab (the Global page), so every APVTS attachment and the verified
    // byte-bridge survive the reorganization unchanged.
    // Declaration order is deliberate for safe teardown: pageSelector_ (holds a
    // raw ref to synthWorkspace_ below) destroys first, then synthWorkspace_ (its
    // nested tabs detach from the pages), then generatedPages_ deletes them.
    std::vector<std::unique_ptr<ParamPage>> generatedPages_;
    // The redesigned MOD MATRIX panel (Wave 1). EDITOR-OWNED. Hosted NON-owned
    // by SynthWorkspace's nested modTabs_ (deleteWhenNotNeeded=false), exactly
    // like the reparented ParamPages, so the view must outlive the TabbedComponent
    // that hosts it. Declared BEFORE synthWorkspace_ on purpose: members destroy in
    // REVERSE declaration order, so synthWorkspace_ (and its modTabs_) tear down
    // FIRST and merely DETACH the non-owned view, then modMatrixView_ deletes it —
    // no use-after-free, no double-free.
    std::unique_ptr<ModMatrixView> modMatrixView_;
    // SYNTH content: the dense, void-free 2-row integrated workspace hosting the
    // 9 synth ParamPages (reparented) in signal-chain columns (OSC | Mixer |
    // Filter) above two nested tab groups (ENV/LFO/ARP/SEQ and MOD MATRIX/
    // MODIFIERS). Owns only its nested tabs; pages stay editor-owned.
    std::unique_ptr<SynthWorkspace> synthWorkspace_;
    // Single-tab page selector [SYNTH] (the lone bar is hidden via depth 0). SYNTH
    // shows synthWorkspace_; GLOBAL is a header-button overlay (globalPage_), not
    // a tab. Non-owned tab content (editor-owned via generatedPages_).
    juce::TabbedComponent pageSelector_ { juce::TabbedButtonBar::TabsAtTop };

    ParvatiAudioProcessor& processorRef_;

    // Theme system (Phase 2a). Direct members: ~ParvatiEditor's body removes the
    // ChangeListener and resets the L&F pointer before these members (and the
    // base Component) are destroyed, so the broadcaster and the L&F stay valid
    // for the whole teardown.
    ThemeManager themeManager_;
    ParvatiLookAndFeel lnf_;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow_;
    double zoom_ = 1.0;

    // Top patch bar. The patch selector is a cascading PresetBrowser (replaces
    // the flat patchCombo_); undo/redo are Path-drawn IconButtons (no font glyph).
    juce::Label      patchCaption_;
    std::unique_ptr<PresetBrowser> presetBrowser_;
    juce::TextButton loadButton_  { "Load" };
    juce::TextButton saveButton_  { "Save" };
    IconButton       undoButton_  { IconButton::Icon::Undo };   // top-bar Undo (Cmd/Ctrl+Z)
    IconButton       redoButton_  { IconButton::Icon::Redo };   // top-bar Redo (Cmd/Ctrl+Shift+Z / Y)
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Top bar: Part selector (bound to the `part_select` APVTS param).
    juce::Label    partCaption_;
    juce::ComboBox partCombo_;
    juce::TextButton multiButton_ { "Multi" };   // header button -> Multi/Setup overlay (not a patch param)
    juce::TextButton globalButton_ { "Global" }; // header button -> Global ParamPage overlay (not a patch param)
    juce::TextButton kbdToggleButton_ { "KBD" };  // header toggle: show/hide the bottom virtual keyboard
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> partComboAttachment_;

    // Top header: brand icon + white "Parvati" wordmark (painted, left) + version label (inline right).
    juce::Label versionLabel_;
    juce::Rectangle<int> logoArea_;   // set in resized(); paint() draws the icon + "Parvati" text here

    // Brand icon: the embedded parvati_logo.svg (true vector art) parsed once
    // into a juce::Drawable. It carries its OWN brand colours and is drawn as-is
    // (NOT theme-tinted); only the adjacent "Parvati" text re-colours with the
    // theme `text` token.
    std::unique_ptr<juce::Drawable> logoDrawable_;

    static constexpr int kBarHeight   = 34;
    static constexpr int kHeaderH     = 40;  // compact header: icon + "Parvati" + version (left) | Patch/Part (centre) | icons (right)
    static constexpr int kPageTabsH   = 28;  // top-level [SYNTH | GLOBAL] page-selector strip
    static constexpr int kKeyboardH   = 104;  // bottom virtual-keyboard strip
    static constexpr int kMeterStripH = 52;   // (legacy) voice-meter strip height
    static constexpr int kVoiceStripH = 22;   // compact voice-meter strip at the very bottom

    // ---- Phase 4a: visualization + settings integration ----
    // Settings side panel (owned here; content owned by the SidePanel).
    std::unique_ptr<juce::SidePanel> settingsPanelHost_;
    SettingsPanel* settingsPanel_ { nullptr };
    IconButton       settingsButton_ { IconButton::Icon::Gear };   // gear icon, top-right

    // Virtual keyboard (bottom strip) + status bar (count + tooltip). The voice
    // ACTIVITY cells live on the Global page (globalVoiceMeter_, owned by that
    // page as a decoration); the bottom strip shows only the active-count + a
    // hover-tooltip bar (the cells + "Voices" word were removed per request).
    std::unique_ptr<KeyboardView>    keyboardView_;
    std::unique_ptr<WheelsComponent> wheels_;   // pitch + mod wheels (left of keyboard)
    VoiceMeter* globalVoiceMeter_ { nullptr };  // cells display; owned by the Global ParamPage
    ParamPage*  globalPage_ { nullptr };        // Global page overlay (toggled by globalButton_; owns the meter as a decoration)

    // Live graph previews (EnvelopeDisplay / OscPreviewDisplay /
    // FilterResponseDisplay) + the theme category token they read for their trace
    // (cyan Env / magenta LFO / amber Audio). Each entry holds a re-tint function
    // (calling the component's setCategoryColour) + a pointer-to-member theme
    // token, so a theme switch can re-resolve the NEW theme's token value and
    // re-push it (a stored Colour snapshot would otherwise freeze on the old
    // theme).
    using ThemeColourField = juce::Colour ParvatiTheme::*;
    using GraphTintFn = std::function<void (const juce::Colour&)>;
    std::vector<std::pair<GraphTintFn, ThemeColourField>> graphCategoryBindings_;
    void reapplyGraphCategoryColours();

    // Re-apply every theme-derived colour across the whole editor tree
    // (sendLookAndFeelChange + per-page/workspace/multi applyThemeColors +
    // category arc/mod tints + ENV/LFO graph traces + status labels +
    // keyboard/voice-meter refresh). Shared by the theme-CHANGE path
    // (changeListenerCallback) AND called once at the end of the ctor so knobs,
    // graphs and mod tints are coloured from the FIRST paint in every context
    // (standalone, headless screen tool, tests) — changeListenerCallback is only
    // invoked when selectByName actually moves the selection, so without this
    // explicit call the category colours could stay on the L&F default.
    void applyAllColoursFromTheme();
    juce::Label statusCountLabel_;              // bottom-left "n/denom" active-voice count
    juce::Label statusTooltipLabel_;            // bottom hover-tooltip bar (fills the rest)

    // Keyboard latching state: notes currently lit on the virtual keyboard so
    // we only fire latchNoteOn/Off on actual transitions (avoids stuck lamps).
    juce::Array<int> latchedNotes_;
    int lastLatchPart_ { -1 };   // last part seen; clear latches when it changes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiEditor)
};
