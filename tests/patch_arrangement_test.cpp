// Phase 1 verification for the "Patch page" PatchArrangement logic.
//
// For each of the 5 non-Custom arrangement templates (voice-budget presets over
// the 96-voice pool): apply it to a fresh SynthEngine, then assert the engine
// state matches the template table — per-part voice count (voiceSlots), MIDI
// channel, key zone, polyphony (PartData byte 15) and spread (PartData byte 3).
// Finally assert inferArrangement round-trips the applied state back to the SAME
// enum (exact preset matching), plus derived-mask invariants (the 6 hardware
// voicecards are always fully and disjointly shared when any part is active) and
// Custom detection for non-template states.
//
// Dependency-light: includes SynthEngine via the Parvati lib only (no GUI).

#include <cstring>

#include "ui/PatchArrangement.h"
#include "unified_test_runner.h"
#include "SynthEngine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace
{
int g_failures = 0;

// This file keeps its own CHECK macro: it wins over the runner copy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { std::printf ("  ok  : %s\n", msg); }            \
        else { std::printf ("  FAIL: %s\n", msg); ++g_failures; }   \
    } while (0)
#pragma clang diagnostic pop

int popcount (uint8_t x)
{
    int n = 0;
    for (; x; x >>= 1)
        n += x & 1;
    return n;
}

// Polyphony modes (SynthEngine.h:49). Only the ones used by these tests are named.
constexpr uint8_t kPolyMono = 0, kPolyPoly = 1, kPolyCyclic = 3;

// The exact template table (voices, channel, zone, poly, spread). Inactive
// parts (voices 0) get channel = partIndex+1 (engine constructor default),
// zone 0..127, poly POLY, spread 0 — see PatchArrangement.cpp.
struct Expect
{
    Arrangement  id;
    const char*  name;
    int          voices  [kNumParts];
    int          channel [kNumParts];
    int          lo      [kNumParts];
    int          hi      [kNumParts];
    int          poly    [kNumParts];
    int          spread  [kNumParts];
};

constexpr Expect kExpect[] = {
    { Arrangement::Mono, "Mono",
      { 1, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 0, 0, 0, 0, 0, 0 } },
    { Arrangement::Poly, "Poly",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 0, 0, 0, 0, 0, 0 } },
    { Arrangement::Unison, "Unison",
      { 16, 0, 0, 0, 0, 0 },
      { 0, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly, kPolyPoly },
      { 8, 0, 0, 0, 0, 0 } },
    { Arrangement::Multitimbral, "Multitimbral",
      { 16, 16, 16, 16, 16, 16 },
      { 1, 2, 3, 4, 5, 6 },
      { 0, 0, 0, 0, 0, 0 }, { 127, 127, 127, 127, 127, 127 },
      { kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono },
      { 0, 0, 0, 0, 0, 0 } },
    { Arrangement::DrumKit, "Drum Kit",
      { 1, 1, 1, 1, 1, 1 },
      { 0, 0, 0, 0, 0, 0 },
      { 36, 38, 39, 42, 46, 45 }, { 36, 38, 39, 42, 46, 45 },
      { kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono, kPolyMono },
      { 0, 0, 0, 0, 0, 0 } },
};
}  // namespace

TEST(patch_arrangement_test)
{
    printf ("[1] arrangementCount() == 5 (built-in templates, excl. Custom)\n");
    CHECK (arrangementCount() == 5, "arrangementCount() == 5");

    printf ("\n[2] arrangementLabel() covers every enum value\n");
    {
        struct L { Arrangement id; const char* want; };
        const L labels[] = {
            { Arrangement::Mono,         "Mono"         },
            { Arrangement::Poly,         "Poly"         },
            { Arrangement::Unison,       "Unison"       },
            { Arrangement::Multitimbral, "Multitimbral" },
            { Arrangement::DrumKit,      "Drum Kit"     },
            { Arrangement::Custom,       "Custom"       },
        };
        for (const auto& l : labels)
        {
            char msg[64];
            std::snprintf (msg, sizeof (msg), "label(%s) == \"%s\"", l.want, l.want);
            // Content compare, NOT pointer compare: identical literals are
            // NOT guaranteed to be merged by the linker (ASan's instrumented
            // globals carry redzones and defeat constant pooling, which made
            // every label() check fail under the sanitized build).
            CHECK (std::strcmp (arrangementLabel (l.id), l.want) == 0, msg);
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
            const int gotSpread = engine.getPart (p).partBytes[3];

            if (gotVoices != e.voices[p] || gotChan != e.channel[p]
                || gotLo != e.lo[p] || gotHi != e.hi[p] || gotPoly != e.poly[p]
                || gotSpread != e.spread[p])
            {
                partOk = false;
                std::printf ("    part %d mismatch: voices exp=%d got=%d | ch exp=%d got=%d | "
                             "zone exp=%d..%d got=%d..%d | poly exp=%d got=%d | spread exp=%d got=%d\n",
                             p, e.voices[p], gotVoices, e.channel[p], gotChan,
                             e.lo[p], e.hi[p], gotLo, gotHi, e.poly[p], gotPoly,
                             e.spread[p], gotSpread);
            }
        }
        {
            char msg[64];
            std::snprintf (msg, sizeof (msg), "%s: per-part voices/ch/zone/poly/spread match table", e.name);
            CHECK (partOk, msg);
        }

        // Voice budget: total slots within the pool; Mono is TRUE mono (1 voice);
        // Drum Kit uses exactly 6 voices (one per drum part).
        {
            int total = 0;
            for (int p = 0; p < kNumParts; ++p)
                total += engine.getPartVoiceSlots (p);
            char msg[80];
            const bool withinPool = total <= kNumVoices;
            const int want = (e.id == Arrangement::Mono) ? 1
                           : (e.id == Arrangement::Poly || e.id == Arrangement::Unison) ? 16
                           : (e.id == Arrangement::DrumKit) ? 6 : 96;
            std::snprintf (msg, sizeof (msg), "%s: total voices == %d (<= %d pool)", e.name, total, kNumVoices);
            CHECK (withinPool && total == want, msg);
        }

        // Derived-mask invariant: the 6 hardware voicecards are shared
        // disjointly (contiguous proportional share via mul_export::deriveMasks).
        // When the requests fit (sum <= 6) each part gets EXACTLY its voice
        // count in cards (the 1 voice = 1 card model — Mono/Drum Kit get 1
        // card per active part); otherwise the full 6 are shared
        // proportionally.
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

        // Poly layout + an undefined poly mode -> Custom.
        applyArrangement (engine, Arrangement::Poly);
        engine.setCurrentPart (0);
        engine.applyPartByte (15, kPolyCyclic);   // neither POLY nor MONO
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "Poly layout + CYCLIC poly -> Custom");

        // 16 voices + MONO poly with NO spread is NOT the Unison preset
        // (Unison requires spread 8) -> Custom.
        applyArrangement (engine, Arrangement::Poly);
        engine.setCurrentPart (0);
        engine.applyPartByte (15, kPolyMono);
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "16 voices + MONO poly + spread 0 != Unison preset (spread differs) -> Custom");

        // Hand-built non-template: Multitimbral with one part's channel moved.
        applyArrangement (engine, Arrangement::Multitimbral);
        engine.setPartMidiChannel (1, 9);   // not the template's ch 2...
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "Multitimbral with part1 on ch9 -> Custom");

        // A spread edit alone breaks the preset match.
        applyArrangement (engine, Arrangement::Unison);
        engine.setCurrentPart (0);
        engine.applyPartByte (3, 3);   // not the template's spread 8
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "Unison with spread 3 -> Custom (spread is part of the match)");

        // Custom is a no-op to apply (engine state unchanged).
        applyArrangement (engine, Arrangement::Custom);
        CHECK (inferArrangement (engine) == Arrangement::Custom,
               "applyArrangement(Custom) is a no-op (still Custom)");
    }

    printf ("\n[5] Preset behaviour (the user-facing points)\n");
    {
        SynthEngine engine;
        applyArrangement (engine, Arrangement::Mono);
        CHECK (engine.getPartVoiceSlots (0) == 1 && engine.getPartPolyphony (0) == kPolyMono,
               "Mono: part0 = 1 voice + MONO poly (true mono, no unison)");
        CHECK (engine.getPartVoiceSlots (1) == 0 && engine.getPartVoiceSlots (5) == 0,
               "Mono: parts 1..5 disabled (0 voices)");

        // Unison: 16 voices + MONO (the whole stack sounds per note) + spread.
        applyArrangement (engine, Arrangement::Unison);
        CHECK (engine.getPartVoiceSlots (0) == 16 && engine.getPartPolyphony (0) == kPolyMono,
               "Unison: part0 = 16 voices + MONO poly (16-voice unison)");
        CHECK (engine.getPartSpread (0) == 8,
               "Unison: part0 spread == 8 (detuned stack)");

        // Drum Kit: 6 single-note parts, one voice each, all Omni.
        applyArrangement (engine, Arrangement::DrumKit);
        CHECK (engine.getPartVoiceSlots (0) == 1 && engine.getPartVoiceSlots (5) == 1,
               "Drum Kit: every part = 1 voice");
        CHECK (engine.getPartChannel (0) == 0 && engine.getPartChannel (5) == 0,
               "Drum Kit: every part Omni (channel 0)");
        CHECK (engine.getPartKeyrangeLow (0) == 36 && engine.getPartKeyrangeHigh (0) == 36
                   && engine.getPartKeyrangeLow (5) == 45 && engine.getPartKeyrangeHigh (5) == 45,
               "Drum Kit: single-note zones (Kick 36, Tom 45)");
        CHECK (engine.getPartPolyphony (3) == kPolyMono,
               "Drum Kit: parts are MONO (one-shot drums)");

        // Re-apply Poly over Mono: part0 -> 16 voices, POLY; disabled parts stay.
        applyArrangement (engine, Arrangement::Poly);
        CHECK (engine.getPartVoiceSlots (0) == 16 && engine.getPartPolyphony (0) == kPolyPoly
                   && engine.getPartSpread (0) == 0,
               "Poly re-applied over Mono: part0 = 16 voices, POLY, spread 0");
    }

    printf ("\n%s (%d failures)\n",
            g_failures ? "PATCH ARRANGEMENT TEST: FAILURES"
                       : "PATCH ARRANGEMENT TEST: ALL CHECKS PASSED",
            g_failures);
    return g_failures == 0;
}
