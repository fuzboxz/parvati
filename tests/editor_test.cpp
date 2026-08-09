// Multitimbral GUI smoke test: the editor builds, a Part selector exists, and
// per-part MIDI-channel editing reaches the engine. Headless (bare create /
// resize / teardown; no real message loop).

#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/ParvatiTheme.h"
#include "ui/PatchPage.h"
#include "ui/PatchArrangement.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    // ---- [1] Editor builds + is an AudioProcessorEditor ----
    std::printf ("[1] Editor construction\n");
    juce::AudioProcessorEditor* editor = proc.createEditor();
    check (editor != nullptr, "createEditor() returns non-null");
    check (dynamic_cast<juce::AudioProcessorEditor*> (editor) != nullptr, "editor is a juce::AudioProcessorEditor");
    if (editor != nullptr)
        editor->setSize (820, 600);

    // ---- [2] Part selector drives the engine's current part ----
    std::printf ("\n[2] Part selector -> engine current part\n");
    proc.getApvts().getParameterAsValue ("part_select") = 2.0f;   // 1-based part 2
    proc.syncAllParamsToEngine();                                  // synchronously apply part_select
    const int curPart = proc.getEngine().getCurrentPart();
    char msg[96];
    std::snprintf (msg, sizeof (msg), "part_select=2 => engine current part is 1 (0-based) [was %d]", curPart);
    check (curPart == 1, msg);

    // ---- [3] Per-part MIDI channel editing round-trips ----
    std::printf ("\n[3] Per-part MIDI channel round-trip (part 1)\n");
    proc.getEngine().setPartMidiChannel (1, 5);
    const int viaGetter = proc.getEngine().getPartChannel (1);
    const int viaStruct = proc.getEngine().getPart (1).midiChannel;
    std::snprintf (msg, sizeof (msg), "setPartMidiChannel(1,5) => getPartChannel==5 [getter=%d]", viaGetter);
    check (viaGetter == 5, msg);
    std::snprintf (msg, sizeof (msg), "Part(1).midiChannel reflects the value [struct=%d]", viaStruct);
    check ((int) viaStruct == 5, msg);

    // ---- [4] Theme category tokens (positional-init alignment guard) ----
    // The 5 factories use POSITIONAL brace init, so a missed/extra/misordered
    // value silently misaligns every later field with no compile error. This
    // guard catches that: every category colour is opaque + pairwise-distinct,
    // isDark is correct per theme, and the dark themes use the exact spec hues.
    std::printf ("\n[4] Theme category tokens (positional-init guard)\n");
    {
        const juce::Colour specAudio (0xffFFB400), specEnv (0xff2DD4BF),
            specLfo (0xffE879F9), specSeq (0xff34D399), specArp (0xff34D399);
        // STRICT family palette: catAudio stays amber; Env=teal, Lfo=magenta,
        // Seq/Arp=mint. catArp == catSeq (Seq + Arp intentionally share the
        // mint sequencer-family hue), so that pair is exempt from the
        // pairwise-distinct guard below (the exact spec-hue match still guards
        // its positional-init alignment).

        const auto opaque = [] (const juce::Colour& c) { return c.getAlpha() == 255; };

        struct ThemeCheck { const char* name; const ParvatiTheme& t; bool expectDark; bool expectSpec; };
        const ThemeCheck themes[] = {
            { "Carbon",   carbonTheme(),   true,  true  },
            { "Midnight", midnightTheme(), true,  true  },
            { "Obsidian", obsidianTheme(), true,  true  },
            { "Paper",    paperTheme(),    false, false },
            { "Crimson",  crimsonTheme(),  true,  false },
        };

        char buf[160];
        for (const auto& tc : themes)
        {
            const juce::Colour cats[] = { tc.t.catAudio, tc.t.catEnv, tc.t.catLfo, tc.t.catSeq, tc.t.catArp };
            bool allOpaque = true;
            for (const auto& c : cats)
                if (! opaque (c)) allOpaque = false;
            // pairwise-distinct (ARGB) within a theme. catSeq (idx 3) and
            // catArp (idx 4) are EXEMPT — they intentionally share the mint
            // sequencer-family hue; the exact spec-hue match above still
            // guards their positional-init alignment.
            bool allDistinct = true;
            for (size_t i = 0; i < 5 && allDistinct; ++i)
                for (size_t j = i + 1; j < 5; ++j)
                {
                    if (i == 3 && j == 4) continue;
                    if (cats[i].getARGB() == cats[j].getARGB()) { allDistinct = false; break; }
                }

            std::snprintf (buf, sizeof (buf), "%s: 5 category colours are opaque", tc.name);
            check (allOpaque, buf);
            std::snprintf (buf, sizeof (buf), "%s: 5 category colours are pairwise-distinct", tc.name);
            check (allDistinct, buf);
            std::snprintf (buf, sizeof (buf), "%s: isDark == %s", tc.name, tc.expectDark ? "true" : "false");
            check (tc.t.isDark == tc.expectDark, buf);

            // keyWhite token: opaque + distinct from the black-key colour
            // (windowBackground is the sharp base in KeyboardView), guarding the
            // positional-init alignment of the new token.
            std::snprintf (buf, sizeof (buf), "%s: keyWhite is opaque", tc.name);
            check (opaque (tc.t.keyWhite), buf);
            std::snprintf (buf, sizeof (buf), "%s: keyWhite is distinct from the black-key colour", tc.name);
            check (tc.t.keyWhite.getARGB() != tc.t.backgroundBase.getARGB(), buf);

            if (tc.expectSpec)
            {
                const juce::Colour spec[] = { specAudio, specEnv, specLfo, specSeq, specArp };
                bool matchSpec = true;
                for (size_t i = 0; i < 5; ++i)
                    if (cats[i].getARGB() != spec[i].getARGB()) matchSpec = false;
                std::snprintf (buf, sizeof (buf), "%s: category colours match the spec hues", tc.name);
                check (matchSpec, buf);
            }
        }
    }

    // ---- [5] filter_card is on the Global page ----
    // The filter voice-card selector is a part-level global option (alongside
    // vca_curve + filter_drive), NOT on the Filter page. Verified by walking
    // the editor's component tree: each ParamPage exposes its group
    // (GroupComponent) names + the paramIDs of its ParamControl children.
    std::printf ("\n[5] filter_card placement (Global page)\n");
    {
        struct PageScan { juce::StringArray groups; juce::StringArray paramIds; };
        juce::Array<PageScan> pageScans;

        juce::Array<juce::Component*> nodes;
        nodes.add (editor);
        for (int i = 0; i < nodes.size(); ++i)
        {
            auto* c = nodes.getUnchecked (i);
            if (auto* page = dynamic_cast<ParamPage*> (c))
            {
                PageScan ps;
                for (auto* child : page->getChildren())
                {
                    if (auto* pc = dynamic_cast<ParamControl*> (child))
                        ps.paramIds.add (pc->getParamID());
                    if (auto* gc = dynamic_cast<juce::GroupComponent*> (child))
                        ps.groups.add (gc->getName());
                }
                pageScans.add (std::move (ps));
            }
            for (auto* child : c->getChildren())
                nodes.add (child);
        }

        bool filterCardOnFilter  = false;
        bool filterCardOnGlobal  = false;
        bool vcaCurveOnGlobal    = false;
        bool filterDriveOnGlobal = false;
        for (const auto& ps : pageScans)
        {
            const bool isFilter = ps.groups.contains ("Filter");
            const bool isGlobal = ps.groups.contains ("Global");
            if (ps.paramIds.contains ("filter_card"))
            {
                if (isFilter) filterCardOnFilter = true;
                if (isGlobal) filterCardOnGlobal = true;
            }
            if (isGlobal)
            {
                vcaCurveOnGlobal    = ps.paramIds.contains ("vca_curve");
                filterDriveOnGlobal = ps.paramIds.contains ("filter_drive");
            }
        }

        check (filterCardOnGlobal,  "filter_card is on the Global page");
        check (! filterCardOnFilter, "filter_card is NOT on the Filter page");
        check (vcaCurveOnGlobal,    "vca_curve stays on the Global page");
        check (filterDriveOnGlobal, "filter_drive stays on the Global page");
    }

    // ---- [6] Patch page is present in the editor ----
    std::printf ("\n[6] Patch page present\n");
    PatchPage* patchPage = nullptr;
    {
        juce::Array<juce::Component*> nodes;
        nodes.add (editor);
        for (int i = 0; i < nodes.size() && patchPage == nullptr; ++i)
        {
            auto* c = nodes.getUnchecked (i);
            if (auto* p = dynamic_cast<PatchPage*> (c)) patchPage = p;
            for (auto* child : c->getChildren())
                nodes.add (child);
        }
    }
    check (patchPage != nullptr, "PatchPage found in the editor component tree");

    if (patchPage != nullptr)
    {
        // ---- [7] Voice-card allocation manipulation via the Patch page ----
        // Drives the REAL UI code path (combo onChange -> cap-check ->
        // contiguous-bitmask write) and the engine->GUI reflection path, then
        // asserts against the engine (the source of truth).
        std::printf ("\n[7] Voice-card allocation via PatchPage\n");
        auto& engine = proc.getEngine();
        auto popcount = [] (uint8_t m) { int n = 0; for (int b = 0; b < 6; ++b) if (m & (1u << b)) ++n; return n; };

        // Start from a known arrangement: Single = all 6 cards on part 0.
        applyArrangement (engine, Arrangement::Single);
        patchPage->refresh();
        check (patchPage->getDisplayedCardCount (0) == 6, "Single: part 0 shows 6 cards");
        check (patchPage->getDisplayedCardCount (1) == 0, "Single: part 1 shows 0 cards");
        check (patchPage->getDisplayedArrangement() == Arrangement::Single,
               "Single: arrangement inferred as Single");

        // Manipulate via the UI path: redistribute to 3 + 3.
        patchPage->chooseCardCount (0, 3);
        patchPage->chooseCardCount (1, 3);
        check (popcount (engine.getPartVoiceAllocation (0)) == 3, "UI 3+3: part 0 owns 3 cards (engine)");
        check (popcount (engine.getPartVoiceAllocation (1)) == 3, "UI 3+3: part 1 owns 3 cards (engine)");
        check (patchPage->getDisplayedCardCount (0) == 3 && patchPage->getDisplayedCardCount (1) == 3,
               "UI 3+3: displayed counts reflect 3/3");
        int totalCards = 0;
        for (int p = 0; p < 6; ++p) totalCards += popcount (engine.getPartVoiceAllocation (p));
        check (totalCards == 6, "UI 3+3: total cards across parts == 6 (no leak)");

        // Cap enforcement: 6 cards already spent, so raising any other part
        // must be REJECTED (total cannot exceed 6; engine + combo untouched).
        const uint8_t p2Before = engine.getPartVoiceAllocation (2);
        patchPage->chooseCardCount (2, 2);   // would make 3 + 3 + 2 = 8 > 6
        check (popcount (engine.getPartVoiceAllocation (2)) == 0, "Cap: part 2 rejected (stays 0 cards)");
        check (engine.getPartVoiceAllocation (2) == p2Before, "Cap: part 2 engine bitmask unchanged");
        check (patchPage->getDisplayedCardCount (2) == 0, "Cap: part 2 combo reverted to 0");

        // Engine -> GUI reflection: load Multi6 directly, refresh, confirm the
        // page mirrors it (1 card each) and re-infers Multi6.
        applyArrangement (engine, Arrangement::Multi6);
        patchPage->refresh();
        bool oneEach = true;
        for (int p = 0; p < 6; ++p)
            if (patchPage->getDisplayedCardCount (p) != 1) oneEach = false;
        check (oneEach, "Multi6: every part shows 1 card (engine->GUI reflection)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Multi6,
               "Multi6: arrangement inferred as Multi6");

        // ---- [8] 6-card budget is self-enforcing (no 6x6) ----
        std::printf ("\n[8] 6-card budget self-enforces\n");
        applyArrangement (engine, Arrangement::Single);   // part 0 = all 6
        patchPage->refresh();
        check (patchPage->getCardCountMax (0) == 6, "Single: part 0 combo offers up to 6");
        check (patchPage->getCardCountMax (1) == 0, "Single: part 1 combo offers only 0 (budget spent)");
        // Freeing cards widens another row's budget: part 0 -> 3 lets part 1 reach 3.
        patchPage->chooseCardCount (0, 3);
        check (patchPage->getCardCountMax (1) == 3, "After part0=3: part 1 combo offers up to 3");
        // Spend the rest: part 1 -> 3 (total 6) collapses part 2's budget to 0.
        patchPage->chooseCardCount (1, 3);
        check (patchPage->getCardCountMax (2) == 0, "After 3+3: part 2 combo offers only 0");
        // No sequence of UI edits can push the engine past 6 total cards: the
        // dynamic per-row cap stops over-budget values even being offered.
        patchPage->chooseCardCount (0, 6);
        patchPage->chooseCardCount (2, 6);
        int total6 = 0;
        for (int p = 0; p < 6; ++p)
            total6 += popcount (engine.getPartVoiceAllocation (p));
        check (total6 == 6, "Over-budget edits rejected: engine total stays exactly 6");
    }

    // ---- teardown ----
    delete editor;

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "EDITOR TEST: FAILURES" : "EDITOR TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}
