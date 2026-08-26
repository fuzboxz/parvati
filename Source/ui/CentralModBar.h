// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// CentralModBar — a self-contained, single-row strip of micro-pills, one per
// MOD_SRC_* modulation source, grouped into the 7 ModSourceCatalog clusters
// (Env / Lfo / SeqArp / Perf / Util / Mod / Const). It is the central hub for
// Picking up a modulation source: click a generator pill to select it, or drag
// ANY pill onto a destination knob to assign it (the drag carries the same
// "parvatiModSrc:<enum>" payload the generator tabs / matrix grip / wheel
// captions already emit, so the existing drop feedback works unchanged).
//
// It also renders the LIVE modulation feedback pill strips: a subtle
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
#include "ModSourceCatalog.h"   // hellcat::Cluster (per-segment family colour in paintSegments)
#include "ModTelemetryTypes.h"  // hellcat::ModTelemetrySnapshot (live history-strip telemetry)

#include <functional>
#include <memory>
#include <vector>

#include "HellcatTheme.h"
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
    static constexpr int kBarHeight = 86;   // 8px sep clearance + label tab (14) + gap 4 + 56pt pills + 4px bottom inset (2026-08-21)
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
    void setTelemetryProvider (std::function<bool (hellcat::ModTelemetrySnapshot&)> fetch);

    /** Animation cadence for the strips (Hz). Valid rates clamp to 5..60; 0
        DISABLES the poll entirely. Takes effect immediately (restarts the
        timer if it is running). */
    void setTelemetryRateHz (int hz);
    int  telemetryRateHz() const noexcept { return telemetryRateHz_; }

    /** Hides every pill's history strip (invalid / reset telemetry). Cheap
        when already clear; the strips return on the next valid frame. */
    void clearTelemetry();

    /** Appends elapsed since the freshest fetched telemetry sample, in
        FRACTIONAL append units ((now - fetchTime) * kAppendHz, clamped).
        Pills multiply by their own px/append to draw the sub-bin scroll
        offset — the rendered position becomes a pure function of wall time,
        immune to fetch/tick jitter (2026-08-22 uniform-scroll fix). Returns 0
        when the engine ring has not moved for 250 ms (audio stopped). */
    double telemetryAppendsSinceFetch() const;

    // VBlank driver (see updateTelemetryTimer): the display-synced animation
    // tick. Private; declared here beside its fallback (timerCallback).
    // TEST-ONLY: which animation driver is live (true = the vsync
    // VBlankAttachment; false = the 60 Hz Timer fallback / none).
    bool usingVBlankDriver() const noexcept { return usingVBlank_; }

    // TEST-ONLY: bumped whenever a pill repaints because its telemetry-driven
    // strip data changed (including the change TO "no data" on a clear), so a
    // headless test can observe "the strip reacted" without painting.
    int telemetryGeneration() const noexcept { return telemetryGeneration_; }

    // ---- TEST-ONLY paint-split seams (modbar_pill_paint_split_test) ----
    // The 2026-08-23 label/strip paint split: each pill's sparkline and label
    // are CHILD components (ModPill itself paints only the cheap chrome).
    // These expose the @p pillIndex-th pill's children (pill order == the
    // ModSourceCatalog order) as plain Components — nullptr when out of
    // range — plus the label child's REAL paint count (0 until a genuine
    // paint cycle runs; the split's contract is that strip animation NEVER
    // increments it).
    juce::Component* pillStripChildForTest (int pillIndex) const;
    juce::Component* pillLabelChildForTest (int pillIndex) const;
    int pillLabelPaintCountForTest (int pillIndex) const;
    int pillStripPaintCountForTest (int pillIndex) const;
    /** The @p pillIndex-th pill itself (the ModPill component; nullptr when
        out of range) — lets the headless paint-split test drive pill mouse
        paths (hover) exactly as a real pointer does. */
    juce::Component* pillComponentForTest (int pillIndex) const;

    /** Re-evaluate the strip-poll timer's run state NOW (public twin of the
        private visibility-hook gate). The EDITOR's status timer calls this
        every ~30 Hz tick: JUCE's visible-before-desktop / content-then-peer
        sequencing does not guarantee the bar's own visibilityChanged /
        parentHierarchyChanged hooks fire once the window appears, and a poll
        that never started renders the whole live-strip feature dead (the
        shipped-invisible bug the [25] e2e check pins). Idempotent: the
        gate itself decides start vs stop from the live isShowing() state. */
    void reassertTelemetryTimer() { updateTelemetryTimer(); }

    //==========================================================================
    // Public accessors used by the file-local pill component.
    const HellcatTheme& theme() const;
    juce::Font          pillFont() const;   // font used to draw + measure pills

    //==========================================================================
    void resized() override;
    // The bar's OWN background fill (2026-08-23 opaque-bar pass — see the .cpp
    // ctor): the bar promises JUCE it covers every pixel (setOpaque), so this
    // must fillAll the full bounds. Required by the base Component::paint
    // contract (an opaque component without a full-coverage paint asserts).
    void paint (juce::Graphics&) override;

private:
    // The two animation drivers (see updateTelemetryTimer): vsync primary,
    // Timer fallback. Both funnel into the shared fetch-cadence tick.
    void vblankTick();
    void telemetryTick();
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
    mutable juce::Array<hellcat::Cluster>     segmentClusters_;   // per-segment cluster (the tab's family colour)

    // ---- live telemetry state (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The fetcher the editor binds (typically engine.readUiTelemetry through
    // the LiveFeedbackHub). The frame is consumed INSIDE timerCallback only
    // (the per-pill caches hold what is actually drawn), so no member copy is
    // retained — a ~4 KB per-tick store nobody read was removed (review nit).
    std::function<bool (hellcat::ModTelemetrySnapshot&)> telemetryFetch_;
    int telemetryRateHz_     = 30;   // 5..60, 0 = disabled — the FETCH rate
    int telemetryGeneration_ = 0;    // TEST-ONLY (see telemetryGeneration())
    int tickCounter_         = 0;   // fetch decimation vs the display rate

    // ---- VSYNC-DRIVEN ANIMATION (2026-08-22, the proper fix for chuggy
    // strips): a juce::VBlankAttachment fires on every display refresh of the
    // bar's peer WITH the vsync timestamp — the animation tick runs there
    // (display-locked, no Timer jitter, no ms quantization). The FETCH still
    // runs at telemetryRateHz_ (decimated from the vblank cadence), and the
    // sub-bin scroll offset is extrapolated from the SAME vsync timestamp,
    // so the rendered position is continuous in TIME and aligned with the
    // compositor. The fallback Timer (headless tests / pre-peer) stays. ----
    std::unique_ptr<juce::VBlankAttachment> vblank_;
    bool  usingVBlank_     = false;   // TEST-ONLY: which driver is live
    int   vblankFetchDiv_  = 2;       // vblanks per fetch (displayHz/telemetryHz)
    double lastVblankMono_ = 0.0;     // watchdog: last vsync callback (seconds)

    // ---- wall-clock scroll anchors (see timerCallback + telemetryAppendsSinceFetch): the ring head unwrapped
    // into a monotonic append count + the fetch/motion timestamps that turn
    // the paint-time sub-bin offset into a pure function of TIME. ----
    double telUnwrappedAppends_ = 0.0;
    int    telPrevHead_         = 0;
    bool   telHaveHead_         = false;
    double telLastFetchMono_    = 0.0;   // seconds (mono clock)
    double telLastMotionMono_   = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CentralModBar)
};
