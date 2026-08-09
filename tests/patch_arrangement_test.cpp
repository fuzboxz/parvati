// Phase 1 verification for the "Patch page" PatchArrangement logic.
//
// For each of the 5 non-Custom arrangement templates: apply it to a fresh
// SynthEngine, then assert the engine state matches the design spec's template
// table — per-part popcount(voiceAllocation), MIDI channel, key zone and
// polyphony (PartData byte 15). Finally assert inferArrangement round-trips the
// applied state back to the SAME enum (except deliberately-ambiguous cases,
// documented inline).
//
// Dependency-light: includes SynthEngine via the Parvati lib only (no GUI).

#include "ui/PatchArrangement.h"
#include "SynthEngine.h"

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
constexpr uint8_t kPolyPoly = 1, kPolyCyclic = 3, kPolyChain = 4;

// The exact template table from the design spec (counts, channel, zone, poly).
// Inactive parts (count 0) get channel = partIndex+1 (engine constructor
// default), zone 0..127, poly POLY — see PatchArrangement.cpp.
struct Expect
{
    Arrangement  id;
    const char*  name;
    int          counts  [kNumParts];
    int          channel [kNumParts];
    int          lo      [kNumParts];
    int          hi      [kNumParts];
    int          poly    [kNumParts];
};

constexpr Expect kExpect[] = {
    { Arrangement::Single, "Single",
      { 6, 0, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Stack, "Stack",
      { 6, 0, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyChain, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Split2, "Split 2",
      { 3, 3, 0, 0, 0, 0 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 60, 0, 0, 0, 0 }, { 59, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Layer2, "Layer 2",
      { 3, 3, 0, 0, 0, 0 },
      { 1, 1, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
    { Arrangement::Multi6, "Multi 6",
      { 1, 1, 1, 1, 1, 1 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly } },
};
}  // namespace

int main()
{
    printf ("[1] arrangementCount() == 5 (built-in templates, excl. Custom)\n");
    CHECK (arrangementCount() == 5, "arrangementCount() == 5");

    printf ("\n[2] arrangementLabel() covers every enum value\n");
    {
        struct L { Arrangement id; const char* want; };
        const L labels[] = {
            { Arrangement::Single, "Single" },
            { Arrangement::Stack,  "Stack"  },
            { Arrangement::Split2, "Split 2" },
            { Arrangement::Layer2, "Layer 2" },
            { Arrangement::Multi6, "Multi 6" },
            { Arrangement::Custom, "Custom" },
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

        // Per-part verification against the spec table.
        bool partOk = true;
        for (int p = 0; p < kNumParts; ++p)
        {
            const int gotCount = popcount (engine.getPartVoiceAllocation (p));
            const int gotChan  = engine.getPartChannel (p);
            const int gotLo    = engine.getPartKeyrangeLow (p);
            const int gotHi    = engine.getPartKeyrangeHigh (p);
            const int gotPoly  = engine.getPart (p).partBytes[15];

            if (gotCount != e.counts[p] || gotChan != e.channel[p]
                || gotLo != e.lo[p] || gotHi != e.hi[p] || gotPoly != e.poly[p])
            {
                partOk = false;
                std::printf ("    part %d mismatch: count exp=%d got=%d | ch exp=%d got=%d | "
                             "zone exp=%d..%d got=%d..%d | poly exp=%d got=%d\n",
                             p, e.counts[p], gotCount, e.channel[p], gotChan,
                             e.lo[p], e.hi[p], gotLo, gotHi, e.poly[p], gotPoly);
            }
        }
        {
            char msg[64];
            std::snprintf (msg, sizeof (msg), "%s: per-part count/ch/zone/poly match table", e.name);
            CHECK (partOk, msg);
        }

        // Total cards never exceed the 6 hardware voicecards.
        {
            int total = 0;
            for (int p = 0; p < kNumParts; ++p)
                total += popcount (engine.getPartVoiceAllocation (p));
            char msg[64];
            std::snprintf (msg, sizeof (msg), "%s: total cards == %d (<= 6)", e.name, total);
            CHECK (total <= kNumVoices, msg);
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
        SynthEngine engine;   // default == Single (6 cards on p0, ch1, POLY)

        // 6 cards on one part but an undefined poly mode -> Custom.
        applyArrangement (engine, Arrangement::Single);
        engine.setCurrentPart (0);
        engine.applyPartByte (15, kPolyCyclic);   // neither POLY nor CHAIN
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "Single layout + CYCLIC poly -> Custom");

        // Hand-built non-template: 2 active parts, distinct channels but
        // OVERLAPPING zones (neither complementary-Split nor shared-Layer).
        applyArrangement (engine, Arrangement::Layer2);   // p0/p1 same ch, overlap
        engine.setPartMidiChannel (1, 9);                 // now distinct channels...
        // ...but zones still overlap (both 0..127) -> Custom.
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "2 parts, distinct chan + overlapping zone -> Custom");

        // Custom is a no-op to apply (engine state unchanged).
        applyArrangement (engine, Arrangement::Custom);
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "applyArrangement(Custom) is a no-op (still Custom)");
    }

    printf ("\n%s (%d failures)\n",
            g_failures ? "PATCH ARRANGEMENT TEST: FAILURES"
                       : "PATCH ARRANGEMENT TEST: ALL CHECKS PASSED",
            g_failures);
    return g_failures ? 1 : 0;
}
