// Phase 1 verification for the "Patch page" PatchArrangement logic.
//
// For each of the 6 non-Custom arrangement templates (voice-budget presets over
// the 96-voice pool): apply it to a fresh SynthEngine, then assert the engine
// state matches the template table — per-part voice count (voiceSlots), MIDI
// channel, key zone and polyphony (PartData byte 15). Finally assert
// inferArrangement round-trips the applied state back to the SAME enum (exact
// preset matching), plus derived-mask invariants (the 6 hardware voicecards are
// always fully and disjointly shared when any part is active) and Custom
// detection for non-template states.
//
// Dependency-light: includes SynthEngine via the Parvati lib only (no GUI).

#include "ui/PatchArrangement.h"
#include "SynthEngine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace
{
int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { std::printf ("  ok  : %s\n", msg); }            \
        else { std::printf ("  FAIL: %s\n", msg); ++g_failures; }   \
    } while (0)

int popcount (uint8_t x)
{
    int n = 0;
    for (; x; x >>= 1)
        n += x & 1;
    return n;
}

// Polyphony modes (SynthEngine.h:49). Only the ones used by these tests are named.
constexpr uint8_t kPolyMono = 0, kPolyPoly = 1, kPolyCyclic = 3;

// The exact template table (voices, channel, zone, poly). Inactive parts
// (voices 0) get channel = partIndex+1 (engine constructor default), zone
// 0..127, poly POLY — see PatchArrangement.cpp.
struct Expect
{
    Arrangement  id;
    const char*  name;
    int          voices  [kNumParts];
    int          channel [kNumParts];
    int          lo      [kNumParts];
    int          hi      [kNumParts];
    int          poly    [kNumParts];
};

constexpr Expect kExpect[] = {
    { Arrangement::Mono, "Mono",
      { 1, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Single, "Single",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::DualLayer, "Dual Layer",
      { 8, 8, 0, 0, 0, 0 },
      { 0, 0, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::DualSplit, "Dual Split",
      { 8, 8, 0, 0, 0, 0 },
      { 0, 0, 3, 4, 5, 6 },
      { 0, 48, 0, 0, 0, 0 }, { 47, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::QuadSplit, "Quad Split",
      { 8, 8, 8, 8, 0, 0 },
      { 0, 0, 0, 0, 5, 6 },
      { 0, 36, 60, 84, 0, 0 }, { 35, 59, 83, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Multi6, "Multi 6",
      { 16, 16, 16, 16, 16, 16 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
};
}  // namespace

int main()
{
    printf ("[1] arrangementCount() == 6 (built-in templates, excl. Custom)\n");
    CHECK (arrangementCount() == 6, "arrangementCount() == 6");

    printf ("\n[2] arrangementLabel() covers every enum value\n");
    {
        struct L { Arrangement id; const char* want; };
        const L labels[] = {
            { Arrangement::Mono,       "Mono"       },
            { Arrangement::Single,     "Single"     },
            { Arrangement::DualLayer,  "Dual Layer" },
            { Arrangement::DualSplit,  "Dual Split" },
            { Arrangement::QuadSplit,  "Quad Split" },
            { Arrangement::Multi6,     "Multi 6"    },
            { Arrangement::Custom,     "Custom"     },
        };
        for (const auto& l : labels)
        {
            char msg[64];
            std::snprintf (msg, sizeof (msg), "label(%s) == \"%s\"", l.want, l.want);
            CHECK (arrangementLabel (l.id) == l.want, msg);
        }
    }

    printf ("\n[3] applyArrangement writes the exact template table + infer round-trips\n");
    for (const auto& e : kExpect)
    {
        printf ("-- %s --\n", e.name);

        SynthEngine engine;   // fresh, unprepared: applyArrangement writes every field

        applyArrangement (engine, e.id);

        // Per-part verification against the template table.
        bool partOk = true;
        for (int p = 0; p < kNumParts; ++p)
        {
            const int gotVoices = engine.getPartVoiceSlots (p);
            const int gotChan   = engine.getPartChannel (p);
            const int gotLo     = engine.getPartKeyrangeLow (p);
            const int gotHi     = engine.getPartKeyrangeHigh (p);
            const int gotPoly   = engine.getPart (p).partBytes[15];

            if (gotVoices != e.voices[p] || gotChan != e.channel[p]
                || gotLo != e.lo[p] || gotHi != e.hi[p] || gotPoly != e.poly[p])
            {
                partOk = false;
                std::printf ("    part %d mismatch: voices exp=%d got=%d | ch exp=%d got=%d | "
                             "zone exp=%d..%d got=%d..%d | poly exp=%d got=%d\n",
                             p, e.voices[p], gotVoices, e.channel[p], gotChan,
                             e.lo[p], e.hi[p], gotLo, gotHi, e.poly[p], gotPoly);
            }
        }
        {
            char msg[64];
            std::snprintf (msg, sizeof (msg), "%s: per-part voices/ch/zone/poly match table", e.name);
            CHECK (partOk, msg);
        }

        // Voice budget: total slots within the pool; Mono is TRUE mono (1 voice).
        {
            int total = 0;
            for (int p = 0; p < kNumParts; ++p)
                total += engine.getPartVoiceSlots (p);
            char msg[80];
            const bool withinPool = total <= kNumVoices;
            const int want = (e.id == Arrangement::Mono) ? 1
                           : (e.id == Arrangement::Single || e.id == Arrangement::DualLayer
                              || e.id == Arrangement::DualSplit) ? 16
                           : (e.id == Arrangement::QuadSplit) ? 32 : 96;
            std::snprintf (msg, sizeof (msg), "%s: total voices == %d (<= %d pool)", e.name, total, kNumVoices);
            CHECK (withinPool && total == want, msg);
        }

        // Derived-mask invariant: the 6 hardware voicecards are shared
        // disjointly (contiguous proportional share via mul_export::deriveMasks).
        // When the requests fit (sum <= 6) each part gets EXACTLY its voice
        // count in cards (the 1 voice = 1 card model — Mono gets 1 card);
        // otherwise the full 6 are shared proportionally.
        {
            int total = 0, totalCards = 0;
            bool disjoint = true;
            uint8_t used = 0;
            for (int p = 0; p < kNumParts; ++p)
            {
                total += engine.getPartVoiceSlots (p);
                const uint8_t m = engine.getPartVoiceAllocation (p);
                totalCards += popcount (m);
                if (used & m) disjoint = false;
                used = static_cast<uint8_t> (used | m);
            }
            const int wantCards = std::min (total, 6);
            char msg[96];
            std::snprintf (msg, sizeof (msg),
                           "%s: derived cards == %d, disjoint (got %d%s)",
                           e.name, wantCards, totalCards, disjoint ? "" : ", OVERLAP");
            CHECK (totalCards == wantCards && disjoint, msg);
        }

        // Round-trip: inferArrangement must reproduce the applied enum.
        {
            const Arrangement inferred = inferArrangement (engine);
            char msg[64];
            std::snprintf (msg, sizeof (msg), "%s: inferArrangement round-trips to %s",
                           e.name, e.name);
            CHECK (inferred == e.id, msg);
        }
    }

    printf ("\n[4] Custom detection (non-template states)\n");
    {
        SynthEngine engine;   // fresh default: 6 voices on p0, ch1, POLY — Custom

        // The fresh-engine default (faithful 6-voice part 0) is NOT a preset.
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "fresh engine (part0=6 voices, ch1) -> Custom (not a preset)");

        // Single layout + an undefined poly mode -> Custom.
        applyArrangement (engine, Arrangement::Single);
        engine.setCurrentPart (0);
        engine.applyPartByte (15, kPolyCyclic);   // neither POLY nor MONO
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "Single layout + CYCLIC poly -> Custom");

        // Single + MONO poly is NOT the Mono preset (voice count differs).
        applyArrangement (engine, Arrangement::Single);
        engine.setCurrentPart (0);
        engine.applyPartByte (15, kPolyMono);
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "16 voices + MONO poly != Mono preset (count differs) -> Custom");

        // Hand-built non-template: Dual Layer with a distinct second channel.
        applyArrangement (engine, Arrangement::DualLayer);   // p0/p1 same ch, overlap
        engine.setPartMidiChannel (1, 9);                    // now distinct channels...
        // ...but zones still overlap (both 0..127) -> Custom.
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "2 parts, distinct chan + overlapping zone -> Custom");

        // Custom is a no-op to apply (engine state unchanged).
        applyArrangement (engine, Arrangement::Custom);
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "applyArrangement(Custom) is a no-op (still Custom)");
    }

    printf ("\n[5] True Mono preset behaviour (the user-facing point)\n");
    {
        SynthEngine engine;
        applyArrangement (engine, Arrangement::Mono);
        CHECK (engine.getPartVoiceSlots (0) == 1 && engine.getPartPolyphony (0) == kPolyMono,
               "Mono: part0 = 1 voice + MONO poly (true mono, no unison)");
        CHECK (engine.getPartVoiceSlots (1) == 0 && engine.getPartVoiceSlots (5) == 0,
               "Mono: parts 1..5 disabled (0 voices)");
        // Re-apply Single over it: part0 -> 16 voices, POLY; disabled parts stay.
        applyArrangement (engine, Arrangement::Single);
        CHECK (engine.getPartVoiceSlots (0) == 16 && engine.getPartPolyphony (0) == kPolyPoly,
               "Single re-applied over Mono: part0 = 16 voices, POLY");
    }

    printf ("\n%s (%d failures)\n",
            g_failures ? "PATCH ARRANGEMENT TEST: FAILURES"
                       : "PATCH ARRANGEMENT TEST: ALL CHECKS PASSED",
            g_failures);
    return g_failures ? 1 : 0;
}
