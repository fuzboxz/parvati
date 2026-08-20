// Phase 2 of the "Patch page" feature: the Patch juce::Component.
//
// A custom Component (no APVTS descriptor magic) that replaces the separate
// Multi/Setup + Global pages. It lets the user pick a high-level arrangement
// (Mono/Poly/Unison/Multitimbral/Drum Kit — voice-budget presets over the
// 96-voice pool) which auto-configures all 6 Parts, then fine-tunes each Part
// through simple controls (a VOICE COUNT 0..16, not a card bitmask: the
// 6-voicecard masks are derived by the engine; 0 = the Part is DISABLED). It
// also HOSTS the editor's existing Section::Global ParamPage (patch-wide
// knobs) at the top of the scrolled body, and MERGES its own arrangement
// summary (Mono/Poly/... + the "Voices Y/96" readout) and 6-part
// voice-allocation table into that page's Global panel (via the page's
// external-decoration slot), so ONE bordered Global section holds the global
// knobs, the arrangement summary AND the part-allocation table.
//
// Design: /tmp/parvati_patch_design.md ("Phase 2"). Phase 1 output
// (Source/ui/PatchArrangement.h) supplies applyArrangement/inferArrangement.
// This component drives the engine purely through its EXISTING public setters —
// it does NOT touch engine internals, file formats, or audio-thread code.
//
// Phase 2 deliverable only: the component must compile via the existing
// GLOB_RECURSE even though nothing instantiates it yet (Phase 3 wires it in).
// Colours come from the inherited ParvatiLookAndFeel / ParvatiTheme — no
// hardcoded palette.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

#include "PatchArrangement.h"   // Arrangement (getDisplayedArrangement return type)

class ParvatiAudioProcessor;
class ThemeManager;
class ParamPage;

//==============================================================================
// Patch page: arrangement selector + the hosted patch-wide ParamPage (whose
// Global panel contains the merged 6-part allocation table). Refreshed/
// relanguaged/rethemed by the editor exactly like the old editor-chrome hooks
// the editor calls.
class PatchPage : public juce::Component
{
public:
    PatchPage (ParvatiAudioProcessor& processor, ThemeManager& themeManager);
    // Destructor defined out-of-line: rows_ holds unique_ptr to the incomplete
    // nested PartRow, so the type must be complete where the dtor runs.
    ~PatchPage() override;

    // Fired when a part name/alias is edited (Parvati extension) so the host
    // editor can relabel its Part selector. Optional (a null callback is a
    // no-op — used by headless tests).
    std::function<void()> onPartNamesChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Parent the editor's existing Section::Global ParamPage (filter_card /
    // vca_curve / filter_drive / part* / master FX mix) at the TOP of the
    // scrolled body, and attach this page's PartTablePanel into that page's
    // "Global" group as an EXTERNAL decoration (non-owning — see
    // setGroupExternalDecoration), so the 6-part voice-allocation table renders
    // INSIDE the bordered Global panel, below the global knobs. PatchPage does
    // NOT own the hosted page (the editor retains ownership); it only reparents
    // + positions it. The hosted page is reflowed to the row width in resized().
    void hostParamPage (juce::Component* paramPage);

    // Re-read all 6 Parts' engine state into the rows (voice count, channel,
    // key zone, polyphony) WITHOUT firing onChange, then re-infer the
    // arrangement from the engine. Idempotent (guarded by refreshing_).
    void refresh();

    // Re-apply every chrome string through TRANS() (heading, arrangement items,
    // row captions, channel/poly combo items). Called by the editor after a
    // live language switch.
    void refreshLanguage();

    // Re-apply theme-derived colours and repaint (page fill is read at paint
    // time; the heading accent is explicit). Colours come from the inherited
    // L&F / the active ParvatiTheme.
    void applyThemeColors();

    // ---- test / automation hooks (observe + drive the exact UI code paths) ----
    // Currently-displayed arrangement (the combo selection; Custom if none).
    Arrangement getDisplayedArrangement() const;
    // Currently-displayed tuning mode for @p part from its Tune combo
    // (0 = 12-EDO, 1..32 = preset; -1 for an out-of-range part).
    int getDisplayedTuningMode (int part) const;
    // Set @p part's tuning as if the user chose it in the Tune combo: sets the
    // selection then runs the normal byte-4 write path
    // (JUCE does not fire onChange for a programmatic setSelectedId).
    void chooseTuningMode (int part, int mode);
    // Currently-displayed voice count for @p part from its Voices combo
    // (0..16; 0 = the Part is DISABLED — a real "0" item, selected at full
    // strength; -1 for an out-of-range part).
    int getDisplayedVoiceSlots (int part) const;
    // Set @p part's voice count as if the user chose it in the Voices combo:
    // sets the selection then runs the normal engine write path
    // (onVoicesChanged -> setPartVoiceSlots, or the legacy
    // setPartVoiceAllocation(part,0) disable for 0 — the public slots setter
    // clamps 0 to 1 by design). JUCE does not fire a combo's onChange for a
    // programmatic setSelectedId, hence the explicit drive.
    // @p slots is clamped into the combo's 0..16 range.
    void chooseVoiceSlots (int part, int slots);
    // Currently-displayed polyphony mode for @p part from its Poly combo
    // (0 = MONO, 1 = POLY, 2 = UNISON 2x, 3 = CYCLIC, 4 = CHAIN;
    // -1 for an out-of-range part). Same observe-only hook contract as
    // getDisplayedVoiceSlots: editor-load tests assert the combo mirrors the
    // engine's PartData byte 15 after a file load.
    int getDisplayedPolyphony (int part) const;

    // ---- Part-character columns (absorbed from the old "Part / Play" page
    // knobs — 2026-08-20 Patch-page simplification). Same observe/drive hook
    // contract as the Tune/Poly columns above: they read/set the row's cells
    // through the normal engine write path (PartData bytes 1 / 5 / 6). ----
    // Octave transpose shown in @p part's Oct combo (-2..+2; 0 for an
    // out-of-range part).
    int getDisplayedOctave (int part) const;
    // Set @p part's octave as if the user chose it in the Oct combo, then run
    // the normal byte-1 write path (clamped to -2..+2).
    void chooseOctave (int part, int octaves);
    // Legato flag shown in @p part's Lgo combo (0 = off, 1 = on; 0 for an
    // out-of-range part).
    int getDisplayedLegato (int part) const;
    // Set @p part's legato as if the user toggled the Lgo combo (0/1).
    void chooseLegato (int part, int on);
    // Portamento shown in @p part's Porta knob (0..63; -1 for an out-of-range
    // part).
    int getDisplayedPortamento (int part) const;
    // Set @p part's portamento as if the user dragged the Porta knob, then run
    // the normal byte-6 write path (clamped 0..63).
    void choosePortamento (int part, int value);

    // ---- Output columns (the completing absorption of the old "Part / Play"
    // page knobs — 2026-08-20 follow-up). Same observe/drive contract: they
    // read/set the row's Vol / Fine / Spr cells through the normal engine
    // write path (PartData bytes 0 / 2 / 3; byte 2 is SIGNED int8). ----
    // Part volume shown in @p part's Vol knob (0..127; -1 out of range).
    int getDisplayedVolume (int part) const;
    // Set @p part's volume as if the user dragged the Vol knob (byte 0,
    // clamped 0..127).
    void chooseVolume (int part, int value);
    // Fine tuning shown in @p part's Fine knob (-127..127 in 1/128-semitone
    // units; -1 out of range).
    int getDisplayedFineTune (int part) const;
    // Set @p part's fine tuning (SIGNED byte 2, clamped -127..127).
    void chooseFineTune (int part, int value);
    // Detune spread shown in @p part's Spr knob (0..40; -1 out of range).
    int getDisplayedSpread (int part) const;
    // Set @p part's spread (byte 3, clamped 0..40).
    void chooseSpread (int part, int value);

private:
    ParvatiAudioProcessor& proc_;
    ThemeManager& themeManager_;

    // The arrangement selector + the "Voices Y/96" pool-budget readout have
    // NO page-level heading chrome: both live INSIDE the Global panel's table
    // (the 44pt summary row above the 6 part rows — see PartTablePanel), so
    // the arrangement sits with the per-part rows it configures.
    juce::Label voicesTotalLabel_;       // "Voices Y/96" pool-budget readout (summary row, right of the arrangement combo)
    juce::ComboBox arrangementCombo_;   // 5 template items (ids 1..5) + separator + a DISABLED infer-only "Custom" item (id 6)
    ParamPage* hostedParamPage_ = nullptr;   // NON-owned (editor owns it)

    // T4 scroll safety net: the hosted patch-wide ParamPage (whose Global
    // panel carries the merged part-allocation table) lives inside this
    // vertical Viewport, so a short host frame (small AUv3 pane) SCROLLS
    // instead of clipping the lower content unrecoverably. At the tuned design
    // size the body fits and reflowToWidth-style sizing grows it to the view
    // height, so no scrollbar ever appears. ScrollBody (declared BEFORE the
    // viewport so the viewport — which views it — is destroyed first) is the
    // scrolled content component, defined in the .cpp like PartRow.
    class ScrollBody;
    std::unique_ptr<ScrollBody> scrollBody_;
    juce::Viewport viewport_;

    // The 6-part voice-allocation table container: parents the PartRows and
    // is itself attached into the HOSTED page's "Global" group as an external
    // decoration (hostParamPage), so the table renders inside that panel.
    // Defined in the .cpp; declares its natural height as kTableH.
    class PartTablePanel;
    std::unique_ptr<PartTablePanel> tablePanel_;

    // One row per Part (0..5), parented into tablePanel_ (which lays them out).
    // Defined in the .cpp.
    class PartRow;
    std::array<std::unique_ptr<PartRow>, 6> rows_;   // declared AFTER tablePanel_: rows (children of the panel) are destroyed first

    bool refreshing_ = false;   // guards onChange during programmatic updates

    // (Re)build the arrangement combo's items from the active LocalisedStrings,
    // preserving the current selection.
    void buildArrangementCombo();
    // Arrangement combo onChange: apply the template, then full refresh.
    void onArrangementChanged();
    // After any per-part engine mutation: refresh dim states + the arrangement
    // label (the inferred arrangement may have become Custom).
    void postPartEdit();
    // Forward a part-name edit to onPartNamesChanged (editor Part-selector
    // relabel). Called by the nested PartRow after a rename commits.
    void partNamesChanged();
    // Set arrangementCombo_ from inferArrangement(engine) (no onChange fired).
    void setArrangementFromEngine();
    // Refresh the "Voices Y/96" pool-budget readout from the engine (source of
    // truth; the total sums the audio-thread-published per-part voiceCount_
    // snapshots).
    void updateVoicesTotal();
    // (Re)lay out scrollBody_ inside viewport_ (the rows + hosted page),
    // sizing it to its natural height — or the view height when it fits.
    void layoutScrollBody();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchPage)
};
