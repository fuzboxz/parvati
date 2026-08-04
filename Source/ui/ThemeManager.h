// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ThemeManager — owns the built-in (and future user) themes plus the currently
// selected one, and broadcasts a change notification whenever the selection
// moves. Persistence is via a juce::ValueTree so it composes with the APVTS
// state later (Phase 3d adds zoom + tooltips to the same tree). Phase 1a of
// docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>   // ValueTree

#include <vector>

#include "ParvatiTheme.h"

//==============================================================================
class ThemeManager : public juce::ChangeBroadcaster
{
public:
    /** Loads the built-in themes with Carbon selected (index 0). */
    ThemeManager();

    /** All theme names, in storage order. */
    const std::vector<juce::String> getThemeNames() const;

    /** The currently selected theme. Never null (Carbon is always index 0). */
    const ParvatiTheme& getCurrentTheme() const;

    /** Theme at @p index, clamped to the valid range. */
    const ParvatiTheme& getTheme (int index) const;

    //==========================================================================
    // Selection. By name is preferred (robust to reordering).
    // Returns false — and broadcasts nothing — if @p name is unknown.
    bool selectByName (const juce::String& name);

private:
    std::vector<ParvatiTheme> themes_;
    int currentIndex_ = 0;
};
