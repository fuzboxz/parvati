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
// Each row exposes: source combo (category-tinted), dest combo, a bipolar depth
// slider (-100%..+100%, centre zero-detent, double-click = reset to 0), a Mute
// toggle and a Clear button. The view is fully self-contained: it owns its 14
// rows + Add button + a local bipolar LookAndFeel, and is wired into the editor
// (replacing the Mod Matrix GroupPager) in a later step.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>   // Component, Viewport, Label, TextButton, Timer

#include <array>
#include <memory>

class ParvatiAudioProcessor;
class ThemeManager;

//==============================================================================
class ModMatrixView : public juce::Component,
                      private juce::Timer
{
public:
    ModMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager);
    ~ModMatrixView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-resolve theme colours onto every row + the local L&F; call after a
    // theme switch (and from the editor's applyThemeColors pass).
    void applyThemeColors();

    // Re-evaluate the active set and rebuild the list layout. Cheap: it early-
    // outs when nothing changed. Safe to call from the timer / after any
    // programmatic param write (mute / clear / add / preset load / undo).
    void refresh();

    // ---- Accessors used by the (file-local) ModRow + the wiring step ----
    ParvatiAudioProcessor& processor() noexcept { return processor_; }
    ThemeManager&          themeManager() noexcept { return themeManager_; }

    // Slot state, 0-based (slot 0 == "Mod 1").
    int  amountForSlot (int slot) const;             // raw int -63..+63 from the APVTS
    bool isSlotMuted (int slot) const noexcept { return muted_[(size_t) juce::jlimit (0, 13, slot)]; }
    int  stashedAmount (int slot) const noexcept { return stashedAmount_[(size_t) juce::jlimit (0, 13, slot)]; }
    bool isSlotActive (int slot) const;              // amount != 0 || muted
    int  firstFreeSlot() const;                      // 0..13 or -1 if the matrix is full

    // Operations invoked from a row (Mute / Clear / Add). All write through the
    // APVTS so they stay byte-bridged + undoable, then trigger a refresh.
    void toggleMute (int slot);
    void clearSlot (int slot);
    void addSlot();

    // Programmatic free-slot assignment — the engine behind addSlot() AND the
    // drag-and-drop drop target. Finds the first free slot (amount==0, !muted),
    // writes its source/dest/amount through the APVTS (byte-bridged + undoable,
    // the same path as typing in the combos), then refreshes. @return false if
    // the 14-slot matrix is full (no free slot).
    bool assignNextFreeSlot (int sourceEnum, int destEnum, int amount = 32);

    // Selected source's display name for a slot ("Env 1", "LFO 2", ...) — used by
    // rows for the category tint + accent bar.
    juce::String sourceNameForSlot (int slot) const;

    // Briefly flash every ACTIVE row currently routed FROM @p sourceEnum (a
    // MOD_SRC_* value), reusing the same transient flash the slot-selection
    // (knob double-click) uses. Called from the CentralModBar when a drag-only
    // (Perf/Util/Const) source pill is clicked, so the user can see where that
    // source is routed. A no-op (besides clearing any prior flash) when no row
    // uses the source. The flash auto-expires via flashTick() on the timer.
    void flashRowsForSource (int sourceEnum);

    // The ModulationDestination (MOD_DST_*) a slot is currently routed to, read
    // live from its mod{N}_dest APVTS raw value (-1 on error). Used by the hover
    // highlight so a row's target dest follows live edits of its dest combo.
    int destForSlot (int slot) const;

    // The ModulationSource (MOD_SRC_*) a slot is currently routed FROM, read
    // live from its mod{N}_source APVTS raw value (-1 on error). Used by the
    // drag-grip so the dragged payload reflects the row's current source.
    int sourceForSlot (int slot) const;

    // APVTS param ID for a 0-based slot: slotParam(3, "_amount") == "mod4_amount".
    static juce::String slotParam (int slot, const char* suffix);

private:
    // Per-slot editor-only mute state (NOT persisted; cleared on external amount change).
    std::array<bool, 14> muted_ {};
    std::array<int, 14>  stashedAmount_ {};

    void timerCallback() override { refresh(); flashTick(); }
    void setAmountForSlot (int slot, int amount);    // writes the APVTS (denormalized -> 0..1)
    void rebuildLayout();
    juce::String buildSignature() const;             // compact "active set" fingerprint

    // ---- ModMatrixHighlight bus (hover + double-click-to-row) ----
    // React to a dest-highlight broadcast: emphasise every row whose current
    // dest matches @p modDst (-1 clears all).
    void onHighlightDest (int modDst);
    // React to a slot-selection broadcast (from double-clicking a knob's ring):
    // scroll the row into view and flash it briefly. -1 clears the flash.
    void onSelectSlot (int slotIndex);
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
    juce::Label    headerLabel_;                     // "N of 14 Used"
    std::unique_ptr<juce::TextButton> addButton_;    // "+ Add Modulation" / "Matrix Full"

    // Local bipolar depth-slider LookAndFeel (owned here; set on every slider).
    // Declared BEFORE rows_ so it is destroyed AFTER them (the sliders reference
    // it via setLookAndFeel): member destruction runs in reverse declaration order.
    class BipolarSliderLNF;
    std::unique_ptr<BipolarSliderLNF> bipolarLnf_;

    std::array<std::unique_ptr<struct ModMatrixRow>, 14> rows_;

    juce::String lastSignature_;   // skip relayout when the active set is unchanged

    // ModMatrixHighlight bus subscriptions (ids for unsubscribe in the dtor) +
    // the transient selection-flash state. The flash marks the row a knob's
    // double-click jumped to; it auto-expires via flashTick() on the timer.
    // Generalized to a SET of slots so the single-slot knob-double-click jump
    // (onSelectSlot) AND the multi-row source-flash (flashRowsForSource) share
    // one timed expiry.
    int destHighlightSub_ = -1;
    int slotSelectSub_    = -1;
    int assignSub_        = -1;   // drag-drop assign handler (unsubscribe in dtor)
    juce::Array<int> flashSlots_;
    juce::uint32 flashStartMs_ = 0;
    static constexpr int kFlashMs = 1200;

    // iOS HIG: taller (48pt) rows give 44pt touch targets; the matrix scrolls
    // vertically so taller rows are free (it just shows fewer rows). Exposed
    // public (access-only; no symbol/codegen change) so the HIG sizing-contract
    // test can static_assert it per platform.
public:
    static constexpr int kRowHeight = 48;   // unified (was iOS 48 / desktop 34)

    // "+ Add Modulation" / "Matrix Full" row: the 44pt HIG touch minimum.
    // The rows above scroll inside the Viewport, so the extra height is free.
    // Pinned by tests/ipad_hig_sizing_test.cpp.
    static constexpr int kAddButtonH = 44;
private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixView)
};
