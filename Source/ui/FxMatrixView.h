// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxMatrixView — the per-part FX modulation-matrix panel. A self-contained
// clone of ModMatrixView (same scrollable active-row list + drag-drop grip +
// bipolar depth slider + Mute/Clear/Add), mechanically adapted for the FX mod
// matrix:
//
//   * 16 slots (kNumFxMatrixSlots) instead of the synth's 14.
//   * Slot params use the "fxmod" prefix: fxmod{1..16}_source / _dest / _amount.
//   * Dest combo reads the FX destinations choice list (the same list
//     fxmod{N}_dest uses: "FX1 Dry/Wet","FX1 Param 1".."FX3 Param 4") directly
//     from its APVTS choice param — no MOD_DST_* coupling.
//   * Amount range stays -63..+63 (reuses the bipolar L&F verbatim).
//   * Drag payload "parvatiModSrc:<enum>" is unchanged, so a drag from an FX row
//     grip drops onto a synth destination knob exactly like a synth matrix row.
//
// ISOLATION NOTE: this view deliberately does NOT subscribe to the editor-scoped
// ModMatrixHighlight bus. That bus is keyed on synth MOD_DST_* values and is
// shared with ModMatrixView; an FX row subscribing would both (a) grab a free
// slot in BOTH matrices on every drop (requestAssign fans out to every handler)
// and (b) falsely highlight rows when a synth knob is hovered (FX_DST values
// 0..14 overlap MOD_DST values 0..14). The FX matrix is instead edited through
// its own combos / Add button / drag grip, and a hovered FX row highlights
// itself locally (no broadcast). The transient source-flash (flashRowsForSource)
// is driven directly by the FX workspace's CentralModBar (drag-only pill click),
// not via the bus.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>   // Component, Viewport, Label, TextButton, Timer

#include <array>
#include <memory>

#include "dsp/fx/FxTypes.h"   // kNumFxMatrixSlots (16)

class ParvatiAudioProcessor;
class ThemeManager;

//==============================================================================
class FxMatrixView : public juce::Component,
                     private juce::Timer
{
public:
    FxMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager);
    ~FxMatrixView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-resolve theme colours onto every row + the local L&F; call after a
    // theme switch (and from the editor's applyThemeColors pass).
    void applyThemeColors();

    // Re-evaluate the active set and rebuild the list layout. Cheap: it early-
    // outs when nothing changed. Safe to call from the timer / after any
    // programmatic param write (mute / clear / add / preset load / undo).
    void refresh();

    // ---- Accessors used by the (file-local) FxMatrixRow + the wiring step ----
    ParvatiAudioProcessor& processor() noexcept { return processor_; }
    ThemeManager&          themeManager() noexcept { return themeManager_; }

    // Slot state, 0-based (slot 0 == "Mod 1").
    int  amountForSlot (int slot) const;             // raw int -63..+63 from the APVTS
    bool isSlotMuted (int slot) const noexcept { return muted_[(size_t) juce::jlimit (0, 15, slot)]; }
    int  stashedAmount (int slot) const noexcept { return stashedAmount_[(size_t) juce::jlimit (0, 15, slot)]; }
    bool isSlotActive (int slot) const;              // amount != 0 || muted
    int  firstFreeSlot() const;                      // 0..15 or -1 if the matrix is full

    // Operations invoked from a row (Mute / Clear / Add). All write through the
    // APVTS so they stay byte-bridged + undoable, then trigger a refresh.
    void toggleMute (int slot);
    void clearSlot (int slot);
    void addSlot();

    // Programmatic free-slot assignment — the engine behind addSlot(). Finds the
    // first free slot (amount==0, !muted), writes its source/dest/amount through
    // the APVTS (byte-bridged + undoable), then refreshes. @return false if the
    // 16-slot matrix is full (no free slot).
    bool assignNextFreeSlot (int sourceEnum, int destEnum, int amount = 32);

    // Selected source's display name for a slot ("Env 1", "LFO 2", ...) — used by
    // rows for the category tint + accent bar.
    juce::String sourceNameForSlot (int slot) const;

    // Briefly flash every ACTIVE row currently routed FROM @p sourceEnum (a
    // MOD_SRC_* value), reusing the same transient flash the slot-selection
    // (knob double-click) uses. Called from the FX workspace's CentralModBar when
    // a drag-only (Perf/Util/Const) source pill is clicked, so the user can see
    // where that source is routed in the FX matrix. A no-op (besides clearing
    // any prior flash) when no row uses the source. The flash auto-expires via
    // flashTick() on the timer.
    void flashRowsForSource (int sourceEnum);

    // The FX destination (FX_DST_*) a slot is currently routed to, read live from
    // its fxmod{N}_dest APVTS raw value (-1 on error).
    int destForSlot (int slot) const;

    // The ModulationSource (MOD_SRC_*) a slot is currently routed FROM, read
    // live from its fxmod{N}_source APVTS raw value (-1 on error). Used by the
    // drag-grip so the dragged payload reflects the row's current source.
    int sourceForSlot (int slot) const;

    // APVTS param ID for a 0-based slot: slotParam(3, "_amount") == "fxmod4_amount".
    static juce::String slotParam (int slot, const char* suffix);

private:
    // Per-slot editor-only mute state (NOT persisted; cleared on external amount change).
    std::array<bool, 16> muted_ {};
    std::array<int, 16>  stashedAmount_ {};

    void timerCallback() override { refresh(); flashTick(); }
    void setAmountForSlot (int slot, int amount);    // writes the APVTS (denormalized -> 0..1)
    void rebuildLayout();
    juce::String buildSignature() const;             // compact "active set" fingerprint

    // Scroll the Viewport so @p slot's row is fully visible.
    void ensureRowVisible (int slot);
    // Advance/clear the transient selection flash (driven from timerCallback).
    void flashTick();

    ParvatiAudioProcessor& processor_;
    ThemeManager&          themeManager_;

    juce::Viewport viewport_;

    // The scrolled content. This JUCE Viewport has no background-colour API, so
    // the content paints its own window-background fill (keeps the list void-free).
    struct ContentPanel : public juce::Component
    {
        juce::Colour bg;
        void paint (juce::Graphics& g) override { g.fillAll (bg); }
    } content_;
    juce::Label    headerLabel_;                     // "N of 16 Used"
    std::unique_ptr<juce::TextButton> addButton_;    // "+ Add Modulation" / "Matrix Full"

    // Local bipolar depth-slider LookAndFeel (owned here; set on every slider).
    // Declared BEFORE rows_ so it is destroyed AFTER them (the sliders reference
    // it via setLookAndFeel): member destruction runs in reverse declaration order.
    class BipolarSliderLNF;
    std::unique_ptr<BipolarSliderLNF> bipolarLnf_;

    std::array<std::unique_ptr<struct FxMatrixRow>, kNumFxMatrixSlots> rows_;

    juce::String lastSignature_;   // skip relayout when the active set is unchanged

    // The transient source-flash state: a SET of slots that share one timed
    // expiry (driven from the FX workspace's CentralModBar drag-only pill click).
    juce::Array<int> flashSlots_;
    juce::uint32 flashStartMs_ = 0;
    static constexpr int kFlashMs = 1200;

    static constexpr int kRowHeight = 34;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxMatrixView)
};
