// Phase 1 of the "Patch page" feature: arrangement templates + inference.
// See PatchArrangement.h for the engine-API deviations (approved), the
// voice-first model note and the design spec references.
//
// Handoff note (Phase 2/3, NOT fixed here): applyArrangement writes polyphony
// directly via applyPartByte, so after a UI apply the APVTS `part_polyphony`
// param for an edited part can be stale until the next part-switch re-sync —
// PatchPage::onArrangementChanged already triggers the same part-param re-sync
// the loader uses after applying.

#include "PatchArrangement.h"

#include <cstdint>

// Polyphony modes (SynthEngine.h:49), PartData byte 15:
// MONO=0, POLY=1, UNISON_2X=2, CYCLIC=3, CHAIN=4. Only MONO and POLY are used
// by the built-in templates (Mono = TRUE mono: 1 voice + MONO mode; everything
// else POLY).
namespace
{
constexpr uint8_t kPolyMono = 0;
constexpr uint8_t kPolyPoly = 1;

// Per-arrangement full 6-Part state. Channels: 0 = Omni, else 1..16. An inactive
// Part (voices 0) is reset to a clean default: channel = partIndex + 1 (the
// engine constructor default), zone 0..127, poly POLY — so state stays
// deterministic and Single/Mono are a no-op on a freshly-constructed engine
// apart from Part 0's own voice count/poly. (The channel contribution of an
// inactive Part is functionally irrelevant — the Part has no voices.)
struct Template
{
    Arrangement  id;
    const char*  label;
    int          voices   [kNumParts];   // voice count per part (0 = disabled)
    int          channel  [kNumParts];   // MIDI channel per part (0 = Omni)
    int          lo       [kNumParts];   // key-zone low per part
    int          hi       [kNumParts];   // key-zone high per part
    uint8_t      poly     [kNumParts];   // polyphony mode per part
};

// The six voice-budget templates (the 96-voice pool model: any combination of
// counts is legal, so these are starting points, not budget partitions).
// Split points are chosen so each split region starts on a C (MIDI 36/48/60/84
// in the zone tables below).
constexpr Template kTemplates[] = {
    // Mono: TRUE mono — one Part, 1 voice, MONO polyphony (one retriggering
    // voice, no unison), full zone, Omni.
    { Arrangement::Mono, "Mono",
      { 1, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Single: one Part maxed — 16 voices, full zone, Omni.
    { Arrangement::Single, "Single",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Dual Layer: two Parts, 8+8, both full zone, both Omni (both sound
    // together on the same input).
    { Arrangement::DualLayer, "Dual Layer",
      { 8, 8, 0, 0, 0, 0 },
      { 0, 0, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Dual Split: two Parts, 8+8, key split [0..47] / [48..127], both Omni.
    { Arrangement::DualSplit, "Dual Split",
      { 8, 8, 0, 0, 0, 0 },
      { 0, 0, 3, 4, 5, 6 },
      { 0, 48, 0, 0, 0, 0 }, { 47, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Quad Split: four Parts, 8 each, key splits [0..35] / [36..59] /
    // [60..83] / [84..127], all Omni.
    { Arrangement::QuadSplit, "Quad Split",
      { 8, 8, 8, 8, 0, 0 },
      { 0, 0, 0, 0, 5, 6 },
      { 0, 36, 60, 84, 0, 0 }, { 35, 59, 83, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },

    // Multi 6: all six Parts maxed — the whole 96-voice pool, one Part per
    // MIDI channel 1..6 (individually addressable, like the old Multi 6).
    { Arrangement::Multi6, "Multi 6",
      { 16, 16, 16, 16, 16, 16 },
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

    // 1) Voice counts + MIDI channels + key zones. A positive count rides the
    //    public slots setter (1..kMaxVoicesPerPart); a 0 count DISABLES the
    //    Part via the legacy materialization path (a zero mask materializes 0
    //    slots — the engine's only disable entry point; the public setter
    //    clamps 0 to 1 by design). The voicecard bitmasks are NOT written:
    //    they are derived from these counts by the engine's rebuild.
    for (int p = 0; p < kNumParts; ++p)
    {
        if (t->voices[p] > 0)
            engine.setPartVoiceSlots (p, t->voices[p]);
        else
            engine.setPartVoiceAllocation (p, 0);
        engine.setPartMidiChannel (p, t->channel[p]);
        engine.setPartKeyZone       (p, t->lo[p], t->hi[p]);
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
    // Exact-preset match: every Part's voice count, channel, key zone and
    // polyphony must equal the template's table (the applyArrangement
    // round-trip is exact by construction — including the INACTIVE Parts'
    // channel/zone/poly defaults, which apply also writes). Any deviation is
    // Custom; the caller (Patch page) then shows the combo's "Custom" label.
    for (const auto& t : kTemplates)
    {
        bool match = true;
        for (int p = 0; p < kNumParts && match; ++p)
        {
            if (engine.getPartVoiceSlots (p) != t.voices[p])            { match = false; break; }
            if (engine.getPartChannel (p) != t.channel[p])              { match = false; break; }
            if (engine.getPartKeyrangeLow (p) != t.lo[p])               { match = false; break; }
            if (engine.getPartKeyrangeHigh (p) != t.hi[p])              { match = false; break; }
            if (engine.getPartPolyphony (p) != t.poly[p])               { match = false; break; }
        }
        if (match)
            return t.id;
    }
    return Arrangement::Custom;
}
