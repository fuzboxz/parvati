// Fixed-size undo history + manual arp-clock regressions (2026-08-19 AUv3
// follow-up wave).
//
//   [1] Fixed undo bound — the APVTS UndoManager is constructed with an
//       explicit unit cap + transaction floor (kUndoMaxUnits /
//       kUndoMinTransactions). Thousands of parameter transactions must not
//       grow the history past the cap, undo/redo must keep working, and the
//       stored history must stay non-empty (the floor). This replaces a
//       runtime memory-pressure seam: a bounded history is the entire policy.
//   [2] Manual arp-clock fallback — with no playhead tempo the clock runs on
//       the persisted MANUAL bpm (default 120 = the old hard-coded value);
//       a playhead carrying a bpm wins and flips the source flag; a
//       playhead WITHOUT a bpm falls back to manual. State round-trips the
//       manual bpm; the setter clamps to 40..300 (restored state is never
//       trusted raw).
//
// Run: ./build_unified/parvati_unified_tests undo_clock_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <limits>

#include <juce_audio_basics/juce_audio_basics.h>   // AudioPlayHead
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParameterLayout.h"     // getPatchParamDescriptors (a real paramID)
#include "PluginProcessor.h"
#include "ui/SettingsPanel.h"    // the Arp Clock UI rows (layout sweep)
#include "ui/ThemeManager.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void renderBlock (ParvatiAudioProcessor& p)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

// A playhead whose PositionInfo carries (or omits) a tempo.
struct StubPlayhead final : public juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        if (withBpm)
            info.setBpm (bpm);
        info.setIsPlaying (playing);
        return info;
    }
    double bpm { 88.0 };
    bool   withBpm { true };
    bool   playing { true };
};
}  // namespace

//==============================================================================
int testFixedUndoBound()
{
    std::printf ("[fixed-size undo history]\n");
    int fails = g_failures;

    ParvatiAudioProcessor proc;
    auto& um = proc.getUndoManager();

    const auto& descs = getPatchParamDescriptors();
    check (! descs.empty(), "descriptor table populated");
    auto value = proc.getApvts().getParameterAsValue (descs[0].paramID);
    const float original = (float) (double) value.getValue();

    // Record FAR more transactions than the cap can hold (a ValueTree property
    // transaction ≈ ~120 units; the cap is 16000 ≈ ~130 steps). Alternating
    // values defeat consecutive-edit coalescing so each write is a real
    // transaction.
    constexpr int kTransactions = 4000;
    for (int i = 0; i < kTransactions; ++i)
    {
        um.beginNewTransaction();
        value.setValue (original + 1.0f + (float) (i & 1));
    }
    check (um.canUndo(), "history is functional after thousands of transactions");
    check (um.getNumberOfUnitsTakenUpByStoredCommands() <= ParvatiAudioProcessor::kUndoMaxUnits,
           "stored undo units never exceed the fixed cap");

    // The stack is BOUNDED: undoing exhausts after a small, fixed number of
    // steps (nowhere near the 4000 recorded). The exact count depends on
    // sizeof(transaction); the bound is what matters.
    int undone = 0;
    while (um.canUndo() && undone < 1000)
    {
        um.undo();
        ++undone;
    }
    check (undone < 1000, "undoing eventually exhausts (bounded history)");
    check (undone <= 400, "bounded history holds far fewer steps than recorded");
    check (! um.canUndo(), "history empty after undoing everything");

    // Redo still works on the bounded stack.
    check (um.canRedo(), "redo available after undoing");

    return g_failures - fails;
}

//==============================================================================
int testManualClock()
{
    std::printf ("[manual arp-clock fallback]\n");
    int fails = g_failures;

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    check (proc.getManualTempoBpm() == 120, "manual bpm defaults to 120 (the old hard-coded value)");

    // --- no playhead at all (the Standalone case): manual tempo drives ---
    proc.setManualTempoBpm (96);
    renderBlock (proc);
    check (proc.getLastClockBpm() == 96.0, "no playhead: clock resolves the manual bpm");
    check (! proc.isHostTempoPresent(), "no playhead: source flag = Manual");

    // --- playhead WITH a tempo: the host value wins ---
    StubPlayhead hostBpm;
    proc.setPlayHead (&hostBpm);
    renderBlock (proc);
    check (proc.getLastClockBpm() == 88.0, "playhead bpm wins over the manual value");
    check (proc.isHostTempoPresent(), "source flag = Host while the playhead carries a bpm");

    // --- playhead WITHOUT a tempo (the GarageBand-class case): manual ---
    StubPlayhead noBpm;
    noBpm.withBpm = false;
    proc.setPlayHead (&noBpm);
    renderBlock (proc);
    check (proc.getLastClockBpm() == 96.0, "playhead without bpm: manual fallback active");
    check (! proc.isHostTempoPresent(), "source flag = Manual while the playhead omits bpm");

    proc.setPlayHead (nullptr);

    // --- degenerate host values (0.0 / NaN) are NOT a usable tempo: the
    //     resolution gate treats them as no-musical-context (manual
    //     fallback) instead of feeding 0/NaN into the tempo-synced paths ---
    StubPlayhead degenerate;
    degenerate.bpm = 0.0;
    proc.setPlayHead (&degenerate);
    renderBlock (proc);
    check (proc.getLastClockBpm() == 96.0, "host bpm 0.0: rejected, manual fallback");
    check (! proc.isHostTempoPresent(), "host bpm 0.0: source flag = Manual");
    degenerate.bpm = std::numeric_limits<double>::quiet_NaN();
    renderBlock (proc);
    check (proc.getLastClockBpm() == 96.0, "host bpm NaN: rejected, manual fallback");
    check (! proc.isHostTempoPresent(), "host bpm NaN: source flag = Manual");
    degenerate.bpm = -4.0;
    renderBlock (proc);
    check (! proc.isHostTempoPresent(), "negative host bpm: rejected, manual fallback");
    proc.setPlayHead (nullptr);

    // --- clamping (the setter and restored state share the range) ---
    proc.setManualTempoBpm (9999);
    check (proc.getManualTempoBpm() == 300, "manual bpm clamps to 300");
    proc.setManualTempoBpm (1);
    check (proc.getManualTempoBpm() == 40, "manual bpm clamps to 40");

    // --- state round-trip ---
    proc.setManualTempoBpm (143);
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        ParvatiAudioProcessor fresh;
        fresh.setStateInformation (state.getData(), (int) state.getSize());
        check (fresh.getManualTempoBpm() == 143, "manual bpm persists through plugin state");
    }

    // --- a REAL legacy state (saved before manual_bpm existed) restores the
    //     default, not the stale in-memory value: strip the property from a
    //     genuine saved state, restore into a processor set to 200 ---
    proc.setManualTempoBpm (200);
    proc.getStateInformation (state);
    {
        auto xml = std::unique_ptr<juce::XmlElement> (proc.getXmlFromBinary (state.getData(), (int) state.getSize()));
        check (xml != nullptr && xml->hasAttribute ("manual_bpm"), "saved state carries manual_bpm");
        if (xml != nullptr)
            xml->removeAttribute ("manual_bpm");
        ParvatiAudioProcessor legacy;
        legacy.setManualTempoBpm (200);   // ensure the restore must OVERRIDE this
        juce::MemoryBlock stripped;
        proc.copyXmlToBinary (*xml, stripped);
        legacy.setStateInformation (stripped.getData(), (int) stripped.getSize());
        check (legacy.getManualTempoBpm() == 120, "state WITHOUT manual_bpm restores the 120 default");

        // --- restored state is never trusted raw: a doctored out-of-range
        //     value clamps at the SAME 40..300 bounds as the setter ---
        xml->setAttribute ("manual_bpm", 9999);
        juce::MemoryBlock doctoredHigh;
        proc.copyXmlToBinary (*xml, doctoredHigh);
        legacy.setStateInformation (doctoredHigh.getData(), (int) doctoredHigh.getSize());
        check (legacy.getManualTempoBpm() == 300, "restored manual_bpm 9999 clamps to 300");
        xml->setAttribute ("manual_bpm", 0);
        juce::MemoryBlock doctoredLow;
        proc.copyXmlToBinary (*xml, doctoredLow);
        legacy.setStateInformation (doctoredLow.getData(), (int) doctoredLow.getSize());
        check (legacy.getManualTempoBpm() == 40, "restored manual_bpm 0 clamps to 40");
    }

    return g_failures - fails;
}

//==============================================================================
int testSettingsPanelDegradation()
{
    std::printf ("[settings panel R3 degradation (Arp Clock rows)]\n");
    int fails = g_failures;

    ParvatiAudioProcessor proc;
    ThemeManager themes;
    SettingsPanel panel (proc, themes, {}, {}, {}, {}, {});

    // Row budget (see SettingsPanel::resized): theme 80 + zoom 80 + tooltips
    // 52 + smoothing 60 + Arp Clock 90 + filter 80 + language 64 = 506, +32pt
    // margins = 538pt for EVERYTHING. The R3 drawer degrades bottom-first, so
    // the Arp Clock block (placed ABOVE Filter Quality/Language deliberately —
    // its target hosts are exactly the short-pane ones) must survive heights
    // where the bottom sections hide.
    panel.setBounds (0, 0, 320, 600);
    check (panel.debugBpmSliderVisible(), "tall drawer: manual-BPM slider row visible");

    panel.setBounds (0, 0, 320, 460);   // ~428pt of rows: arp fits, filter/lang hide
    check (panel.debugBpmSliderVisible(), "460pt drawer: Arp Clock block still visible");

    panel.setBounds (0, 0, 320, 300);   // ~268pt of rows: arp hides too
    check (! panel.debugBpmSliderVisible(), "300pt drawer: Arp Clock row hides (bottom-first)");

    // R3 invariant at EVERY height: no two VISIBLE children overlap and none
    // escapes the panel bounds (hide-not-spill, the compacted-layout gate).
    for (int h = 240; h <= 640; h += 40)
    {
        panel.setBounds (0, 0, 320, h);
        juce::Array<juce::Component*> kids;
        for (auto* c : panel.getChildren())
            if (c->isVisible())
                kids.add (c);
        bool clean = true;
        for (int i = 0; i < kids.size() && clean; ++i)
        {
            if (! panel.getLocalBounds().contains (kids[i]->getBounds()))
                clean = false;
            for (int j = i + 1; j < kids.size(); ++j)
                if (kids[i]->getBounds().intersects (kids[j]->getBounds()))
                    clean = false;
        }
        check (clean, ("no visible-child overlap/escape at height " + juce::String (h)).toRawUTF8());
    }

    return g_failures - fails;
}

//==============================================================================
TEST(undo_clock_test)
{
    juce::ScopedJuceInitialiser_GUI juce;   // MessageManager on this thread

    const int a = testFixedUndoBound();
    const int b = testManualClock();
    const int d = testSettingsPanelDegradation();

    std::printf ("\n%s: %d failure(s) (undo-bound %d, clock %d, panel %d)\n",
                 (g_failures == 0) ? "PASS" : "FAIL", g_failures, a, b, d);
    return g_failures == 0;
}
