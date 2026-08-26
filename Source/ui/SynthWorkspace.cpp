// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.  See SynthWorkspace.h.

#include "SynthWorkspace.h"

#include "ChromeRule.h"         // hellcat::ChromeRule (the bottom-row seam rule)
#include "ModMatrixView.h"
#include "ParamPage.h"        // ParamPage complete type
#include "HellcatLookAndFeel.h"   // hellcat::isY2kTheme / themeFor (the Y2K matrix gap)

//==============================================================================
SynthWorkspace::SynthWorkspace (ThemeManager& themeManager)
    : GeneratorHostWorkspace (themeManager)
{
    // The synth-page bottom seam: a 1px rule at the BOTTOM row's top edge —
    // the border between the cards panel above and the bottom page that
    // hosts the mod matrix. The falloff falls ABOVE the line (onto the cards
    // panel), so the bottom row reads raised. SYNTH ONLY (the FX workspace
    // keeps no seam) and on EVERY theme; same ChromeRule family as the
    // editor chrome rules.
    bottomRule_ = std::make_unique<hellcat::ChromeRule> (false, true, true);
    addAndMakeVisible (*bottomRule_);
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
    // The shared splitRows() caps the bottom row and gives every freed strip
    // (including the hidden bar) to the TOP row (see GeneratorHost.h).
    const auto rows   = splitRows (area, kBottomRowMaxH);
    auto mainRow      = area.removeFromTop (rows.main);
    auto barRow       = area.removeFromTop (rows.bar);
    auto bottomRow    = area;   // exactly rows.bottom (main consumes the rest)

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

    // ---- Middle seam + bottom row: the shared helpers (GeneratorHost.h) ----
    layoutBarSeam (barRow);
    layoutBottomRow (bottomRow, modMatrixView_);

    // The bottom seam rule: full workspace width at the bottom row's top
    // edge. shadowBelow=false puts the 1px line AT the seam and the falloff
    // ABOVE it (onto the cards panel); the bounds start kRuleShadowH higher
    // so the line row is unchanged. toFront(false) keeps it above the row's
    // children (the editor host + the matrix view are added later).
    if (bottomRule_ != nullptr)
    {
        bottomRule_->setBounds (bottomRow.getX(), bottomRow.getY() - hellcat::kRuleShadowH,
                                bottomRow.getWidth(), 1 + hellcat::kRuleShadowH);
        bottomRule_->toFront (false);
    }

    // Y2K: the 3px gap around the mod matrix (see applyY2kMatrixGap).
    if (modMatrixView_ != nullptr)
    {
        matrixFlushBounds_ = modMatrixView_->getBounds();
        applyY2kMatrixGap();
    }
}

void SynthWorkspace::applyY2kMatrixGap()
{
    if (modMatrixView_ == nullptr || matrixFlushBounds_.isEmpty())
        return;
    if (hellcat::isY2kTheme (hellcat::themeFor (*this)))
        modMatrixView_->setBounds (matrixFlushBounds_.reduced (6));
    else
        modMatrixView_->setBounds (matrixFlushBounds_);
}

//==============================================================================
void SynthWorkspace::applyThemeColors()
{
    if (mainOscPage_   != nullptr) mainOscPage_->applyThemeColors();
    if (mainLeftPage_  != nullptr) mainLeftPage_->applyThemeColors();
    if (mainRightPage_ != nullptr) mainRightPage_->applyThemeColors();

    // A theme switch re-resolves the Y2K matrix gap (applyThemeColors runs on
    // the selection, not only on a resize).
    applyY2kMatrixGap();

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
