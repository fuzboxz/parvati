// .MUL export-fallback test — verifies the hardware-export strategy solver
// (Source/MulExport) and its application in saveMultiFile:
//
//   * Solver unit checks for every strategy (proportional largest-remainder,
//     priority, even split, mono fold, chain split units, as-is).
//   * End-to-end: a slot-extended multi saves as .MUL under each strategy and
//     re-loads (loadMultiFile) with the solved bitmasks + folded modes.
//   * Chain split writes the sibling unit files, each reloading standalone.
//   * .parvati (the Parvati-native format) round-trips the slots UNCHANGED —
//     the native format never goes through the fallback.
//   * Default (AsIs) keeps the legacy behaviour: bitmasks unchanged.

#include <cstdio>
#include <set>
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
#include "ui/MulExportDialog.h"
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

using namespace parvati::mul_export;

// The standard over-capacity scenario used by the end-to-end sections:
// part 0 requests 10 (2 cards), part 1 requests 8 (2 cards), part 2 requests
// 6 (2 cards) => 24 requested vs 6 cards.
Setup makeOverSetup()
{
    Setup s;
    for (int p = 0; p < 3; ++p)
    {
        s.cards[(size_t) p] = 2;
        s.active[(size_t) p] = true;
        s.polyMode[(size_t) p] = 1;   // POLY
    }
    s.requested = { 10, 8, 6, 0, 0, 0 };
    return s;
}

// Configure a live processor into the same over-capacity state.
void setupProcessor (ParvatiAudioProcessor& proc)
{
    proc.prepareToPlay (48000.0, 256);
    renderIdle (proc, 2);
    SynthEngine& e = proc.getEngine();
    e.setPartVoiceAllocation (0, 0b000011);
    e.setPartVoiceAllocation (1, 0b001100);
    e.setPartVoiceAllocation (2, 0b110000);
    e.setPartVoiceSlots (0, 10);
    e.setPartVoiceSlots (1, 8);
    e.setPartVoiceSlots (2, 6);
    renderIdle (proc, 2);
}

juce::File tempDir()
{
    const auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("parvati_export_fallback_test");
    d.createDirectory();
    return d;
}
}  // namespace

int main()
{
    std::printf ("EXPORT FALLBACK TEST\n");

    // ---- [a] needsFallback ----
    {
        std::printf ("\n[a] needsFallback\n");
        const auto s = makeOverSetup();
        check (needsFallback (s), "over-capacity setup needs a fallback");
        Setup fits = s;
        fits.requested = { 2, 2, 2, 0, 0, 0 };
        check (! needsFallback (fits), "within-cards setup does not");
        Setup autoSlots = s;
        autoSlots.requested = { 2, 2, 2, 0, 0, 0 };
        check (! needsFallback (autoSlots), "AUTO-equivalent (requested == cards) does not");
    }

    // ---- [b] Proportional: largest-remainder split of the 6 cards ----
    {
        std::printf ("\n[b] Proportional\n");
        const auto sol = solve (makeOverSetup(), Strategy::Proportional);
        // 6 * 10/24 = 2.50, 6 * 8/24 = 2.00, 6 * 6/24 = 1.50
        // base = {2,2,1}, leftover 1 -> largest remainder (part 0) => {3,2,1}
        check (popcount8 (sol.masks[0]) == 3 && popcount8 (sol.masks[1]) == 2 && popcount8 (sol.masks[2]) == 1,
               "largest-remainder split 10/8/6 -> 3/2/1 cards");
        check (! sol.polyOverridden[0] && ! sol.polyOverridden[1] && ! sol.polyOverridden[2],
               "no mode rewrites under Proportional");
        // Contiguity: part0 = bits 0-2, part1 = bits 3-4, part2 = bit 5
        check (sol.masks[0] == 0b000111 && sol.masks[1] == 0b011000 && sol.masks[2] == 0b100000,
               "masks are contiguous in part order");
        // Many small parts: 6 actives requesting 3 each -> 1 card each.
        Setup six;
        for (int p = 0; p < 6; ++p) { six.cards[(size_t) p] = 1; six.active[(size_t) p] = true; }
        six.requested = { 3, 3, 3, 3, 3, 3 };
        const auto sol6 = solve (six, Strategy::Proportional);
        bool allOne = true;
        for (int p = 0; p < 6; ++p) allOne = allOne && popcount8 (sol6.masks[(size_t) p]) == 1;
        check (allOne, "six equal requests -> one card each (min-1 guarantee)");
    }

    // ---- [c] Priority: first-wins ----
    {
        std::printf ("\n[c] Priority\n");
        const auto sol = solve (makeOverSetup(), Strategy::Priority);
        // part0 gets min(10, 6) = 6; parts 1-2 starve.
        check (popcount8 (sol.masks[0]) == 6 && popcount8 (sol.masks[1]) == 0 && popcount8 (sol.masks[2]) == 0,
               "priority serves part 0 fully, starves the rest");
    }

    // ---- [d] EvenSplit ----
    {
        std::printf ("\n[d] EvenSplit\n");
        const auto sol = solve (makeOverSetup(), Strategy::EvenSplit);
        // share = 2 each (6/3), no caps hit (all requests > 2) -> {2,2,2}
        check (popcount8 (sol.masks[0]) == 2 && popcount8 (sol.masks[1]) == 2 && popcount8 (sol.masks[2]) == 2,
               "even split of 3 actives -> 2 cards each");
        // A small-request part caps and frees its surplus to the others.
        Setup mix = makeOverSetup();
        mix.requested = { 1, 8, 8, 0, 0, 0 };
        const auto solM = solve (mix, Strategy::EvenSplit);
        // share 2 -> part0 capped at 1; leftover 1 redistributed -> {1,3,2} or {1,2,3}
        check (popcount8 (solM.masks[0]) == 1 && popcount8 (solM.masks[1]) + popcount8 (solM.masks[2]) == 5,
               "capped small request redistributes its surplus");
    }

    // ---- [e] MonoFold: proportional + constrained parts fold to MONO ----
    {
        std::printf ("\n[e] MonoFold\n");
        const auto sol = solve (makeOverSetup(), Strategy::MonoFold);
        bool allFolded = true;
        for (int p = 0; p < 3; ++p)
            allFolded = allFolded && sol.polyOverridden[(size_t) p] && sol.polyMode[(size_t) p] == 0;
        check (allFolded, "all constrained parts fold to MONO");
    }

    // ---- [f] ChainSplit: units of <= 6 cards, CHAIN heads ----
    {
        std::printf ("\n[f] ChainSplit\n");
        const auto units = solveChain (makeOverSetup());
        check (units.size() == 4, "24 requested cards -> 4 units (6+6+6+6)");
        // Segments per unit: unit0 {6,0,0} unit1 {4,2,0} unit2 {0,4,2} unit3 {0,0,4}
        check (popcount8 (units[0].masks[0]) == 6, "unit 0: part 0 head = 6 cards");
        check (units[0].polyMode[0] == 4 && units[0].polyOverridden[0], "unit 0: part 0 head is CHAIN");
        check (popcount8 (units[1].masks[0]) == 4 && popcount8 (units[1].masks[1]) == 2,
               "unit 1: part 0 continues (4) + part 1 starts (2)");
        check (units[1].polyMode[0] == 1 && ! units[1].polyOverridden[0],
               "unit 1: part 0 final segment (6+4=10) keeps POLY");
        check (units[1].polyMode[1] == 4 && units[1].polyOverridden[1],
               "unit 1: part 1 non-final segment is CHAIN");
        check (units[3].polyMode[2] == 1 && ! units[3].polyOverridden[2],
               "final segment keeps its original mode (POLY)");
        int totalSegs = 0;
        for (const auto& u : units)
            for (int p = 0; p < kParts; ++p)
                totalSegs += popcount8 (u.masks[(size_t) p]);
        check (totalSegs == 24, "segments sum to the full 24 requested");
    }

    // ---- [g] AsIs echoes the setup's own cards ----
    {
        std::printf ("\n[g] AsIs\n");
        const auto sol = solve (makeOverSetup(), Strategy::AsIs);
        check (sol.masks[0] == 0b000011 && sol.masks[1] == 0b001100 && sol.masks[2] == 0b110000,
               "AsIs rebuilds the engine's contiguous bitmasks unchanged");
    }

    // ---- [g2] preview + summary copy (plain-language output) ----
    {
        std::printf ("\n[g2] preview/summary copy\n");
        const auto setup = makeOverSetup();
        PreviewContext ctx { { "Lead", "Pad", "Bass", "", "", "" } };

        const auto sol = solve (setup, Strategy::Proportional);
        const auto lines = previewLines (setup, sol, 0, &ctx);
        check (lines.size() == 3, "one preview line per active part");
        check (lines[0].find ("Lead: 10 -> 3 voices (Poly)") != std::string::npos,
               "named part + arrow + always-shown mode");

        const auto linesNoNames = previewLines (setup, sol);
        check (linesNoNames[0].find ("Part 1: 10 -> 3 voices (Poly)") != std::string::npos,
               "unnamed part falls back to \"Part N\"");

        const auto solMono = solve (setup, Strategy::MonoFold);
        const auto linesMono = previewLines (setup, solMono, 0, &ctx);
        check (linesMono[0].find ("(Mono, switched)") != std::string::npos,
               "a rewritten mode is marked \"switched\"");

        const auto sumSingle = summarize (setup, Strategy::Proportional);
        check (sumSingle.find ("Fits on one Ambika") != std::string::npos
               && sumSingle.find ("6 of your 24 voices") != std::string::npos,
               "single-file summary states the honest cost");
        const auto sumChain = summarize (setup, Strategy::ChainSplit);
        check (sumChain.find ("4 chained Ambikas") != std::string::npos
               && sumChain.find ("All 24 voices are kept") != std::string::npos,
               "chain summary states the unit count + full fidelity");
    }

    // ---- [h] end-to-end: save + reload each single-file strategy ----
    {
        std::printf ("\n[h] end-to-end save/reload\n");
        const struct { int strat; const char* name; } cases[] = {
            { 0, "AsIs" }, { 1, "Proportional" }, { 2, "Priority" }, { 3, "EvenSplit" }, { 4, "MonoFold" },
        };
        for (const auto& c : cases)
        {
            ParvatiAudioProcessor proc;
            setupProcessor (proc);
            const auto f = tempDir().getChildFile (juce::String (c.name) + ".MUL");
            check (proc.saveMultiFile (f, c.strat), "saveMultiFile with strategy");
            AmbikaMulti m;
            check (parseAmbikaMultiFile (f, m) && m.ok, ".MUL re-parses");

            // Reload through the real path and inspect the engine bitmasks.
            ParvatiAudioProcessor other;
            other.prepareToPlay (48000.0, 256);
            renderIdle (other, 2);
            check (other.loadMultiFile (f), ".MUL reloads via loadMultiFile");
            renderIdle (other, 2);
            const auto& e = other.getEngine();

            const auto setup = proc.getMulExportSetup();
            const auto expect = solve (setup, static_cast<Strategy> (c.strat));
            bool masksOk = true, modesOk = true;
            for (int p = 0; p < kParts; ++p)
            {
                masksOk = masksOk && e.getPartVoiceAllocation (p) == expect.masks[(size_t) p];
                const uint8_t mode = e.getPartPolyphony (p);
                modesOk = modesOk && mode == expect.polyMode[(size_t) p];
            }
            char msg[96];
            std::snprintf (msg, sizeof (msg), "%s: reloaded bitmasks match the solver", c.name);
            check (masksOk, msg);
            std::snprintf (msg, sizeof (msg), "%s: reloaded polyphony modes match", c.name);
            check (modesOk, msg);
        }
    }

    // ---- [i] ChainSplit end-to-end: sibling unit files ----
    {
        std::printf ("\n[i] ChainSplit end-to-end\n");
        ParvatiAudioProcessor proc;
        setupProcessor (proc);
        const auto f = tempDir().getChildFile ("chain.MUL");
        check (proc.saveMultiFile (f, 5), "chain save succeeds");
        for (int u = 0; u < 4; ++u)
        {
            const auto uf = u == 0 ? f
                : f.getParentDirectory().getChildFile ("chain-" + juce::String (u + 1) + ".MUL");
            AmbikaMulti m;
            char msg[96];
            std::snprintf (msg, sizeof (msg), "unit %d file exists + parses", u);
            check (uf.existsAsFile() && parseAmbikaMultiFile (uf, m) && m.ok, msg);
            // Routing must be IDENTICAL across units (matching is by channel+zone).
            bool routingOk = true;
            for (int p = 0; p < kParts; ++p)
                routingOk = routingOk
                    && m.multiData[(size_t) (p * 4)] == proc.getEngine().getPartChannel (p)
                    && m.multiData[(size_t) (p * 4 + 1)] == proc.getEngine().getPartKeyrangeLow (p)
                    && m.multiData[(size_t) (p * 4 + 2)] == proc.getEngine().getPartKeyrangeHigh (p);
            std::snprintf (msg, sizeof (msg), "unit %d carries identical routing", u);
            check (routingOk, msg);
        }
    }

    // ---- [j] .parvati round-trips slots unchanged (no fallback) ----
    {
        std::printf ("\n[j] .parvati fidelity\n");
        ParvatiAudioProcessor proc;
        setupProcessor (proc);
        const auto yaml = parvati::preset::serializeParvatiMulti (proc);
        check (yaml.contains ("voice_slots: 10") && yaml.contains ("voice_slots: 8") && yaml.contains ("voice_slots: 6"),
               "native multi carries the exact slot counts");
        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (parvati::preset::applyParvatiMulti (other, yaml), "native multi re-applies");
        check (other.getEngine().getPartVoiceSlots (0) == 10
               && other.getEngine().getPartVoiceSlots (1) == 8
               && other.getEngine().getPartVoiceSlots (2) == 6,
               "slots round-trip untouched (native format never degrades)");
    }

    // ---- [k] default save (no strategy arg) = legacy AsIs ----
    {
        std::printf ("\n[k] default legacy behaviour\n");
        ParvatiAudioProcessor proc;
        setupProcessor (proc);
        const auto f = tempDir().getChildFile ("legacy.MUL");
        check (proc.saveMultiFile (f), "saveMultiFile(file) compiles + saves");
        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        other.loadMultiFile (f);
        renderIdle (other, 2);
        check (other.getEngine().getPartVoiceAllocation (0) == 0b000011
               && other.getEngine().getPartVoiceAllocation (1) == 0b001100,
               "default = engine bitmasks unchanged (pre-extension behaviour)");
    }

    // ---- [l] dialog constructs + refreshes headlessly ----
    {
        std::printf ("\n[l] dialog wiring\n");
        juce::ScopedJuceInitialiser_GUI juceInit;
        int result = -2;   // sentinel: "callback never fired"
        MulExportDialog dlg (makeOverSetup(), { "Lead", "Pad", "Bass", "", "", "" },
                             [&result] (int r) { result = r; });
        check (true, "dialog constructs with names + over-capacity setup");
        // Default selection = item 1 = Proportional.
        dlg.refreshPreviewPublic();
        check (true, "default preview renders (no crash)");
    }

    tempDir().deleteRecursively();
    std::printf ("\nEXPORT FALLBACK TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
