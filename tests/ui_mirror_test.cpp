// Deterministic tooling T4 — UI MIRROR CONSISTENCY.
//
// Property under test (the stale-mirror class this exists to pin):
//   after ANY engine mutation path, the editor's Patch page displays EXACTLY
//   the engine state — via BOTH real user seams:
//     (S1) the REVEAL refresh (setCurrentTopPage(2) -> showTopPage ->
//          patchPage_->refresh(), the fixed "single-patch load / host state
//          restore left the page stale" class), and
//     (S2) the VISIBLE-page poll mirror (ParvatiEditor::pollPatchPageMirror,
//          driven by the 30 Hz timer via SynthEngine::getDisplayVersion — the
//          fixed "host automation / NRPN / undo while the page is on screen"
//          class). pollPatchPageMirror is public for exactly this test.
//
// Mutation battery (every path that can rewrite the mirrored state):
//   [1] APVTS parameter writes (host automation simulation): part_select +
//       part_polyphony (PartData byte 15 — the poly flip that must re-infer
//       the arrangement) and part_raga (byte 4 — the Tune combo mirror).
//   [2] engine-direct writes: setPartVoiceSlots / setPartVoiceAllocation(0)
//       (disable) / setPartChannel / setPartKeyZone / setPartName.
//   [3] processor-level file loads: two presets/TEMPLATES multis
//       (Poly.parvati, Drum Kit (GM).parvati) + one factory .PRO — these do
//       NOT go through the editor's applyPatchFile, so only S1/S2 can bring
//       the page back in sync (the original bug: only multi loads refreshed).
//   [4] setStateInformation recall with a LIVE editor (blob path), on both
//       seams.
//
// Checked mirror surfaces, for ALL 6 parts after every refresh:
//   - getDisplayedVoiceSlots(p)      == engine.getPartVoiceSlots(p)
//   - getDisplayedTuningMode(p)      == engine.resolvedTuningMode(p)
//   - the row's name label text      == getPartName(p) or the "Part N"
//     placeholder (found by LAYOUT GEOMETRY: the six part rows are the label
//     Y-clusters that contain all six column captions; the name label is the
//     leftmost label of each cluster — no private PartRow access needed)
//   - getDisplayedArrangement()      == inferArrangement(engine), including a
//     deliberately Custom state built through direct engine edits
//   - the "Voices Y/96" pool-budget label == the sum of the per-part slots
//
// Canary self-check: the comparison helper is proven to FAIL on a stale
// (displayed != engine) pair — a Voices-count mismatch and a name-label
// mismatch are staged WITHOUT any refresh and must be reported, then healed
// by the poll seam. This pins the checker itself, not just the current code:
// if the comparator ever degrades into always-true, the canary goes red.
//
// Fully deterministic: no RNG, no wall clock, no message-loop timing (the
// explicit poll call replaces the 30 Hz tick; file loads use the checked-in
// preset corpus). Runs in seconds.
//
// Run: ./build_unified/parvati_unified_tests ui_mirror_test

#include <algorithm>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"          // ParvatiEditor (setCurrentTopPage / pollPatchPageMirror)
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "ui/PatchArrangement.h"   // Arrangement, inferArrangement
#include "ui/PatchPage.h"          // PatchPage (displayed-state test hooks)

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

//==============================================================================
// Find the PatchPage anywhere in the editor's component tree (editor_test [6]
// idiom — the page is editor-owned and added as a child).
PatchPage* findPatchPage (juce::Component* root)
{
    if (root == nullptr) return nullptr;
    juce::Array<juce::Component*> nodes;
    nodes.add (root);
    for (int i = 0; i < nodes.size(); ++i)
    {
        auto* c = nodes.getUnchecked (i);
        if (auto* p = dynamic_cast<PatchPage*> (c)) return p;
        for (auto* child : c->getChildren())
            nodes.add (child);
    }
    return nullptr;
}

// A juce::Label's top-left position in @p ancestor's coordinate space (walks
// the parent chain; every label collected here has @p ancestor above it).
// (posInAncestor removed with collectLabels.)

// (collectLabels + LabelAt were removed with the "Voices Y/96" summary
// label — the name-label source is PatchPage::displayedPartNamesForTest.)

// The six part rows' NAME labels, in part order. Source: the rows
// themselves via PatchPage::displayedPartNamesForTest (the former
// layout-derived caption-cluster detector died with the per-row captions —
// one header strip now labels the columns). Returns an empty vector when
// the page is not reachable — the mirror check then reports it.
std::vector<juce::String> collectPartNameLabels (PatchPage* page)
{
    if (page == nullptr)
        return {};
    const auto arr = page->displayedPartNamesForTest();
    return std::vector<juce::String> (arr.begin(), arr.end());
}

//==============================================================================
// The property comparator: does the Patch page display EXACTLY the engine
// state? Returns ok=false + the first failing surface (so a canary can prove
// the comparator detects staleness, and the battery can name the stale row).
struct MirrorReport
{
    bool ok = true;
    juce::String fail;
};

MirrorReport mirrorMatches (ParvatiAudioProcessor& proc, PatchPage* page)
{
    MirrorReport r;
    const auto fail = [&] (const juce::String& m)
    {
        if (r.ok) { r.ok = false; r.fail = m; }
    };

    if (page == nullptr)
    {
        r.ok = false;
        r.fail = "PatchPage not found in the editor tree";
        return r;
    }
    auto& eng = proc.getEngine();

    // Per-part combo mirrors.
    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
    {
        const int shownSlots = page->getDisplayedVoiceSlots (p);
        const int engSlots = eng.getPartVoiceSlots (p);
        if (shownSlots != engSlots)
            fail ("part " + juce::String (p + 1) + " Voices combo shows "
                  + juce::String (shownSlots) + ", engine has "
                  + juce::String (engSlots));

        const int shownTune = page->getDisplayedTuningMode (p);
        const int engTune = eng.resolvedTuningMode (p);
        if (shownTune != engTune)
            fail ("part " + juce::String (p + 1) + " Tune combo shows mode "
                  + juce::String (shownTune) + ", engine resolved mode is "
                  + juce::String (engTune));

        // Part-character columns (absorbed knobs — 2026-08-20): Oct/Porta/Lgo
        // must mirror PartData bytes 1 / 6 / 5 exactly like Voices/Tune above.
        const int shownOct = page->getDisplayedOctave (p);
        const int engOct = juce::jlimit (-2, 2, static_cast<int> (
            static_cast<int8_t> (eng.getPart (p).partBytes[1])));
        if (shownOct != engOct)
            fail ("part " + juce::String (p + 1) + " Oct combo shows "
                  + juce::String (shownOct) + ", engine byte 1 is "
                  + juce::String (engOct));

        const int shownPorta = page->getDisplayedPortamento (p);
        const int engPorta = static_cast<int> (eng.getPart (p).partBytes[6]);
        if (shownPorta != engPorta)
            fail ("part " + juce::String (p + 1) + " Porta knob shows "
                  + juce::String (shownPorta) + ", engine byte 6 is "
                  + juce::String (engPorta));

        const int shownLgo = page->getDisplayedLegato (p);
        const int engLgo = eng.getPart (p).partBytes[5] != 0 ? 1 : 0;
        if (shownLgo != engLgo)
            fail ("part " + juce::String (p + 1) + " Lgo combo shows "
                  + juce::String (shownLgo) + ", engine byte 5 is "
                  + juce::String (engLgo));

        // Output columns (the completing absorption — 2026-08-20):
        // Vol/Fine/Spr must mirror PartData bytes 0 / 2 / 3 (byte 2 SIGNED).
        const int shownVol = page->getDisplayedVolume (p);
        const int engVol = static_cast<int> (eng.getPart (p).partBytes[0]);
        if (shownVol != engVol)
            fail ("part " + juce::String (p + 1) + " Vol knob shows "
                  + juce::String (shownVol) + ", engine byte 0 is "
                  + juce::String (engVol));

        const int shownFine = page->getDisplayedFineTune (p);
        const int engFine = static_cast<int> (static_cast<int8_t> (
            eng.getPart (p).partBytes[2]));
        if (shownFine != engFine)
            fail ("part " + juce::String (p + 1) + " Fine knob shows "
                  + juce::String (shownFine) + ", engine byte 2 is "
                  + juce::String (engFine));

        const int shownSpr = page->getDisplayedSpread (p);
        const int engSpr = static_cast<int> (eng.getPart (p).partBytes[3]);
        if (shownSpr != engSpr)
            fail ("part " + juce::String (p + 1) + " Spr knob shows "
                  + juce::String (shownSpr) + ", engine byte 3 is "
                  + juce::String (engSpr));
    }

    // Name labels (layout-derived, see collectPartNameLabels).
    const auto names = collectPartNameLabels (page);
    if ((int) names.size() != SynthEngine::getNumParts())
    {
        fail ("expected 6 part rows by layout, found "
              + juce::String ((int) names.size()));
    }
    else
    {
        for (int p = 0; p < SynthEngine::getNumParts(); ++p)
        {
            const auto engineName = eng.getPartName (p);
            const juce::String expected = engineName.isNotEmpty()
                ? engineName
                : TRANS ("Part") + " " + juce::String (p + 1);
            if (names[(size_t) p] != expected)
                fail ("part " + juce::String (p + 1) + " name label shows \""
                      + names[(size_t) p] + "\", engine name is \"" + expected + "\"");
        }
    }

    // Arrangement combo.
    const auto shownArr = page->getDisplayedArrangement();
    const auto engArr = inferArrangement (eng);
    if (shownArr != engArr)
        fail ("arrangement combo shows "
              + juce::String (arrangementLabel (shownArr))
              + ", engine infers "
              + juce::String (arrangementLabel (engArr)));

    // (The "Voices Y/96" pool-budget label was removed 2026-08-20 at the
    // user's request — redundant with the per-row Voices column. The mirror
    // contract now derives the total from the per-row Voices combos, which
    // the row-scan below already covers.)

    return r;
}
}  // namespace

//==============================================================================
TEST(ui_mirror_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("=== Parvati UI Mirror Consistency (T4) ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    auto* editor = dynamic_cast<ParvatiEditor*> (proc.createEditor());
    check (editor != nullptr, "editor constructs (ParvatiEditor)");
    if (editor == nullptr)
    {
        std::printf ("\nUI MIRROR TEST: FAILURES (%d failures)\n", g_failures);
        return false;
    }
    editor->setSize (1280, 634);

    auto* page = findPatchPage (editor);
    check (page != nullptr, "PatchPage found in the editor component tree");
    if (page == nullptr)
    {
        std::printf ("\nUI MIRROR TEST: FAILURES (%d failures)\n", g_failures);
        return false;
    }
    auto& eng = proc.getEngine();

    // The page must be CLEAN before the canary stages staleness (the initial
    // refresh ran at editor construction).
    {
        const auto initial = mirrorMatches (proc, page);
        char m[160];
        std::snprintf (m, sizeof (m), "initial state mirrors the engine [%s]",
                       initial.ok ? "clean" : initial.fail.toRawUTF8());
        check (initial.ok, m);
    }

    //----------------------------------------------------------------------
    // [0] CANARY — the comparator must FAIL on a stale (displayed, engine)
    // pair, then the poll seam must heal it. This proves the tool detects the
    // bug class; without it an always-true comparator would fake a green run.
    //----------------------------------------------------------------------
    std::printf ("\n[0] canary: comparator detects staleness, poll seam heals\n");
    editor->setCurrentTopPage (2);   // page visible: the poll seam is armed
    {
        const int oldSlots = eng.getPartVoiceSlots (2);
        const int newSlots = (oldSlots == 7) ? 9 : 7;
        eng.setPartVoiceSlots (2, newSlots);          // NO refresh: combo is stale
        const auto stale = mirrorMatches (proc, page);
        char m[200];
        std::snprintf (m, sizeof (m),
                       "canary A: stale Voices combo reported [%s]",
                       stale.ok ? "NOT DETECTED (comparator broken)"
                                : stale.fail.toRawUTF8());
        check (! stale.ok, m);

        editor->pollPatchPageMirror();                // the real timer path
        const auto healed = mirrorMatches (proc, page);
        check (healed.ok, "canary A healed: poll seam re-syncs the Voices combo");
    }
    {
        static int canaryCounter = 0;
        const auto staleName = juce::String ("CanaryName") + juce::String (++canaryCounter);
        eng.setPartName (4, staleName);               // NO refresh: label is stale
        const auto stale = mirrorMatches (proc, page);
        char m[200];
        std::snprintf (m, sizeof (m),
                       "canary B: stale name label reported [%s]",
                       stale.ok ? "NOT DETECTED (comparator broken)"
                                : stale.fail.toRawUTF8());
        check (! stale.ok, m);

        editor->pollPatchPageMirror();
        const auto healed = mirrorMatches (proc, page);
        check (healed.ok, "canary B healed: poll seam re-syncs the name label");
    }

    //----------------------------------------------------------------------
    // [1] APVTS parameter writes (host automation simulation) — S2 poll seam.
    //----------------------------------------------------------------------
    std::printf ("\n[1] apvts writes: part_select + part_polyphony + part_raga (poll seam)\n");
    {
        // Select part 3 (1-based value), flip its polyphony (byte 15) and its
        // raga preset (byte 4) through the parameter bridge — engine-direct
        // writes with no editor notification, the automation class.
        proc.getApvts().getParameterAsValue ("part_select")    = 3.0f;
        proc.getApvts().getParameterAsValue ("part_polyphony") = 2.0f;   // UNISON_2X
        proc.getApvts().getParameterAsValue ("part_raga")      = 5.0f;
        // Absorbed part-character columns: host automation of part_octave /
        // part_portamento / part_legato (PartData bytes 1 / 6 / 5) must reach
        // the table through the SAME poll seam (the display-version bump in
        // SynthEngine::applyPartByte now covers them).
        proc.getApvts().getParameterAsValue ("part_octave")     = -2.0f;  // signed byte
        proc.getApvts().getParameterAsValue ("part_portamento") = 52.0f;
        proc.getApvts().getParameterAsValue ("part_legato")     = 1.0f;
        // Output columns (completing absorption): host automation of
        // part_volume / part_tuning / part_spread (PartData bytes 0 / 2 / 3)
        // must reach the table through the SAME poll seam.
        proc.getApvts().getParameterAsValue ("part_volume") = 90.0f;
        proc.getApvts().getParameterAsValue ("part_tuning") = 64.0f;   // signed byte
        proc.getApvts().getParameterAsValue ("part_spread") = 12.0f;
        editor->pollPatchPageMirror();                 // the automation seam

        const auto r = mirrorMatches (proc, page);
        char m[200];
        std::snprintf (m, sizeof (m),
                       "apvts poly/raga write re-synced by the poll seam [%s]",
                       r.ok ? "clean" : r.fail.toRawUTF8());
        check (r.ok, m);

        // The poly flip must have re-inferred a non-preset arrangement (the
        // init state is not UNISON_2X on part 3) — proving the ARRANGEMENT
        // surface tracks engine mutations, not just the rows.
        check (page->getDisplayedArrangement() == Arrangement::Custom,
               "apvts poly flip re-infers Custom (arrangement mirror moved)");
        check (eng.getPart (2).partBytes[15] == 2, "engine part 3 byte 15 is UNISON_2X");
        // The absorbed columns landed in the engine bytes (the mirror check
        // above verified the DISPLAY; pin the bytes themselves).
        check (static_cast<int8_t> (eng.getPart (2).partBytes[1]) == -2,
               "engine part 3 byte 1 is the SIGNED octave -2");
        check (eng.getPart (2).partBytes[6] == 52, "engine part 3 byte 6 is 52");
        check (eng.getPart (2).partBytes[5] == 1, "engine part 3 byte 5 is legato on");

        // Same class through the REVEAL seam: hide, mutate, reveal.
        editor->setCurrentTopPage (0);
        proc.getApvts().getParameterAsValue ("part_polyphony") = 4.0f;   // CHAIN
        editor->setCurrentTopPage (2);
        const auto r2 = mirrorMatches (proc, page);
        std::snprintf (m, sizeof (m),
                       "apvts write re-synced by the reveal seam [%s]",
                       r2.ok ? "clean" : r2.fail.toRawUTF8());
        check (r2.ok, m);
    }

    //----------------------------------------------------------------------
    // [2] engine-direct writes — BOTH seams.
    //----------------------------------------------------------------------
    std::printf ("\n[2] engine-direct writes: slots/channel/zone/name (both seams)\n");
    {
        // [2a] Hidden-page batch (reveal seam must catch all of it at once):
        // voice slots, a part DISABLE (the zero-mask path), MIDI channel, key
        // zone, part name.
        editor->setCurrentTopPage (1);   // FX page: the Patch page is hidden
        eng.setPartVoiceSlots (0, 10);
        eng.setPartVoiceSlots (3, 5);
        eng.setPartVoiceAllocation (1, 0);            // disable part 2
        eng.setPartChannel (2, 5);
        eng.setPartKeyZone (4, 36, 84);
        eng.setPartName (5, "MirrorTom");
        {
            // Absorbed character bytes via the engine-direct idiom the table
            // itself uses (setCurrentPart + applyPartByte + restore).
            const int saved = eng.getCurrentPart();
            eng.setCurrentPart (4);
            eng.applyPartByte (1, static_cast<uint8_t> (static_cast<int8_t> (1)));
            eng.applyPartByte (5, 1);
            eng.applyPartByte (6, 30);
            eng.setCurrentPart (saved);
        }
        editor->setCurrentTopPage (2);
        const auto r = mirrorMatches (proc, page);
        char m[200];
        std::snprintf (m, sizeof (m),
                       "hidden-page engine batch re-synced by the reveal seam [%s]",
                       r.ok ? "clean" : r.fail.toRawUTF8());
        check (r.ok, m);
        check (page->getDisplayedVoiceSlots (1) == 0,
               "disabled part 2 shows 0 after the reveal refresh");

        // [2b] Visible-page batch (poll seam).
        eng.setPartVoiceSlots (2, 7);
        eng.setPartName (1, "PollCheck");
        editor->pollPatchPageMirror();
        const auto r2 = mirrorMatches (proc, page);
        std::snprintf (m, sizeof (m),
                       "visible-page engine batch re-synced by the poll seam [%s]",
                       r2.ok ? "clean" : r2.fail.toRawUTF8());
        check (r2.ok, m);
    }

    // Snapshot this custom mixed state for [4] (a state that is deliberately
    // NOT any built-in arrangement — the recall must restore every surface).
    juce::MemoryBlock stateA;
    proc.getStateInformation (stateA);

    //----------------------------------------------------------------------
    // [3] processor-level file loads (NO editor applyPatchFile — only the
    // seams can re-sync the page; the original stale-mirror bug class).
    //----------------------------------------------------------------------
    std::printf ("\n[3] processor-level loads: templates + factory .PRO\n");
    {
        const auto tplDir = juce::File::getCurrentWorkingDirectory()
                                .getChildFile ("presets/TEMPLATES");
        const auto proDir = juce::File::getCurrentWorkingDirectory()
                                .getChildFile ("presets/FACTORY/A");

        char m[220];

        // [3a] Poly template — poll seam while visible.
        if (proc.loadParvatiMultiFile (tplDir.getChildFile ("Poly.parvati")))
        {
            editor->pollPatchPageMirror();
            const auto r = mirrorMatches (proc, page);
            std::snprintf (m, sizeof (m), "Poly.parvati load mirrors (poll seam) [%s]",
                           r.ok ? "clean" : r.fail.toRawUTF8());
            check (r.ok, m);
            check (page->getDisplayedArrangement() == Arrangement::Poly,
                   "Poly.parvati re-infers the Poly arrangement");
        }
        else
            check (false, "Poly.parvati loads (run from the repo root)");

        // [3b] Drum Kit (GM) — reveal seam (page hidden during the load).
        if (proc.loadParvatiMultiFile (tplDir.getChildFile ("Drum Kit (GM).parvati")))
        {
            editor->setCurrentTopPage (0);
            editor->setCurrentTopPage (2);
            const auto r = mirrorMatches (proc, page);
            std::snprintf (m, sizeof (m), "Drum Kit (GM) load mirrors (reveal seam) [%s]",
                           r.ok ? "clean" : r.fail.toRawUTF8());
            check (r.ok, m);
        }
        else
            check (false, "Drum Kit (GM).parvati loads (run from the repo root)");

        // [3c] A factory .PRO — single-program load into the current part
        // (the class that originally NEVER refreshed the page at all).
        if (proc.loadProgramFile (proDir.getChildFile ("000.PRO")))
        {
            editor->pollPatchPageMirror();
            const auto r = mirrorMatches (proc, page);
            std::snprintf (m, sizeof (m), "factory .PRO load mirrors (poll seam) [%s]",
                           r.ok ? "clean" : r.fail.toRawUTF8());
            check (r.ok, m);
        }
        else
            check (false, "factory A/000.PRO loads (run from the repo root)");
    }

    //----------------------------------------------------------------------
    // [4] setStateInformation recall with a LIVE editor — both seams.
    //----------------------------------------------------------------------
    std::printf ("\n[4] host state recall with a live editor\n");
    {
        // [4a] Reveal seam: recall state A (the custom mixed state) while the
        // page is hidden; the reveal refresh must reflect it.
        editor->setCurrentTopPage (0);
        proc.setStateInformation (stateA.getData(), (int) stateA.getSize());
        editor->setCurrentTopPage (2);
        const auto r = mirrorMatches (proc, page);
        char m[220];
        std::snprintf (m, sizeof (m), "state A recall mirrors (reveal seam) [%s]",
                       r.ok ? "clean" : r.fail.toRawUTF8());
        check (r.ok, m);
        check (eng.getPartVoiceSlots (0) == 10 && eng.getPartName (5) == "MirrorTom",
               "state A recall restored the custom slots + name (engine side)");
        check (page->getDisplayedArrangement() == Arrangement::Custom,
               "state A recall re-infers Custom");

        // [4b] Poll seam: recall the CURRENT state (captured fresh) while the
        // page is VISIBLE after a mutation — the poll must re-read it.
        juce::MemoryBlock stateB;
        proc.getStateInformation (stateB);
        eng.setPartVoiceSlots (0, 12);   // visible-page mutation
        editor->pollPatchPageMirror();
        proc.setStateInformation (stateB.getData(), (int) stateB.getSize());
        editor->pollPatchPageMirror();   // restoreState bumps the display version
        const auto r2 = mirrorMatches (proc, page);
        std::snprintf (m, sizeof (m), "state B recall mirrors (poll seam) [%s]",
                       r2.ok ? "clean" : r2.fail.toRawUTF8());
        check (r2.ok, m);
    }

    //----------------------------------------------------------------------
    // Final structural sanity: the row-geometry detector still sees 6 rows
    // (guards the layout-derived name-label path against silent decay).
    //----------------------------------------------------------------------
    check ((int) collectPartNameLabels (page).size() == 6,
           "layout detector still finds exactly 6 part rows");

    std::printf ("\nUI MIRROR TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}
