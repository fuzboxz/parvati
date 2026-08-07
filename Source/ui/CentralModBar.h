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
    static constexpr int kBarHeight = 38;

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

    /** The true minimum width to show every pill + cluster label with no
        clipping (the host uses this as the window minimum width). */
    int preferredWidth() const;

    //==========================================================================
    // Public accessors used by the file-local pill component.
    const ParvatiTheme& theme() const;
    juce::Font          pillFont() const;   // font used to draw + measure pills

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** File-local pill component (defined in the .cpp). */
    struct ModPill;
    friend struct ModPill;

    /** Forwards a pill click to the registered callback. */
    void invokeClicked (int modSrcEnum);

    /**
        Lays the 7 clusters out left -> right. When @p positionChildren is true
        the pills are positioned; either way the total preferred width is
        returned. If @p outLabelRects is non-null it is filled with the seven
        cluster-caption rectangles (used by paint()).
    */
    int computeLayout (bool positionChildren,
                       std::vector<juce::Rectangle<int>>* outLabelRects) const;

    juce::Font labelFont() const;   // small cluster-caption font

    ThemeManager& themeManager_;
    std::function<void (int)> onPillClicked_;

    std::vector<std::unique_ptr<ModPill>> pills_;
    std::vector<juce::Rectangle<int>>     clusterLabelRects_;   // one per cluster
    int activeEnum_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CentralModBar)
};
