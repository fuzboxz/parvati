// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiEditor — the full Ambika GUI. A tabbed editor whose controls are
// generated entirely from the PatchParamDescriptor table (ParameterLayout.h),
// so the GUI and the APVTS byte-bridge can never drift apart. Every one of
// the 104 patch/part parameters gets a control (rotary Slider or ComboBox)
// plus an APVTS attachment.
//
// Colours come from the ParvatiTheme via a single ParvatiLookAndFeel set on the
// editor and inherited by the whole component tree — no per-control palette.
// Phase 2a of docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ui/KeyboardView.h"
#include "ui/WheelsComponent.h"
#include "ui/ParvatiLookAndFeel.h"
#include "ui/SettingsPanel.h"
#include "ui/ThemeManager.h"
#include "ui/VoiceMeter.h"
#include "ui/IconButton.h"
#include "ui/PresetBrowser.h"

class MultiPage;

//==============================================================================
// One control cell: a rotary Slider (numeric params) or a ComboBox (choice
// params), plus a label, all bound to one APVTS parameter. Colours are taken
// from the editor-wide ParvatiLookAndFeel (inherited via the component tree),
// and the cell exposes its parameter's help text as a tooltip.
class ParamControl : public juce::Component,
                     public juce::TooltipClient,
                     public juce::AudioProcessorValueTreeState::Listener
{
public:
    ParamControl (ParvatiAudioProcessor& processor, const PatchParamDescriptor& descriptor);

    void resized() override;

    ~ParamControl() override;

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

    // Right-click (popup) on this cell — or on its child Slider/ComboBox, which
    // registers `this` as a MouseListener (Component is already a MouseListener,
    // so no extra base is needed) — shows a context menu (Reset to default /
    // Randomize). Non-popup clicks fall through to normal interaction.
    void mouseDown (const juce::MouseEvent& e) override;

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

    juce::String helpText_;         // cached getParamHelp(paramID); set in ctor
    static bool tooltipsEnabled_;   // toggled from the Settings panel

    juce::String paramIDStr_;        // cached juce::String (desc_.paramID)
    juce::String lengthParamID_;     // sibling length param; empty for non-steps

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
               const juce::String& heading,
               const std::vector<const PatchParamDescriptor*>& descriptors,
               int columns, int cellWidth, int cellHeight);

    void paint (juce::Graphics&) override;
    void resized() override;

    int getContentWidth()  const noexcept { return contentWidth_; }
    int getContentHeight() const noexcept { return contentHeight_; }

    // Re-apply the theme-derived colours (page fill is read at paint time, the
    // heading accent is explicit) and repaint. Called by the editor when the
    // active theme changes. Group borders are themed via the LookAndFeel.
    void applyThemeColors();

    // Re-set the page heading text (called by the editor after a live language
    // switch so the heading matches the translated tab name).
    void setHeadingText (const juce::String& text);

    // Re-flow the grouped layout to @p targetWidth (called by the editor when
    // the tab / window resizes, so the group panels wrap to the available
    // width). Lays out, then sizes the page to (targetWidth, contentHeight_) so
    // the parent Viewport scrolls vertically only.
    void reflowToWidth (int targetWidth);

    // Attach an auxiliary "decoration" component (e.g. an EnvelopeDisplay ADSR
    // preview) to a named group panel. ParamPage owns the component and lays it
    // out below the group's control cells, spanning the panel width (its height
    // is reserved in the group's computed size). Used on the Env/LFO page so the
    // envelope shape is visible while editing. No-op (besides owning the
    // pointer) if @p groupName does not match an existing group.
    void setGroupDecoration (const juce::String& groupName,
                             std::unique_ptr<juce::Component> decoration);

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
        int internalCols = 1;             // cell columns inside the panel
        int cellW = 0, cellH = 0;         // per-control cell size for this group
        bool singleRow = false;           // mod/modifier 3-wide horizontal strip
        bool stepGrid  = false;           // sequencer step grid
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

    juce::Label heading_;
    std::vector<std::unique_ptr<ParamControl>> controls_;
    std::vector<std::unique_ptr<juce::GroupComponent>> groupComponents_;
    std::vector<GroupLayout> groups_;
    std::vector<std::unique_ptr<juce::Component>> decorations_;   // owned group decorations

    // Layout constants (pixels).
    static constexpr int kMargin      = 16;  // page edge padding
    static constexpr int kHeadingH    = 30;  // page heading height
    static constexpr int kHeadingGap  = 8;   // gap below the heading
    static constexpr int kGroupGap    = 12;  // gap between group panels (h + v)
    static constexpr int kGroupPad    = 12;  // inset inside a group border
    static constexpr int kGroupTitleH = 18;  // room reserved for the group title
    static constexpr int kDecorationH   = 100; // reserved height for a group decoration
    static constexpr int kDecorationGap = 8;   // gap between control cells and a decoration

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
                     private juce::FileDragAndDropTarget,
                     private juce::Timer,
                     private juce::ChangeListener
{
public:
    explicit ParvatiEditor (ParvatiAudioProcessor&);
    ~ParvatiEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

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

private:
    // juce::FileDragAndDropTarget — accept dropped Ambika .PRO/.MUL files.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

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

    // The Multi page is owned here (added to the tab bar with takeOwnership=false).
    std::unique_ptr<MultiPage> multiPage_;
    // Generated ParamPages, non-owning (each is owned by its Viewport, which the
    // tab bar owns). Kept so theme changes can refresh them.
    std::vector<ParamPage*> generatedPages_;
    // Viewports wrapping the generated pages (non-owning; owned by the tab bar).
    // Kept so the editor can reflow each page to its tab width on resize.
    std::vector<juce::Viewport*> pageViewports_;
    // English (key) tab names in tab order (the 9 section pages + "Multi"), so a
    // live language switch can re-translate every tab + matching page heading.
    std::vector<juce::String> tabKeys_;

    ParvatiAudioProcessor& processorRef_;
    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };

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
    juce::TextButton loadButton_  { "Load .PRO..." };
    juce::TextButton saveButton_  { "Save..." };
    IconButton       undoButton_  { IconButton::Icon::Undo };   // top-bar Undo (Cmd/Ctrl+Z)
    IconButton       redoButton_  { IconButton::Icon::Redo };   // top-bar Redo (Cmd/Ctrl+Shift+Z / Y)
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Top bar: Part selector (bound to the `part_select` APVTS param).
    juce::Label    partCaption_;
    juce::ComboBox partCombo_;
    juce::TextButton multiButton_ { "Multi" };   // header button -> Multi/Setup overlay (not a patch param)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> partComboAttachment_;

    // Top header: ASCII-art logo (painted, left) + version label (under it).
    juce::Label versionLabel_;
    juce::Rectangle<int> logoArea_;   // set in resized(); paint() draws the logo here

    static constexpr int kBarHeight   = 34;
    static constexpr int kHeaderH     = 56;  // merged top header: ASCII logo + version (left) + menu buttons (right)
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
    ParamPage*  globalPage_ { nullptr };        // Global tab (owns the meter as a decoration)
    juce::Label statusCountLabel_;              // bottom-left "n/denom" active-voice count
    juce::Label statusTooltipLabel_;            // bottom hover-tooltip bar (fills the rest)

    // Keyboard latching state: notes currently lit on the virtual keyboard so
    // we only fire latchNoteOn/Off on actual transitions (avoids stuck lamps).
    juce::Array<int> latchedNotes_;
    int lastLatchPart_ { -1 };   // last part seen; clear latches when it changes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiEditor)
};
