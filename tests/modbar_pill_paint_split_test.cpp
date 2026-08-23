// modbar_pill_paint_split_test — pins the 2026-08-23 ModPill label/strip
// PAINT SPLIT (the message-thread CPU hot spot from the sampled profile:
// every 30-60 Hz strip-animation repaint used to re-run the pill's WHOLE
// paint, including Graphics::drawText label layout — ~63% of the sampled
// message-thread cost).
//
// The split: each ModPill paints only its cheap chrome (fill / accent band /
// family underline); the history sparkline is a CHILD component
// (HistoryStripView) that the bar's per-tick strip repaints dirty EXCLUSIVELY
// (CentralModBar now routes p->repaintStrip() instead of
// repaint(pill.stripRect())); the label is a CHILD above it (PillLabelView)
// with a CACHED resolved font, BUFFERED TO AN IMAGE, whose paint() runs only
// when the label's own state changes.
//
// MECHANISM NOTE (2026-08-23 review settlement): JUCE's peer dispatch walks
// the whole hierarchy clipped to the dirty rect — a strip-tick dirty rect
// DOES dispatch a clipped paint to the label sibling. The CPU contract holds
// because that dispatch answers from the label's BUFFERED IMAGE (a bitmap
// blit, paint() uninvoked), not because sibling dispatch is avoided. An
// earlier revision of [3] pumped an off-screen desktop peer and asserted the
// same thing; probing showed such a peer delivers NO paints after the
// initial one (the assertion passed vacuously), so [3] now REPLAYS the peer
// dispatch deterministically instead (clip to the strip rect +
// paintEntireComponent — exactly what ComponentPeer::handlePaint does).
//
// Sections:
//   [1] Structural (headless, deterministic): the two children exist under
//       each pill, the strip child's bounds ARE the pill's strip rect, the
//       label child overlays the full pill, z-order is strip-below-label,
//       both children are mouse-transparent, and hit-testing through them
//       still resolves to the pill (clicks/drags/tooltips unchanged — the
//       modbar_pill_click_test contract).
//   [2] The label still paints: a full-tree offscreen render increments the
//       label child's paint counter (the text is drawn, not lost in the split).
//   [3] THE CPU CONTRACT (deterministic dispatch replay): a dirty rect over
//       the strip child walks the strip child's paint (count increments every
//       dispatch — the positive control) but leaves the label child's paint()
//       count FROZEN (buffered blit); the label re-renders exactly once after
//       its own state change (hover) invalidates the buffer.
//   [4] LABEL HOVER-TIER PARITY with the pre-split paint (confirmed against
//       git HEAD): generator labels change on hover (textSecondary ->
//       brighter), drag-only labels are hover-INVARIANT (always
//       textSecondary). Pinned by pixel-exact comparison of the label's
//       hovered-vs-plain renders.

#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "test_utils.h"
#include "unified_test_runner.h"

#include "ui/CentralModBar.h"
#include "ui/ModTelemetryTypes.h"   // ModTelemetrySnapshot (synthetic strip history)
#include "ui/ThemeManager.h"
#include "dsp/patch.h"              // MOD_SRC_LFO_1

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Collect every descendant (DFS), c included.
void collectAll (juce::Component* c, std::vector<juce::Component*>& out)
{
    if (c == nullptr)
        return;
    out.push_back (c);
    for (auto* child : c->getChildren())
        collectAll (child, out);
}

// The generator pill whose tooltip matches fullName (pills are the file-local
// ModPill; reachable only as SettableTooltipClient children — the
// modbar_pill_click_test discovery idiom).
juce::Component* findPillByTooltip (CentralModBar* bar, const juce::String& fullName)
{
    std::vector<juce::Component*> all;
    collectAll (bar, all);
    for (auto* c : all)
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
            if (tc->getTooltip() == fullName)
                return c;
    return nullptr;
}

// The bar's pill INDEX whose strip child's parent is @p pill (correlates the
// public index-based accessors with the tooltip-discovered pill, so the test
// never depends on the ModSourceCatalog ordering).
int pillIndexOf (CentralModBar& bar, juce::Component* pill)
{
    for (int i = 0; i < 64; ++i)
        if (auto* sc = bar.pillStripChildForTest (i); sc != nullptr && sc->getParentComponent() == pill)
            return i;
    return -1;
}
}  // namespace

TEST(modbar_pill_paint_split_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    ThemeManager themeManager;
    CentralModBar bar (themeManager);
    bar.setBounds (0, 0, 900, CentralModBar::kBarHeight);

    // ---- [1] Structural: the split children + geometry + mouse parity ----
    std::printf ("[1] paint-split structure (strip child / label child / hit parity)\n");
    {
        auto* lfoPill = findPillByTooltip (&bar, "LFO 2");   // a generator pill (full-height strip)
        check (lfoPill != nullptr, "generator pill (LFO 2) located via tooltip");
        const int idx = lfoPill != nullptr ? pillIndexOf (bar, lfoPill) : -1;
        check (idx >= 0, "the tooltip-discovered pill correlates with a bar pill index");

        auto* strip = bar.pillStripChildForTest (idx);
        auto* label = bar.pillLabelChildForTest (idx);
        check (strip != nullptr, "strip child exists (HistoryStripView)");
        check (label != nullptr, "label child exists (PillLabelView)");
        if (strip != nullptr && label != nullptr && lfoPill != nullptr)
        {
            // Both children belong to the pill.
            check (strip->getParentComponent() == lfoPill && label->getParentComponent() == lfoPill,
                   "both children are parented to the pill");

            // Strip child bounds == the pill's strip rect: full inner width
            // minus 3pt per side, spanning between the top accent band (5pt)
            // and the bottom family underline (3pt) — the CentralModBar.cpp
            // strip constants pinned here (update together if they change).
            const auto expected = lfoPill->getLocalBounds()
                                       .withTrimmedTop (5).withTrimmedBottom (3)
                                       .withTrimmedLeft (3).withTrimmedRight (3);
            char m[160];
            std::snprintf (m, sizeof (m),
                           "strip child bounds == the strip rect (got %s, want %s)",
                           strip->getBounds().toString().toRawUTF8(),
                           expected.toString().toRawUTF8());
            check (strip->getBounds() == expected, m);

            // Label child overlays the FULL pill (the old label rects).
            check (label->getBounds() == lfoPill->getLocalBounds(),
                   "label child bounds == the full pill");

            // Z-ORDER: strip added BEFORE the label => the label paints ON
            // TOP of the sparkline (the Pigments-style reading is unchanged).
            int stripZ = -1, labelZ = -1;
            const auto& siblings = lfoPill->getChildren();
            for (int i = 0; i < siblings.size(); ++i)
            {
                if (siblings[i] == strip) stripZ = i;
                if (siblings[i] == label) labelZ = i;
            }
            check (stripZ >= 0 && labelZ >= 0 && stripZ < labelZ,
                   "z-order: strip child below the label child");

            // Both children are mouse-transparent (the pill keeps every
            // click/drag/tooltip/hover to itself).
            bool a = true, b = true;
            strip->getInterceptsMouseClicks (a, b);
            check (! a && ! b, "strip child intercepts nothing");
            label->getInterceptsMouseClicks (a, b);
            check (! a && ! b, "label child intercepts nothing");

            // Hit-test parity THROUGH the children: a point inside the strip
            // child (its centre) must still resolve to the PILL — the
            // setInterceptsMouseClicks(false,false) contract that keeps
            // getComponentAt / real mouse events on the pill.
            const auto centre = strip->getBounds().getCentre();
            auto* hit = lfoPill->getComponentAt (centre);
            check (hit == lfoPill,
                   "getComponentAt(strip centre) resolves to the pill (children transparent)");

            // The label paint seam starts at zero (no paint cycle has run).
            check (bar.pillLabelPaintCountForTest (idx) == 0,
                   "label paint count starts at 0 (no paint yet)");
        }
    }

    // ---- [2] The label still paints (offscreen full-tree render) ----
    std::printf ("\n[2] label child paints in a full-tree render\n");
    {
        auto* lfoPill = findPillByTooltip (&bar, "LFO 2");
        const int idx = lfoPill != nullptr ? pillIndexOf (bar, lfoPill) : -1;
        check (idx >= 0, "pill index resolvable for the render check");
        if (idx >= 0)
        {
            // The screen_shots.cpp idiom: render the WHOLE bar (children
            // included) into an image context — the label child's paint must
            // run (the split must not lose the text, only decouple it).
            juce::Image img (juce::Image::ARGB, bar.getWidth(), bar.getHeight(), true);
            {
                juce::Graphics g (img);
                bar.paintEntireComponent (g, false);
            }
            const int count = bar.pillLabelPaintCountForTest (idx);
            char m[96];
            std::snprintf (m, sizeof (m), "label child painted exactly once per full render (count=%d)", count);
            check (count == 1, m);
        }
    }

    // ---- [3] THE CPU CONTRACT: strip dirty rects never re-raster the label ----
    // MECHANISM-FAITHFUL DISPATCH (2026-08-23 review settlement): a real
    // peer's paint callback walks the WHOLE hierarchy CLIPPED to the dirty
    // rect (juce_ComponentPeer::handlePaint -> paintEntireComponent), and
    // JUCE has NO per-component dirty tracking — so a strip-tick dirty rect
    // DOES dispatch a clipped paint to the label sibling too (the earlier
    // off-screen-peer pump pinned nothing here: the probe window proved such
    // a peer delivers NO paints after the initial one, so the old assertion
    // passed vacuously). The contract is therefore pinned by REPLAYING the
    // peer dispatch deterministically: a Graphics clipped to the strip
    // child's bounds-in-bar, then bar.paintEntireComponent — exactly what
    // handlePaint does with the strip's dirty rect. The label child is
    // BUFFERED TO AN IMAGE, so that dispatch answers with a cached-bitmap
    // BLIT (Component::paintWithinParentContext -> cachedImage->paint) and
    // its paint() — glyph layout + text raster — must NOT run; the strip
    // child's paint count (the positive control) must increment every
    // dispatch, proving the walk really reached the pair.
    std::printf ("\n[3] strip dirty-rect dispatch leaves the label paint() uninvoked (blit path)\n");
    {
        ThemeManager themeMgr3;
        CentralModBar bar3 (themeMgr3);
        bar3.setBounds (0, 0, 900, CentralModBar::kBarHeight);
        bar3.setVisible (true);
        auto* lfoPill = findPillByTooltip (&bar3, "LFO 2");
        const int idx = lfoPill != nullptr ? pillIndexOf (bar3, lfoPill) : -1;
        check (idx >= 0, "pill index resolvable for the dispatch check");
        if (idx >= 0)
        {
            auto* stripChild = bar3.pillStripChildForTest (idx);
            auto* labelChild = bar3.pillLabelChildForTest (idx);
            check (stripChild != nullptr && labelChild != nullptr,
                   "strip + label children resolvable");
            if (stripChild != nullptr && labelChild != nullptr)
            {
                // The strip child's bounds in BAR coordinates — accumulate
                // the origin through the WHOLE parent chain (the pills live
                // inside the bar's scrolled Viewport, so the pill's own
                // parent-space position is NOT bar space): the dirty rect a
                // strip animation repaint produces.
                juce::Point<int> chain {};
                for (auto* c = stripChild; c != nullptr && c != &bar3; c = c->getParentComponent())
                    chain += c->getPosition();
                const auto stripRectInBar = stripChild->getBounds().withPosition (chain);

                // One full render FIRST: builds the label's buffered image
                // (its paint() runs exactly once — the [2] contract).
                {
                    juce::Image img (juce::Image::ARGB, bar3.getWidth(), bar3.getHeight(), true);
                    juce::Graphics g (img);
                    bar3.paintEntireComponent (g, false);
                }
                const int labelAfterFull  = bar3.pillLabelPaintCountForTest (idx);
                const int stripAfterFull  = bar3.pillStripPaintCountForTest (idx);
                check (labelAfterFull == 1,
                       "full render painted the label exactly once (buffer built)");

                // Replay the strip dirty-rect dispatch several times.
                for (int dispatch = 0; dispatch < 5; ++dispatch)
                {
                    juce::Image img (juce::Image::ARGB, bar3.getWidth(), bar3.getHeight(), true);
                    juce::Graphics g (img);
                    g.reduceClipRegion (stripRectInBar);
                    bar3.paintEntireComponent (g, false);
                }
                {
                    char m[160];
                    std::snprintf (m, sizeof (m),
                                   "strip dirty-rect dispatches reached the STRIP child (count %d -> %d)",
                                   stripAfterFull, bar3.pillStripPaintCountForTest (idx));
                    check (bar3.pillStripPaintCountForTest (idx) >= stripAfterFull + 5, m);
                }
                {
                    char m[160];
                    std::snprintf (m, sizeof (m),
                                   "label paint() NEVER invoked by strip dispatches (count %d -> %d; blit path)",
                                   labelAfterFull, bar3.pillLabelPaintCountForTest (idx));
                    check (bar3.pillLabelPaintCountForTest (idx) == labelAfterFull, m);
                }

                // The label's OWN change (a hover flip, exactly as a real
                // pointer enters) must invalidate the buffered image and
                // re-render it on the next dispatch — the split must not
                // freeze real state changes.
                if (auto* pill = bar3.pillComponentForTest (idx))
                {
                    const auto source = juce::Desktop::getInstance().getMainMouseSource();
                    const auto now = juce::Time::getCurrentTime();
                    const juce::Point<float> pos (pill->getLocalBounds().getCentre().toFloat());
                    pill->mouseEnter (juce::MouseEvent (source, pos, juce::ModifierKeys(),
                                                         juce::MouseInputSource::defaultPressure,
                                                         juce::MouseInputSource::defaultOrientation,
                                                         juce::MouseInputSource::defaultRotation,
                                                         juce::MouseInputSource::defaultTiltX,
                                                         juce::MouseInputSource::defaultTiltY,
                                                         pill, pill, now, pos, now, 1, false));
                    {
                        juce::Image img (juce::Image::ARGB, bar3.getWidth(), bar3.getHeight(), true);
                        juce::Graphics g (img);
                        bar3.paintEntireComponent (g, false);
                    }
                    check (bar3.pillLabelPaintCountForTest (idx) == labelAfterFull + 1,
                           "label re-renders once after its own state change (buffer invalidated)");
                }
            }
        }
    }

    // ---- [4] LABEL COLOUR PARITY (pre-split behaviour, confirmed against
    // git HEAD by the 2026-08-23 review): GENERATOR pills lift their label
    // tier on hover (textSecondary -> brighter), DRAG-ONLY pills NEVER do
    // (always textSecondary; their hover cue is the pill fill). Pinned by
    // pixel-exact comparison of the label child's own render, hovered vs
    // not — deterministic (same context, same size), so equality/inequality
    // is exact with no thresholds.
    std::printf ("\n[4] label hover-tier parity (generators lift, drag-only never)\n");
    {
        ThemeManager themeMgr4;
        CentralModBar bar4 (themeMgr4);
        bar4.setBounds (0, 0, 900, CentralModBar::kBarHeight);
        bar4.setVisible (true);

        auto renderLabel = [&bar4] (int pillIdx) -> juce::Image
        {
            auto* label = bar4.pillLabelChildForTest (pillIdx);
            juce::Image img (juce::Image::ARGB, label->getWidth(), label->getHeight(), true);
            juce::Graphics g (img);
            label->paintEntireComponent (g, false);
            return img;
        };
        auto sendEnter = [&bar4] (int pillIdx)
        {
            if (auto* pill = bar4.pillComponentForTest (pillIdx))
            {
                const auto source = juce::Desktop::getInstance().getMainMouseSource();
                const auto now = juce::Time::getCurrentTime();
                const juce::Point<float> pos (pill->getLocalBounds().getCentre().toFloat());
                pill->mouseEnter (juce::MouseEvent (source, pos, juce::ModifierKeys(),
                                                     juce::MouseInputSource::defaultPressure,
                                                     juce::MouseInputSource::defaultOrientation,
                                                     juce::MouseInputSource::defaultRotation,
                                                     juce::MouseInputSource::defaultTiltX,
                                                     juce::MouseInputSource::defaultTiltY,
                                                     pill, pill, now, pos, now, 1, false));
            }
        };
        auto sendExit = [&bar4] (int pillIdx)
        {
            if (auto* pill = bar4.pillComponentForTest (pillIdx))
            {
                const auto source = juce::Desktop::getInstance().getMainMouseSource();
                const auto now = juce::Time::getCurrentTime();
                const juce::Point<float> pos (pill->getLocalBounds().getCentre().toFloat());
                pill->mouseExit (juce::MouseEvent (source, pos, juce::ModifierKeys(),
                                                   juce::MouseInputSource::defaultPressure,
                                                   juce::MouseInputSource::defaultOrientation,
                                                   juce::MouseInputSource::defaultRotation,
                                                   juce::MouseInputSource::defaultTiltX,
                                                   juce::MouseInputSource::defaultTiltY,
                                                   pill, pill, now, pos, now, 1, false));
            }
        };
        auto imagesEqual = [] (const juce::Image& a, const juce::Image& b) -> bool
        {
            if (a.getBounds() != b.getBounds()) return false;
            juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly);
            juce::Image::BitmapData db (b, juce::Image::BitmapData::readOnly);
            for (int y = 0; y < da.height; ++y)
                for (int x = 0; x < da.width; ++x)
                    if (da.getPixelColour (x, y) != db.getPixelColour (x, y))
                        return false;
            return true;
        };

        auto* genPill = findPillByTooltip (&bar4, "LFO 2");          // generator (has history strip)
        auto* dragPill = findPillByTooltip (&bar4, "Wheel");          // drag-only (Perf cluster)
        const int genIdx = genPill  != nullptr ? pillIndexOf (bar4, genPill)  : -1;
        const int dragIdx = dragPill != nullptr ? pillIndexOf (bar4, dragPill) : -1;
        check (genIdx >= 0 && dragIdx >= 0, "generator + drag-only pills resolvable");
        if (genIdx >= 0 && dragIdx >= 0)
        {
            // GENERATOR: unhovered vs hovered label renders must DIFFER (the
            // hover tier lift is a real colour change).
            const auto genPlain = renderLabel (genIdx);
            sendEnter (genIdx);
            const auto genHover = renderLabel (genIdx);
            sendExit (genIdx);
            check (! imagesEqual (genPlain, genHover),
                   "generator label changes on hover (tier lift preserved by the split)");

            // DRAG-ONLY: unhovered vs hovered label renders must be IDENTICAL
            // (pre-split parity: the label tier NEVER moves on hover).
            const auto dragPlain = renderLabel (dragIdx);
            sendEnter (dragIdx);
            const auto dragHover = renderLabel (dragIdx);
            sendExit (dragIdx);
            check (imagesEqual (dragPlain, dragHover),
                   "drag-only label is hover-invariant (pre-split parity restored)");
        }
    }

    std::printf ("\nMODBAR PILL PAINT SPLIT TEST: %s (%d failure%s)\n",
                 g_failures ? "FAILURES" : "ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
