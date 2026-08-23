// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthWorkspace.h.

#include "SynthWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "ModMatrixView.h"
#include "ModSourceCatalog.h"   // parvati::entryFor (generator vs drag-only)
#include "ThemeManager.h"
#include "ChromeRule.h"      // parvati::ChromeRule (the mod-bar top rule — one family with the editor chrome rules)

//==============================================================================
SynthWorkspace::SynthWorkspace (ThemeManager& tm)
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
    barRule_ = std::make_unique<parvati::ChromeRule> (true);
    addAndMakeVisible (*barRule_);
    barRule_->setVisible (false);   // laid out only while the seam is shown

    // TOP row: the three main-row columns (OSC | MIX | FILTER) live in a
    // vertical-scroll Viewport host (R3). At the tuned design size each page
    // fits its cell — reflowToWidth grows a fitting page to at least the view
    // height — so no scrollbar ever appears and the layout is byte-identical
    // to the old direct hosting. In a SHORT frame (compacted window, small
    // AUv3 pane) the pages keep their natural, taller height and the row
    // SCROLLS instead of the pages painting over the bar / bottom rows (the
    // reported "compacting overlaps" bug). Same scroll-gesture properties as
    // the bottom-left host below.
    topRowHost_ = std::make_unique<juce::Component>();
    topRowViewport_ = std::make_unique<juce::Viewport>();
    topRowViewport_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only on overflow
    topRowViewport_->setViewedComponent (topRowHost_.get(), false);
    addAndMakeVisible (*topRowViewport_);

    // BOTTOM-LEFT: a vertical-scroll host (T4 safety net) that reparents one
    // generator page at a time. At the tuned design size every generator page
    // fits its cell — reflowToWidth grows the page to at least the view height
    // — so NO scrollbar ever appears and the layout is byte-identical to the
    // old plain host. Only in a SHORT host frame (small AUv3 pane, dense page
    // subset) does the page keep its natural, taller height and a vertical
    // scrollbar appear, turning previously unreachable clipped content into
    // reachable scrolled content.
    //   * Desktop mouse drags never scroll-on-drag: the Viewport's default
    //     ScrollOnDragMode::nonHover is touch-only.
    //   * The mouse WHEEL scrolls: knobs have their wheel disabled
    //     (setScrollWheelEnabled(false)) and juce bubbles an unhandled wheel up
    //     the ancestors to the Viewport, so wheel-over-knob scrolls instead of
    //     editing (the knob-wheel policy is unchanged).
    //   * A TOUCH drag starting on a control does not scroll (ParamControl sets
    //     the viewport ignore-drag flag on its cells), so knob edits and page
    //     scrolling never fight; a touch drag on the page background scrolls.
    activeEditorHost_ = std::make_unique<juce::Viewport>();
    activeEditorHost_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only when the page overflows
    addAndMakeVisible (*activeEditorHost_);

    // Wire the bar's pill clicks. A GENERATOR pill (catalogue isGenerator) swaps
    // the bottom-left active editor to its page+group AND lights the pill's
    // underline glow. A drag-only pill (Perf/Util/Const) delegates to the
    // editor-registered handler (ModMatrixView row flash for rows routed from
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
        if (parvati::entryFor (src).isGenerator)
            setActiveGenerator (src);
        else if (onDragOnlyPillClicked_)
            onDragOnlyPillClicked_ (src);
    });
}

//==============================================================================
void SynthWorkspace::setMainLeft (ParamPage* page)
{
    // Mixer — direct child of the top-row host (editor-owned page, reparented
    // not regenerated).
    mainLeftPage_ = page;
    if (page != nullptr)
        topRowHost_->addAndMakeVisible (*page);
}

void SynthWorkspace::setOscillators (ParamPage* page)
{
    // Oscillators — shown DIRECTLY (both "Osc 1"/"Osc 2" visible; an empty
    // visibleGroups_ set => all groups render). The page stays editor-owned
    // (reparented, never regenerated).
    mainOscPage_ = page;
    if (page != nullptr)
        topRowHost_->addAndMakeVisible (*page);
}

void SynthWorkspace::setMainRight (ParamPage* page)
{
    // Filter — direct child of the top-row host (editor-owned page, reparented
    // not regenerated).
    mainRightPage_ = page;
    if (page != nullptr)
        topRowHost_->addAndMakeVisible (*page);
}

void SynthWorkspace::registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                            const juce::StringArray& groupNames)
{
    generators_[modSrcEnum] = { page, groupNames };
}

void SynthWorkspace::setOnDragOnlyPillClicked (std::function<void (int)> cb)
{
    onDragOnlyPillClicked_ = std::move (cb);
}

void SynthWorkspace::setOnActiveGeneratorChanged (std::function<void (int)> cb)
{
    onActiveGenChanged_ = std::move (cb);
}

void SynthWorkspace::setModMatrixView (ModMatrixView* view)
{
    // Host the editor-owned ModMatrixView directly as the BOTTOM-RIGHT panel
    // (single content, no tab bar). NON-owned (the editor retains ownership via
    // modMatrixView_): addAndMakeVisible reparents without transferring
    // ownership, exactly like the reparented ParamPages, so the teardown order
    // (synthWorkspace_ before modMatrixView_) stays safe.
    modMatrixView_ = view;
    if (view != nullptr)
        addAndMakeVisible (*view);
}

//==============================================================================
void SynthWorkspace::showGenerator (int modSrcEnum)
{
    const auto it = generators_.find (modSrcEnum);
    if (it == generators_.end() || it->second.page == nullptr)
        return;

    auto* page = it->second.page;

    // Reparent the page into the active-editor host (the page is NEVER
    // regenerated — only its parent + visible-group subset change). The prior
    // page is detached (non-owned: the Viewport drops it without deleting, as
    // it was attached with delete-on-remove = false). setViewedComponent also
    // resets the scroll to the top, so every generator swap starts at the page
    // head instead of inheriting the previous page's scroll offset.
    if (activePage_ != page)
    {
        activeEditorHost_->setViewedComponent (page, false);
        activePage_ = page;
    }

    // Show just this generator's group subset (EMPTY array => ALL groups, e.g.
    // ARP). The stored array is passed through unchanged so a multi-group entry
    // (e.g. the Note Sequencer's "Note Pitch" + "Note Velocity") reveals every
    // group it names.
    page->setVisibleGroups (it->second.groups);

    reflowActiveEditor();
}

void SynthWorkspace::setActiveGenerator (int modSrcEnum)
{
    showGenerator (modSrcEnum);
    modBar_->setActiveGenerator (modSrcEnum);   // underline-glow the pill
    if (onActiveGenChanged_)
        onActiveGenChanged_ (modSrcEnum);
}

void SynthWorkspace::releaseActiveEditor()
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

void SynthWorkspace::reflowActiveEditor()
{
    if (activePage_ == nullptr)
        return;
    const auto b = activeEditorHost_->getLocalBounds();
    if (b.isEmpty())
        return;
    // Anchor at the Viewport origin (setViewedComponent resets the scroll, and
    // setSize below preserves the top-left, so this is only load-bearing on the
    // first layout after a swap).
    activePage_->setTopLeftPosition (0, 0);
    // Lay the page out at the FULL view width first. reflowToWidth grows the
    // page to at least the view height, so a page that FITS produces exactly
    // the old plain-host layout: no scrollbar, no width change. Only when the
    // page's natural height overflows the view is it re-laid one
    // scrollbar-thickness narrower so the vertical scrollbar never covers the
    // right edge (the FxMatrixView precedent subtracts the thickness
    // unconditionally; here it is subtracted only when it actually scrolls).
    activePage_->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
    if (activePage_->getHeight() > b.getHeight())
        activePage_->reflowToWidth (juce::jmax (150, b.getWidth()
                                                      - activeEditorHost_->getScrollBarThickness()),
                                    juce::jmax (0, b.getHeight()));
}

//==============================================================================
int SynthWorkspace::barPreferredWidth() const
{
    return modBar_ != nullptr ? modBar_->preferredWidth() : 0;
}

//==============================================================================
void SynthWorkspace::paint (juce::Graphics& g)
{
    // Flat windowBackground fill so any integer-division remainder between the
    // rigid cells (or a short page) never bleeds the default component colour.
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

namespace
{
// Bottom-row cap: exactly 4 rows visible in the bottom-right matrix view -
// 2*4 outer inset + 22 header + 4 gap + 4 first-row inset + 4 * (row + gap).
// The rows scroll inside the view's own Viewport, so a longer routing list
// stays reachable; the freed height goes to the top (synth/fx) row.
constexpr int kBottomRowMaxH = 8 + 22 + 4 + 4 + 4 * (ModMatrixView::kRowHeight + 4);
}

void SynthWorkspace::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    // ---- 3 rows: TOP (main) | MIDDLE (bar) | BOTTOM (generators | matrix) ----
    // The bottom row keeps the height it would have WITH the bar shown and is
    // capped at kBottomRowMaxH (exactly 4 matrix rows + chrome); everything
    // else - including the freed bar strip when [MOD] hides the seam - goes to
    // the TOP row, so toggling the pill bar grows the synth section only (the
    // envelope/matrix bottom section keeps its size). Mirrored by FxWorkspace
    // so SYNTH<->FX never reflows on the difference. (The pill bar is a
    // power-user surface, not a requirement — modulation is also reachable via
    // the Mod Matrix rows and tap-to-assign.)
    constexpr float kBottomShare = 0.60f;
    const int withBarH = juce::jmax (0, area.getHeight() - CentralModBar::kBarHeight);
    const int bottomH = juce::jmin (juce::roundToInt (static_cast<float> (withBarH) * kBottomShare),
                                    kBottomRowMaxH);
    const int barH = modBarVisible_ ? CentralModBar::kBarHeight : 0;
    const int mainH = juce::jmax (0, area.getHeight() - barH - bottomH);

    auto mainRow  = area.removeFromTop (mainH);
    auto barRow   = area.removeFromTop (barH);
    auto bottomRow = area;   // exactly bottomH (mainH consumes the rest)

    // ---- Main row columns: OSCILLATORS 40% | MIXER 20% | FILTER 40% ----
    // The columns live inside topRowHost_ (viewed by topRowViewport_): each
    // page is laid at its column rect and reflowed to the column width with
    // the viewport height as the minimum, so a fitting page fills the view
    // exactly (byte-identical to the old direct layout) and an over-tall page
    // makes the host scroll. When the host does scroll it is re-laid one
    // scrollbar-thickness narrower so the scrollbar never covers the FILTER
    // column's right edge.
    topRowViewport_->setBounds (mainRow);
    // NOTE: mainRow (NOT Viewport::getViewWidth()) is the width source — the
    // viewport caches its visible area and can be stale mid-cascade, which
    // collapsed the first layout to the 150pt floor.
    const int rowW = juce::jmax (150, mainRow.getWidth());
    const int rowH = juce::jmax (0, mainRow.getHeight());
    auto layoutTopRow = [&] (int w)
    {
        // SPACIOUS + HARMONIZED layout (2026-08-20; gaps unified 2026-08-23):
        // a uniform kRowGap margin is taken off ALL FOUR sides of the row,
        // and the columns are separated by kColGap — the SAME inter-module
        // value the FX page uses — so the bordered OSC/MIX/FILTER
        // GroupComponent panels sit in whitespace exactly like the FX page's
        // cards and switching SYNTH<->FX keeps one visual rhythm. (The panels'
        // own header padding INSIDE each GroupComponent is ParamPage/L&F
        // territory and is unchanged; this seam adds the AROUND-panel
        // breathing room.) The 40/20/40 split is preserved against the column
        // budget (inner width minus the two inter-column gaps).
        auto hostRow = juce::Rectangle<int> (0, 0, w, rowH).reduced (kRowGap);
        const int colBudget = juce::jmax (0, hostRow.getWidth() - 2 * kColGap);
        auto oc = hostRow.removeFromLeft (colBudget * 40 / 100);
        hostRow.removeFromLeft (kColGap);
        auto mc = hostRow.removeFromLeft (colBudget * 20 / 100);
        hostRow.removeFromLeft (kColGap);
        auto fc = hostRow;
        auto sizeDirect = [] (ParamPage* page, const juce::Rectangle<int>& b,
                              int fullRowH)
        {
            if (page == nullptr)
                return 0;
            // Reflow against the FULL row height (not the inset band): a
            // fitting page grows to at least the view height, and the reduced
            // band's 2*kRowGap of vertical padding stays as breathing room
            // instead of forcing the page taller than its host (reflowToWidth
            // ends with setSize(w, contentHeight_), which would otherwise
            // override the inset band's height — so re-position AFTER).
            page->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, fullRowH));
            page->setTopLeftPosition (b.getPosition());
            // Report the page's BOTTOM edge (position + height): the inset
            // band starts kRowGap down, so the host must account for the top
            // inset too or the page escapes it by exactly that amount.
            return b.getY() + page->getHeight();
        };
        return juce::jmax (juce::jmax (sizeDirect (mainOscPage_,   oc, rowH),
                                       sizeDirect (mainLeftPage_,  mc, rowH)),
                            juce::jmax (sizeDirect (mainRightPage_, fc, rowH), rowH));
    };
    int hostH = layoutTopRow (rowW);
    if (hostH > rowH)
        hostH = layoutTopRow (juce::jmax (150, rowW - topRowViewport_->getScrollBarThickness()));
    topRowHost_->setSize (rowW, hostH);

    // ---- Middle seam: full-width bar ----
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
                                 barRow.getWidth(), 1 + parvati::kRuleShadowH);
    }

    // ---- Bottom row: LEFT 50% = active editor, RIGHT 50% = ModMatrixView ----
    auto modLeft  = bottomRow.removeFromLeft (bottomRow.getWidth() / 2);
    auto modRight = bottomRow;

    activeEditorHost_->setBounds (modLeft);
    reflowActiveEditor();

    if (modMatrixView_ != nullptr)
        modMatrixView_->setBounds (modRight);   // its resized() lays out the rows
}

//==============================================================================
void SynthWorkspace::applyThemeColors()
{
    if (mainOscPage_   != nullptr) mainOscPage_->applyThemeColors();
    if (mainLeftPage_  != nullptr) mainLeftPage_->applyThemeColors();
    if (mainRightPage_ != nullptr) mainRightPage_->applyThemeColors();

    if (modBar_ != nullptr)
        modBar_->applyThemeColors();

    // The active generator page re-themes here; non-active pages are themed by
    // the editor's generatedPages_ pass in applyAllColoursFromTheme.
    if (activePage_ != nullptr)
        activePage_->applyThemeColors();

    if (modMatrixView_ != nullptr)
        modMatrixView_->applyThemeColors();

    repaint();
}
