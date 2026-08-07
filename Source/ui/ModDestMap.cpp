// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ModDestMap.h.

#include "ui/ModDestMap.h"

#include <dsp/patch.h>  // ambika::dsp::ModulationDestination (MOD_DST_*), kNumModulations

#include <juce_core/juce_core.h>  // juce::String

namespace parvati::ModDestMap
{
namespace
{
// One entry per MOD_DST_* enum value, indexed by that enum value (index == enum
// value). An empty string means the destination has NO visible base knob (e.g.
// VCA, the envelope ADR offsets, the raw oscillator-pitch offsets) and is
// therefore not a ring/drag target. The row order matches enum
// ModulationDestination in Source/dsp/patch.h; a static_assert below pins it.
struct DestEntry
{
    const char* paramID;
};

constexpr DestEntry kDestTable[] = {
    { "osc1_param" },     // 0  MOD_DST_PARAMETER_1
    { "osc2_param" },     // 1  MOD_DST_PARAMETER_2
    { "" },               // 2  MOD_DST_OSC_1          (no base knob)
    { "" },               // 3  MOD_DST_OSC_2          (no base knob)
    { "" },               // 4  MOD_DST_OSC_1_2_COARSE (no base knob)
    { "" },               // 5  MOD_DST_OSC_1_2_FINE   (no base knob)
    { "mix_balance" },    // 6  MOD_DST_MIX_BALANCE
    { "mix_param" },      // 7  MOD_DST_MIX_PARAM
    { "mix_noise" },      // 8  MOD_DST_MIX_NOISE
    { "mix_sub" },        // 9  MOD_DST_MIX_SUB_OSC
    { "mix_fuzz" },       // 10 MOD_DST_MIX_FUZZ
    { "mix_crush" },      // 11 MOD_DST_MIX_CRUSH
    { "filter1_cutoff" }, // 12 MOD_DST_FILTER_CUTOFF
    { "filter1_reso" },   // 13 MOD_DST_FILTER_RESONANCE
    { "" },               // 14 MOD_DST_ATTACK         (no base knob)
    { "" },               // 15 MOD_DST_DECAY          (no base knob)
    { "" },               // 16 MOD_DST_RELEASE        (no base knob)
    { "voice_lfo_rate" }, // 17 MOD_DST_LFO_4
    { "" },               // 18 MOD_DST_VCA            (no base knob)
};

constexpr int kNumDests = static_cast<int> (ambika::dsp::MOD_DST_LAST);
static_assert (sizeof (kDestTable) / sizeof (kDestTable[0]) == kNumDests,
               "kDestTable must have exactly one entry per MOD_DST_* enum value");

constexpr int kNumSlots = static_cast<int> (ambika::dsp::kNumModulations);  // 14

bool validDest (ModDst dest) noexcept
{
    return dest >= 0 && dest < kNumDests;
}

bool nonEmpty (const char* s) noexcept
{
    return s != nullptr && s[0] != '\0';
}

juce::String slotDestParamID (int slot)   // slot is 0-based
{
    return "mod" + juce::String (slot + 1) + "_dest";
}

juce::String slotAmountParamID (int slot) // slot is 0-based
{
    return "mod" + juce::String (slot + 1) + "_amount";
}
}  // namespace

//==============================================================================
ModDst destForParamID (const juce::String& paramID)
{
    for (int d = 0; d < kNumDests; ++d)
        if (nonEmpty (kDestTable[d].paramID) && paramID == kDestTable[d].paramID)
            return d;
    return -1;
}

juce::String paramIDForDest (ModDst dest)
{
    if (! validDest (dest) || ! nonEmpty (kDestTable[dest].paramID))
        return {};
    return juce::String (kDestTable[dest].paramID);
}

bool hasVisibleKnob (ModDst dest)
{
    return validDest (dest) && nonEmpty (kDestTable[dest].paramID);
}

int aggregateAmount (juce::AudioProcessorValueTreeState& apvts, ModDst dest, int excludeSlot)
{
    if (! validDest (dest))
        return 0;

    int sum = 0;
    for (int slot = 0; slot < kNumSlots; ++slot)
    {
        if (slot == excludeSlot)
            continue;

        if (auto* d = apvts.getRawParameterValue (slotDestParamID (slot)))
            if (static_cast<ModDst> (d->load()) == dest)
                if (auto* a = apvts.getRawParameterValue (slotAmountParamID (slot)))
                    sum += static_cast<int> (a->load());
    }
    return sum;
}

std::vector<int> slotsForDest (juce::AudioProcessorValueTreeState& apvts, ModDst dest)
{
    std::vector<int> slots;
    if (! validDest (dest))
        return slots;

    for (int slot = 0; slot < kNumSlots; ++slot)
        if (auto* d = apvts.getRawParameterValue (slotDestParamID (slot)))
            if (static_cast<ModDst> (d->load()) == dest)
                slots.push_back (slot);

    return slots;
}
}  // namespace parvati::ModDestMap
