// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ModMatrixView — the redesigned modulation-matrix panel. It replaces the old
// paginated (1-4 / 5-8 / 9-12 / 13-14) sub-tab matrix with a single, vertically
// scrollable list of ACTIVE routings plus a trailing "+ Add Modulation" row.
//
// Ambika backward compatibility is preserved by CONVENTION ONLY (no patch/DSP
// change): every one of the 14 engine slots is the existing APVTS triplet
//   mod{1..14}_source (AudioParameterChoice), mod{1..14}_dest (choice),
//   mod{1..14}_amount (AudioParameterInt, -63..+63).
// A slot is ACTIVE iff amount != 0, OR it is transiently muted (editor-only
// state). Inactive slots are hidden from the list (their attachments stay
// alive — only visibility toggles). Mute stashes the amount, writes 0 to the
// engine (true bypass) and keeps the row visible/greyed; it is in-memory only
// and does NOT round-trip through presets (agreed convention).
//
// The shared list/row machinery lives in MatrixViewBase (with the FX twin of
// this view). This header adds only the synth-side specifics: the 14-slot
// config, the APVTS-attached dest combos, and the test seams.
//
// Modulators are dragged ONLY from the CentralModBar pills — matrix rows are
// NOT drag sources (user feedback 2026-08-20); dropping a pill on a
// destination knob still assigns through this view's assignNextFreeSlot.

#pragma once

#include "MatrixViewBase.h"

class ParvatiAudioProcessor;
class ThemeManager;

//==============================================================================
class ModMatrixView : public MatrixViewBase
{
public:
    ModMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager);

    // Test hook: the category colour the given row's lamp/slider/tint resolve
    // to for its CURRENT source (the same rowCategoryColour mapping the row
    // uses; exposed so tests can pin the lamp == modulator-colour contract
    // without duplicating the mapping).
    juce::Colour rowCategoryColourForTest (int slot) const;

    // APVTS param ID for a 0-based slot: slotParam(3, "_amount") == "mod4_amount".
    static juce::String slotParam (int slot, const char* suffix);

    // ---- TEST-ONLY introspection (tests/mod_matrix_ui_test.cpp) ----
    // Rows must NOT be drag sources (user feedback 2026-08-20: modulators are
    // dragged ONLY from the CentralModBar pills; the former per-row drag-grip
    // was removed). Pins the contract; always false by construction now.
    bool canStartDragFromRowForTest() const noexcept { return false; }
    // The row component for a slot as a generic Component, or nullptr. Lets
    // tests reach the row's children (mute lamp / delete X / combos) without
    // exposing the row type.
    juce::Component* rowForSlotForTest (int slot) const;

    // iOS HIG: taller (48pt) rows give 44pt touch targets; the matrix scrolls
    // vertically so taller rows are free (it just shows fewer rows). Exposed
    // public (access-only; no symbol/codegen change) so the HIG sizing-contract
    // test can static_assert it per platform.
    static constexpr int kRowHeight = MatrixViewBase::kRowHeight;      // 48 (was iOS 48 / desktop 34)

    // "+ Add Modulation" / "Matrix Full" row: the 44pt HIG touch minimum.
    // The rows above scroll inside the Viewport, so the extra height is free.
    // Pinned by tests/ipad_hig_sizing_test.cpp.
    static constexpr int kAddButtonH = MatrixViewBase::kAddButtonH;    // 44

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixView)
};
