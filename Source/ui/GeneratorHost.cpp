// Copyright (c) 2026 805Labs Kft. / Hellcat.  See GeneratorHost.h.

#include "GeneratorHost.h"

#include "ChromeRule.h"      // hellcat::ChromeRule (the mod-bar top rule — one family with the editor chrome rules)
#include "ModSourceCatalog.h"   // hellcat::entryFor (generator vs drag-only)
#include "ParamPage.h"        // ParamPage complete type (ParamControl statics via it)
#include "ThemeManager.h"
#include "HellcatLookAndFeel.h"   // hellcat::isY2kTheme (Y2K window-chrome transparency)

//==============================================================================
GeneratorHostWorkspace::GeneratorHostWorkspace (ThemeManager& tm)
    : themeManager_ (tm)
{
    // The full-width Central Modulation Bar (middle seam).
    modBar_ = std::make_unique<CentralModBar> (themeManager_);
    addAndMakeVisible (*modBar_);
    // MOD-BAR TOP RULE (2026-08-21 fix): the rule component occupies the FIRST
    // 1+kRuleShadowH px of the bar row and paints with shadowBelow=true — the
    // LINE sits at the bar's very top edge (the seam) and the falloff shades
    // DOWN into the bar's 8px empty top pad. (The old shadowBelow=false drew
    // the line at the rule's BOTTOM — 6px into the bar, straight through the
    // coloured label tabs: the "separator overlaps the pill top" report. It
    // cannot live above the seam: the top-row Viewport is added after this
    // rule and would cover it.) Same ChromeRule family as the editor's
    // header/status/keyboard rules. Non-interactive.
    barRule_ = std::make_unique<hellcat::ChromeRule> (true);
    addAndMakeVisible (*barRule_);
    barRule_->setVisible (false);   // laid out only while the seam is shown

    // TOP row: the workspace's own modules live in a vertical-scroll Viewport
    // host (R3). At the tuned design size the row fits its cell, so no
    // scrollbar ever appears. In a SHORT frame (compacted window, small AUv3
    // pane) the row keeps its natural, taller height and SCROLLS instead of
    // the modules painting over the bar / bottom rows (the reported
    // "compacting overlaps" bug). Same scroll-gesture properties as the
    // bottom-left host below.
    topRowHost_ = std::make_unique<juce::Component>();
    topRowViewport_ = std::make_unique<juce::Viewport>();
    topRowViewport_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only on overflow
    topRowViewport_->setViewedComponent (topRowHost_.get(), false);
    addAndMakeVisible (*topRowViewport_);

    // BOTTOM-LEFT: a vertical-scroll host (T4 safety net) that reparents one
    // generator page at a time. At the tuned design size every generator page
    // fits its cell — reflowActiveEditor grows the page to at least the view
    // height — so NO scrollbar ever appears and the layout is byte-identical
    // to the old plain host. Only in a SHORT host frame (small AUv3 pane,
    // dense page subset) does the page keep its natural, taller height and a
    // vertical scrollbar appear, turning previously unreachable clipped
    // content into reachable scrolled content.
    //   * Desktop mouse drags never scroll-on-drag: the Viewport's default
    //     ScrollOnDragMode::nonHover is touch-only.
    //   * The mouse WHEEL scrolls: knobs have their wheel disabled
    //     (setScrollWheelEnabled(false)) and juce bubbles an unhandled wheel
    //     up the ancestors to the Viewport, so wheel-over-knob scrolls instead
    //     of editing (the knob-wheel policy is unchanged).
    //   * A TOUCH drag starting on a control does not scroll (ParamControl
    //     sets the viewport ignore-drag flag on its cells), so knob edits and
    //     page scrolling never fight; a touch drag on the page background
    //     scrolls.
    activeEditorHost_ = std::make_unique<juce::Viewport>();
    activeEditorHost_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only when the page overflows
    addAndMakeVisible (*activeEditorHost_);

    // Wire the bar's pill clicks. A GENERATOR pill (catalogue isGenerator)
    // swaps the bottom-left active editor to its page+group AND lights the
    // pill's underline glow. A drag-only pill (Perf/Util/Const) delegates to
    // the editor-registered handler (matrix row flash for rows routed from
    // that source). The drag itself (any pill) is handled inside the bar and
    // needs no wiring here — it carries the "parvatiModSrc:<enum>" payload.
    modBar_->setOnPillClicked ([this] (int src)
    {
        // Tap-to-assign mode: a pill tap selects this mod source for the next
        // dest tap and SUPPRESSES the generator-page flip (assign mode is
        // focused). Bar-only sentinels (src < 0, e.g. the Note Sequencer) are
        // never a valid mod source and are skipped. Inert unless [MOD] is on.
        if (ParamControl::tapAssignActive())
        {
            if (src >= 0)
                ParamControl::setTapSelectedSource (src);
            return;
        }
        if (hellcat::entryFor (src).isGenerator)
            setActiveGenerator (src);
        else if (onDragOnlyPillClicked_)
            onDragOnlyPillClicked_ (src);
    });
}

//==============================================================================
void GeneratorHostWorkspace::registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                                    const juce::StringArray& groupNames)
{
    generators_[modSrcEnum] = { page, groupNames };
}

void GeneratorHostWorkspace::setOnDragOnlyPillClicked (std::function<void (int)> cb)
{
    onDragOnlyPillClicked_ = std::move (cb);
}

void GeneratorHostWorkspace::setOnActiveGeneratorChanged (std::function<void (int)> cb)
{
    onActiveGenChanged_ = std::move (cb);
}

//==============================================================================
void GeneratorHostWorkspace::showGenerator (int modSrcEnum)
{
    const auto it = generators_.find (modSrcEnum);
    if (it == generators_.end() || it->second.page == nullptr)
        return;

    auto* page = it->second.page;

    // Reparent the page into the active-editor host (the page is NEVER
    // regenerated — only its parent + visible-group subset change). Because
    // the SAME page is shared between the two workspaces, reparenting it here
    // detaches it from the other workspace's host on a SYNTH<->FX toggle
    // (single active selection — no double-parent: a JUCE Component can only
    // have one parent). The Viewport API (delete-on-remove = false) only
    // reparents, never deletes, and resets the scroll to the top on every
    // swap, so every generator swap starts at the page head.
    if (activePage_ != page)
    {
        activeEditorHost_->setViewedComponent (page, false);
        activePage_ = page;
    }

    // Show just this generator's group subset (EMPTY array => ALL groups,
    // e.g. ARP). The stored array is passed through unchanged so a
    // multi-group entry (e.g. the Note Sequencer's "Note Pitch" + "Note
    // Velocity") reveals every group it names.
    page->setVisibleGroups (it->second.groups);

    reflowActiveEditor();
}

void GeneratorHostWorkspace::setActiveGenerator (int modSrcEnum)
{
    showGenerator (modSrcEnum);
    modBar_->setActiveGenerator (modSrcEnum);   // underline-glow the pill
    if (onActiveGenChanged_)
        onActiveGenChanged_ (modSrcEnum);
}

void GeneratorHostWorkspace::releaseActiveEditor()
{
    if (activePage_ != nullptr)
    {
        // Detach via the Viewport API (non-owned: nullptr with the same
        // delete-on-remove = false used at attach, so the page is only
        // reparented away, never deleted).
        activeEditorHost_->setViewedComponent (nullptr, false);
        activePage_ = nullptr;
    }
}

void GeneratorHostWorkspace::reflowActiveEditor()
{
    if (activePage_ == nullptr)
        return;
    const auto b = activeEditorHost_->getLocalBounds();
    if (b.isEmpty())
        return;
    // Anchor at the Viewport origin (setViewedComponent resets the scroll, and
    // setSize below preserves the top-left, so this is only load-bearing on
    // the first layout after a swap).
    activePage_->setTopLeftPosition (0, 0);
    // Lay the page out at the FULL view width first. reflowToWidth grows the
    // page to at least the view height, so a page that FITS produces exactly
    // the old plain-host layout: no scrollbar, no width change. Only when the
    // page's natural height overflows the view is it re-laid one
    // scrollbar-thickness narrower so the vertical scrollbar never covers the
    // right edge (the matrix-view precedent subtracts the thickness
    // unconditionally; here it is subtracted only when it actually scrolls).
    activePage_->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
    if (activePage_->getHeight() > b.getHeight())
        activePage_->reflowToWidth (juce::jmax (150, b.getWidth()
                                                      - activeEditorHost_->getScrollBarThickness()),
                                    juce::jmax (0, b.getHeight()));
}

//==============================================================================
void GeneratorHostWorkspace::paint (juce::Graphics& g)
{
    // Y2K: NO fill — the editor's liquid-chrome WINDOW sweep shows through
    // (the module cards and trays paint over it).
    if (! hellcat::isY2kTheme (&themeManager_.getCurrentTheme()))
        g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

GeneratorHostWorkspace::RowHeights
GeneratorHostWorkspace::splitRows (const juce::Rectangle<int>& area, int bottomRowMaxH) const
{
    constexpr float kBottomShare = 0.60f;
    const int withBarH = juce::jmax (0, area.getHeight() - CentralModBar::kBarHeight);
    const int bottomH  = juce::jmin (juce::roundToInt (static_cast<float> (withBarH) * kBottomShare),
                                     bottomRowMaxH);
    const int barH     = modBarVisible_ ? CentralModBar::kBarHeight : 0;
    const int mainH    = juce::jmax (0, area.getHeight() - barH - bottomH);
    return { mainH, barH, bottomH };
}

void GeneratorHostWorkspace::layoutBarSeam (const juce::Rectangle<int>& barRow)
{
    if (modBar_ != nullptr)
    {
        modBar_->setVisible (modBarVisible_);
        if (modBarVisible_)
            modBar_->setBounds (barRow);
    }
    // The seam's top separator: full workspace width at the bar's top edge,
    // shown only with the bar (2026-08-20 user request — same family as the
    // editor's chrome rules; the depth falloff points up into the top row).
    if (barRule_ != nullptr)
    {
        barRule_->setVisible (modBarVisible_);
        if (modBarVisible_)
            barRule_->setBounds (barRow.getX(), barRow.getY(),
                                 barRow.getWidth(), 1 + hellcat::kRuleShadowH);
    }
}

void GeneratorHostWorkspace::layoutBottomRow (juce::Rectangle<int> bottomRow,
                                              juce::Component* matrixView)
{
    // LEFT 50% = active editor, RIGHT 50% = the matrix view.
    auto modLeft  = bottomRow.removeFromLeft (bottomRow.getWidth() / 2);
    auto modRight = bottomRow;

    activeEditorHost_->setBounds (modLeft);
    reflowActiveEditor();

    if (matrixView != nullptr)
        matrixView->setBounds (modRight);   // its resized() lays out the rows
}
