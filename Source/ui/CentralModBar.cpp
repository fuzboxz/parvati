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
    constexpr int kStripMaxPts = 24;   // downsampled points per strip

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
// telemetry tick feeds it data, the pill additionally draws a family-coloured
// sparkline of the source's recent values spanning the pill's inner height
// (label on top — the Pigments-style scope reading). Flat and quiet — no
// fill, no glow, no grid; the accent-on-dark contrast keeps the strip an
// indicator, not a focus. Const-cluster pills never carry one; the bar-only
// Note Sequencer sentinel DOES — its melody trace rides the spare
// kNoteSeqSlot (see the telemetry tick). With no telemetry the strip simply
// stays empty.
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
    }

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
        if (a != active_) { active_ = a; repaint(); }
    }

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

    /** Downsamples @p count OLDEST->NEWEST history samples into the cached
        strip points (at most kStripMaxPts, spread evenly and pinned at both
        ends so the newest sample is always the LAST plotted point). Returns
        true when the drawn data changed (the caller repaints the strip rect);
        a signature-identical frame returns false and costs nothing. */
    bool updateStripFromHistory (const uint8_t* samples, int count)
    {
        if (count <= 0)
            return clearStrip();   // no history yet (fresh reset / no voice): hide

        constexpr float kEps = 1.0f / 255.0f;   // one value quantum

        const int m = juce::jmin (kStripMaxPts, count);
        float v[kStripMaxPts];
        float lo = 1.0f, hi = 0.0f;
        float moment = 0.0f;   // Σ j·v[j]: POSITION-weighted signature term
        for (int j = 0; j < m; ++j)
        {
            const int idx = (m > 1)
                ? juce::roundToInt ((float) j * (float) (count - 1) / (float) (m - 1))
                : (count - 1);
            const float val = (float) samples[(size_t) juce::jlimit (0, count - 1, idx)]
                            * (1.0f / 255.0f);
            v[j] = val;
            lo = juce::jmin (lo, val);
            hi = juce::jmax (hi, val);
            moment += static_cast<float> (j) * val;
        }

        // DIFF GATE (the idle-cost control): the engine's history ring slides
        // at its append rate, so the OLDEST/NEWEST ends move whenever fresh
        // data lands, while min/max guard the interior shape. The moment Σ j·v[j]
        // adds a POSITION-sensitive term: a pulse (GATE / VELOCITY hit / ARP
        // step) sliding through an otherwise-flat window keeps the same
        // first/last/min/max but shifts the centroid, so the strip follows it
        // instead of freezing until the pulse reaches an endpoint. A parked
        // source (wheel at rest, gate low) still reproduces an identical
        // signature every tick and triggers ZERO repaints.
        if (m == stripCount_
            && std::fabs (v[0] - sigFirst_) <= kEps
            && std::fabs (v[m - 1] - sigLast_) <= kEps
            && std::fabs (lo - sigMin_) <= kEps
            && std::fabs (hi - sigMax_) <= kEps
            && std::fabs (moment - sigMoment_) <= kEps)
            return false;

        for (int j = 0; j < m; ++j)
            stripVals_[(size_t) j] = v[j];
        stripCount_ = m;
        sigFirst_ = v[0];
        sigLast_  = v[m - 1];
        sigMin_   = lo;
        sigMax_   = hi;
        sigMoment_ = moment;
        return true;
    }

    /** Clears the cached strip. Returns true when something was actually
        drawn (i.e. a repaint is needed to erase it). */
    bool clearStrip()
    {
        if (stripCount_ == 0 && sigFirst_ < 0.0f)
            return false;   // already hidden
        stripCount_ = 0;
        stripVals_.fill (0.0f);
        sigFirst_ = sigLast_ = sigMin_ = sigMax_ = -1.0f;
        sigMoment_ = -1.0f;
        return true;
    }

    /** The history sparkline: a thin family-coloured polyline inside the strip
        band, just above the underline. Bipolar sources (LFO / bend / note)
        swing around the band's vertical midline (value 128 = centre);
        unipolar ones rise from the band's bottom. */
    void drawHistoryStrip (juce::Graphics& g)
    {
        // Const pills are static amounts (no history); every OTHER pill has a
        // telemetry slot — including the bar-only Note Sequencer sentinel
        // (enumValue_ < 0), whose melody trace rides the spare kNoteSeqSlot
        // (see the bar's telemetry tick).
        if (cluster_ == parvati::Cluster::Const)
            return;   // Const pills carry no history
        if (stripCount_ <= 0)
            return;   // no history yet (e.g. after a reset): the band stays empty

        const auto sr = stripRect().toFloat();
        // Keep the round-capped 1.25px stroke inside the band.
        // JITTER FIX (2026-08-21): the round-capped 2.2px stroke extends ~1.1px
        // beyond each path point, and the tick's repaint dirty region is
        // exactly stripRect() — plotting to the rect's edges let the stroke's
        // extremes get CLIPPED on animated repaints (the reported top/bottom
        // jitter). Inset the plotted range by the stroke radius + an AA hair so
        // the whole stroke always lands inside the dirty region.
        constexpr float kStrokeW   = 2.2f;
        constexpr float kPlotInset  = kStrokeW * 0.5f + 0.6f;
        const float usable = juce::jmax (1.0f, sr.getHeight() - 2.0f * kPlotInset);

        juce::Path path;
        float singleY = 0.0f;   // the lone point's y (stripCount_ == 1 dot below)
        // Horizontal inset TOO (same class as the vertical jitter fix): the
        // round caps would otherwise poke ~1.1px past the strip rect's
        // left/right edges — past the animated repaint's dirty region AND
        // into the pill's rounded-corner curve, leaving one-cap-width specks
        // at both ends (the reported tiny artifacts). The x span now shares
        // the same kPlotInset clearance.
        const float x0 = sr.getX() + kPlotInset;
        const float x1 = sr.getRight() - kPlotInset;
        for (int i = 0; i < stripCount_; ++i)
        {
            const float t = (stripCount_ > 1)
                ? (float) i / (float) (stripCount_ - 1)
                : 0.5f;
            const float x = x0 + t * (x1 - x0);
            const float v = juce::jlimit (0.0f, 1.0f, stripVals_[(size_t) i]);
            const float y = stripBipolar_
                ? sr.getCentreY() - (v - 0.5f) * 2.0f * (usable * 0.5f)
                : sr.getBottom() - kPlotInset - v * usable;
            singleY = y;
            if (i == 0)  path.startNewSubPath (x, y);
            else         path.lineTo (x, y);
        }
        g.setColour (accent_.withAlpha (active_ ? 0.95f : 0.85f));
        // A single sample has no segment to stroke (startNewSubPath alone
        // draws nothing) — draw a small dot instead so the strip's first
        // ~12 ms (one append at the 81.7 Hz cadence) still shows a value.
        if (stripCount_ == 1)
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (
                               juce::Point<float> (sr.getCentreX(), singleY)));
        else
            g.strokePath (path, juce::PathStrokeType (kStrokeW,
                              juce::PathStrokeType::JointStyle::curved,
                              juce::PathStrokeType::EndCapStyle::rounded));
    }

    void paint (juce::Graphics& g) override
    {
        const auto&      t = owner_.theme();
        const juce::Font f = owner_.pillFont();
        const juce::Rectangle<float> r = getLocalBounds().toFloat().reduced (0.5f);

        if (isGenerator_)
        {
            // Active clarity: the active generator pill gets a SOLID slightly-
            // lighter background (tabSelectedBg) with HIGH-CONTRAST text
            // (textPrimary) — the solid fill is the PRIMARY active cue, with NO
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
            drawHistoryStrip (g);

            g.setColour (active_ ? t.textPrimary
                                 : (hovered_ ? t.textSecondary.brighter (0.20f)
                                             : t.textSecondary));
            g.setFont (f);
            // Label centred over the FULL pill, on top of the full-height
            // history strip (the Pigments-style scope reading).
            g.drawText (shortLabel_, getLocalBounds(), juce::Justification::centred, true);
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
            drawHistoryStrip (g);

            g.setColour (t.textSecondary);
            g.setFont (f);
            // Label centred over the full pill (minus the drag-only horizontal
            // inset), on top of the full-height history strip.
            g.drawText (shortLabel_, getLocalBounds().reduced (5, 0), juce::Justification::centred, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered_ = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered_ = false; repaint(); }

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
        ddc->startDragging ("parvatiModSrc:" + juce::String (enumValue_), this, buildDragImage(), true);
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

    // ---- history strip cache (live telemetry; docs/LIVE_MOD_FEEDBACK_DESIGN.md).
    // Public like the members above: only CentralModBar's telemetry tick writes
    // them; paint() reads them. stripVals_ holds the DOWNSAMPLED normalized
    // 0..1 values (oldest -> newest); the sig* fields are the last-drawn diff
    // gate (see updateStripFromHistory; -1 = never drawn). ----
    bool                  stripBipolar_ = false;    // display polarity (ctor: isBipolarModSource)
    int                   stripCount_   = 0;        // cached point count (0 = strip hidden)
    std::array<float, kStripMaxPts> stripVals_ {};  // downsampled normalized values
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
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        return lnf->appFont (size, juce::Font::plain);
    return juce::Font (juce::FontOptions (size));
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
    // had data painted repaint — bounded to the strip rect again, never the
    // whole pill — and the hide counts as a data change for the test seam.
    bool anyRepainted = false;
    for (auto& p : pills_)
        if (p->clearStrip())
        {
            p->repaint (p->stripRect());
            anyRepainted = true;
        }
    if (anyRepainted)
        ++telemetryGeneration_;
}

void CentralModBar::timerCallback()
{
    // Cheap per-tick guard: isVisible() (a STABLE flag, not the peer-derived
    // isShowing()) + the provider. A hidden/unparented bar costs one no-op
    // pass; the data-driven repaint gating below stays the real cost control.
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
        if (p->updateStripFromHistory (hist, n))
        {
            // THE GPU-COST CONTROL: repaint ONLY the strip's bounding rect, so
            // an animating source never re-rasters the pill tile / label, and
            // an idle source (signature unchanged) repaints nothing at all.
            p->repaint (p->stripRect());
            anyRepainted = true;
        }
    }
    if (anyRepainted)
        ++telemetryGeneration_;
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
    if (telemetryFetch_ != nullptr && telemetryRateHz_ >= 5)
        startTimerHz (telemetryRateHz_);
    else
        stopTimer();
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
