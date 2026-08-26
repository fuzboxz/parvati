// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FxMatrixView — the per-part FX modulation-matrix panel. The FX twin of
// ModMatrixView: the shared list/row machinery lives in MatrixViewBase; this
// class adds the FX-side specifics:
//
//   * 16 slots (kNumFxMatrixSlots) instead of the synth's 14.
//   * Slot params use the "fxmod" prefix: fxmod{1..16}_source / _dest / _amount.
//   * The dest combo reads the FX destinations, but NOT through an APVTS
//     attachment: its labels are DYNAMIC (each slot's actual parameter names
//     from the live fx{1,2,3}_type) and it is index-bound manually. The
//     stored fxmod{N}_dest value stays the stable FX_DST_* index.
//   * Amount range stays -63..+63 (the shared bipolar depth slider).
//
// BUS INTEGRATION: this view subscribes to the editor-scoped
// ModMatrixHighlight bus. FX destinations are OFFSET-ENCODED above the synth
// range (FX_DST_* + kFxModDstOffset == 19..33, disjoint from synth
// MOD_DST_* 0..18). Every cross-domain signal guards on the offset (in
// MatrixViewBase), so the synth and FX matrices never bleed into each other:
//   * onAssignRequest   - drag-drop onto an FX knob assigns the next free FX
//     slot; synth dests (< 19) are ignored (the synth handler owns those).
//   * onDestHighlighted - hovering an FX knob / FX row highlights every FX row
//     routed to that dest (synth broadcasts 0..18 are rejected).
//   * onSlotSelected    - double-clicking an FX knob scrolls + flashes its FX
//     row (the slot is offset-encoded; synth-encoded slots are rejected).
// The transient source-flash (flashRowsForSource) is driven directly by the
// FX workspace's CentralModBar (drag-only pill click).

#pragma once

#include "MatrixViewBase.h"

#include "dsp/fx/FxTypes.h"   // FxType, kNumFxMatrixSlots (16)

#include <array>

class HellcatAudioProcessor;
class ThemeManager;

//==============================================================================
class FxMatrixView : public MatrixViewBase
{
public:
    FxMatrixView (HellcatAudioProcessor& processor, ThemeManager& themeManager);

    // Base refresh plus the FX-dest combo reconcile: when a slot's FX type
    // changes (a type edit or a part switch) every row's dest combo is
    // rebuilt to the slots' ACTUAL parameter names. The dest combos carry no
    // APVTS attachment, so their item lists + selections are reconciled here
    // every tick regardless of the active-set signature.
    void refresh() override;

    // Test hook: the live row component for @p slot (0-based) — lets headless
    // tests sweep the row's children (icon buttons / lamp presence, the
    // no-text-button regression) without reaching into the file-local struct.
    juce::Component* rowForSlotForTest (int slot);

    // APVTS param ID for a 0-based slot: slotParam(3, "_amount") == "fxmod4_amount".
    static juce::String slotParam (int slot, const char* suffix);

    // iOS HIG: taller (48pt) rows give 44pt touch targets; the matrix scrolls
    // vertically so taller rows are free (it just shows fewer rows). Exposed
    // public (access-only; no symbol/codegen change) so the HIG sizing-contract
    // test can static_assert it per platform.
    static constexpr int kRowHeight = MatrixViewBase::kRowHeight;      // 48 (was iOS 48 / desktop 34)

    // "+ Add Modulation" / "Matrix Full" row: the 44pt HIG touch minimum.
    // The rows above scroll inside the Viewport, so the extra height is free.
    // Pinned by tests/ipad_hig_sizing_test.cpp.
    static constexpr int kAddButtonH = MatrixViewBase::kAddButtonH;    // 44

private:
    // The current per-slot FX types (fx1/2/3_type), read live from the APVTS.
    // When these change (a type edit or a part switch) every row's FX-dest
    // combo is rebuilt to the slots' ACTUAL parameter names (e.g. "FX1
    // Position" for a Looping Delay) instead of the static "FX{N} Param K"
    // labels.
    std::array<FxType, 3> currentSlotTypes() const;

    // Last-seen per-slot FX types; initialised to FxType::Count so the first
    // refresh() always (re)builds the dynamic FX-dest combo labels. Compared
    // against currentSlotTypes() each tick to trigger a rebuild only on change.
    std::array<FxType, 3> lastSlotTypes_ { FxType::Count, FxType::Count, FxType::Count };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxMatrixView)
};
