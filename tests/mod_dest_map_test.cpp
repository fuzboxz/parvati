// ModDestMap unit test — verifies the paramID <-> MOD_DST mapping table and the
// per-slot modulation-amount aggregation helpers (aggregateAmount /
// slotsForDest) used by the modulation-ring, hover-highlight, and drag-and-drop
// features. Drives a real ParvatiAudioProcessor APVTS so the raw-value paths
// are exercised exactly as the UI will call them.
// Run: ./build_unified/parvati_unified_tests mod_dest_map_test

#include <cstdio>
#include "unified_test_runner.h"
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)
#include "ui/ModDestMap.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Route slot (0-based) -> dest with amount.
void setMod (ParvatiAudioProcessor& proc, int slot, int dest, int amount)
{
    char id[32];
    std::snprintf (id, sizeof (id), "mod%d_dest", slot + 1);   setParam (proc, id, dest);
    std::snprintf (id, sizeof (id), "mod%d_amount", slot + 1); setParam (proc, id, amount);
}

// Route an FX-mod slot (0-based) -> raw FX_DST index with amount.
void setFxMod (ParvatiAudioProcessor& proc, int slot, int fxDest, int amount)
{
    char id[32];
    std::snprintf (id, sizeof (id), "fxmod%d_dest", slot + 1);   setParam (proc, id, fxDest);
    std::snprintf (id, sizeof (id), "fxmod%d_amount", slot + 1); setParam (proc, id, amount);
}
}  // namespace

TEST(mod_dest_map_test)
{
    using namespace parvati::ModDestMap;

    juce::ScopedJuceInitialiser_GUI juceInit;

    // { dest enum value, expected knob paramID } for the knob-backed destinations.
    struct Map { int dest; const char* paramID; };
    constexpr Map mapped[] = {
        { 0,  "osc1_param" },
        { 1,  "osc2_param" },
        { 6,  "mix_balance" },
        { 7,  "mix_param" },
        { 8,  "mix_noise" },
        { 9,  "mix_sub" },
        { 10, "mix_fuzz" },
        { 11, "mix_crush" },
        { 12, "filter1_cutoff" },
        { 13, "filter1_reso" },
        { 17, "voice_lfo_rate" },
    };
    constexpr int numMapped = static_cast<int> (sizeof (mapped) / sizeof (mapped[0]));

    std::printf ("[1] paramID <-> MOD_DST round-trip for knob-backed destinations\n");
    int rtOk = 0;
    for (int i = 0; i < numMapped; ++i)
    {
        const juce::String pid = paramIDForDest (mapped[i].dest);
        const ModDst back = destForParamID (mapped[i].paramID);
        if (pid == mapped[i].paramID && back == mapped[i].dest && hasVisibleKnob (mapped[i].dest))
            ++rtOk;
    }
    check (rtOk == numMapped, "all 11 knob-backed dests round-trip paramID <-> dest");

    std::printf ("\n[2] no-knob destinations report no visible knob\n");
    constexpr int noKnob[] = { 2, 3, 4, 5, 14, 15, 16, 18 };  // OSC_1/2, COARSE/FINE, ADR, VCA
    int noKnobOk = 0;
    for (int d : noKnob)
        if (! hasVisibleKnob (d) && paramIDForDest (d).isEmpty())
            ++noKnobOk;
    check (noKnobOk == 8, "all 8 no-knob destinations report no visible knob");

    // Unknown / out-of-range inputs are rejected.
    check (destForParamID ("osc1_shape") == -1, "non-destination paramID -> -1");
    check (destForParamID ("does_not_exist") == -1, "bogus paramID -> -1");
    check (! hasVisibleKnob (-1), "dest -1 -> no knob");
    check (paramIDForDest (-1).isEmpty(), "dest -1 -> empty paramID");
    check (paramIDForDest (999).isEmpty(), "out-of-range dest -> empty paramID");

    std::printf ("\n[3] aggregateAmount + slotsForDest (APVTS-driven)\n");
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // Neutralise all 14 slots first for deterministic aggregation.
    constexpr int kSlots = 14;
    for (int s = 0; s < kSlots; ++s)
        setMod (proc, s, 18 /*VCA*/, 0);

    // Slot 0 (mod1) + slot 4 (mod5) -> FILTER_CUTOFF(12); slot 2 -> MIX_BALANCE(6).
    setMod (proc, 0, 12, 20);
    setMod (proc, 4, 12, -10);
    setMod (proc, 2, 6, 5);
    proc.syncAllParamsToEngine();

    auto& apvts = proc.getApvts();
    check (aggregateAmount (apvts, 12) == 10, "cutoff aggregate == 20 + (-10) = 10");
    check (aggregateAmount (apvts, 12, 0) == -10, "cutoff aggregate excluding slot 0 == -10");
    check (aggregateAmount (apvts, 6) == 5, "balance aggregate == 5");
    check (aggregateAmount (apvts, 18) == 0, "vca aggregate == 0 (neutralised)");
    check (aggregateAmount (apvts, -1) == 0, "invalid dest aggregate == 0");

    const auto cutoffSlots = slotsForDest (apvts, 12);
    check (cutoffSlots.size() == 2 && cutoffSlots[0] == 0 && cutoffSlots[1] == 4,
           "cutoff slots == {0, 4}");

    const auto balanceSlots = slotsForDest (apvts, 6);
    check (balanceSlots.size() == 1 && balanceSlots[0] == 2, "balance slots == {2}");

    const auto vcaSlots = slotsForDest (apvts, 18);
    check (vcaSlots.size() == 11, "vca slots == 11 (the 11 neutralised leftover slots)");

    const auto invalidSlots = slotsForDest (apvts, -1);
    check (invalidSlots.empty(), "invalid dest -> empty slot list");

    std::printf ("\n[4] FX-dest domain (offset encoding + fxmod aggregation)\n");
    // FX param-id -> encoded dest (FX_DST_* + kFxModDstOffset == 19).
    check (destForParamID ("fx1_drywet") == kFxModDstOffset + 0,  "fx1_drywet -> 19 (FX_DST_FX1_DRYWET + offset)");
    check (destForParamID ("fx1_param1") == kFxModDstOffset + 1,  "fx1_param1 -> 20 (FX_DST_FX1_P1 + offset)");
    check (destForParamID ("fx2_param2") == kFxModDstOffset + 8,  "fx2_param2 -> 27");
    check (destForParamID ("fx3_param4") == kFxModDstOffset + 16, "fx3_param4 -> 35 (FX_DST_FX3_P4 + offset)");
    check (destForParamID ("fx_nonexistent") == -1,               "unknown FX paramID -> -1");

    // Domain boundary (the single range check the assign handlers + editor use):
    // synth dests < offset; FX dests >= offset. This is what makes a synth drop
    // ignored by the FX handler and vice-versa.
    check (isFxDest (kFxModDstOffset),        "isFxDest(19) == true (first FX dest)");
    check (! isFxDest (kFxModDstOffset - 1),  "isFxDest(18) == false (last synth dest)");
    check (isFxDest (kFxModDstOffset + 17),   "isFxDest(36) == true (last FX dest)");
    check (! isFxDest (-1),                   "isFxDest(-1) == false");

    // fxmod-driven aggregation (mirror of [3] over the 16 fxmod slots).
    constexpr int kFxSlots = kFxNumSlots;
    for (int s = 0; s < kFxSlots; ++s)
        setFxMod (proc, s, 0 /*FX_DST_FX1_DRYWET*/, 0);
    setFxMod (proc, 0, 1 /*FX_DST_FX1_P1*/, 25);
    setFxMod (proc, 5, 1 /*FX_DST_FX1_P1*/, -15);
    setFxMod (proc, 2, 0 /*FX_DST_FX1_DRYWET*/, 8);
    proc.syncAllParamsToEngine();

    const ModDst fxP1 = kFxModDstOffset + 1;   // encoded FX_DST_FX1_P1
    check (aggregateAmount (apvts, fxP1) == 10,         "fx1_param1 aggregate == 25 + (-15) = 10");
    check (aggregateAmount (apvts, fxP1, 0) == -15,      "fx1_param1 aggregate excluding fxmod slot 0 == -15");
    check (aggregateAmount (apvts, kFxModDstOffset + 0) == 8, "fx1_drywet aggregate == 8");
    const auto fxP1Slots = slotsForDest (apvts, fxP1);
    check (fxP1Slots.size() == 2 && fxP1Slots[0] == 0 && fxP1Slots[1] == 5,
           "fx1_param1 fxmod slots == {0, 5}");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
