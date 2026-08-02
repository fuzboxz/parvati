// Copyright (c) 2024 805LABS / Parvati.
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

    /** Number of available themes (>= 5). */
    int getNumThemes() const;

    /** All theme names, in storage order. */
    const std::vector<juce::String> getThemeNames() const;

    /** The currently selected theme. Never null (Carbon is always index 0). */
    const ParvatiTheme& getCurrentTheme() const;

    /** Theme at @p index, clamped to the valid range. */
    const ParvatiTheme& getTheme (int index) const;

    /** Index of the currently selected theme. */
    int getCurrentIndex() const;

    /** Name of the currently selected theme. */
    juce::String getCurrentThemeName() const;

    //==========================================================================
    // Selection. By name is preferred (robust to reordering).
    // Returns false — and broadcasts nothing — if @p name is unknown.
    bool selectByName (const juce::String& name);

    // Clamps @p index to the valid range, then broadcasts.
    void selectByIndex (int index);

    //==========================================================================
    // Persistence. The round-tripped tree has:
    //   ValueTree type="ParvatiUI"  prop "theme"="<name>"
    // (Phase 3d will add "zoom"(double) + "tooltips"(bool); forward-compatible.)
    juce::ValueTree toValueTree() const;

    // Restores the selection by theme name from @p vt and broadcasts on success.
    // Returns false (no broadcast) if @p vt is not a "ParvatiUI" tree or its
    // "theme" name is unknown.
    bool fromValueTree (const juce::ValueTree& vt);

private:
    std::vector<ParvatiTheme> themes_;
    int currentIndex_ = 0;
};
