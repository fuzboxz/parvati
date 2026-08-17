// Phase 1 of the "Patch page" feature: arrangement templates + inference.
// See PatchArrangement.h for the engine-API deviations (approved), the
// voice-first model note and the design spec references.
//
// Handoff note (Phase 2/3, NOT fixed here): applyArrangement writes polyphony
// and spread directly via applyPartByte, so after a UI apply the APVTS
// `part_polyphony` / `part_spread` params for an edited part can be stale until
// the next part-switch re-sync — PatchPage::onArrangementChanged already
// triggers the same part-param re-sync the loader uses after applying.

#include "PatchArrangement.h"

#include <cstdint>

// Polyphony modes (SynthEngine.h:49), PartData byte 15:
// MONO=0, POLY=1, UNISON_2X=2, CYCLIC=3, CHAIN=4. Only MONO and POLY are used
// by the built-in templates (Mono = TRUE mono: 1 voice + MONO mode; Unison =
// MONO mode with the whole 16-voice stack sounding per note; Drum Kit = 1 voice
// MONO per drum part).
namespace
{
constexpr uint8_t kPolyMono = 0;
constexpr uint8_t kPolyPoly = 1;

// Per-arrangement full 6-Part state. Channels: 0 = Omni, else 1..16. An inactive
// Part (voices 0) is reset to a clean default: channel = partIndex + 1 (the
// engine constructor default), zone 0..127, poly POLY, spread 0 — so state stays
// deterministic and Mono/Poly are a no-op on a freshly-constructed engine apart
// from Part 0's own voice count/poly/spread. (The channel contribution of an
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
    uint8_t      spread   [kNumParts];   // per-voice detune spread (PartData byte 3)
};

// The five voice-budget templates (the 96-voice pool model: any combination of
// counts is legal, so these are starting points, not budget partitions).
// The Drum Kit zones are single GM percussion notes (lo == hi) so each Part is
// exactly one drum.
constexpr Template kTemplates[] = {
    // Mono: TRUE mono — one Part, 1 voice, MONO polyphony (one retriggering
    // voice, no unison), full zone, Omni.
    { Arrangement::Mono, "Mono",
      { 1, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 0, 0, 0, 0, 0, 0 } },

    // Poly: one Part maxed — 16 voices, POLY, full zone, Omni.
    { Arrangement::Poly, "Poly",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 0, 0, 0, 0, 0, 0 } },

    // Unison: one Part, 16 voices, MONO — the whole 16-voice stack sounds on
    // every note (the engine's unison size for MONO is the Part's voice count),
    // with a per-voice detune SPREAD so the stack reads fat: drift =
    // voiceIndex * spread in 1/128-semitone units, so spread 8 spaces adjacent
    // voices ~6 cents apart (~94 cents across the stack).
    { Arrangement::Unison, "Unison",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 8, 0, 0, 0, 0, 0 } },

    // Multitimbral: all six Parts maxed — 16 voices each, all MONO (per-part
    // monophonic), one Part per MIDI channel 1..6 (individually addressable).
    { Arrangement::Multitimbral, "Multitimbral",
      { 16, 16, 16, 16, 16, 16 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono },
      { 0, 0, 0, 0, 0, 0 } },

    // Drum Kit: six Parts, 1 voice each, all MONO (a drum is one-shot — mono
    // means the latest hit retriggers the same voice), all Omni (the kit
    // answers on ANY channel), each mapped to a single GM percussion note
    // (Kick 36 / Snare 38 / Clap 39 / Closed Hat 42 / Open Hat 46 / Tom 45).
    { Arrangement::DrumKit, "Drum Kit",
      { 1, 1, 1, 1, 1, 1 },
      { 0, 0, 0, 0, 0, 0 },
      { 36, 38, 39, 42, 46, 45 }, { 36, 38, 39, 42, 46, 45 },
      { kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono },
      { 0, 0, 0, 0, 0, 0 } },
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

    // 2) Polyphony + spread LAST, via the EXISTING public pair (PartData bytes
    //    15 and 3 — see the header deviation note).
    for (int p = 0; p < kNumParts; ++p)
    {
        engine.setCurrentPart (p);
        engine.applyPartByte (15, t->poly[p]);
        engine.applyPartByte (3, t->spread[p]);
    }

    engine.setCurrentPart (savedPart);
}

Arrangement inferArrangement (const SynthEngine& engine)
{
    // Exact-preset match: every Part's voice count, channel, key zone,
    // polyphony and spread must equal the template's table (the
    // applyArrangement round-trip is exact by construction — including the
    // INACTIVE Parts' channel/zone/poly/spread defaults, which apply also
    // writes). Any deviation is Custom; the caller (Patch page) then selects
    // the combo's "Custom" item.
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
            if (engine.getPartSpread (p) != t.spread[p])                { match = false; break; }
        }
        if (match)
            return t.id;
    }
    return Arrangement::Custom;
}
