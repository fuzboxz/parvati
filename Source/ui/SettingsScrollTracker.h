// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// SettingsScrollTracker — sizes the SettingsPanel inside the settings
// drawer's Viewport. Extracted from PluginEditor.h unchanged (the editor
// header held the full ~110-line class definition).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SettingsPanel.h"

//==============================================================================
// Sizes the SettingsPanel inside the settings drawer's Viewport (2026-08-21):
// a juce::Viewport never resizes its viewed component, and the SidePanel
// parents the VIEWPORT, so the panel must be sized from the viewport's live
// view area — width minus any auto-shown scrollbar, height = the panel's FULL
// row budget (never shorter than the view, so scrolling reads 1:1). Without
// this the panel kept its 0×0 birth size and the drawer rendered blank.
class SettingsScrollTracker : public juce::ComponentListener
{
public:
    SettingsScrollTracker (juce::Viewport& vp, SettingsPanel& panel)
        : viewport_ (vp), panel_ (panel)
    {
        apply();
    }

    // MUST deregister: juce::ComponentListener does NOT auto-detach, and this
    // tracker is declared after the viewport (destroyed BEFORE it) — without
    // the explicit removal the viewport's listener list keeps a dangling
    // pointer and its own destructor calls into freed memory (the teardown
    // segfault this class introduced).
    ~SettingsScrollTracker() override
    {
        viewport_.removeComponentListener (this);
    }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
            apply();
    }

    // Editor hook (onPanelShowHide): the slide animation may not fire a
    // component-resize on the viewport, so the show edge re-applies directly.
    void applyFromEditor() { apply(); }

    // OPEN-PATH PRE-SIZE (2026-08-22 open-latency fix): call immediately
    // BEFORE SidePanel::showOrHide(true). JUCE's slide animation runs through
    // a PROXY COMPONENT that snapshots the drawer at animation start, and
    // onPanelShowHide (the normal apply site) fires only AFTER the ~250 ms
    // slide — so the drawer slid in BLANK and the content popped at the end
    // (the visible first-open latency). Sizing here (while still hidden) puts
    // the laid-out rows into the proxy snapshot: the drawer slides in already
    // drawn. Bypasses the isShowing() gate on purpose; every other invariant
    // (closed drawer stays 0x0 — the close edge re-collapses via apply())
    // holds.
    void preSizeForOpen() { apply (true); }

private:
    void apply (bool bypassShowingGate = false)
    {
        // Only size a SHOWING drawer: while closed, the panel must stay 0×0 so
        // its children are not "placed" (the HIG 44pt audit walks the tree and
        // zero-sized-bounds buttons at the closed drawer's off-screen position
        // read as sub-44 violations). The show path re-applies via the
        // resize listener + the editor's onPanelShowHide hook.
        //
        // WIDTH SOURCE (the 2026-08-22 blank-drawer fix): use the VIEWPORT's
        // own bounds, NOT getViewWidth()/getViewHeight(). Those return
        // lastVisibleArea, which Viewport::updateVisibleArea computes as
        // jmin(viewedComponentExtent, viewportExtent) — and the viewed
        // component is exactly the panel this tracker sizes (0×0 while
        // collapsed). That was a circular gate that could never open: the
        // drawer rendered blank (all 18 rows 0×0) because viewW stayed 0
        // until the panel was sized, and the panel was never sized because
        // viewW was 0.
        if (! bypassShowingGate && (! viewport_.isShowing() || viewport_.getWidth() <= 0))
        {
            if (panel_.getWidth() != 0 || panel_.getHeight() != 0)
                panel_.setSize (0, 0);
            return;
        }
        const int preferredH = panel_.computePreferredHeight();
        const int viewH = viewport_.getHeight();
        // Budget for the auto vertical scrollbar: when the full row budget
        // exceeds the view, the viewport narrows its content area by the
        // scrollbar thickness — size the panel for that so rows are not
        // clipped under the bar.
        const int scrollbarW = (preferredH > viewH) ? viewport_.getScrollBarThickness() : 0;
        const int w = viewport_.getWidth() - scrollbarW;
        const int h = juce::jmax (preferredH, viewH);
        if (panel_.getWidth() != w || panel_.getHeight() != h)
        {
            panel_.setTopLeftPosition (0, 0);
            panel_.setSize (w, h);
        }
        // Bar visibility, set EXPLICITLY: JUCE's updateVisibleArea has an
        // early-return path (content repositioning) that skips the bar's
        // setVisible — observed as content-area narrowed for a bar that never
        // appeared. Mirroring the intended state here keeps the affordance
        // real; idempotent with the viewport's own management.
        viewport_.getVerticalScrollBar().setVisible (h > viewH);
    }

    juce::Viewport&   viewport_;
    SettingsPanel&    panel_;
};
