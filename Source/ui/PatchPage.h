// Phase 2 of the "Patch page" feature: the Patch juce::Component.
//
// A custom Component (no APVTS descriptor magic) that replaces the separate
// Multi/Setup + Global pages. It lets the user pick a high-level arrangement
// (Mono/Single/Dual Layer/Dual Split/Quad Split/Multi 6 — voice-budget presets
// over the 96-voice pool) which auto-configures all 6 Parts, then fine-tunes
// each Part through simple controls (a VOICE COUNT 1..16, not a card bitmask:
// the 6-voicecard masks are derived by the engine). It also HOSTS the editor's
// existing Section::Global ParamPage (patch-wide knobs + the voice-activity
// meter decoration) below the 6 part rows.
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
#include "VoicePoolView.h"    // VoicePoolFrame (setVoicePoolProvider type)

class ParvatiAudioProcessor;
class ThemeManager;
class ParamPage;

//==============================================================================
// Patch page: arrangement selector + 6 Part rows + the hosted patch-wide
// ParamPage. Refreshed/relanguaged/rethemed by the editor exactly like the old
// editor-chrome hooks the editor calls.
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
    // vca_curve / filter_drive / part* / master FX mix, plus the voice-activity
    // meter decoration) BELOW the 6 part rows. PatchPage does NOT own it (the
    // editor retains ownership); it only reparents + positions it. The hosted
    // page is reflowed to the row width in resized().
    void hostParamPage (juce::Component* paramPage);

    // Inject the state provider for the global voice-pool view (the pool
    // picture lives ONLY here — the Global page's VoiceMeter is part-relative).
    // Same decoupled pattern as the meter: the editor builds a VoicePoolFrame
    // (per part: label + one entry per allocated voice) at ~30 Hz; the view
    // owns nothing from the engine.
    void setVoicePoolProvider (std::function<VoicePoolFrame()> provider);

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
    // (0 = 12-EDO, 1..32 = preset, 33 = Custom; -1 for an out-of-range part).
    int getDisplayedTuningMode (int part) const;
    // Set @p part's tuning as if the user chose it in the Tune combo: sets the
    // selection then runs the normal byte-4 write / editor-open path
    // (JUCE does not fire onChange for a programmatic setSelectedId).
    // mode 33 opens the Custom… popover exactly like the UI — do not use in
    // headless tests (instantiate TuningEditor directly instead).
    void chooseTuningMode (int part, int mode);
    // Currently-displayed voice count for @p part from its Voices combo
    // (1..16; 0 = no selection = a DISABLED part — the combo offers no 0 item,
    // disabling is the arrangements'/loaders' job; -1 for an out-of-range part).
    int getDisplayedVoiceSlots (int part) const;
    // Set @p part's voice count as if the user chose it in the Voices combo:
    // sets the selection then runs the normal engine write path
    // (onVoicesChanged -> setPartVoiceSlots). JUCE does not fire a combo's
    // onChange for a programmatic setSelectedId, hence the explicit drive.
    // @p slots is clamped into the combo's 1..16 range (0 clamps to 1 —
    // enabling a disabled part; the public engine setter likewise never
    // disables).
    void chooseVoiceSlots (int part, int slots);

private:
    ParvatiAudioProcessor& proc_;
    ThemeManager& themeManager_;

    juce::Label heading_;
    juce::Label voicesTotalLabel_;       // "Voices Y/96" pool-budget readout (next to the arrangement combo)
    juce::ComboBox arrangementCombo_;   // 6 selectable items (ids 1..6); Custom = no selection
    ParamPage* hostedParamPage_ = nullptr;   // NON-owned (editor owns it)

    // T4 scroll safety net: the 6 part rows + the hosted patch-wide ParamPage
    // live inside this vertical Viewport, so a short host frame (small AUv3
    // pane) SCROLLS instead of clipping the lower rows / page unrecoverably.
    // At the tuned design size the body fits and reflowToWidth-style sizing
    // grows it to the view height, so no scrollbar ever appears. ScrollBody
    // (declared BEFORE the viewport so the viewport — which views it — is
    // destroyed first) is the scrolled content component, defined in the .cpp
    // like PartRow.
    class ScrollBody;
    std::unique_ptr<ScrollBody> scrollBody_;
    juce::Viewport viewport_;

    // Global voice-pool view (owned) + its caption. Lives INSIDE the scrolled
    // body below the 6 part rows and above the hosted Global ParamPage, so the
    // whole-part picture is reachable exactly where parts are configured.
    juce::Label voicePoolCaption_;
    std::unique_ptr<VoicePoolView> voicePoolView_;

    // One row per Part (0..5). Defined in the .cpp.
    class PartRow;
    std::array<std::unique_ptr<PartRow>, 6> rows_;

    bool refreshing_ = false;   // guards onChange during programmatic updates

    // (Re)build the arrangement combo's items from the active LocalisedStrings,
    // preserving the current selection.
    void buildArrangementCombo();
    // Arrangement combo onChange: apply the template, then full refresh.
    void onArrangementChanged();
    // After any per-part engine mutation: refresh dim states + the arrangement
    // label (the inferred arrangement may have become Custom).
    void postPartEdit();
    // Open the per-part custom tuning popover (Tune column, "Custom…"). The
    // editor's live edits re-sync this row's combo through the change
    // callback (the engine's resolved mode becomes 33 after the first edit).
    void openTuningEditor (int part);
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
