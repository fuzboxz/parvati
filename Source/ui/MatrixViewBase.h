// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MatrixViewBase — the shared implementation of the two modulation-matrix
// panels (ModMatrixView and FxMatrixView). One view serves the synth matrix
// (14 "mod" slots), the other the per-part FX matrix (16 "fxmod" slots).
// Both show a vertically scrollable list of ACTIVE routings plus a trailing
// "+ Add Modulation" row. A slot is active when its amount is non-zero, or
// when it is transiently muted. Mute stashes the amount and writes zero to
// the engine. Mute is editor-only state. It does not round-trip through
// presets.
//
// Each row holds: a mute/bypass lamp on the far left, the source combo, the
// dest combo, a bipolar depth slider, the value readout, and a delete X on
// the far right. Modulators are dragged only from the CentralModBar pills.
// Matrix rows are never drag sources. The two views differ in few points
// only. MatrixViewConfig lists every difference. The base owns everything
// else, so a fix applies to both views at once.
//
// The dest combo policy differs per view. The synth view binds its dest
// combo through an APVTS attachment. The FX view keeps its dest combo
// detached. It labels items from the live FX types and writes the selected
// index back by hand (see FxMatrixView::refresh).
//
// Bus integration: both views subscribe to the ModMatrixHighlight bus. The
// FX view encodes its dests and slot picks above the synth range
// (FX_DST_* + kFxModDstOffset == 19..33). The offset guards in this base
// keep synth and FX signals apart.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>   // Component, Viewport, Label, TextButton, Timer

#include <array>
#include <memory>

#include "ParvatiLookAndFeel.h"   // ParvatiLookAndFeel (appFontOr), ParvatiModuleLamp
#include "ParvatiTheme.h"         // ParvatiTheme (category colour tokens)

class ParvatiAudioProcessor;
class ThemeManager;

//==============================================================================
namespace parvati::matrixview
{
// Category colour for a mod-source display name. The mapping mirrors the
// STRICT family palette and ModSourceCatalog::clusterAccent: Env=teal,
// LFO=magenta, Seq/Arp=mint, Op=purple, Const=indigo, Perf=amber,
// Gate/Noise/Random=orange. An unknown name returns a transparent colour.
juce::Colour sourceCategoryColour (const ParvatiTheme& t, const juce::String& sourceName);

// Row category colour: every source resolves to its family cat* token. An
// unknown name falls back to the neutral accent. The result is never
// transparent. The row tint, the slider fill and the combo tag use it.
juce::Colour rowCategoryColour (const ParvatiTheme& t, const juce::String& sourceName);

// Resolve the app font through the inherited editor L&F when present, else
// the JUCE default. Keeps the view usable before it joins the editor tree.
juce::Font appFontOr (const juce::Component& c, float height);

// Row-index label width. JUCE Label draws a default 5px border per side, so
// an 18pt label leaves an 8px text box. The two-digit '16' needs 13px at the
// 12pt app font. The constant covers the text plus 6px slack, floored at 18.
// A constant width keeps every row identical. The UI test pins the fit.
constexpr int kMatrixIndexLabelW = 20;
}  // namespace parvati::matrixview

//==============================================================================
// Every structural difference between the two matrix views. The base reads
// this table; it never tests the view type. Add a field only for a real
// difference, and give both views their TODAY value.
struct MatrixViewConfig
{
    // ---- identity ----
    const char* paramPrefix;        // APVTS slot prefix: "mod" / "fxmod"
    int numSlots;                   // slot count: 14 / 16

    // ---- strings (translation keys; the count is part of the key) ----
    const char* usedSuffixKey;      // "of 14 Used" / "of 16 Used"
    const char* matrixFullKey;      // "Matrix Full (14/14)" / "(16/16)"
    const char* rowA11yPrefix;      // row accessibility name: "Mod " / "FX Mod "

    // ---- dest-combo policy ----
    // True: the dest combo loads its choices from the APVTS param and binds
    // through a ComboBoxAttachment (synth). False: the combo stays detached;
    // the owning view builds its items and syncs the selection (FX).
    bool destComboAttached;

    // ---- row layout ----
    // True: apply the two-stage squeeze. Narrow rows shrink the source combo
    // first, then hard-floor both combos at 44pt (synth). False: plain
    // clamped caps (FX keeps its historical layout).
    bool comboShrinkFallback;

    // ---- hover emphasis ----
    // True: a hovered row highlights itself besides the bus broadcast (FX).
    bool highlightSelfOnHover;

    // ---- mute lamp ----
    // Drawn dot diameter. A value <= 0 keeps the theme default. The synth
    // rows pin the FX-card size (15pt). The FX rows keep the default.
    float lampDiameter;
    // True: the lamp ON colour follows the row's modulator category colour.
    // False: the lamp ON colour stays the theme accent (FX).
    bool lampCarriesCategoryColour;

    // ---- dest-domain encoding on the highlight bus ----
    // 0 for the synth view. kFxModDstOffset for the FX view.
    int destBusOffset;
    // assignNextFreeSlot rejects a dest at or above this value. The synth
    // view rejects the FX domain; the FX view takes raw indices only.
    int rejectDestAtOrAbove;

    // ---- addSlot defaults ----
    int addDefaultSource;           // MOD_SRC_* enum of the default source
    int addDefaultDest;             // dest enum of the default target
};

//==============================================================================
class MatrixViewBase : public juce::Component,
                       private juce::Timer
{
public:
    MatrixViewBase (ParvatiAudioProcessor& processor, ThemeManager& themeManager,
                    MatrixViewConfig config);
    ~MatrixViewBase() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-resolve theme colours onto every row. Call after a theme switch and
    // from the editor's applyThemeColors pass.
    void applyThemeColors();

    // Re-evaluate the active set and rebuild the list layout. The call is
    // cheap: it early-outs when nothing changed. Safe from the timer, after
    // any programmatic param write, and after a preset load or undo. The FX
    // view overrides this to also reconcile its dest-combo labels.
    virtual void refresh();

    // ---- Accessors used by the rows and the wiring step ----
    ParvatiAudioProcessor& processor() const noexcept { return processor_; }
    ThemeManager&          themeManager() const noexcept { return themeManager_; }

    // Slot state, 0-based (slot 0 == "Mod 1").
    int  amountForSlot (int slot) const;             // raw int -63..+63 from the APVTS
    bool isSlotMuted (int slot) const noexcept;
    int  stashedAmount (int slot) const noexcept;
    bool isSlotActive (int slot) const;              // amount != 0 || muted
    int  firstFreeSlot() const;                      // first free index, or -1 when full

    // Operations invoked from a row. All write through the APVTS so they
    // stay byte-bridged and undoable, then trigger a refresh.
    void toggleMute (int slot);
    void clearSlot (int slot);
    void addSlot();

    // Programmatic free-slot assignment. Finds the first free slot, writes
    // its source/dest/amount through the APVTS, then refreshes. Returns
    // false when the matrix is full, or when the dest is outside the view's
    // domain (see MatrixViewConfig::rejectDestAtOrAbove).
    bool assignNextFreeSlot (int sourceEnum, int destEnum, int amount = 32);

    // Display name of a slot's selected source ("Env 1", "LFO 2", ...).
    juce::String sourceNameForSlot (int slot) const;

    // Flash every ACTIVE row routed from @p sourceEnum (a MOD_SRC_* value).
    // The flash auto-expires via flashTick on the timer.
    void flashRowsForSource (int sourceEnum);

    // Live slot routing, read from the APVTS raw values (-1 on error).
    int destForSlot (int slot) const;
    int sourceForSlot (int slot) const;

    // APVTS param ID for a 0-based slot:
    // slotParamFor (3, "_amount") == "mod4_amount" / "fxmod4_amount".
    juce::String slotParamFor (int slot, const char* suffix) const;

    static constexpr int kRowHeight  = 48;   // 44pt touch targets in a scrolling list
    static constexpr int kAddButtonH = 44;   // the HIG touch minimum

protected:
    const MatrixViewConfig& config() const noexcept { return config_; }
    int numSlots() const noexcept { return config_.numSlots; }
    void setAmountForSlot (int slot, int amount);    // writes the APVTS

    // The row component for a slot, or nullptr when out of range. Lets the
    // derived views and tests reach a row without seeing the row type.
    juce::Component* rowAtOrNull (int slot) const noexcept;

    // A row's dest combo, or nullptr when out of range. The FX view
    // rebuilds and syncs these by hand.
    juce::ComboBox* rowDestCombo (int slot) const noexcept;

    // ---- ModMatrixHighlight bus (hover + double-click-to-row) ----
    // Emphasise every row routed to @p modDst. The FX view encodes its dests
    // above the synth range; the offset guard rejects the other domain. -1
    // clears all rows.
    void onHighlightDest (int modDst);
    // Flash and scroll the row for an (offset-encoded) slot broadcast. A
    // slot outside this view's domain is rejected.
    void onSelectSlot (int slotIndex);
    // Scroll the viewport so the row is fully visible.
    void ensureRowVisible (int slot);
    // Advance or clear the transient flash (driven from timerCallback).
    void flashTick();

    // The derived view registers its own onAssignRequest handler here. The
    // base dtor unsubscribes it. -1 means "not registered".
    int assignSub_ = -1;

private:
    // The shared row type (defined in the .cpp). It reaches this view's
    // protected config/slot-param seams, so it is a friend.
    friend struct MatrixRow;

    // Per-slot editor-only mute state. It is NOT persisted. An external
    // amount change clears it (see refresh).
    std::array<bool, 16> muted_ {};
    std::array<int, 16>  stashedAmount_ {};

    void timerCallback() override { refresh(); flashTick(); }
    // F-ios-perf-3 (iOS hunt 2026-08-19): gate the 30 Hz poll on visibility.
    // The TabbedComponent unparents non-current pages. An AUv3 host can keep
    // the process alive with the editor closed. The idle tick then burns
    // battery for nothing. The callbacks are change-only, so the gating sets
    // the wakeup cadence, not the tick cost.
    void visibilityChanged() override;
    void rebuildLayout();
    juce::String buildSignature() const;             // compact "active set" fingerprint

    ParvatiAudioProcessor& processor_;
    ThemeManager&          themeManager_;
    const MatrixViewConfig config_;   // by value: views build theirs in a local factory

    juce::Viewport viewport_;

    // The scrolled content. This JUCE Viewport has no background-colour API,
    // so the content paints its own window-background fill.
    struct ContentPanel : public juce::Component
    {
        juce::Colour bg;
        void paint (juce::Graphics& g) override { g.fillAll (bg); }
    } content_;
    juce::Label    headerLabel_;                     // "N of 14 Used"
    std::unique_ptr<juce::TextButton> addButton_;    // "+ Add Modulation" / "Matrix Full"

    // Local bipolar depth-slider LookAndFeel. Declared BEFORE rows_ so it is
    // destroyed AFTER them. The sliders reference it through setLookAndFeel.
    class BipolarSliderLNF;
    std::unique_ptr<BipolarSliderLNF> bipolarLnf_;

    std::array<std::unique_ptr<struct MatrixRow>, 16> rows_;

    juce::String lastSignature_;   // skip relayout when the active set is unchanged

    // The transient flash state: a set of slots with one timed expiry. The
    // knob-double-click jump and the multi-row source-flash share it.
    juce::Array<int> flashSlots_;
    juce::uint32 flashStartMs_ = 0;
    static constexpr int kFlashMs = 1200;

    // ModMatrixHighlight bus subscription ids (unsubscribe in the dtor).
    int destHighlightSub_ = -1;   // onDestHighlighted (hover/drag row highlight)
    int slotSelectSub_    = -1;   // onSlotSelected (knob double-click -> jump)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatrixViewBase)
};
