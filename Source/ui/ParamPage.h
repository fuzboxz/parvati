// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// ParamPage — one page (tab) of ParamControl cells generated from the
// descriptor table, partitioned into bordered group panels that reflow to
// the page width. Extracted unchanged from PluginEditor.h so the ui/ layer
// no longer includes the editor header for this type.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "ParamControl.h"   // controls_ (the page creates and owns one per descriptor)

class ThemeManager;

//==============================================================================
// One page (tab) of controls for a logical section. The controls are generated
// from the descriptor table (one ParamControl per descriptor) and partitioned
// into bordered GroupComponents derived from the param-ID prefixes. The groups
// flow left-to-right and wrap to the page width, so the layout reflows when the
// window / tab resizes (Phase 2b of docs/UI_MODERNIZATION_PLAN.md).
class ParamPage : public juce::Component
{
public:
    ParamPage (HellcatAudioProcessor& processor,
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

    // Attach an EXTERNAL (non-owned) decoration component to a named group
    // panel: laid out below the group's owned decoration (if any), spanning
    // the panel width, with @p height reserved in the group's computed size.
    // setGroupDecoration is single-slot per group (GroupLayout::decoration),
    // so a second, caller-owned component (e.g. the Patch page's 6-part
    // voice-allocation table merged into the Global panel) rides THIS slot.
    // LIFETIME CONTRACT: the caller keeps ownership and must outlive this page
    // (or guarantee no relayout runs after the owner dies). This page only
    // PARENTS the component (addAndMakeVisible) and positions it in applyLayout
    // — JUCE removes a destroyed child from its parent cleanly, so teardown is
    // safe in any order; a relayout after the owner's destruction would
    // position a dangling pointer, hence the contract. No-op (besides the
    // parenting, which is reversed on destruction) if @p groupName does not
    // match an existing group.
    void setGroupExternalDecoration (const juce::String& groupName,
                                     juce::Component* external, int height);

    // Show only the named group panels (hide the rest) and re-layout so the page
    // contains just that subset. Used by GroupPager sub-tabs to paginate a dense
    // section (e.g. OSC1/OSC2, MOD MATRIX slots 1-4/5-8) WITHOUT regenerating a
    // single control or APVTS attachment. An empty array shows ALL groups.
    void setVisibleGroups (const juce::StringArray& groupNames);

    // TEST-ONLY accessors: the live decoration / inline-preview components
    // for a named group (nullptr when the group or the component does not
    // exist). Lets headless tests reach the real preview displays (cast to
    // OscPreviewDisplay / EnvelopeDisplay / FilterResponseDisplay) and observe
    // their generation counters. Not used by product code.
    juce::Component* getGroupDecorationForTest (const juce::String& groupName) const;
    juce::Component* getGroupInlinePreviewForTest (const juce::String& groupName) const;

    // Headless layout check (called by hellcat_editor_coverage_check): every group
    // panel has positive size, no two panels overlap, every (active) control
    // sits inside its group, and at least one non-dense row fills the page width.
    // Returns true when the flexible-width grid is well-formed.
    bool layoutIsSane() const;

private:
    // Maps a paramID to its bordered-group display name (e.g. "osc1_*"->"Osc 1",
    // "mod3_*"->"Mod 3"). Derived purely from the param-ID prefixes so the
    // checked APVTS byte-bridge is untouched.
    static juce::String groupForId (const juce::String& id);

    // One logical group of controls sharing a bordered panel.
    struct GroupLayout
    {
        juce::String name;
        std::vector<int> controlIndices;   // indices into controls_ (descriptor order)
        juce::GroupComponent* groupComp = nullptr;
        juce::Component* decoration = nullptr;  // optional aux component laid out below the cells
        juce::Component* inlinePreview = nullptr;  // optional preview laid out INLINE (OSC: beside the shape combo)
        juce::Component* externalDecoration = nullptr;  // optional NON-OWNED component below the owned decoration (see setGroupExternalDecoration)
        int externalDecorationH = 0;      // reserved height for externalDecoration (0 = none attached)
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
    // SEQ page rather than the sparse look the wider values produced. kMargin is
    // public so the sizing-contract test can assert it.
public:
    static constexpr int kMargin      = 8;   // page edge padding (unified)
private:
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
    // occupy space, overlap, nor fail the layout check).
    juce::StringArray visibleGroups_;
    bool groupVisible (const GroupLayout& g) const noexcept
    { return visibleGroups_.isEmpty() || visibleGroups_.contains (g.name); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamPage)
};
