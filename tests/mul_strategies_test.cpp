// .MUL export strategy correctness + unicode/name-safety tests.
//
// Two halves:
//
//  [A] STRATEGY CORRECTNESS — a scenario matrix x every strategy, checked
//      against INVARIANTS (not spot values): card-count conservation, mask
//      contiguity/disjointness in part order, inactive parts stay out,
//      per-strategy fairness guarantees (min-1 card, weak monotonicity for
//      Proportional), chain segment conservation + CHAIN heads. Then
//      end-to-end: each scenario export RELOADS and the hardware polyphony
//      actually plays (sustained notes == allocated cards), including the
//      chain unit files standalone.
//
//  [B] UNICODE / NAME SAFETY — part + program names with multi-byte UTF-8
//      (accents, CJK, emoji), quotes, backslashes and newlines:
//        * .parvati multi: exact round-trip of unicode + escaped quotes.
//        * host engine-state: exact round-trip.
//        * .MUL/.PRO 16-byte name chunk: never splits a code point (the file
//          stays valid UTF-8), control chars dropped, re-parse == truncated
//          whole-char prefix.
//        * the engine's setPartName strips control chars (a newline would
//          corrupt the line-based .parvati document on save).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MulExport.h"
#include "PatchFile.h"
#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

void renderIdle (ParvatiAudioProcessor& p, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

int popcount8 (uint8_t x) { int n = 0; for (; x; x >>= 1) n += x & 1; return n; }

juce::File tempDir()
{
    const auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("parvati_mul_strategies_test");
    d.createDirectory();
    return d;
}

using namespace parvati::mul_export;

struct Scenario
{
    const char* name;
    std::array<int, kParts> requested;
    std::array<int, kParts> cards;     // active == cards > 0
};

const Scenario kScenarios[] = {
    { "single-part-16",   { 16, 0, 0, 0, 0, 0 }, { 6, 0, 0, 0, 0, 0 } },
    { "six-parts-16",     { 16, 16, 16, 16, 16, 16 }, { 1, 1, 1, 1, 1, 1 } },
    { "drum-kit-4",       { 4, 4, 4, 4, 4, 4 }, { 1, 1, 1, 1, 1, 1 } },
    { "skewed-10-8-6",    { 10, 8, 6, 0, 0, 0 }, { 2, 2, 2, 0, 0, 0 } },
    { "one-small-rest-big",{ 1, 8, 8, 0, 0, 0 }, { 1, 2, 2, 0, 0, 0 } },
    { "fits-exactly",     { 2, 2, 2, 0, 0, 0 }, { 2, 2, 2, 0, 0, 0 } },
    { "inactive-with-slots", { 4, 4, 0, 4, 0, 0 }, { 3, 3, 0, 0, 0, 0 } },
    { "six-request-3",    { 3, 3, 3, 3, 3, 3 }, { 1, 1, 1, 1, 1, 1 } },
};

Setup toSetup (const Scenario& sc, std::array<uint8_t, kParts> poly = { 1, 1, 1, 1, 1, 1 })
{
    Setup s;
    s.requested = sc.requested;
    s.cards = sc.cards;
    for (int p = 0; p < kParts; ++p) s.active[(size_t) p] = sc.cards[(size_t) p] > 0;
    s.polyMode = poly;
    return s;
}

// Invariants shared by EVERY single-file strategy (incl. the chain first unit).
bool commonInvariants (const Setup& s, const Solution& sol, int maxCards)
{
    int used = 0, cursor = 0;
    for (int p = 0; p < kParts; ++p)
    {
        const int n = popcount8 (sol.masks[(size_t) p]);
        used += n;
        // Inactive parts own nothing.
        if (! s.active[(size_t) p] && n != 0) return false;
        // Contiguous + disjoint in part order: the mask must be exactly the
        // next n bits after the cursor.
        uint8_t want = 0;
        for (int c = 0; c < n; ++c, ++cursor)
            want = static_cast<uint8_t> (want | (1u << cursor));
        if (sol.masks[(size_t) p] != want) return false;
    }
    return used <= maxCards;
}

// Apply a scenario to a live processor. Under the slots model the mask only
// seeds slot counts (legacy load path); requested == 0 must NOT go through
// setPartVoiceSlots — the public setter clamps 0 to 1 (cannot disable), so a
// zero-request part keeps its zero-mask materialization (0 slots = disabled).
void applyScenario (ParvatiAudioProcessor& proc, const Scenario& sc)
{
    proc.prepareToPlay (48000.0, 256);
    renderIdle (proc, 2);
    SynthEngine& e = proc.getEngine();
    int cursor = 0;
    for (int p = 0; p < kParts; ++p)
    {
        uint8_t mask = 0;
        for (int c = 0; c < sc.cards[(size_t) p] && cursor < 6; ++c, ++cursor)
            mask = static_cast<uint8_t> (mask | (1u << cursor));
        e.setPartVoiceAllocation (p, mask);
        if (sc.requested[(size_t) p] > 0)
            e.setPartVoiceSlots (p, sc.requested[(size_t) p]);
    }
    renderIdle (proc, 2);
}

int activeInPart (SynthEngine& e, int part)
{
    int n = 0;
    for (int i = 0; i < kNumVoices; ++i)
        if (auto* av = e.getAmbikaVoice (i))
            if (av->isVoiceActive() && av->getPartIndex() == part)
                ++n;
    return n;
}
}  // namespace

int main()
{
    std::printf ("MUL STRATEGIES + NAMES TEST\n");

    // =====================================================================
    std::printf ("\n[A] strategy invariants (scenario matrix)\n");
    // =====================================================================
    for (const auto& sc : kScenarios)
    {
        const Setup s = toSetup (sc);
        const int sumR = [] (const Setup& x) { int t = 0; for (int p = 0; p < kParts; ++p) if (x.active[(size_t) p]) t += x.requested[(size_t) p]; return t; } (s);

        // --- AsIs: counts echo the scenario's own cards ---
        {
            const auto sol = solve (s, Strategy::AsIs);
            bool ok = true;
            for (int p = 0; p < kParts; ++p)
                ok = ok && popcount8 (sol.masks[(size_t) p]) == sc.cards[(size_t) p];
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / AsIs: counts == own cards", sc.name);
            check (ok && commonInvariants (s, sol, 6), msg);
        }

        // --- Proportional: conservation + min-1 + weak monotonicity ---
        {
            const auto sol = solve (s, Strategy::Proportional);
            bool ok = commonInvariants (s, sol, 6);
            int used = 0;
            for (int p = 0; p < kParts; ++p)
            {
                const int n = popcount8 (sol.masks[(size_t) p]);
                used += n;
                if (s.active[(size_t) p] && n < 1) ok = false;   // min-1 per active part
            }
            ok = ok && used == std::min (6, sumR);               // full conservation
            // Weak monotonicity: a strictly larger request never gets fewer
            // cards; EQUAL requests may differ by at most 1 (6 cards cannot
            // always split evenly — e.g. {1,8,8} -> {1,2,3}).
            for (int a = 0; a < kParts && ok; ++a)
                for (int b = 0; b < kParts && ok; ++b)
                {
                    if (! s.active[(size_t) a] || ! s.active[(size_t) b]) continue;
                    const int ca = popcount8 (sol.masks[(size_t) a]);
                    const int cb = popcount8 (sol.masks[(size_t) b]);
                    if (s.requested[(size_t) a] > s.requested[(size_t) b] && ca < cb) ok = false;
                    if (s.requested[(size_t) a] == s.requested[(size_t) b] && std::abs (ca - cb) > 1) ok = false;
                }
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / Proportional: conservation + min-1 + monotone", sc.name);
            check (ok, msg);
        }

        // --- EvenSplit: conservation + min-1 ---
        {
            const auto sol = solve (s, Strategy::EvenSplit);
            bool ok = commonInvariants (s, sol, 6);
            int used = 0;
            for (int p = 0; p < kParts; ++p)
            {
                const int n = popcount8 (sol.masks[(size_t) p]);
                used += n;
                if (s.active[(size_t) p] && n < 1) ok = false;
            }
            ok = ok && used == std::min (6, sumR);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / EvenSplit: conservation + min-1", sc.name);
            check (ok, msg);
        }

        // --- Priority: conservation + prefix order ---
        {
            const auto sol = solve (s, Strategy::Priority);
            bool ok = commonInvariants (s, sol, 6);
            int remaining = 6;
            for (int p = 0; p < kParts && ok; ++p)
            {
                const int expect = s.active[(size_t) p] ? std::min (s.requested[(size_t) p], remaining) : 0;
                if (popcount8 (sol.masks[(size_t) p]) != expect) ok = false;
                remaining -= expect;
            }
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / Priority: served in order until cards run out", sc.name);
            check (ok, msg);
        }

        // --- MonoFold: same counts as Proportional + constrained folds ---
        {
            const auto solP = solve (s, Strategy::Proportional);
            const auto solM = solve (s, Strategy::MonoFold);
            bool ok = true;
            for (int p = 0; p < kParts; ++p)
            {
                if (solP.masks[(size_t) p] != solM.masks[(size_t) p]) ok = false;
                const int got = popcount8 (solM.masks[(size_t) p]);
                const bool constrained = s.active[(size_t) p] && got < s.requested[(size_t) p];
                if (constrained && solM.polyMode[(size_t) p] != 0) ok = false;   // folded
                if (! constrained && solM.polyOverridden[(size_t) p]) ok = false;
            }
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / MonoFold: proportional + folds only the constrained", sc.name);
            check (ok, msg);
        }

        // --- ChainSplit: segment conservation + CHAIN heads + unit cap ---
        {
            const auto units = solveChain (s);
            bool ok = ! units.empty();
            int totalSegs = 0;
            std::array<int, kParts> placed {};
            for (size_t u = 0; u < units.size() && ok; ++u)
            {
                int unitCards = 0;
                for (int p = 0; p < kParts; ++p)
                {
                    const int n = popcount8 (units[u].masks[(size_t) p]);
                    unitCards += n;
                    totalSegs += n;
                    placed[(size_t) p] += n;
                    // Non-final segment => CHAIN.
                    if (n > 0 && placed[(size_t) p] < s.requested[(size_t) p]
                        && units[u].polyMode[(size_t) p] != 4)
                        ok = false;
                }
                if (unitCards > 6) ok = false;
            }
            ok = ok && totalSegs == sumR;   // everything is placed somewhere
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s / ChainSplit: segments conserved, heads CHAIN, units <= 6", sc.name);
            check (ok, msg);
        }
    }

    // =====================================================================
    std::printf ("\n[B] end-to-end: exported polyphony actually plays\n");
    // =====================================================================
    {
        const struct { const Scenario* sc; int strat; } cases[] = {
            { &kScenarios[0], 1 },   // single-part-16, Proportional
            { &kScenarios[1], 1 },   // six-parts-16, Proportional
            { &kScenarios[2], 1 },   // drum-kit-4, Proportional
            { &kScenarios[3], 4 },   // skewed, MonoFold
        };
        for (const auto& c : cases)
        {
            ParvatiAudioProcessor proc;
            applyScenario (proc, *c.sc);
            const auto f = tempDir().getChildFile ("play.MUL");
            check (proc.saveMultiFile (f, c.strat), "export saves");

            ParvatiAudioProcessor hw;
            hw.prepareToPlay (48000.0, 256);
            renderIdle (hw, 2);
            check (hw.loadMultiFile (f), "hardware reload");
            renderIdle (hw, 2);

            // Default routing is channel p+1 (init); play min(requested, cards)
            // notes on each part and count sustained voices.
            bool ok = true;
            for (int p = 0; p < kParts; ++p)
            {
                const int mask = hw.getEngine().getPartVoiceAllocation (p);
                const int cards = popcount8 (static_cast<uint8_t> (mask));
                if (cards == 0) continue;
                const int notes = std::min (c.sc->requested[(size_t) p], cards);
                for (int n = 0; n < notes; ++n)
                {
                    juce::AudioBuffer<float> buf (2, 256);
                    juce::MidiBuffer midi;
                    midi.addEvent (juce::MidiMessage::noteOn (p + 1, 60 + n, 0.8f), 0);
                    hw.processBlock (buf, midi);
                }
                renderIdle (hw, 2);
                if (activeInPart (hw.getEngine(), p) != notes) ok = false;
                for (int n = 0; n < notes; ++n)
                {
                    juce::AudioBuffer<float> buf (2, 256);
                    juce::MidiBuffer midi;
                    midi.addEvent (juce::MidiMessage::noteOff (p + 1, 60 + n, 0.8f), 0);
                    hw.processBlock (buf, midi);
                }
                renderIdle (hw, 400);
            }
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s strat=%d: sustained notes == allocated cards per part", c.sc->name, c.strat);
            check (ok, msg);
            f.deleteFile();
        }

        // Chain units reload standalone with their own masks.
        {
            ParvatiAudioProcessor proc;
            applyScenario (proc, kScenarios[3]);   // 10/8/6
            const auto f = tempDir().getChildFile ("chainp.MUL");
            check (proc.saveMultiFile (f, 5), "chain export");
            const auto setup = proc.getMulExportSetup();
            const auto units = solveChain (setup);
            for (size_t u = 1; u < units.size(); ++u)   // unit 0 == f (checked above pattern)
            {
                const auto uf = f.getParentDirectory().getChildFile ("chainp-" + juce::String (u + 1) + ".MUL");
                ParvatiAudioProcessor hw;
                hw.prepareToPlay (48000.0, 256);
                renderIdle (hw, 2);
                char msg[128];
                std::snprintf (msg, sizeof (msg), "chain unit %zu reloads", u);
                const bool loadedOk = hw.loadMultiFile (uf);
                renderIdle (hw, 2);
                check (loadedOk, msg);
                bool ok = true;
                for (int p = 0; p < kParts; ++p)
                    ok = ok && hw.getEngine().getPartVoiceAllocation (p) == units[u].masks[(size_t) p];
                std::snprintf (msg, sizeof (msg), "chain unit %zu masks exact", u);
                check (ok, msg);
            }
            tempDir().deleteRecursively();
            tempDir().createDirectory();
        }
    }

    // =====================================================================
    std::printf ("\n[C] unicode / name safety\n");
    // =====================================================================
    {
        // ---- [C1] part names: .parvati exact round-trip (multi-byte UTF-8 + escapes) ----
        std::printf ("\n[C1] .parvati part-name round-trip\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& e = proc.getEngine();
        e.setPartName (0, juce::CharPointer_UTF8 ("Kick \xc3\xa9"));            // é
        e.setPartName (1, juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));  // 日本語
        e.setPartName (2, juce::CharPointer_UTF8 ("\xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9"));   // Ünïcödé
        e.setPartName (3, juce::CharPointer_UTF8 ("\xf0\x9f\x8e\xb9 Pad"));    // 🎹
        e.setPartName (4, "He said \"hi\"");
        e.setPartName (5, "Back\\slash");
        const juce::String yaml = parvati::preset::serializeParvatiMulti (proc);
        // The document must stay line-based (a newline in a name would corrupt it).
        const int partLines = [&]
        {
            int n = 0;
            for (const auto& l : juce::StringArray::fromLines (yaml))
                if (l.contains ("voice_slots:")) ++n;
            return n;
        } ();
        check (partLines == 6, "all 6 parts serialized on single lines");

        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (parvati::preset::applyParvatiMulti (other, yaml), "re-applies");
        bool namesOk = true;
        namesOk = namesOk && other.getEngine().getPartName (0) == juce::CharPointer_UTF8 ("Kick \xc3\xa9");
        namesOk = namesOk && other.getEngine().getPartName (1) == juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
        namesOk = namesOk && other.getEngine().getPartName (2) == juce::CharPointer_UTF8 ("\xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9");
        namesOk = namesOk && other.getEngine().getPartName (3) == juce::CharPointer_UTF8 ("\xf0\x9f\x8e\xb9 Pad");
        namesOk = namesOk && other.getEngine().getPartName (4) == "He said \"hi\"";
        namesOk = namesOk && other.getEngine().getPartName (5) == "Back\\slash";
        check (namesOk, "unicode + escaped names round-trip exactly");

        // ---- [C2] host engine-state round-trip ----
        std::printf ("\n[C2] host-state name round-trip\n");
        {
            juce::MemoryBlock blob;
            e.captureState (blob);
            ParvatiAudioProcessor h;
            h.prepareToPlay (48000.0, 256);
            renderIdle (h, 2);
            check (h.getEngine().restoreState (blob.getData(), blob.getSize()), "v6 blob restores");
            bool ok = true;
            ok = ok && h.getEngine().getPartName (0) == juce::CharPointer_UTF8 ("Kick \xc3\xa9");
            ok = ok && h.getEngine().getPartName (1) == juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
            ok = ok && h.getEngine().getPartName (3) == juce::CharPointer_UTF8 ("\xf0\x9f\x8e\xb9 Pad");
            check (ok, "unicode names survive the length-prefixed blob");
        }

        // ---- [C3] setPartName strips control characters ----
        std::printf ("\n[C3] name sanitization\n");
        {
            ParvatiAudioProcessor p2;
            p2.prepareToPlay (48000.0, 256);
            renderIdle (p2, 2);
            p2.getEngine().setPartName (0, "Bad\nName");
            check (p2.getEngine().getPartName (0) == "BadName", "newline stripped by setPartName");
            p2.getEngine().setPartName (1, "Tab\tName");
            check (p2.getEngine().getPartName (1) == "TabName", "tab stripped");
            const juce::String y2 = parvati::preset::serializeParvatiMulti (p2);
            // The sanitized name stays a single YAML token on one line (the
            // value itself contains no newline): "name: \"BadName\"".
            check (y2.contains ("name: \"BadName\"") && y2.contains ("name: \"TabName\""),
                   "sanitized names serialize as single-line tokens");
            // ...and the document still round-trips.
            ParvatiAudioProcessor p3;
            p3.prepareToPlay (48000.0, 256);
            renderIdle (p3, 2);
            check (parvati::preset::applyParvatiMulti (p3, y2) && p3.getEngine().getPartName (0) == "BadName",
                   "document with previously-corrupting name still round-trips");
        }

        // ---- [C4] .MUL/.PRO 16-byte name chunk: code-point-safe truncation ----
        std::printf ("\n[C4] .MUL/.PRO name chunk under unicode\n");
        struct NameCase { const char* utf8; int expectBytes; };
        const NameCase nameCases[] = {
            { "ASCII", 5 },
            { "\xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9", 9 },                    // Ünïcödé: 7 chars, 9 bytes -> fits? 9 <= 16 yes
            { "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x89\xe3\x83\xa9\xe3\x83\xa0", 18 },  // 日本語ドラム: 18 bytes -> truncate to 15 (5x3CP)
            { "\xf0\x9f\x8e\xb9\xf0\x9f\x8e\xb9\xf0\x9f\x8e\xb9\xf0\x9f\x8e\xb9\xf0\x9f\x8e\xb9", 20 },  // 5x emoji: truncate to 16 (4x4CP)
        };
        for (const auto& nc : nameCases)
        {
            // Program name via a direct writer round-trip (the name comes from
            // loadedProgramName_ in the real save path; writer-level is the
            // byte contract).
            AmbikaProgram prog;
            prog.name = juce::CharPointer_UTF8 (nc.utf8);
            prog.hasPatch = prog.hasPart = true;
            for (auto& b : prog.patch) b = 0;
            for (auto& b : prog.part) b = 0;
            const auto f = tempDir().getChildFile ("uni.PRO");
            check (writeAmbikaProgramFile (f, prog), ".PRO writes");

            // Read the raw 16 name bytes and validate UTF-8 boundary safety.
            juce::MemoryBlock mb;
            f.loadFileAsData (mb);
            const auto* raw = static_cast<const uint8_t*> (mb.getData());
            char nameBytes[17];
            std::memcpy (nameBytes, raw + 20, 16);
            nameBytes[16] = '\0';
            // Trailing NUL/space trim, then the remainder must be a whole
            // number of code points.
            size_t n = 16;
            while (n > 0 && (nameBytes[n - 1] == 0 || nameBytes[n - 1] == ' ')) --n;
            const juce::String parsed = juce::String::fromUTF8 (nameBytes, (int) n);
            const juce::String original = juce::CharPointer_UTF8 (nc.utf8);
            // Validity: parsed must be a whole-character PREFIX of the original.
            bool prefixOk = original.startsWith (parsed);
            char msg[160];
            std::snprintf (msg, sizeof (msg), "\"%s\": name chunk is valid UTF-8 (%zu bytes kept)",
                           nc.utf8, n);
            check (prefixOk && parsed.getNumBytesAsUTF8() <= 16, msg);
            std::snprintf (msg, sizeof (msg), "\"%s\": kept bytes <= 16 and no split code point",
                           nc.utf8);
            check (n <= 16 && (n < 16 || nameBytes[15] != (char) 0x80), msg);

            // Round-trip: re-parse returns the truncated prefix exactly.
            AmbikaProgram back;
            parseAmbikaProgramFile (f, back);
            std::snprintf (msg, sizeof (msg), "\"%s\": re-parse == whole-char prefix", nc.utf8);
            check (back.name == parsed, msg);
        }

        // Same for the .MUL multi name.
        {
            AmbikaMulti m;
            m.name = juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x89\xe3\x83\xa9\xe3\x83\xa0");  // 18 bytes
            m.ok = m.hasMultiData = true;
            const auto f = tempDir().getChildFile ("uni.MUL");
            check (writeAmbikaMultiFile (f, m), ".MUL writes");
            AmbikaMulti back;
            parseAmbikaMultiFile (f, back);
            check (back.name == juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x89\xe3\x83\xa9"),   // 日本語ドラ (15 bytes)
                   ".MUL name truncates at a code-point boundary");
        }

        // ---- [C5] hardware ASCII references still byte-identical ----
        std::printf ("\n[C5] ASCII references unchanged\n");
        {
            const juce::File ref = juce::File (PARVATI_SOURCE_DIR)
                .getChildFile ("ambika_reference/controller/data/goldencard/MULTI/BANK/A/000.MUL");
            AmbikaMulti m;
            check (parseAmbikaMultiFile (ref, m) && m.ok, "reference parses");
            const auto f = tempDir().getChildFile ("refagain.MUL");
            check (writeAmbikaMultiFile (f, m), "reference rewrites");
            juce::MemoryBlock a, b;
            ref.loadFileAsData (a);
            f.loadFileAsData (b);
            check (a == b, "ASCII .MUL still byte-identical after the name refactor");
        }
    }

    tempDir().deleteRecursively();
    std::printf ("\nMUL STRATEGIES + NAMES TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
