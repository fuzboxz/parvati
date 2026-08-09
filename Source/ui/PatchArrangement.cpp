// Phase 1 of the "Patch page" feature: arrangement templates + inference.
// See PatchArrangement.h for the engine-API deviations (approved) and the
// design spec (/tmp/parvati_patch_design.md, "Phase 1").
//
// Handoff note (Phase 2/3, NOT fixed here): applyArrangement writes polyphony
// directly via applyPartByte, so after a UI apply the APVTS `part_polyphony`
// param for an edited part can be stale until the next part-switch re-sync —
// Phase 2/3 wiring should trigger the same part-param re-sync the loader uses
// after applying.

#include "PatchArrangement.h"

#include <cstdint>

// Polyphony modes (SynthEngine.h:49), PartData byte 15:
// MONO=0, POLY=1, UNISON_2X=2, CYCLIC=3, CHAIN=4. Only POLY and CHAIN are used
// by the built-in templates (Single=POLY, Stack=CHAIN; everything else POLY).
namespace
{
constexpr uint8_t kPolyPoly  = 1;
constexpr uint8_t kPolyChain = 4;

// Per-arrangement full 6-Part state. Channels: 0 = Omni, else 1..16. An inactive
// Part (count 0) is reset to a clean default: channel = partIndex + 1 (the
// engine constructor default), zone 0..127, poly POLY — so state stays
// deterministic and Single/Stack are a no-op on a freshly-constructed engine.
// (The spec: "For any part with count 0 ... set its zone to 0..127 and poly to
// POLY (defaults)". Its channel contribution is functionally irrelevant — the
// Part has no voices — so it is left at the constructor default.)
struct Template
{
    Arrangement  id;
    const char*  label;
    int          counts   [kNumParts];   // voicecards per part (sum <= 6)
    int          channel  [kNumParts];   // MIDI channel per part
    int          lo       [kNumParts];   // key-zone low per part
    int          hi       [kNumParts];   // key-zone high per part
    uint8_t      poly     [kNumParts];   // polyphony mode per part
};

// The exact template table from the design spec. Split point = C4 = MIDI 60.
constexpr Template kTemplates[] = {
    // Single: 1 Part, all 6 cards, ch 1, 0..127, POLY.
    { Arrangement::Single, "Single",
      { 6, 0, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Stack: 1 Part, all 6 cards, ch 1, 0..127, CHAIN.
    { Arrangement::Stack, "Stack",
      { 6, 0, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyChain, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Split 2: 2 Parts, 3+3 cards, p0=ch1/0..59, p1=ch2/60..127, POLY.
    { Arrangement::Split2, "Split 2",
      { 3, 3, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 60, 0, 0, 0, 0 }, { 59, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Layer 2: 2 Parts, 3+3 cards, both ch 1, both 0..127, POLY.
    { Arrangement::Layer2, "Layer 2",
      { 3, 3, 0, 0, 0, 0 },
      { 1, 1, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Multi 6: 6 Parts, 1 card each, ch 1..6, all 0..127, POLY.
    { Arrangement::Multi6, "Multi 6",
      { 1, 1, 1, 1, 1, 1 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
};

const Template* findTemplate (Arrangement a)
{
    for (const auto& t : kTemplates)
        if (t.id == a)
            return &t;
    return nullptr;
}

int popcount (uint8_t x)
{
    int n = 0;
    for (; x; x >>= 1)
        n += x & 1;
    return n;
}
}  // namespace

const char* arrangementLabel (Arrangement a)
{
    if (const Template* t = findTemplate (a))
        return t->label;
    return "Custom";
}

int arrangementCount()
{
    return static_cast<int> (sizeof (kTemplates) / sizeof (kTemplates[0]));
}

void applyArrangement (SynthEngine& engine, Arrangement a)
{
    if (a == Arrangement::Custom)
        return;   // Custom is inferred, never applied.

    const Template* t = findTemplate (a);
    if (t == nullptr)
        return;

    const int savedPart = engine.getCurrentPart();

    // 1) Voice allocation + MIDI channel + key zone, per part. Cards are
    //    assigned CONTIGUOUSLY in part order from the per-part counts, so the
    //    resulting bitmasks are disjoint and setPartVoiceAllocation's exclusive
    //    ownership never needs to steal across Parts.
    int cardCursor = 0;   // next free voicecard index (0..5)
    for (int p = 0; p < kNumParts; ++p)
    {
        uint8_t mask = 0;
        for (int c = 0; c < t->counts[p]; ++c)
            mask |= static_cast<uint8_t> (1u << (cardCursor + c));
        cardCursor += t->counts[p];

        engine.setPartVoiceAllocation (p, mask);
        engine.setPartMidiChannel     (p, t->channel[p]);
        engine.setPartKeyZone         (p, t->lo[p], t->hi[p]);
    }

    // 2) Polyphony LAST, via the EXISTING public pair (the spec's
    //    setPartByte(part,15,mode) does not exist; see header deviation note).
    for (int p = 0; p < kNumParts; ++p)
    {
        engine.setCurrentPart (p);
        engine.applyPartByte (15, t->poly[p]);
    }

    engine.setCurrentPart (savedPart);
}

Arrangement inferArrangement (const SynthEngine& engine)
{
    int counts[kNumParts] {};
    int chan  [kNumParts] {};
    int lo    [kNumParts] {};
    int hi    [kNumParts] {};
    int poly  [kNumParts] {};

    int numActive   = 0;
    int firstActive  = -1;
    int secondActive = -1;

    for (int p = 0; p < kNumParts; ++p)
    {
        counts[p] = popcount (engine.getPartVoiceAllocation (p));
        chan[p]   = engine.getPartChannel (p);
        lo[p]     = engine.getPartKeyrangeLow (p);
        hi[p]     = engine.getPartKeyrangeHigh (p);
        poly[p]   = engine.getPartPolyphony (p);   // PartData byte 15 = polyphony

        if (counts[p] > 0)
        {
            if (firstActive < 0)       firstActive  = p;
            else if (secondActive < 0) secondActive = p;
            ++numActive;
        }
    }

    // Single / Stack: exactly one Part owns all 6 cards. Distinguished by that
    // Part's polyphony (Single = POLY, Stack = CHAIN).
    if (numActive == 1 && firstActive >= 0 && counts[firstActive] == 6)
    {
        if (poly[firstActive] == kPolyPoly)  return Arrangement::Single;
        if (poly[firstActive] == kPolyChain) return Arrangement::Stack;
        return Arrangement::Custom;   // 6 cards but an undefined poly mode
    }

    // Multi 6: all 6 Parts have exactly 1 card AND 6 distinct non-Omni channels.
    if (numActive == kNumParts)
    {
        bool oneEach = true;
        for (int p = 0; p < kNumParts; ++p)
            if (counts[p] != 1) { oneEach = false; break; }

        bool distinctNonOmni = oneEach;
        if (oneEach)
        {
            for (int i = 0; i < kNumParts && distinctNonOmni; ++i)
            {
                if (chan[i] == 0) { distinctNonOmni = false; break; }   // Omni present
                for (int j = i + 1; j < kNumParts; ++j)
                    if (chan[i] == chan[j]) { distinctNonOmni = false; break; }
            }
        }
        if (oneEach && distinctNonOmni)
            return Arrangement::Multi6;
    }

    // Split 2 / Layer 2: exactly two active Parts, distinguished by channel
    // (distinct vs shared) and key zone (complementary vs overlapping).
    if (numActive == 2 && firstActive >= 0 && secondActive >= 0)
    {
        const int a = firstActive, b = secondActive;
        const bool distinctChannel = chan[a] != chan[b];
        const bool complementary   = hi[a] < lo[b] || hi[b] < lo[a];   // one's hi < other's lo
        const bool overlapping     = lo[a] <= hi[b] && lo[b] <= hi[a];
        const bool sameChannel     = chan[a] == chan[b];

        if (distinctChannel && complementary)
            return Arrangement::Split2;
        if (sameChannel && overlapping)
            return Arrangement::Layer2;
    }

    return Arrangement::Custom;
}
