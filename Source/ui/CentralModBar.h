// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// CentralModBar — a self-contained, single-row strip of micro-pills, one per
// MOD_SRC_* modulation source, grouped into the 7 ModSourceCatalog clusters
// (Env / Lfo / SeqArp / Perf / Util / Mod / Const). It is the central hub for
// Picking up a modulation source: click a generator pill to select it, or drag
// ANY pill onto a destination knob to assign it (the drag carries the same
// "parvatiModSrc:<enum>" payload the generator tabs / matrix grip / wheel
// captions already emit, so the existing drop feedback works unchanged).
//
// PHASE 1: this component exists and builds, but is NOT yet wired into the
// editor layout (that is a separate follow-up phase). It owns no APVTS state —
// it only reads theme colours and emits clicks/drags.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ModSourceCatalog.h"   // parvati::Cluster (per-segment family colour in paintSegments)

#include <functional>
#include <memory>
#include <vector>

#include "ParvatiTheme.h"
#include "ThemeManager.h"

//==============================================================================
class CentralModBar : public juce::Component
{
public:
    /** Fixed total bar height (host sets the component height to this). */
    // A taller bar (82pt) hosts ~1.5x bigger (72pt) pills grouped into labelled
    // category segments, and the pill row scrolls horizontally inside a
    // juce::Viewport so 25+ 72pt pills never widen the editor. Scrolling is driven
    // by prominent `<` / `>` nav pills (not a scrollbar). kPillH / kPillGap are
    // exposed here so the sizing-contract test can assert them. Single UI on all
    // platforms (the former compact desktop bar is gone).
    static constexpr int kBarHeight = 92;   // taller bar: ~1.5x pills + a coloured label tab above them + nav arrows
    static constexpr int kPillH     = 72;   // ~1.5x bigger blips (was 50)
    static constexpr int kPillGap   = 8;    // minimum pill spacing

    explicit CentralModBar (ThemeManager& themeManager);
    ~CentralModBar() override;

    //==========================================================================
    // Click callback — fired on a real click (no drag) on any pill, with the
    // MOD_SRC_* enum value of the clicked pill.
    void setOnPillClicked (std::function<void (int modSrcEnum)> cb);

    /** Highlights the given GENERATOR pill's underline glow (-1 clears). */
    void setActiveGenerator (int modSrcEnum);

    /** Re-resolve every pill's accent colour from the active theme; call after
        a theme switch (and once from the ctor). */
    void applyThemeColors();

    /** The true minimum width to show every pill with no clipping (the host
        uses this as the window minimum width). */
    int preferredWidth() const;

    //==========================================================================
    // Public accessors used by the file-local pill component.
    const ParvatiTheme& theme() const;
    juce::Font          pillFont() const;   // font used to draw + measure pills

    //==========================================================================
    void resized() override;

private:
    /** File-local pill component (defined in the .cpp). */
    struct ModPill;
    friend struct ModPill;

    /** Forwards a pill click to the registered callback. */
    void invokeClicked (int modSrcEnum);

    /** The scrolled content of the horizontal Viewport (painted: segment
        backgrounds + cluster labels), defined in the .cpp. */
    struct PillContent;

    /** Draws the per-cluster segment backgrounds + short labels on the scrolled
        content (coordinates match computeLayout's pill positions). */
    void paintSegments (juce::Graphics&) const;

    /**
        Lays the 7 clusters out left -> right. When @p positionChildren is true
        the pills are positioned; either way the total preferred width is
        returned. Clusters are separated only by the inter-cluster gap (no
        caption — the family-coloured underline identifies each cluster).
        On iOS each cluster is wrapped in a labelled segment background and the
        pill row scrolls inside a Viewport.
    */
    int computeLayout (bool positionChildren) const;

    ThemeManager& themeManager_;
    std::function<void (int)> onPillClicked_;

    std::vector<std::unique_ptr<ModPill>> pills_;
    int activeEnum_ = -1;

    // Horizontal-scroll wrapper + its viewed content (pills are children of the
    // content, not of this bar, so the bar clips/scrolls them). PillContent is
    // defined in the .cpp; forward-declared above.
    std::unique_ptr<juce::Viewport> viewport_;
    std::unique_ptr<PillContent>    pillContent_;

    // `<` / `>` nav pills that page-scroll the viewport (replacing the scrollbar).
    std::unique_ptr<juce::ShapeButton> navPrev_;
    std::unique_ptr<juce::ShapeButton> navNext_;
    void scrollPills (int deltaPx);   // page-scroll the viewport by deltaPx (clamped)
    void updateNavEnabled();          // enable/disable nav buttons at the scroll ends

    // Per-cluster segment background rects + short labels (populated by
    // computeLayout, read by paintSegments). Mutable because
    // computeLayout/preferredWidth are const.
    mutable juce::Array<juce::Rectangle<int>> segmentRects_;
    mutable juce::Array<juce::String>         segmentLabels_;
    mutable juce::Array<parvati::Cluster>     segmentClusters_;   // per-segment cluster (the tab's family colour)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CentralModBar)
};
