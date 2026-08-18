// Scala (.scl / .kbm) import verification for Parvati.
//
// Covers the parser grammar (comments, trailing text, blank lines, negative
// cents, bare-integer ratios, malformed tokens) and the full fixture corpus
// with EXPECTED offset tables. The fixtures follow the canonical Scala test
// corpus names (12tet / ji12 / penta / edo19 / bohlen / root62 / x432); every
// expected table below was hand-derived INDEPENDENTLY from the documented
// math in ScalaImport.h (dev/ref-dev/reference-frequency fold, single final
// llround(cents * 1.28)) and cross-checked digit-by-digit — a parser/expected
// mismatch is a PARSER bug unless re-derivation proves otherwise.
//
// Built by default. Run with: ./build/parvati_scala_import_test

#include <cstdio>
#include <cstring>

#include <juce_core/juce_core.h>

#include "ScalaImport.h"
#include "TuningTables.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr int16_t kSil = parvati::kTuningSilence;   // 32767

bool tablesEqual (const int16_t (&got)[12], const int16_t (&want)[12])
{
    return std::memcmp (got, want, sizeof (got)) == 0;
}

void printTable (const int16_t (&t)[12])
{
    std::printf ("       [");
    for (int i = 0; i < 12; ++i)
        std::printf ("%s%d", i ? ", " : "", t[i]);
    std::printf ("]\n");
}

// ---------------------------------------------------------------------------
// Fixture corpus (byte-exact raw strings).
// ---------------------------------------------------------------------------

const char* kScl12tet = R"SCL(! 12tet.scl
!
 12
!
 100.0
 200.0
 300.0
 400.0
 500.0
 600.0
 700.0
 800.0
 900.0
 1000.0
 1100.0
 2/1
)SCL";

const char* kSclJi12 = R"SCL(! ji12.scl — 5-limit just intonation
 12
 16/15
 9/8
 6/5
 5/4
 4/3
 45/32
 3/2
 8/5
 5/3
 9/5
 15/8
 2/1
)SCL";

// Minor pentatonic fixture. Degrees 2..5 sit at 9/8 / 397.6 / 3/2 / 5/3 so the
// mapped classes land at +3.91 / -2.40 / +1.955 / -15.64 cents from 12-EDO
// (the 397.6 line is a meantone-flavoured cent value: no clean ratio sits in
// the required window, and mixed ratio/cents files are legal .scl).
const char* kSclPenta = R"SCL(! penta.scl
 5
 9/8
 397.6
 3/2
 5/3
 2/1
)SCL";

// 12-key mapping for the pentatonic: classes 0/2/4/7/9 -> degrees 0..4.
const char* kKbmPenta = R"KBM(! penta.kbm
!
 12
!
 0
 11
 60
 60
 261.6255653
 5
 0
 x
 1
 x
 2
 x
 x
 3
 x
 4
 x
 x
)KBM";

// 19-EDO, 19 cent tones (k * 1200/19).
const char* kSclEdo19 = R"SCL(! edo19.scl
 19
 63.1578947368421
 126.315789473684
 189.473684210526
 252.631578947368
 315.789473684211
 378.947368421053
 442.105263157895
 505.263157894737
 568.421052631579
 631.578947368421
 694.736842105263
 757.894736842105
 821.052631578947
 884.210526315789
 947.368421052632
 1010.52631578947
 1073.68421052632
 1136.84210526316
 2/1
)SCL";

// 19-EDO across 12 keys: the classic 2+1 degree pattern (0,2,3,5,6,8,9,11,
// 12,14,15,17 — covers 12 of the 19 degrees).
const char* kKbmEdo19 = R"KBM(! edo19.kbm
!
 12
!
 0
 11
 60
 60
 261.6255653
 19
 0
 2
 3
 5
 6
 8
 9
 11
 12
 14
 15
 17
)KBM";

// Bohlen-Pierce (13 tones, formal octave 3/1 = 1901.955 cents).
const char* kSclBohlen = R"SCL(! bohlen.scl
 13
 27/25
 25/21
 9/7
 7/5
 75/49
 5/3
 9/5
 49/25
 15/7
 7/3
 63/25
 25/9
 3/1
)SCL";

// Any S=12 keymap — the period gate must fire before the table matters.
const char* kKbmPlain12 = R"KBM(! plain12.kbm
 12
 0
 11
 60
 60
 261.6255653
 13
 0
 1
 2
 3
 4
 5
 6
 7
 8
 9
 10
 11
)KBM";

// Middle note 62, reference A4 = 440 (rotates which classes the ratios land on).
const char* kKbmRoot62 = R"KBM(! root62.kbm
!
 12
!
 0
 11
 62
 69
 440.0
 12
 0
 1
 2
 3
 4
 5
 6
 7
 8
 9
 10
 11
)KBM";

// A = 432 Hz reference with 12-EDO: a constant -31.77-cent (= -41 unit) offset
// everywhere, plus one unmapped class ('x' at key index 2).
const char* kKbmX432 = R"KBM(! x432.kbm
!
 12
!
 0
 11
 60
 69
 432.0
 12
 0
 1
 x
 3
 4
 5
 6
 7
 8
 9
 10
 11
)KBM";

// ---------------------------------------------------------------------------
// Cases.
// ---------------------------------------------------------------------------

void testCorpus()
{
    std::printf ("[corpus: expected tables]\n");

    {   // F1 — 12tet: identity.
        const auto r = parvati::importScala (kScl12tet);
        const int16_t want[12] = {};
        check (r.ok && r.error.isEmpty(), "F1 12tet imports");
        check (tablesEqual (r.offsets, want), "F1 all-zero table");
    }
    {   // F2 — ji12 (no kbm: identity map, M=R=60, F=261.6255653).
        const auto r = parvati::importScala (kSclJi12);
        const int16_t want[12] = { 0, 15, 5, 20, -18, -3, -13, 3, 18, -20, 23, -15 };
        check (r.ok, "F2 ji12 imports");
        if (! tablesEqual (r.offsets, want))
        {
            printTable (r.offsets);
            check (false, "F2 table matches hand-derived values");
        }
        else
            check (true, "F2 table matches hand-derived values");
    }
    {   // F3 — penta standalone: S = 5 -> reject.
        const auto r = parvati::importScala (kSclPenta);
        check (! r.ok && r.error.contains ("S = 5"), "F3 penta standalone rejected (S != 12)");
    }
    {   // F3b — penta + 12-key kbm: 5 mapped classes, rest muted.
        const auto r = parvati::importScala (kSclPenta, kKbmPenta);
        const int16_t want[12] = { 0, kSil, 5, kSil, -3, kSil, kSil, 3, kSil, -20, kSil, kSil };
        check (r.ok, "F3b penta.kbm imports");
        if (! tablesEqual (r.offsets, want))
        {
            printTable (r.offsets);
            check (false, "F3b table matches hand-derived values");
        }
        else
            check (true, "F3b table matches hand-derived values");
        bool sawMuted = false;
        for (const auto& w : r.warnings)
            if (w.contains ("muted"))
                sawMuted = true;
        check (sawMuted, "F3b warns about muted classes");
    }
    {   // F4 — edo19 standalone: S = 19 -> reject.
        const auto r = parvati::importScala (kSclEdo19);
        check (! r.ok && r.error.contains ("S = 19"), "F4 edo19 standalone rejected (S != 12)");
    }
    {   // F4b — edo19 + the 2+1 pattern kbm (o = 19).
        const auto r = parvati::importScala (kSclEdo19, kKbmEdo19);
        const int16_t want[12] = { 0, 34, -13, 20, -27, 7, -40, -7, -54, -20, -67, -34 };
        check (r.ok, "F4b edo19.kbm imports");
        if (! tablesEqual (r.offsets, want))
        {
            printTable (r.offsets);
            check (false, "F4b table matches hand-derived values");
        }
        else
            check (true, "F4b table matches hand-derived values");
        bool sawSubset = false;
        for (const auto& w : r.warnings)
            if (w.contains ("subset"))
                sawSubset = true;
        check (sawSubset, "F4b notes the 19->12 subset mapping");
    }
    {   // F5 — Bohlen-Pierce: standalone rejects on S, with a kbm on period.
        const auto r1 = parvati::importScala (kSclBohlen);
        check (! r1.ok && r1.error.contains ("S = 13"), "F5 bohlen standalone rejected (S != 12)");
        const auto r2 = parvati::importScala (kSclBohlen, kKbmPlain12);
        check (! r2.ok && r2.error.contains ("1901.9"), "F5 bohlen+kbm rejected on non-octave period");
    }
    {   // F6 — ji12 with M = 62, R = 69, F = 440 (class rotation).
        const auto r = parvati::importScala (kSclJi12, kKbmRoot62);
        const int16_t want[12] = { 20, -18, -3, 13, 3, 18, -20, -5, -15, 0, 15, -23 };
        check (r.ok, "F6 root62 imports");
        if (! tablesEqual (r.offsets, want))
        {
            printTable (r.offsets);
            check (false, "F6 table matches hand-derived values");
        }
        else
            check (true, "F6 table matches hand-derived values");
    }
    {   // F7 — 12tet at A = 432 Hz with one unmapped class.
        const auto r = parvati::importScala (kScl12tet, kKbmX432);
        const int16_t want[12] = { -41, -41, kSil, -41, -41, -41, -41, -41, -41, -41, -41, -41 };
        check (r.ok, "F7 x432 imports");
        if (! tablesEqual (r.offsets, want))
        {
            printTable (r.offsets);
            check (false, "F7 table matches hand-derived values (class 2 muted, others -41)");
        }
        else
            check (true, "F7 table matches hand-derived values (class 2 muted, others -41)");
    }
}

void testMalformed()
{
    std::printf ("[malformed inputs]\n");

    {   // M1 — declared count exceeds provided tones.
        const char* s = "! bad count\n 12\n 100.0\n 200.0\n";
        const auto r = parvati::importScala (s);
        check (! r.ok && r.error.contains ("declared 12") && r.error.contains ("only 2"),
               "M1 short pitch block rejected with counts");
    }
    {   // M2 — negative ratio.
        const char* s = "! negative ratio\n 3\n 9/8\n -3/2\n 2/1\n";
        const auto r = parvati::importScala (s);
        check (! r.ok && r.error.contains ("-3/2"), "M2 negative ratio rejected, value cited");
    }
    {   // M3 — kbm with a stray character on a key line.
        const char* kbm = "! bad kbm\n 12\n 0\n 11\n 60\n 60\n 261.6255653\n 12\n 0\n 1\n y\n 3\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n 11\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("y"), "M3 kbm 'y' key rejected, value cited");
    }
    {   // kbm with 11 key lines (D7: trailing unmapped may NOT be omitted).
        const char* kbm = "! short kbm\n 12\n 0\n 11\n 60\n 60\n 261.6255653\n 12\n 0\n 1\n 2\n 3\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("key entry 12"), "kbm with 11 key lines rejected (D7)");
    }
    {   // kbm S = 0 (linear map).
        const char* kbm = "! linear map\n 0\n 0\n 11\n 60\n 60\n 261.6255653\n 0\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("S = 0"), "kbm S = 0 (linear map) rejected");
    }
    {   // Hostile huge S (bug hunt 2026-08-18, F-static-3/F-state-2): S fed
        // keys.reserve() BEFORE any range gate, so a crafted S (e.g. 999999999)
        // attempted a multi-GB allocation and an uncaught bad_alloc terminated
        // the host. The parse-time cap must reject it cleanly (mirroring
        // parseScl's 1..1024 tone gate) without allocating.
        const char* kbm = "! hostile S\n 999999999\n 0\n 11\n 60\n 60\n 261.6255653\n 0\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("out of range"),
               "kbm hostile S = 999999999 rejected at parse time (no giant reserve)");
    }
    {   // Same gate, just past the cap (1025) — the boundary itself.
        const char* kbm = "! S one past cap\n 1025\n 0\n 11\n 60\n 60\n 261.6255653\n 0\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("out of range"), "kbm S = 1025 rejected (cap boundary)");
    }
    {   // degree beyond the formal octave (D8).
        const char* kbm = "! degree beyond o\n 12\n 0\n 11\n 60\n 60\n 261.6255653\n 5\n 0\n 1\n 2\n 3\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n 11\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("degree 6") && r.error.contains ("o = 5"),
               "kbm degree > o rejected, degree and o cited");
    }
    {   // o beyond the scale size.
        const char* kbm = "! o beyond scale\n 12\n 0\n 11\n 60\n 60\n 261.6255653\n 13\n 0\n 1\n 2\n 3\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n 11\n";
        const auto r = parvati::importScala (kSclPenta, kbm);
        check (! r.ok && r.error.contains ("formal octave"), "o > N rejected");
    }
    {   // Empty scl.
        const auto r = parvati::importScala ("");
        check (! r.ok, "empty scl rejected");
    }
    {   // Decimal-comma cents (locale trap).
        const char* s = "! comma\n 2\n 100,0\n 2/1\n";
        const auto r = parvati::importScala (s);
        check (! r.ok && r.error.contains ("100,0"), "comma decimal rejected, token cited");
    }
    {   // R on an unmapped key.
        const char* kbm = "! R unmapped\n 12\n 0\n 11\n 60\n 63\n 261.6255653\n 12\n 0\n 1\n 2\n x\n 4\n 5\n 6\n 7\n 8\n 9\n 10\n 11\n";
        const auto r = parvati::importScala (kScl12tet, kbm);
        check (! r.ok && r.error.contains ("reference note"), "R on an 'x' key rejected");
    }
}

void testGrammarEdges()
{
    std::printf ("[grammar edges]\n");

    {   // EOL comments + trailing junk after values + blank lines in the block.
        const char* s = "! desc\n 12\n\n 100.0 ! one hundred\n 200.0 two hundred\n\n 300.0\n 400.0\n 500.0\n"
                        " 600.0\n 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 2/1 the octave\n! trailing comment\n";
        const auto r = parvati::importScala (s);
        const int16_t want[12] = {};
        check (r.ok && tablesEqual (r.offsets, want), "comments/junk/blank lines tolerated");
    }
    {   // Negative cents: class 2 detuned by -5.0 cents (-6 units after 1.28x).
        const char* s = "! neg cents\n 12\n 100.0\n 195.0\n 300.0\n 400.0\n 500.0\n 600.0\n 700.0\n"
                        " 800.0\n 900.0\n 1000.0\n 1100.0\n 2/1\n";
        const auto r = parvati::importScala (s);
        const int16_t want[12] = { 0, 0, -6 };
        check (r.ok && tablesEqual (r.offsets, want), "negative cents accepted (-5.0 -> -6 units)");
    }
    {   // Bare-integer ratio '2' == 2/1 == the octave.
        const char* s = "! bare int\n 12\n 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n 700.0\n 800.0\n"
                        " 900.0\n 1000.0\n 1100.0\n 2\n";
        const auto r = parvati::importScala (s);
        const int16_t want[12] = {};
        check (r.ok && tablesEqual (r.offsets, want), "bare integer '2' parsed as 2/1");
    }
    {   // Clamp: a +200-cent deviation (degree 5 of the fixture sits at
        // 700 cents -> class 5 deviates by +200) clamps to +127 with a warning.
        const char* s = "! clamp\n 12\n 100.0\n 200.0\n 300.0\n 400.0\n 700.0\n 600.0\n 700.0\n 800.0\n"
                        " 900.0\n 1000.0\n 1100.0\n 2/1\n";
        const auto r = parvati::importScala (s);
        check (r.ok && r.offsets[5] == 127, "clamped class saturates at +127");
        bool sawClamp = false;
        for (const auto& w : r.warnings)
            if (w.contains ("clamped"))
                sawClamp = true;
        check (sawClamp, "clamp warning emitted");
    }
    {   // The resolution-cap warning is always present on success.
        const auto r = parvati::importScala (kScl12tet);
        bool saw = false;
        for (const auto& w : r.warnings)
            if (w.contains ("quantized"))
                saw = true;
        check (saw, "always-on resolution-cap warning present");
    }
    {   // Extra pitch lines after the Nth are ignored.
        const char* s = "! extra lines\n 2\n 200.0\n 2/1\n 999.0\n";
        const auto r = parvati::importScala (s);
        check (! r.ok, "N=2 still rejects on mapping size (extra lines irrelevant)");
    }
}
}  // namespace

int main()
{
    std::printf ("parvati_scala_import_test\n");
    testCorpus();
    testMalformed();
    testGrammarEdges();

    if (g_failures == 0)
    {
        std::printf ("ALL PASS\n");
        return 0;
    }
    std::printf ("%d FAILURE(S)\n", g_failures);
    return 1;
}
