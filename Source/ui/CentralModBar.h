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
// It additionally renders the LIVE modulation feedback pill strips: a subtle
// family-coloured sparkline of the RECENT values each source produced (the
// Pigments-style history indicator), fed by a telemetry provider the editor
// binds to SynthEngine::readUiTelemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
// With no provider set the strips simply never populate and the bar renders
// exactly as before.
//
// PHASE 1: this component exists and builds, but is NOT yet wired into the
// editor layout (that is a separate follow-up phase). It owns no APVTS state —
// it only reads theme colours and emits clicks/drags (plus the optional
// telemetry strips above).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ModSourceCatalog.h"   // parvati::Cluster (per-segment family colour in paintSegments)
#include "ModTelemetryTypes.h"  // parvati::ModTelemetrySnapshot (live history-strip telemetry)

#include <functional>
#include <memory>
#include <vector>

#include "ParvatiTheme.h"
#include "ThemeManager.h"

//==============================================================================
class CentralModBar : public juce::Component,
                      private juce::Timer   // ONE bar-wide telemetry poll (never per pill)
{
public:
    /** Fixed total bar height (host sets the component height to this). */
    // A 78pt bar hosts compact (56pt) pills grouped into labelled category
    // segments, and the pill row scrolls horizontally inside a juce::Viewport
    // so 25+ pills never widen the editor. Scrolling is driven by prominent
    // `<` / `>` nav pills (not a scrollbar). kPillH / kPillGap are exposed here
    // so the sizing-contract test can assert them. Single UI on all platforms
    // (the former compact desktop bar is gone). 56pt keeps the pill a full
    // HIG 44pt+ touch target while reclaiming vertical space for the content
    // rows (the bar is a fixed seam in both workspaces; see also the [MOD]
    // header toggle, which collapses the seam entirely).
    static constexpr int kBarHeight = 78;   // label tab (14) + gap (4) + 56pt pills + insets
    static constexpr int kPillH     = 56;   // compact pills (was 72 — still >= 44pt HIG touch target)
    static constexpr int kPillGap   = 8;    // minimum pill spacing
    static constexpr int kNavHitW   = 44;   // < > nav hit-band width (F-ios-touch-1: the ONLY pill-band scrollers — HIG floor; the visible chevron glyph stays visually small inside it)

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
    // ---- Live modulation telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The bar polls ONE engine frame per tick through @p fetch (a false return
    // = torn seqlock read or a stale reset epoch — the strips hide until valid
    // data returns) and pushes each source's recent history into its pill as
    // the bottom-strip sparkline. Repaints are per-pill and bounded to the
    // strip rect only; signature-identical frames cost nothing.
    void setTelemetryProvider (std::function<bool (parvati::ModTelemetrySnapshot&)> fetch);

    /** Animation cadence for the strips (Hz). Valid rates clamp to 5..60; 0
        DISABLES the poll entirely. Takes effect immediately (restarts the
        timer if it is running). */
    void setTelemetryRateHz (int hz);
    int  telemetryRateHz() const noexcept { return telemetryRateHz_; }

    /** Hides every pill's history strip (invalid / reset telemetry). Cheap
        when already clear; the strips return on the next valid frame. */
    void clearTelemetry();

    // TEST-ONLY: bumped whenever a pill repaints because its telemetry-driven
    // strip data changed (including the change TO "no data" on a clear), so a
    // headless test can observe "the strip reacted" without painting.
    int telemetryGeneration() const noexcept { return telemetryGeneration_; }

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
    std::unique_ptr<juce::TextButton> navPrev_;
    std::unique_ptr<juce::TextButton> navNext_;
    // Per-button font override for the two tall nav pills (see NavButtonLnf).
    std::unique_ptr<juce::LookAndFeel> navLnf_;
    void scrollPills (int deltaPx);   // page-scroll the viewport by deltaPx (clamped)
    void updateNavEnabled();          // enable/disable nav buttons at the scroll ends

    // juce::Timer — the single bar-wide telemetry poll (one timer, never per
    // pill). See updateTelemetryTimer for the visibility gate.
    void timerCallback() override;

    // F-ios-perf-3 dual-hook visibility gate (see EnvelopeDisplay.cpp's
    // updatePollTimer for the canonical form + frozen-preview rationale):
    // visibilityChanged fires while still unparented and never again from
    // ancestor changes, while parentHierarchyChanged recurses on every
    // hierarchy change including the editor gaining its peer. BOTH funnel
    // into updateTelemetryTimer so a hidden bar never polls.
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void updateTelemetryTimer();

    // Per-cluster segment background rects + short labels (populated by
    // computeLayout, read by paintSegments). Mutable because
    // computeLayout/preferredWidth are const.
    mutable juce::Array<juce::Rectangle<int>> segmentRects_;
    mutable juce::Array<juce::String>         segmentLabels_;
    mutable juce::Array<parvati::Cluster>     segmentClusters_;   // per-segment cluster (the tab's family colour)

    // ---- live telemetry state (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The fetcher the editor binds (typically engine.readUiTelemetry through
    // the LiveFeedbackHub). The frame is consumed INSIDE timerCallback only
    // (the per-pill caches hold what is actually drawn), so no member copy is
    // retained — a ~4 KB per-tick store nobody read was removed (review nit).
    std::function<bool (parvati::ModTelemetrySnapshot&)> telemetryFetch_;
    int telemetryRateHz_     = 30;   // 5..60, 0 = disabled (setTelemetryRateHz clamps)
    int telemetryGeneration_ = 0;    // TEST-ONLY (see telemetryGeneration())

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CentralModBar)
};
