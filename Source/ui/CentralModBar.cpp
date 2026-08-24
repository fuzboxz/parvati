// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See CentralModBar.h.
//
// Layout geometry (px):
//   kBarHeight = 78, pill height 56 -> top/bottom inset (single UI, all platforms).
//   Pills are left-aligned within each cluster; clusters are separated ONLY by
//   the inter-cluster gap (no caption — the family colour identifies the
//   cluster instead, via each pill's persistent family-coloured underline).
//   Each cluster is wrapped in a labelled segment background and the pill row
//   scrolls horizontally inside a juce::Viewport so 25+ pills never widen the
//   editor.
//   preferredWidth() returns the exact width needed so every pill fits with no
//   clipping.

#include "CentralModBar.h"

#include <array>
#include <cmath>

#include "ModSourceCatalog.h"
#include "ModTelemetryTypes.h"   // parvati::isBipolarModSource (strip polarity)
#include "ParvatiLookAndFeel.h"   // appFont() when the bar is reparented
#include "ParvatiTheme.h"

namespace
{
    // kPillH / kPillGap live as PUBLIC members (CentralModBar::kPillH /
    // kPillGap = 72/8) so the sizing-contract test can assert them; they are
    // referenced unqualified below and resolve to the static members from
    // within CentralModBar's members.
    constexpr int kPillHPad        = 8;    // horizontal padding inside a pill
    constexpr int kPillMinW        = 36;   // minimum pill width (wider floor for the bigger pills)
    constexpr int kClusterGap      = 20;   // gap between clusters
    constexpr int kSideGap         = 40;   // larger gap splitting generators (L) from drag-only (R)
    constexpr int kEdgePad         = 6;    // left/right outer padding

    // Category-segment geometry: each cluster is wrapped in a labelled rounded
    // segment background, and the pill row scrolls horizontally so 25+ pills
    // never widen the editor. Vertical budget (2026-08-21, user-specified): a
    // SYMMETMETRIC 4px above the label tab and below the pills (was zero bottom
    // inset — the pills touched the bar's bottom edge). Horizontal paddings
    // stay at their original tight values (no extra whitespace requested).
    constexpr int kSegPad        = 3;    // segment-edge horizontal padding around the pills
    constexpr int kSegVPad       = 8;    // top inset above the coloured tab (2026-08-21: separator-vs-tab breathing room)
    constexpr int kSegBottomPad  = 4;    // bottom inset below the pills (symmetric with the top)
    constexpr int kLabelTabH     = 14;   // coloured label-tab header height (above the pills)
    constexpr int kLabelTabGap    = 4;    // gap between the label tab and the pills
    static_assert (kSegVPad + kLabelTabH + kLabelTabGap + CentralModBar::kPillH + kSegBottomPad
                   == CentralModBar::kBarHeight, "mod-bar vertical budget must equal kBarHeight");

    // `<` / `>` nav scrollers that flank the viewport (replacing the scrollbar).
    // QUIET chrome (2026-08, F-ios-touch-1 2026-08-19): the VISUAL glyph is a
    // bare dim 30pt chevron that only lights on hover/press — but the BUTTON
    // itself now carries a full 44x44 HIG hit band (they are the ONLY way to
    // scroll the overflowing mod-source band; a 30x30 target beside 56pt
    // pills was a mis-tap magnet that landed on a PILL and switched generator
    // pages). The TextButton centres its '<'/'>' label in the wider band, so
    // the rendered chrome stays identical in weight.
    constexpr int kNavW   = CentralModBar::kNavHitW;   // public for the HIG contract test
    constexpr int kNavGap = 4;    // gap between a nav hit band and the viewport

    // ---- History-strip geometry (the live telemetry sparklines, ----
    // docs/LIVE_MOD_FEEDBACK_DESIGN.md). 2026-08-21 redesign (user request):
    // the sparkline spans the WHOLE PILL BEHIND THE LABEL — a Pigments-style
    // scope reading, not a separate bottom band. The trace is drawn BEFORE
    // the label text and kept clear of the top accent band / bottom
    // underline so those cues stay crisp. kStripMaxPts still caps the
    // downsampled polyline so both the per-tick diff gate and the stroke stay
    // O(24) per pill — the bar repaints at most the trace's rect per
    // ANIMATING pill.
    constexpr int kStripXInset = 3;    // horizontal inset inside the pill
    constexpr int kStripTopGap = 5;    // clearance below the top accent band
    constexpr int kStripBotGap = 3;    // clearance above the family underline
    // 2026-08-21 final scope (user request): the strip spans the ENTIRE pill
    // inner height — bottom to top, between the accent band and the underline
    // — with the label drawn on top (Pigments-style scope reading). A brief
    // top-band diagnostic layout confirmed the pipeline first; this restores
    // the full-pill span the design intends.
    constexpr int kStripMaxPts = 96;   // downsampled points per strip (2026-08-22: 48 -> 96
                                       // with kHistoryLen 256 — a fast LFO (~14 appends/cycle)
                                       // sampled every ~5 appends aliases into mush; 96 ≈ 1px
                                       // spacing on the pill width, the useful ceiling, and the
                                       // Bézier smoothing below rounds the rest)

    // Short cluster label drawn at the left of each segment.
    juce::String clusterShortLabel (parvati::Cluster c)
    {
        switch (c)
        {
            case parvati::Cluster::Env:    return "ENVELOPE";
            case parvati::Cluster::Lfo:    return "LFO";
            case parvati::Cluster::SeqArp: return "SEQUENCER";
            case parvati::Cluster::Mod:    return "MOD";
            case parvati::Cluster::Perf:   return "PERF";
            case parvati::Cluster::Util:   return "UTIL";
            case parvati::Cluster::Const:  return "CONST";
        }
        return {};
    }

    // Quiet nav scrollers: the `<` / `>` buttons are CHROME, not pills — no
    // tile, no accent bands. A bare chevron in dim text (textSecondary) at
    // rest, lifting to textPrimary on hover and the theme accent on press;
    // a hover/press shows only a faint rounded fill so the hit area is
    // discoverable without visual weight. Disabled dims to 40%. (Was drawn
    // exactly like a mod pill — filled tile + top accent band + underline —
    // which competed with the pills it flanks.)
    class NavButtonLnf : public ParvatiLookAndFeel
    {
    public:
        juce::Font getTextButtonFont (juce::TextButton&, int) override
        {
            return appFont (16.0f, juce::Font::bold);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                   const juce::Colour& /*backgroundColour*/,
                                   bool isMouseOverButton, bool isButtonDown) override
        {
            const ParvatiTheme* t = getTheme();
            if (t == nullptr || (! isMouseOverButton && ! isButtonDown))
                return;   // borderless at rest — the glyph alone

            const float alpha = b.isEnabled() ? 1.0f : 0.4f;
            const juce::Rectangle<float> r = b.getLocalBounds().toFloat().reduced (0.5f);

            juce::Colour fill = t->tabUnselectedBg;
            if (isButtonDown)  fill = fill.brighter (0.25f);
            else               fill = fill.brighter (0.10f);
            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (r, 4.0f);
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b,
                             bool isMouseOverButton, bool isButtonDown) override
        {
            const ParvatiTheme* t = getTheme();
            if (t == nullptr)
                return;
            const float alpha = b.isEnabled() ? 1.0f : 0.4f;
            const juce::Colour c = isButtonDown ? t->accentPrimary
                                 : isMouseOverButton ? t->textPrimary
                                                     : t->textSecondary;
            g.setColour (c.withMultipliedAlpha (alpha));
            g.setFont (getTextButtonFont (b, b.getHeight()));
            g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, true);
        }
    };
}  // namespace

//==============================================================================
// ModPill — a single modulation-source micro-pill. EVERY pill draws a persistent
// family-coloured underline (resolved from its ModSourceCatalog cluster -> the
// cat* family token): the underline now carries the cluster identity, since the
// cluster text captions were removed. The active generator pill keeps its solid
// lighter background + bright text and STRENGTHENS the underline (thicker / full
// alpha); inactive pills use a dark fill + dim text + the subtle family underline.
// All flat (no outline / glow / bevel). Geometry is unchanged from the bar layout.
// Drag-only sources (Perf/Util/Const) are identified by their family colour
// alone (the dotted left-handle was removed 2026-08-21 — it read as noise).
//
// Mouse handling mirrors the existing drag sources (DraggableTabButton /
// ModSourceDragGrip / WheelDragLabel): a drag past ~5px starts an INTERNAL
// DragAndDropContainer drag carrying "parvatiModSrc:<enum>"; a clean click (no
// drag) fires the bar's click callback. The same payload keeps the existing
// drop-side feedback (concentric rings, padlock cursor, ModMatrixHighlight)
// working with zero changes on the destination side.
//
// ModPill is a PRIVATE nested struct of CentralModBar, so its public data
// members are only reachable from CentralModBar (which needs them for layout,
// theme resolution and active-state).
//
// LIVE HISTORY STRIP (docs/LIVE_MOD_FEEDBACK_DESIGN.md): when the bar's
// telemetry tick feeds it data, the pill also draws a family-coloured
// sparkline of the source's recent values spanning the pill's inner height
// (label on top — the Pigments-style scope reading). Flat and quiet — no
// fill, no glow, no grid; the accent-on-dark contrast keeps the strip an
// indicator, not a focus. Const-cluster pills never carry one; the bar-only
// Note Sequencer sentinel DOES — its melody trace rides the spare
// kNoteSeqSlot (see the telemetry tick). With no telemetry the strip simply
// stays empty.
//
// PAINT SPLIT (2026-08-23 CPU fix, the message-thread hot spot from the
// sampled profile): the pill's paint is split across THREE components —
//   * the PILL itself paints the fill / accent band / family underline
//     (only on full repaints: hover, active change, theme switch);
//   * a strip CHILD (HistoryStripView) paints the sparkline and is the ONLY
//     thing the bar's per-tick repaint(pill.stripRect()) dirties — a sibling
//     dirty region can never reach the label, so strip animation no longer
//     re-rasters the pill tile or re-runs the label's text layout
//     (Graphics::drawText was ~63% of the sampled message-thread cost:
//     every 30-60 Hz strip tick used to re-layout the label glyphs);
//   * a label CHILD (PillLabelView) paints the short label ABOVE the strip
//     (later sibling => on top; the "label over the scope" reading is
//     unchanged) and owns a CACHED resolved font (pillFont()'s appFont /
//     FontOptions construction is not free — resolved once per L&F change
//     instead of per paint).
// Both children are fully transparent to the mouse, so hit-testing,
// clicks, drags, tooltips and accessibility are byte-identical to the
// single-component pill (pinned by modbar_pill_click_test + the new
// modbar_pill_paint_split_test).
struct CentralModBar::ModPill : public juce::Component,
                                public juce::SettableTooltipClient
{
    ModPill (CentralModBar& owner, const parvati::SourceEntry& e)
        : owner_ (owner),
          enumValue_ (e.enumValue),
          shortLabel_ (e.shortLabel),
          fullName_ (e.fullName),
          cluster_ (e.cluster),
          isGenerator_ (e.isGenerator),
          stripBipolar_ (parvati::isBipolarModSource (e.enumValue))   // display polarity (mirrors the voice mod-matrix AC coupling)
    {
        setTooltip (fullName_);
        // Accessibility-only: name the pill after its full source name (the
        // short painted label is a compact abbreviation; fullName_ is the
        // tooltip text). ModSourceCatalog names are Ambika hardware terms and
        // intentionally NOT translated (Translations.h policy).
        setTitle (fullName_);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setInterceptsMouseClicks (true, false);

        // PAINT SPLIT (2026-08-23, see the class comment): strip FIRST, label
        // SECOND — children paint after the parent and in add order, so the
        // composite reads exactly like the old single paint: fill / band /
        // underline (parent) -> sparkline -> label on top. Both children are
        // mouse-transparent (see their ctors); the pill keeps every mouse /
        // tooltip / a11y path to itself.
        stripView_ = std::make_unique<HistoryStripView> (*this);
        labelView_ = std::make_unique<PillLabelView> (*this);
        addAndMakeVisible (*stripView_);
        addAndMakeVisible (*labelView_);
    }

    // ---- PAINT-SPLIT children (2026-08-23 CPU fix; see the class comment) ----

    /** The history sparkline as its own CHILD component: the bar's per-tick
        repaint(pill.stripRect()) dirties ONLY this child, so strip animation
        never re-rasters the pill tile — and the label child, being
        BUFFERED TO AN IMAGE (see PillLabelView), answers the same dispatch
        with a cached-bitmap blit instead of a text raster (the sampled hot
        spot). Non-opaque (the pill fill shows through); fully transparent to
        the mouse (all hit-testing / clicks / drags stay on the pill). */
    struct HistoryStripView : public juce::Component
    {
        explicit HistoryStripView (ModPill& pill) : pill_ (pill)
        {
            // Named for headless test discovery (no API surface needed —
            // tests walk the component tree by name).
            setName ("modPillStrip");
            setInterceptsMouseClicks (false, false);
        }

        void paint (juce::Graphics& g) override
        {
            ++paintCountForTest_;   // TEST-ONLY seam (see the bar's accessors)
            // The child IS the pill's stripRect() (resized() keeps them
            // equal), so its local bounds carry the same geometry the old
            // single-paint drawHistoryStrip computed from stripRect().
            pill_.paintHistoryStrip (g, getLocalBounds().toFloat());
        }

        ModPill& pill_;
        int paintCountForTest_ = 0;   // TEST-ONLY: real paints of the strip
    };

    /** The pill's short label as its own CHILD, added AFTER the strip child
        so it paints ON TOP of the sparkline (the Pigments-style "label over
        the scope" reading is unchanged). Owns the CACHED resolved pill
        font: pillFont() goes through appFont()/FontOptions construction,
        which the profile showed is not free — it is resolved once per L&F
        change (lookAndFeelChanged fires on setLookAndFeel AND the editor's
        theme switch via sendLookAndFeelChange; parentHierarchyChanged
        catches the inherited-L&F case at the initial reparent) instead of
        on every paint. BUFFERED TO AN IMAGE (2026-08-23 review settlement):
        JUCE's peer paint dispatch walks the WHOLE hierarchy clipped to the
        dirty region, so a strip-tick dirty rect DOES dispatch a clipped
        paint to this sibling too (its bounds span the pill) — but with a
        cached component image the dispatch becomes a StandardCached-
        ComponentImage BLIT: this component's paint() (glyph layout, text
        raster, font use) runs only when the label's OWN state changes and
        repaint() invalidates the cache (hover / active / theme / font).
        The paint count therefore moves on real content changes only. */
    struct PillLabelView : public juce::Component
    {
        explicit PillLabelView (ModPill& pill) : pill_ (pill)
        {
            setName ("modPillLabel");   // headless test discovery
            setInterceptsMouseClicks (false, false);
            // The blit-cache (see the struct doc): turns foreign-region
            // dispatches into drawImage calls; repaint() on THIS component
            // re-renders it. Safe here because every visual input is either
            // a colour token re-resolved through lookAndFeelChanged ->
            // refreshFont() -> repaint(), or a pill-state flip that the
            // setters already route to a label repaint.
            setBufferedToImage (true);
        }

        void paint (juce::Graphics& g) override
        {
            ++paintCountForTest_;   // TEST-ONLY seam (see the bar's accessors)
            if (! fontCached_)
                refreshFont();      // defensive: no L&F hook fired before the first paint

            // Colour tier exactly as the pre-split single-paint label (git
            // parity, 2026-08-23 review): GENERATOR pills — the ACTIVE one
            // reads textPrimary, hover lifts textSecondary, rest stays
            // textSecondary; DRAG-ONLY pills are ALWAYS textSecondary (their
            // hover cue is the pill FILL lift, not the label).
            const auto& t = pill_.owner_.theme();
            g.setColour (! pill_.isGenerator_ ? t.textSecondary
                        : (pill_.active_ ? t.textPrimary
                        : (pill_.hovered_ ? t.textSecondary.brighter (0.20f)
                                          : t.textSecondary)));
            g.setFont (cachedFont_);
            // Generators centre over the FULL pill; drag-only pills keep the
            // old 5px horizontal inset (same rects as the removed branches).
            g.drawText (pill_.shortLabel_,
                        pill_.isGenerator_ ? getLocalBounds()
                                           : getLocalBounds().reduced (5, 0),
                        juce::Justification::centred, true);
        }

        void lookAndFeelChanged() override   { refreshFont(); }
        void parentHierarchyChanged() override { refreshFont(); }

        void refreshFont()
        {
            cachedFont_ = pill_.owner_.pillFont();
            fontCached_ = true;
            repaint();   // a new typeface/size needs a fresh raster
        }

        ModPill& pill_;
        // FontOptions ctor: the default Font ctor is deprecated in JUCE 9.
        // refreshFont() overwrites this before the first paint.
        juce::Font cachedFont_ { juce::FontOptions {} };
        bool fontCached_ = false;
        int paintCountForTest_ = 0;   // TEST-ONLY: real paints of the label
    };

    // Accessibility: role `button` + a `press` action wired to the SAME
    // callback a real click fires (owner_.invokeClicked), so switch-control /
    // VoiceOver activation selects this modulation source exactly like a tap.
    // No painting/layout/mouse-path change. Title comes from the component
    // title set in the ctor; drag-assign remains pointer-only (the internal
    // DnD drag is not expressible as an a11y action).
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this,
                juce::AccessibilityRole::button,
                juce::AccessibilityActions().addAction (
                    juce::AccessibilityActionType::press,
                    [this] { owner_.invokeClicked (enumValue_); }));
    }

    /** Preferred width for this pill given the pill font. */
    int widthForFont (const juce::Font& f) const
    {
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, shortLabel_);
        return juce::jmax (kPillMinW, textW + 2 * kPillHPad);
    }

    void setActive (bool a)
    {
        // The label child repaints TOO: its text tier follows active_
        // (textPrimary vs textSecondary — the paint split moved the label
        // into its own component, so a pill repaint no longer re-rasters it).
        if (a != active_) { active_ = a; repaint(); if (labelView_) labelView_->repaint(); }
    }

    // Positions the paint-split children (the pill's bounds are set by the
    // bar's computeLayout; children follow on every resize). The strip child
    // IS the strip rect — the bar's per-tick animation repaints that child
    // alone — and the label overlays the FULL pill (both rects are the exact
    // regions the old single-paint drew into).
    void resized() override
    {
        if (stripView_ != nullptr)
            stripView_->setBounds (stripRect());
        if (labelView_ != nullptr)
            labelView_->setBounds (getLocalBounds());
    }

    /** Route a strip animation repaint to the STRIP CHILD only — the entire
        point of the paint split: the pill tile and the label (and its text
        layout) stay clean while telemetry scrolls. */
    void repaintStrip()
    {
        if (stripView_ != nullptr)
            stripView_->repaint();
    }

    // TEST-ONLY passthroughs (the child types are file-local; tests and the
    // bar's public accessors consume them as plain Components).
    juce::Component* stripChildForTest() const { return stripView_.get(); }
    juce::Component* labelChildForTest() const { return labelView_.get(); }
    int labelPaintCountForTest() const { return labelView_ != nullptr ? labelView_->paintCountForTest_ : -1; }
    int stripPaintCountForTest() const { return stripView_  != nullptr ? stripView_->paintCountForTest_  : -1; }

    /** Persistent family-coloured underline (the cluster cue now that the
        captions are gone). Subtle (thin, dim) on every pill; STRENGTHENED
        (thicker, full alpha) on the active generator. Solid flat stroke. */
    void drawFamilyUnderline (juce::Graphics& g, const juce::Rectangle<float>& r) const
    {
        const float ux = r.getX() + 3.0f;
        const float uw = r.getWidth() - 6.0f;
        const float uy = r.getBottom() - 1.5f;

        if (active_)
        {
            // Strengthened on the active generator: solid 2px full-alpha stroke.
            g.setColour (accent_);
            g.fillRect (juce::Rectangle<float> (ux, uy - 1.0f, uw, 2.0f));
        }
        else
        {
            // Subtle on inactive pills: thin 1px low-alpha stroke (identifies
            // the family without competing with the active pill).
            g.setColour (accent_.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (ux, uy, uw, 1.0f));
        }
    }

    /** Bounding rect of the history trace (pill-local). paint() draws the
        sparkline inside THIS rect and the bar's telemetry tick passes it to
        repaint(), so the bounded dirty region always covers exactly what the
        trace can draw — the GPU-cost control for the animating pills
        (docs/LIVE_MOD_FEEDBACK_DESIGN.md). FULL-PILL scope (2026-08-21 final):
        bottom to top of the pill, clear of the top accent band and the bottom
        family underline; the label text draws ON TOP of the trace (the
        Pigments-style scope reading — confirmed visible in the real app). */
    juce::Rectangle<int> stripRect() const
    {
        auto r = getLocalBounds();
        r.removeFromTop (kStripTopGap);
        r.removeFromBottom (kStripBotGap);
        return { r.getX() + kStripXInset, r.getY(),
                 r.getWidth() - 2 * kStripXInset, r.getHeight() };
    }

    /** Renders the FULL history window (kHistoryLen slots, OLDEST→NEWEST)
        into the cached MIN-MAX band — a fixed-length circular-buffer view
        (the window length NEVER changes; new values overwrite the oldest;
        never-written slots read as ZERO, so an unmodulated source shows a
        full-width zero line from the very first tick exactly like a
        modulated one, and the scroll speed is constant end to end).
        Each of the kStripMaxPts plotted points covers an AGE RANGE of raw
        samples; its cached min/max is the true amplitude envelope over that
        range (oscilloscope-style band — fast modulators alias into mush when
        point-sampled no matter the point count). Returns true when the drawn
        data changed (the caller repaints the strip rect); a signature-
        identical frame returns false and costs nothing. */
    bool updateStripFromHistory (const uint8_t* samples, int count, uint8_t currentVal)
    {
        constexpr int kWindow = parvati::ModTelemetrySnapshot::kHistoryLen;
        constexpr float kEps = 1.0f / 255.0f;   // one value quantum
        constexpr int m = kStripMaxPts;         // the window is ALWAYS full-width

        if (count > kWindow)
            count = kWindow;

        // FLAT-SKIP FAST PATH (2026-08-23 CPU fix — the idle diff-gate cost):
        // the full recompute below evaluates ~96 points x their age ranges
        // (~2.7 samples each) PER PILL PER FETCH — ~6k byte reads x 24 pills
        // x 30 Hz in Debug just to conclude "nothing changed" on a parked
        // strip. But a strip whose last signature was FLAT (every point
        // within kEps of one level L) can only change through the NEWLY
        // APPENDED bytes: the window either grows at its right edge (ring
        // not yet full) or slides (full — old bytes age out on the left,
        // which cannot break flatness), and every retained byte was already
        // ≈ L. So: no new appends (delta == 0), or all delta new bytes ≈ L
        // => the signature is provably identical — return without the
        // recompute. A single moving byte falls back to the full path (and
        // re-arms flatness only when the window settles flat again).
        if (stripFlat_ && stripRawCount_ > 0)
        {
            int delta = count - stripRawCount_;
            if (delta < 0)
                delta += kWindow;   // the linearized window slid (ring wrapped)
            if (delta == 0)
                return false;   // identical frame (same count, head unchanged upstream)
            if (delta > 0 && delta < 32)
            {
                bool clean = true;
                for (int i = count - delta; i < count; ++i)
                {
                    const float v = (float) samples[(size_t) i] * (1.0f / 255.0f);
                    if (std::fabs (v - stripFlatLevel_) > kEps) { clean = false; break; }
                }
                if (clean)
                {
                    stripRawCount_ = count;   // track the window edge or delta grows unbounded
                    return false;   // flat window stayed flat: nothing to draw differently
                }
            }
            // (delta >= 32 = a reset/wipe-scale jump: fall through to the full
            // recompute — the fast path must never guess across a discontinuity.)
        }

        float mn[m], mx[m];
        float lo = 1.0f, hi = 0.0f;
        float moment = 0.0f;   // Σ j·v[j] on the max series: POSITION-weighted signature term
        const float step = (m > 1) ? (float) (kWindow - 1) / (float) (m - 1) : (float) kWindow;
        // The UNWRITTEN prefix (older than the appended count) renders as the
        // CURRENT source value, NOT zero (2026-08-23 — "PB starts from zero
        // instead of the default value"): the always-on window starts empty at
        // launch / after a telemetry wipe and fills right-to-left at the
        // append rate; drawing the empty prefix as the floor made every
        // resting-at-mid bipolar strip (Pitch Bend rests at 128 = 50%) ramp up
        // from ZERO for a full ~3.1 s window. "The unwritten past = the
        // current state" is the honest always-on semantics (the strip shows
        // the modulator's state; a moment ago it was, as far as the frame
        // knows, what it is now) — constants, held wheels and resting bends
        // render flat at truth from the first frame, while per-voice
        // generators (sources[] == 0 at rest) keep the zero floor exactly as
        // before.
        const auto ageVal = [&] (int age) -> float   // age behind newest -> value
        {
            return (age < count)
                ? (float) samples[(size_t) (count - 1 - age)] * (1.0f / 255.0f)
                : (float) currentVal * (1.0f / 255.0f);
        };
        for (int j = 0; j < m; ++j)
        {
            // Plotted slot j ↔ absolute AGE behind the newest sample,
            // spread evenly over the whole window and pinned at both ends
            // (j == m-1 is the newest sample, age 0). The point COVERS the
            // half-step of ages around it — that range's min/max is the band.
            const float ageF = (m > 1)
                ? (float) ((m - 1) - j) * (float) (kWindow - 1) / (float) (m - 1)
                : 0.0f;
            const int ageLo = juce::jmax (0, (int) std::floor (ageF - step * 0.5f));
            const int ageHi = juce::jmin (kWindow - 1, (int) std::ceil (ageF + step * 0.5f));
            float vmin = 1.0f, vmax = 0.0f;
            for (int age = ageLo; age <= ageHi; ++age)
            {
                const float val = ageVal (age);
                vmin = juce::jmin (vmin, val);
                vmax = juce::jmax (vmax, val);
            }
            if (ageLo > ageHi)   // degenerate guard (cannot happen with m>1)
                vmin = vmax = ageVal ((int) ageF);
            mn[j] = vmin;
            mx[j] = vmax;
            lo = juce::jmin (lo, vmax);
            hi = juce::jmax (hi, vmax);
            moment += static_cast<float> (j) * vmax;
        }

        // DIFF GATE (the idle-cost control): first/last/min/max/moment on the
        // max series — a pulse sliding through an otherwise-flat window keeps
        // the same first/last/min/max but shifts the centroid, so the strip
        // follows it; a parked source (never-modulated zeros included)
        // reproduces an identical signature every tick and repaints NOTHING.
        if (m == stripCount_
            && std::fabs (mx[0] - sigFirst_) <= kEps
            && std::fabs (mx[m - 1] - sigLast_) <= kEps
            && std::fabs (lo - sigMin_) <= kEps
            && std::fabs (hi - sigMax_) <= kEps
            && std::fabs (moment - sigMoment_) <= kEps)
        {
            stripRawCount_ = count;   // signature-identical: still track the window edge
            return false;
        }

        for (int j = 0; j < m; ++j)
        {
            stripMin_[(size_t) j] = mn[j];
            stripMax_[(size_t) j] = mx[j];
        }
        stripCount_ = m;
        stripRawCount_ = count;
        sigFirst_ = mx[0];
        sigLast_  = mx[m - 1];
        sigMin_   = lo;
        sigMax_   = hi;
        sigMoment_ = moment;
        // FLAT-SKIP bookkeeping: a signature whose whole band spans <= kEps
        // is "flat at level L" — the fast path above can then vet future
        // ticks by scanning only the newly appended bytes.
        stripFlat_       = (hi - lo) <= kEps;
        stripFlatLevel_  = 0.5f * (lo + hi);
        return true;
    }

    /** Clears the cached strip. Returns true when something was actually
        drawn (i.e. a repaint is needed to erase it). */
    bool clearStrip()
    {
        if (stripCount_ == 0 && sigFirst_ < 0.0f)
            return false;   // already hidden
        stripCount_ = 0;
        stripRawCount_ = 0;
        stripMin_.fill (0.0f);
        stripMax_.fill (0.0f);
        sigFirst_ = sigLast_ = sigMin_ = sigMax_ = -1.0f;
        sigMoment_ = -1.0f;
        stripFlat_ = false;   // fast path re-arms only after a fresh flat recompute
        return true;
    }

    /** The history sparkline: a thin family-coloured polyline inside the
        strip band, just above the underline. Bipolar sources (LFO / bend /
        note) swing around the band's vertical midline (value 128 = centre);
        unipolar ones rise from the band's bottom.
        PAINT SPLIT (2026-08-23): takes the strip RECT (the strip child's
        local bounds == the pill's stripRect()) instead of computing it — the
        identical geometry, now drawn by the HistoryStripView child so the
        bar's animation repaints dirty only that child. */
    void paintHistoryStrip (juce::Graphics& g, const juce::Rectangle<float>& sr)
    {
        // Const pills are static amounts (no history); every OTHER pill has a
        // telemetry slot — including the bar-only Note Sequencer sentinel
        // (enumValue_ < 0), whose melody trace rides the spare kNoteSeqSlot
        // (see the bar's telemetry tick).
        if (cluster_ == parvati::Cluster::Const)
            return;   // Const pills carry no history
        if (stripCount_ <= 0)
            return;   // no history yet (e.g. after a reset): the band stays empty

        // Keep the stroke inside the band (see the 2026-08-21 jitter note:
        // the tick's repaint dirty region is exactly stripRect(); inset the
        // plotted range by the stroke radius + an AA hair — vertically AND
        // horizontally, so nothing clips or pokes past the dirty region).
        constexpr float kStrokeW  = 1.4f;
        constexpr float kPlotInset = kStrokeW * 0.5f + 0.6f;
        const float usable = juce::jmax (1.0f, sr.getHeight() - 2.0f * kPlotInset);

        const float x0 = sr.getX() + kPlotInset;
        const float x1 = sr.getRight() - kPlotInset;
        // SUB-BIN SCROLL SHIFT (2026-08-22 uniform-scroll fix): the snapshot
        // the bins were built from is up to one fetch period old at paint
        // time; shift the whole band LEFT by the wall-clock-extrapolated
        // appends elapsed since that fetch (fraction of a bin) so the rendered
        // position advances LINEARLY IN TIME between fetches instead of
        // lurching once per tick. The newest bin extends flat to the right
        // edge (its data is the freshest we have; the <= 1-fetch lag is
        // invisible), and everything clips to the strip rect.
        constexpr int kWindowAppends = parvati::ModTelemetrySnapshot::kHistoryLen;
        const float pxPerAppend = (x1 - x0) / (float) kWindowAppends;
        const float shiftPx = static_cast<float> (owner_.telemetryAppendsSinceFetch()) * pxPerAppend;
        // Point caches for the MIN-MAX band (see updateStripFromHistory):
        // pyMax/pyMin are the per-point amplitude envelope edges. The window
        // is CONSTANT (the full pre-zeroed ring), so points spread
        // proportionally: newest at the right, constant scroll speed, and a
        // never-modulated source renders a full-width zero line.
        float px[kStripMaxPts], pyMax[kStripMaxPts], pyMin[kStripMaxPts];
        float singleY = 0.0f;   // the lone point's y (stripCount_ == 1 dot below)
        const auto yFor = [&] (float v)
        {
            v = juce::jlimit (0.0f, 1.0f, v);
            return stripBipolar_
                ? sr.getCentreY() - (v - 0.5f) * 2.0f * (usable * 0.5f)
                : sr.getBottom() - kPlotInset - v * usable;
        };
        for (int i = 0; i < stripCount_; ++i)
        {
            const float t = (stripCount_ > 1)
                ? (float) i / (float) (stripCount_ - 1)
                : 0.5f;
            px[i] = x0 + t * (x1 - x0) - shiftPx;
            pyMax[i] = yFor (stripMax_[(size_t) i]);
            pyMin[i] = yFor (stripMin_[(size_t) i]);
            singleY = pyMax[i];
        }

        // MIN-MAX ENVELOPE RENDERING (2026-08-22, supersedes the Bézier-
        // smoothed centerline): a closed band — forward along the max edge,
        // back along the min edge — FILLED at low alpha, with the outline
        // stroked at full strength. A fast LFO/env/arp (more than ~1 cycle in
        // the window) renders as its TRUE amplitude envelope band instead of
        // aliasing into mush; a slow source has min ≈ max everywhere and the
        // two outline edges coincide — reading exactly as the plain trace
        // line at the same visual weight as before.
        if (stripCount_ == 1)
        {
            g.setColour (accent_.withAlpha (active_ ? 0.95f : 0.85f));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (
                               juce::Point<float> (x1, singleY)));
            return;
        }
        // The band polygon: forward along the max edge, back along the min
        // edge, EXTENDING the newest bin flat to the un-shifted right edge
        // (fills the <= 1-fetch gap the sub-bin shift opens), then clipped to
        // the strip rect so the shifted left edge cannot paint outside it.
        juce::Path band;
        band.startNewSubPath (px[0], pyMax[0]);
        for (int i = 1; i < stripCount_; ++i)
            band.lineTo (px[i], pyMax[i]);
        band.lineTo (x1, pyMax[stripCount_ - 1]);   // extend newest flat
        band.lineTo (x1, pyMin[stripCount_ - 1]);
        for (int i = stripCount_ - 1; i >= 0; --i)
            band.lineTo (px[i], pyMin[i]);
        band.closeSubPath();
        g.saveState();
        g.reduceClipRegion (sr.toNearestInt());
        g.setColour (accent_.withAlpha (active_ ? 0.30f : 0.24f));
        g.fillPath (band);
        g.setColour (accent_.withAlpha (active_ ? 0.95f : 0.85f));
        g.strokePath (band, juce::PathStrokeType (kStrokeW,
                          juce::PathStrokeType::JointStyle::curved,
                          juce::PathStrokeType::EndCapStyle::rounded));
        g.restoreState();
    }

    void paint (juce::Graphics& g) override
    {
        // PAINT SPLIT (2026-08-23): this paint covers ONLY the cheap chrome —
        // fill / top accent band / family underline. The sparkline lives in
        // the strip CHILD (the bar's animation repaints dirty it alone) and
        // the label in the label CHILD above it (cached font, painted only on
        // its own state changes). A full pill repaint therefore fires on
        // hover / active / theme changes only — never on strip animation.
        const auto& t = owner_.theme();
        const juce::Rectangle<float> r = getLocalBounds().toFloat().reduced (0.5f);

        if (isGenerator_)
        {
            // Active clarity: the active generator pill gets a SOLID slightly-
            // lighter background (tabSelectedBg) with HIGH-CONTRAST text
            // (textPrimary — drawn by the label child) — the solid fill is the
            // PRIMARY active cue, with NO
            // outline. Inactive pills keep the dark fill (tabUnselectedBg) +
            // dim text (textSecondary), lifted a touch on hover. Every pill
            // draws a persistent FAMILY-coloured underline (the family colour
            // replaces the removed cluster caption); the active pill's underline
            // is STRENGTHENED (thicker / full alpha). Geometry unchanged; flat.
            const juce::Colour fill = active_ ? t.tabSelectedBg
                                    : (hovered_ ? t.tabUnselectedBg.brighter (0.20f)
                                                : t.tabUnselectedBg);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);
            // Strong family-colour cue: a full-width accent band across the TOP
            // of the pill (the inverse of the underline), clipped to the rounded
            // pill so its corners follow the radius.
            g.saveState();
            g.reduceClipRegion (r.toNearestInt());
            g.setColour (active_ ? accent_ : accent_.withAlpha (0.85f));
            g.fillRect (r.withHeight (3.5f));
            g.restoreState();
            drawFamilyUnderline (g, r);
        }
        else
        {
            // drag-only: flat dark-gray fill + faint label + the persistent
            // family-coloured underline (the drag-source cue is the FAMILY
            // COLOUR band + underline; the old dotted left-handle was removed
            // 2026-08-21 — the tiny specks read as noise). No border / glow.
            // Geometry unchanged.
            const juce::Colour inactiveFill = t.tabUnselectedBg;
            const juce::Colour fill = hovered_ ? inactiveFill.brighter (0.20f) : inactiveFill;
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);
            // Drag-only pills get the same family-colour top band for parity.
            g.saveState();
            g.reduceClipRegion (r.toNearestInt());
            g.setColour (accent_.withAlpha (0.85f));
            g.fillRect (r.withHeight (3.5f));
            g.restoreState();

            drawFamilyUnderline (g, r);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        // The label child repaints TOO (its text tier lifts on hover) — see
        // the paint split note in setActive.
        hovered_ = true;  repaint(); if (labelView_ != nullptr) labelView_->repaint();
    }
    void mouseExit  (const juce::MouseEvent&) override
    {
        hovered_ = false; repaint(); if (labelView_ != nullptr) labelView_->repaint();
    }

    void mouseDown (const juce::MouseEvent&) override { dragStarted_ = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Bar-only sentinel pills (e.g. the Note Sequencer, enumValue < 0) have
        // no real MOD_SRC_* enum, so a drag would route a non-existent source.
        // They are click-only: skip startDragging entirely and let mouseUp fire
        // the plain-click callback (it opens the generator editor).
        if (enumValue_ < 0)
            return;

        // One drag per press; ignore sub-threshold jitter so a stray wiggle never
        // fires a phantom drag (same threshold as the other drag sources).
        if (dragStarted_ || e.getDistanceFromDragStart() <= 5)
            return;

        auto* ddc = findParentComponentOfClass<juce::DragAndDropContainer>();
        if (ddc == nullptr)
            return;   // no DragAndDropContainer ancestor (e.g. a headless test)

        dragStarted_ = true;
        ddc->startDragging ("parvatiModSrc:" + juce::String (enumValue_), this,
                            juce::ScaledImage (buildDragImage()), true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasDrag = dragStarted_;
        dragStarted_ = false;
        // A real click (no drag, negligible movement) fires the callback.
        if (! wasDrag && e.getDistanceFromDragStart() <= 5)
            owner_.invokeClicked (enumValue_);
    }

    // ---- public state (only reachable from CentralModBar) ----
    CentralModBar&    owner_;
    int               enumValue_;
    juce::String      shortLabel_;
    juce::String      fullName_;
    parvati::Cluster  cluster_;
    bool              isGenerator_;
    bool              dragStarted_ = false;
    bool              active_      = false;
    bool              hovered_     = false;
    juce::Colour      accent_;          // family colour (ModSourceCatalog cluster -> cat* token)

    // ---- paint-split children (2026-08-23 CPU fix; see the class comment ----
    //      and the HistoryStripView / PillLabelView docs above). Public like
    //      the members around them: only CentralModBar + the pill itself
    //      touch them, and the bar's TEST-ONLY accessors read through. ----
    std::unique_ptr<HistoryStripView> stripView_;   // the sparkline (receives the strip-rect repaints)
    std::unique_ptr<PillLabelView>    labelView_;   // the label above it (cached font, paints only on its own changes)

    // ---- history strip cache (live telemetry; docs/LIVE_MOD_FEEDBACK_DESIGN.md).
    // Public like the members above: only CentralModBar's telemetry tick writes
    // them; paint() reads them. stripMin_/stripMax_ hold the per-point
    // min/max of the RAW ring samples each plotted point covers (the
    // amplitude-envelope band); the sig* fields are the last-drawn diff gate
    // (see updateStripFromHistory; -1 = never drawn). ----
    bool                  stripBipolar_ = false;    // display polarity (ctor: isBipolarModSource)
    int                   stripCount_   = 0;        // cached point count (0 = strip hidden)
    // Raw ring count behind the last downsample (2026-08-22): positions each
    // plotted point by ABSOLUTE AGE from the newest sample so the strip
    // scrolls at a CONSTANT apparent speed — a partial buffer (count <
    // kHistoryLen) renders right-anchored and fills toward the left, instead
    // of stretching across the full width and "speeding off" for the first
    // few seconds after every open/reset.
    int                   stripRawCount_ = 0;       // history ring count when the strip was drawn
    // FLAT-SKIP fast path (see updateStripFromHistory): the last signature
    // was a flat band at stripFlatLevel_ — future ticks scan only the newly
    // appended bytes against it instead of recomputing all 96 points.
    bool                  stripFlat_ = false;
    float                 stripFlatLevel_ = 0.0f;
    // MIN-MAX band cache (2026-08-22): per plotted point, the min/max of the
    // RAW ring samples whose age falls in the point's coverage — an
    // oscilloscope-style envelope. A fast LFO/env/arp (more than ~1 cycle in
    // the window) point-sampled as mush regardless of point count; the
    // min-max band renders its TRUE amplitude envelope with zero aliasing.
    // Slow sources degrade gracefully: min ≈ max everywhere and the band's
    // outline reads as the plain trace line.
    std::array<float, kStripMaxPts> stripMin_ {};
    std::array<float, kStripMaxPts> stripMax_ {};
    // Animation bookkeeping (2026-08-22 uniform-scroll fix): the bar repaints
    // the strip EVERY 60 Hz tick while it is in motion (not only when the bin
    // data changed) so the wall-clock sub-bin offset renders; a pill whose
    // content has been static for 0.5 s drops out of the animation set (the
    // idle-cost guarantee).
    bool                  stripAnimating_    = false;
    double                stripLastChangeMono_ = 0.0;
    float                 sigFirst_     = -1.0f;    // signature: oldest point
    float                 sigLast_      = -1.0f;    // ... newest point
    float                 sigMin_       = -1.0f;    // ... interior minimum
    float                 sigMax_       = -1.0f;    // ... interior maximum
    float                 sigMoment_    = -1.0f;    // ... Σ j·v[j] centroid (a sliding pulse)

private:
    // A small themed drag chip (mirrors ModSourceDragGrip / DraggableTabButton):
    // the cluster accent bar + short label on a container-fill rounded tile.
    juce::Image buildDragImage() const
    {
        const auto&      t = owner_.theme();
        const juce::Font f = owner_.pillFont();
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, shortLabel_);
        const int w = juce::jmax (48, 12 + 8 + textW + 10);
        const int h = 22;

        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        g.setColour (t.containerFill);
        g.fillRoundedRectangle (img.getBounds().toFloat(), 5.0f);

        g.setColour (accent_);
        g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 5.0f, 7.0f, static_cast<float> (h) - 10.0f), 2.0f);

        g.setColour (t.textPrimary);
        g.setFont (f);
        g.drawText (shortLabel_, juce::Rectangle<int> (17, 0, w - 17, h), juce::Justification::centredLeft, true);

        g.setColour (t.accentPrimary.withAlpha (0.6f));
        g.drawRoundedRectangle (img.getBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        return img;
    }
};

//==============================================================================
// PillContent — the horizontally-scrolled content of the CentralModBar's
// Viewport. It owns no state: it paints the per-cluster segment backgrounds +
// short labels (delegated to CentralModBar::paintSegments, whose coordinates
// match the pill positions computeLayout set), then the pills (children of this
// content) paint themselves on top.
struct CentralModBar::PillContent : public juce::Component
{
    CentralModBar& owner;
    explicit PillContent (CentralModBar& o) : owner (o) {}
    void paint (juce::Graphics& g) override { owner.paintSegments (g); }
};

//==============================================================================
CentralModBar::CentralModBar (ThemeManager& themeManager)
    : themeManager_ (themeManager)
{
    // One pill per catalogue entry, in cluster display order.
    for (const auto& e : parvati::kAllSources)
        pills_.push_back (std::make_unique<ModPill> (*this, e));
    // Wrap the pill row in a horizontal-scroll Viewport (pills become children
    // of the scrolled content) so 25+ pills never widen the editor.
    viewport_   = std::make_unique<juce::Viewport>();
    pillContent_ = std::make_unique<PillContent> (*this);
    viewport_->setScrollBarsShown (false, false, false, false);   // hidden: < > nav pills scroll instead
    viewport_->setViewedComponent (pillContent_.get(), false);   // viewport does NOT delete content
    // 'never': touch drags must start a pill drag-to-assign, NOT scroll the bar,
    // so we disable scroll-on-drag entirely. Scrolling happens via the `<` / `>`
    // nav pills (created below), avoiding any drag/scroll gesture conflict.
    viewport_->setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    // ---- OPAQUE BAR + SCROLLED CONTENT (2026-08-23, CPU "amplifier" fix —
    // finding 3 of the post-note CPU investigation): the pill strips' dirty
    // rects (bounded to the strip area since the paint split) used to cascade
    // through ~15 NON-OPAQUE ancestors all the way to the DocumentWindow —
    // the sampled app spent the majority of its message-thread time painting
    // the workspace / editor / window fills INSIDE those small dirty rects,
    // at display rate, for every animating strip. Both the bar and the
    // scrolled pill content paint their FULL bounds (paint() below and
    // paintSegments() fillAll the theme backgroundBase), so they can promise
    // JUCE they cover every pixel: the renderer then treats them as opaque
    // occluders and the strips' repaints stop AT the bar — the surrounding
    // fills inside the dirty rects disappear.
    //   * VISUALLY IDENTICAL: the bar's fill is the SAME backgroundBase the
    //     hosting workspace paints behind it (SynthWorkspace/FxWorkspace
    //     ::paint fillAll the same token), so the opaque promise changes
       //     nothing on screen — it only lets the compositor skip work.
    //   * TOP RULE UNAFFECTED: the seam's ChromeRule (barRule_) is a HIGHER-Z
    //     sibling laid over the bar's top ~8px pad; opacity only lets JUCE
    //     skip components BENEATH an opaque one, and any dirty region that
    //     intersects the rule still paints it (after the bar's fresh fill),
    //     so its shadow falloff into the bar's top pad is never erased.
    //   * PILLS STAY NON-OPAQUE (deliberate): their 5px rounded corners need
    //     the alpha channel — an opaque pill would stamp corner artefacts
    //     over the segment background. The bar/content opaqueness BEHIND the
    //     pills is what merges their dirty rects into one opaque parent; the
    //     Viewport itself paints nothing (this JUCE's Viewport has no
    //     background fill), so the bar's own fill covers the viewport's area
    //     too, including the region right of the scrolled content when the
    //     content is narrower than the view.
    setOpaque (true);
    pillContent_->setOpaque (true);
    addAndMakeVisible (*viewport_);
    for (auto& p : pills_)
        pillContent_->addAndMakeVisible (*p);

    // ---- `<` / `>` nav pills: real text-glyph buttons that page-scroll the
    // viewport left/right (replacing the thin scrollbar). Children of the BAR
    // (not pillContent_) so they flank the viewport and stay put while it
    // scrolls. Plain ASCII "<" / ">" characters in the app font (a Unicode
    // chevron glyph would be a font-stack gamble; ASCII is present everywhere),
    // vertically centred by the TextButton itself. The per-button NavButtonLnf
    // draws the tile with the SAME pill geometry/fill tokens (rounded tile,
    // top band + underline in accentPrimary, hover lift, press darken), so the
    // scrollers read as siblings of the mod pills. ----
    navLnf_ = std::make_unique<NavButtonLnf>();
    navPrev_ = std::make_unique<juce::TextButton> ("navPrev");
    navPrev_->setButtonText ("<");
    navPrev_->setLookAndFeel (navLnf_.get());
    addAndMakeVisible (*navPrev_);
    navPrev_->onClick = [this] { if (viewport_) scrollPills (-juce::jmax (1, viewport_->getViewWidth())); };

    navNext_ = std::make_unique<juce::TextButton> ("navNext");
    navNext_->setButtonText (">");
    navNext_->setLookAndFeel (navLnf_.get());
    addAndMakeVisible (*navNext_);
    navNext_->onClick = [this] { if (viewport_) scrollPills (juce::jmax (1, viewport_->getViewWidth())); };
    // Per-scrollbar override (NOT the global L&F): the mod-bar horizontal
    // scrollbar is hidden (the `<` / `>` nav pills scroll instead), but its
    // colours are themed for completeness — a dark-grey thumb (backgroundInput)
    // on a chassis-coloured track (backgroundBase) so it would recede instead of
    // flashing the accent. Re-applied in applyThemeColors() after a theme switch.
    {
        const auto& tt = theme();
        auto& hbar = viewport_->getHorizontalScrollBar();
        hbar.setColour (juce::ScrollBar::thumbColourId, tt.backgroundInput);
        hbar.setColour (juce::ScrollBar::trackColourId, tt.backgroundBase);
    }

    applyThemeColors();
}

CentralModBar::~CentralModBar()
{
    stopTimer();   // the telemetry poll (harmless when never started)
}

void CentralModBar::setOnPillClicked (std::function<void (int)> cb)
{
    onPillClicked_ = std::move (cb);
}

void CentralModBar::setActiveGenerator (int modSrcEnum)
{
    activeEnum_ = modSrcEnum;
    for (auto& p : pills_)
        p->setActive (p->enumValue_ == modSrcEnum && p->isGenerator_);
}

void CentralModBar::applyThemeColors()
{
    const auto& t = theme();
    // FAMILY-COLOURED: each pill resolves its underline colour from its
    // ModSourceCatalog cluster -> the cat* family token (Env=teal, LFO=magenta,
    // Perf=amber, Seq/Arp/Note=mint, Util=orange, Mod=purple, Const=slate-blue).
    // The cluster captions are gone, so the family colour now carries the
    // cluster identity on every pill (its persistent underline). The active
    // generator pill stays highlighted via its solid fill + bright text (see
    // ModPill::paint); the family colour only tints the underline.
    for (auto& p : pills_)
        p->accent_ = parvati::clusterAccent (p->cluster_, t);
    // (The iOS Viewport has no background-colour API; the scrolled content fills
    // its own background in paintSegments, so the bar reads as one colour.)
    // Theme the `<` / `>` nav glyphs from the active theme (the scrollbar is
    // hidden; these scroll the bar). The per-button Lnf is re-pointed at the
    // theme so its quiet-chrome colours (dim glyph at rest, hover lift,
    // accent press — see NavButtonLnf) track theme switches; the TextButton
    // colour ids below are inert (both background and text are fully
    // overridden by the Lnf) and kept only as sensible fallbacks.
    if (navLnf_ != nullptr)
        if (auto* nl = dynamic_cast<NavButtonLnf*> (navLnf_.get()))
            nl->setTheme (t);
    if (navPrev_ != nullptr)
    {
        navPrev_->setColour (juce::TextButton::buttonColourId,  t.tabUnselectedBg);
        navPrev_->setColour (juce::TextButton::textColourOffId, t.textSecondary);
        navPrev_->repaint();
    }
    if (navNext_ != nullptr)
    {
        navNext_->setColour (juce::TextButton::buttonColourId,  t.tabUnselectedBg);
        navNext_->setColour (juce::TextButton::textColourOffId, t.textSecondary);
        navNext_->repaint();
    }
    repaint();
}

void CentralModBar::invokeClicked (int modSrcEnum)
{
    if (onPillClicked_)
        onPillClicked_ (modSrcEnum);
}

const ParvatiTheme& CentralModBar::theme() const
{
    return themeManager_.getCurrentTheme();
}

juce::Font CentralModBar::pillFont() const
{
    const float size = 14.0f;   // proportional with the ~1.5x bigger pills
    return parvati::appFontFor (*this, size);
}

// ---- TEST-ONLY paint-split accessors (see the header) ----
juce::Component* CentralModBar::pillComponentForTest (int pillIndex) const
{
    return (pillIndex >= 0 && pillIndex < (int) pills_.size())
        ? pills_[(size_t) pillIndex].get() : nullptr;
}

juce::Component* CentralModBar::pillStripChildForTest (int pillIndex) const
{
    return (pillIndex >= 0 && pillIndex < (int) pills_.size())
        ? pills_[(size_t) pillIndex]->stripChildForTest() : nullptr;
}
juce::Component* CentralModBar::pillLabelChildForTest (int pillIndex) const
{
    return (pillIndex >= 0 && pillIndex < (int) pills_.size())
        ? pills_[(size_t) pillIndex]->labelChildForTest() : nullptr;
}

int CentralModBar::pillLabelPaintCountForTest (int pillIndex) const
{
    return (pillIndex >= 0 && pillIndex < (int) pills_.size())
        ? pills_[(size_t) pillIndex]->labelPaintCountForTest() : -1;
}

int CentralModBar::pillStripPaintCountForTest (int pillIndex) const
{
    return (pillIndex >= 0 && pillIndex < (int) pills_.size())
        ? pills_[(size_t) pillIndex]->stripPaintCountForTest() : -1;
}

void CentralModBar::paint (juce::Graphics& g)
{
    // THE BAR'S OWN BACKGROUND (2026-08-23 opaque-bar pass — see the ctor's
    // opacity block for the full rationale). Fills the WHOLE bounds so the
    // setOpaque promise is honest: this covers the two nav-pill side bands
    // (previously parent-painted — the workspace paints the SAME token, so
    // this is visually identical) AND the area under the Viewport (this
    // JUCE's Viewport paints nothing itself, so the bar's fill shows through
    // wherever the scrolled content or its gaps do not cover — including the
    // region right of the content when it is narrower than the view). The
    // colour is re-resolved from the live theme every paint and
    // applyThemeColors() ends with a full repaint(), so theme switches
    // re-skin the fill. The pills, segment tabs, nav tiles and the top rule
    // paint OVER it as children / higher-z siblings.
    g.fillAll (theme().backgroundBase);
}

void CentralModBar::resized()
{
    // The pill row scrolls horizontally inside the Viewport, flanked by the
    // `<` / `>` nav pills. Size the scrolled content to its full no-clip
    // preferred width, then position the pills + segment backgrounds within it
    // (the Viewport clips/scrolls). The nav pills sit at the bar's two ends and
    // stay fixed while the viewport scrolls between them.
    if (viewport_ != nullptr && pillContent_ != nullptr)
    {
        const auto b = getLocalBounds();
        // Nav hit bands (F-ios-touch-1): 44x44 HIG floor, vertically CENTRED ON
        // THE PILL BAND (the pill strip: kSegVPad + kLabelTabH + kLabelTabGap ..
        // + kPillH). The 44pt band sits beside 56pt pills; the centred chevron
        // glyph keeps the old quiet visual weight.
        const int pillBandY = kSegVPad + kLabelTabH + kLabelTabGap;
        const int navH = juce::jmin (kNavW, kPillH);
        const int navY = pillBandY + (kPillH - navH) / 2;
        if (navPrev_ != nullptr) navPrev_->setBounds (b.getX(), navY, kNavW, navH);
        if (navNext_ != nullptr) navNext_->setBounds (b.getRight() - kNavW, navY, kNavW, navH);
        viewport_->setBounds (b.withTrimmedLeft (kNavW + kNavGap).withTrimmedRight (kNavW + kNavGap));
        const int contentW = computeLayout (false);   // full no-clip width
        pillContent_->setSize (contentW, kBarHeight);
        computeLayout (true);                         // position pills + segment rects
        pillContent_->repaint();
        updateNavEnabled();
    }
}

int CentralModBar::preferredWidth() const
{
    return computeLayout (false);
}

int CentralModBar::computeLayout (bool positionChildren) const
{
    const juce::Font f = pillFont();
    const int        pillY = kSegVPad + kLabelTabH + kLabelTabGap;   // pills sit BELOW the coloured tab

    const auto& clusters = parvati::clustersInOrder();

    int x = kEdgePad;
    size_t idx = 0;   // walks kAllSources, which is already in cluster order

    // ---- group each cluster into a LABELLED segment background ----
    if (positionChildren)
    {
        segmentRects_.clearQuick();
        segmentLabels_.clearQuick();
        segmentClusters_.clearQuick();
    }

    for (size_t ci = 0; ci < clusters.size(); ++ci)
    {
        const parvati::Cluster c = clusters[ci];

        // Separator BEFORE this cluster (skipped before the very first): the
        // normal inter-cluster gap, EXCEPT at the generators->drag-only split,
        // where a larger kSideGap clearly separates the two halves of the bar.
        if (ci > 0)
        {
            const bool prevGen = parvati::isGeneratorCluster (clusters[ci - 1]);
            const bool thisGen = parvati::isGeneratorCluster (c);
            x += (prevGen && ! thisGen) ? kSideGap : kClusterGap;
        }

        const int segStart = x;   // pills now start at segStart (no horizontal label reservation)

        // Pills belonging to this cluster (kAllSources is ordered to match).
        bool first = true;
        for (; idx < parvati::kAllSources.size() && parvati::kAllSources[idx].cluster == c; ++idx)
        {
            if (! first) x += kPillGap;
            first = false;

            const int w = pills_[idx]->widthForFont (f);
            if (positionChildren)
                pills_[idx]->setBounds (x, pillY, w, kPillH);
            x += w;
        }

        if (positionChildren)
        {
            // The coloured label TAB header sits at the top of the cluster column
            // (full cluster width, tab height); the pills are positioned below it.
            segmentRects_.add (juce::Rectangle<int> (segStart - kSegPad, kSegVPad,
                                                     x - segStart + 2 * kSegPad,
                                                     kLabelTabH));
            segmentLabels_.add (clusterShortLabel (c));
            segmentClusters_.add (c);
        }
    }

    return x + kEdgePad;   // right edge padding
}

void CentralModBar::scrollPills (int deltaPx)
{
    if (viewport_ == nullptr || pillContent_ == nullptr)
        return;
    const int cur = viewport_->getViewPositionX();
    const int max = juce::jmax (0, pillContent_->getWidth() - viewport_->getViewWidth());
    viewport_->setViewPosition (juce::jlimit (0, max, cur + deltaPx), 0);
    updateNavEnabled();
}

void CentralModBar::updateNavEnabled()
{
    if (viewport_ == nullptr || pillContent_ == nullptr)
        return;
    const int cur = viewport_->getViewPositionX();
    const int max = juce::jmax (0, pillContent_->getWidth() - viewport_->getViewWidth());
    if (navPrev_) navPrev_->setEnabled (cur > 0);
    if (navNext_) navNext_->setEnabled (cur < max - 1);
}

//==============================================================================
// ---- Live telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----

void CentralModBar::setTelemetryProvider (std::function<bool (parvati::ModTelemetrySnapshot&)> fetch)
{
    telemetryFetch_ = std::move (fetch);
    // A provider arriving (or being cleared) re-evaluates the poll: with none
    // set the timer stays off and the bar renders exactly as it did before the
    // telemetry feature existed.
    updateTelemetryTimer();
}

void CentralModBar::setTelemetryRateHz (int hz)
{
    // Valid rates are 5..60; 0 is the explicit "disable" sentinel. Anything
    // else clamps into that set, and the (re)started timer picks the new
    // cadence up immediately.
    const int clamped = (hz <= 0) ? 0 : juce::jlimit (5, 60, hz);
    if (clamped == telemetryRateHz_)
        return;
    telemetryRateHz_ = clamped;
    updateTelemetryTimer();
}

void CentralModBar::clearTelemetry()
{
    // Hide every strip (an invalid frame / a reset). Only pills that actually
    // had data painted repaint — bounded to the strip child, never the whole
    // pill — and the hide counts as a data change for the test seam.
    bool anyRepainted = false;
    for (auto& p : pills_)
        if (p->clearStrip())
        {
            p->repaintStrip();   // PAINT SPLIT
            anyRepainted = true;
        }
    if (anyRepainted)
        ++telemetryGeneration_;
}

void CentralModBar::timerCallback()
{
    if (! isVisible() || telemetryFetch_ == nullptr)
        return;

    // WATCHDOG role while the vblank attachment is alive: check vsync
    // callbacks are actually arriving; healthy (< 250 ms stale) -> stand down
    // and let the display-locked driver run. Stalled -> promote this Timer to
    // the fallback driver (resets the animation cadence to 60 Hz).
    if (usingVBlank_)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (now - lastVblankMono_ < 0.250)
            return;   // vsync is delivering: watchdog no-op
        // VBlank stalled (off-screen window / display asleep / no vsync in
        // this harness): detach it and take over at 60 Hz.
        vblank_ = nullptr;
        usingVBlank_ = false;
        startTimerHz (60);
    }

    // Timer FALLBACK driver: a fixed 60 Hz stand-in for the VBlankAttachment,
    // with the same fetch decimation.
    const int fetchDivisor = juce::jmax (1, juce::roundToInt (60.0 / (double) juce::jmax (5, telemetryRateHz_)));
    if (++tickCounter_ % fetchDivisor != 0)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (auto& p : pills_)
            if (p->stripAnimating_ && now - p->stripLastChangeMono_ < 0.5)
                p->repaintStrip();   // PAINT SPLIT
            else
                p->stripAnimating_ = false;
        return;
    }
    telemetryTick();
}

void CentralModBar::telemetryTick()
{
    // SHARED tick (fetch cadence) — called by the vsync driver or the fallback
    // Timer. Cheap guard: isVisible() (a STABLE flag, not the peer-derived
    // isShowing()) + the provider; the data-driven repaint gating below stays
    // the real cost control.
    if (! isVisible() || telemetryFetch_ == nullptr)
        return;

    parvati::ModTelemetrySnapshot snap;
    if (! telemetryFetch_ (snap))
    {
        // Torn seqlock read OR a stale epoch (patch load / part switch reset):
        // hide the strips until a valid frame returns.
        clearTelemetry();
        return;
    }

    // Wall-clock anchors for the paint-time extrapolation: unwrap the ring
    // head into a monotonic append count (the fractional position between
    // fetches is (now - fetchTime) * kAppendHz appends — see the pill paint),
    // and remember when the ring last MOVED (audio flowing) so a stopped
    // engine freezes the strips instead of sliding them out from stale data.
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (telHaveHead_)
        {
            int delta = (snap.historyHead - telPrevHead_) % parvati::ModTelemetrySnapshot::kHistoryLen;
            if (delta < 0)
                delta += parvati::ModTelemetrySnapshot::kHistoryLen;
            if (delta > 0 && delta < 32)   // sane single-interval advance
            {
                telUnwrappedAppends_ += delta;
                telLastMotionMono_ = now;
            }
        }
        telPrevHead_ = snap.historyHead;
        telHaveHead_ = true;
        telLastFetchMono_ = now;
    }

    const int n = juce::jlimit (0, parvati::ModTelemetrySnapshot::kHistoryLen, snap.historyCount);
    bool anyRepainted = false;
    for (auto& p : pills_)
    {
        // Const pills are static amounts (no history). Real sources index the
        // frame by their MOD_SRC_* enum; the bar-only Note Sequencer sentinel
        // (enumValue_ < 0) reads the spare kNoteSeqSlot — the tracked part's
        // sounding sequencer note, a melody trace with rests as gaps.
        if (p->cluster_ == parvati::Cluster::Const)
            continue;
        const int slot = (p->enumValue_ >= 0)
            ? p->enumValue_
            : parvati::ModTelemetrySnapshot::kNoteSeqSlot;
        const uint8_t* hist = snap.history
            + (size_t) slot * (size_t) parvati::ModTelemetrySnapshot::kHistoryLen;
        if (p->updateStripFromHistory (hist, n, snap.sources[(size_t) slot]))
        {
            p->stripLastChangeMono_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
            p->stripAnimating_ = true;
            anyRepainted = true;
        }
        // UNIFORM 0.5 s DROPOUT (2026-08-23 CPU fix — the idle hot spot):
        // this loop used to repaint ANY stripAnimating_ pill unconditionally
        // on every fetch tick, but the 0.5 s park lived ONLY in the vblank /
        // timer SUB-tick branches — so whenever vblankFetchDiv_ == 1 (a 60 Hz
        // refresh pref on a ~60 Hz display) the sub-ticks never ran, a pill
        // that animated ONCE (e.g. the always-on window filling at launch)
        // stayed stripAnimating_ FOREVER, and every strip repainted at the
        // fetch cadence on flat data indefinitely (~35% CPU measured in the
        // Standalone at idle). The SAME park rule now applies here: a strip
        // static for 0.5 s drops out of the animation set (matches the
        // documented design and the sub-tick behaviour).
        const double nowMono = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (p->stripAnimating_ && nowMono - p->stripLastChangeMono_ < 0.5)
        {
            // THE GPU-COST CONTROL stays: repaint ONLY the strip child's
            // bounding rect, every animation tick while in motion; a pill
            // static for 0.5 s drops out of the animation set entirely. The
            // pill tile never re-rasters, and the label child — though the
            // peer dispatch may walk it clipped to this rect — answers from
            // its BUFFERED IMAGE (a blit, not a text raster).
            p->repaintStrip();   // PAINT SPLIT: dirty the strip CHILD only
        }
        else
        {
            p->stripAnimating_ = false;
        }
    }
    if (anyRepainted)
        ++telemetryGeneration_;
}

double CentralModBar::telemetryAppendsSinceFetch() const
{
    // Appends elapsed since the freshest fetched sample, in APPEND units
    // (fractional): (now - fetchTime) * kAppendHz, clamped sane. Frozen at 0
    // when the ring has not moved for 250 ms (audio stopped — the engine only
    // appends from renderPartFx), so strips park instead of sliding on stale
    // data. Pills multiply this by their own px/append for the sub-bin shift.
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (now - telLastMotionMono_ > 0.25 || telLastFetchMono_ <= 0.0)
        return 0.0;
    const double since = juce::jlimit (0.0, 0.25, now - telLastFetchMono_);
    return since * parvati::ModTelemetrySnapshot::kAppendHz;
}

void CentralModBar::visibilityChanged()
{
    updateTelemetryTimer();
}

void CentralModBar::parentHierarchyChanged()
{
    updateTelemetryTimer();
}

void CentralModBar::updateTelemetryTimer()
{
    // START gate (2026-08-21 reliability fix): the timer starts whenever a
    // provider + enabled rate exist — deliberately NOT gated on isShowing().
    // isShowing() is peer-derived and proved unreliable for STARTING (JUCE's
    // visible-before-desktop sequencing can starve the visibility hooks, and
    // a deactivated host process flips the peer state under the tick): a poll
    // that never started renders the whole live-strip feature dead. The
    // TICK itself carries the cheap guard (isVisible + fetch) so a genuinely
    // hidden bar costs one no-op pass, not repaints.
    // 2026-08-22 (uniform-scroll fix, JUCE-NATIVE): the ANIMATION is driven
    // by juce::VBlankAttachment — a callback at every display refresh of the
    // bar's peer WITH the vsync timestamp: display-locked, no Timer jitter,
    // no ms quantization, aligned with the compositor. The FETCH still runs
    // at telemetryRateHz_ (decimated from the vblank cadence) and the sub-bin
    // scroll offset extrapolates from the wall clock, so the rendered
    // position is continuous in time AND frame-aligned. A 60 Hz Timer
    // fallback covers headless/pre-peer contexts; both drivers route into
    // telemetryTick().
    const bool want = telemetryFetch_ != nullptr && telemetryRateHz_ >= 5;
    if (want)
    {
        if (getPeer() != nullptr)
        {
            // Decimate fetches from the display rate (~60; ProMotion higher).
            // ~60 Hz typical; ProMotion displays report higher — decimate the
            // FETCH to telemetryRateHz_ from whatever the display runs.
            double displayHz = 60.0;
            if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                if (d->verticalFrequencyHz.has_value())
                    displayHz = juce::jmax (30.0, *d->verticalFrequencyHz);
            vblankFetchDiv_ = juce::jmax (1, juce::roundToInt (
                displayHz / (double) juce::jmax (5, telemetryRateHz_)));
            if (vblank_ == nullptr)
            {
                vblank_ = std::make_unique<juce::VBlankAttachment> (this,
                    [this] (double) { vblankTick(); });
                usingVBlank_ = true;
                // WATCHDOG, not the driver: a slow 4 Hz Timer supervises the
                // vblank attachment — an off-screen window, a sleeping display
                // or a harness without vsync delivery stalls it silently, and
                // the strips must never freeze. A healthy vblank (< 250 ms
                // since the last callback) makes the watchdog a no-op.
                startTimerHz (4);
            }
        }
        else if (vblank_ == nullptr)
        {
            startTimerHz (60);   // fallback until a peer exists
            usingVBlank_ = false;
        }
    }
    else
    {
        vblank_ = nullptr;
        usingVBlank_ = false;
        stopTimer();
    }
}

void CentralModBar::vblankTick()
{
    // Animation tick at DISPLAY cadence (message thread, per the JUCE
    // contract). Fetch every vblankFetchDiv_-th callback; the in-between
    // ticks repaint the animating strips so the sub-bin scroll offset
    // renders at full display smoothness. (lastVblankMono_ feeds the Timer
    // watchdog: if these stop arriving, the Timer promotes itself.)
    lastVblankMono_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (! isVisible() || telemetryFetch_ == nullptr)
        return;
    if (++tickCounter_ % vblankFetchDiv_ != 0)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (auto& p : pills_)
            if (p->stripAnimating_ && now - p->stripLastChangeMono_ < 0.5)
                p->repaintStrip();   // PAINT SPLIT
            else
                p->stripAnimating_ = false;
        return;
    }
    telemetryTick();
}

void CentralModBar::paintSegments (juce::Graphics& g) const
{
    const auto& t = theme();

    // Fill the scrolled content with the bar background so the gaps between /
    // around the cluster tabs read as one continuous bar.
    g.fillAll (t.backgroundBase);

    // One small COLOURED LABEL TAB header per cluster (its family colour + short
    // label), sitting above the pills — replaces the former full-height 'cube'.
    const juce::Font tabFont = juce::Font (juce::FontOptions (9.0f)).boldened();
    for (int i = 0; i < segmentRects_.size(); ++i)
    {
        const auto& seg = segmentRects_.getReference (i);   // the tab rect (top of the cluster column)
        g.setColour (parvati::clusterAccent (segmentClusters_.getReference (i), t));
        g.fillRoundedRectangle (seg.toFloat().reduced (0.5f), 4.0f);

        g.setColour (t.backgroundBase);   // dark text reads on every saturated family colour
        g.setFont (tabFont);
        g.drawText (segmentLabels_.getReference (i), seg,
                    juce::Justification::centred, true);
    }
}
