// Multitimbral GUI smoke test: the editor builds, a Part selector exists, and
// per-part MIDI-channel editing reaches the engine. Headless (bare create /
// resize / teardown; no real message loop).

#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/CentralModBar.h"     // [MOD] toggle check (dynamic_cast target)

// Headless run-loop pump for the asynchronous triggerClick (Apple-only; the
// JUCE MessageQueue IS a CFRunLoopSource on the main loop — the
// perf-smoke-test idiom). defined(__APPLE__), not JUCE_MAC: this precedes the
// JUCE includes, which are what defines the JUCE_MAC macro.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif
#include "TuningTables.h"              // tuningPresetTable (Tune combo assertions)
#include "ui/ParvatiTheme.h"
#include "ui/PatchPage.h"
#include "ui/PatchArrangement.h"
#include "ui/TuningEditor.h"           // custom-tuning popover (direct instantiation)

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
    // The 6 factories use POSITIONAL brace init, so a missed/extra/misordered
    // value silently misaligns every later field with no compile error. This
    // guard catches that: every category colour is opaque + pairwise-distinct,
    // isDark is correct per theme, and the dark themes use the exact spec hues.
    std::printf ("\n[4] Theme category tokens (positional-init guard)\n");
    {
        const juce::Colour specEnv (0xff2DD4BF),
            specLfo (0xffE879F9), specSeq (0xff34D399), specArp (0xff34D399);
        // STRICT family palette everywhere: Env=teal, Lfo=magenta, Seq/Arp=mint
        // (and Perf=amber / Util=orange / Mod=purple / Const=indigo in the
        // factories). catAudio is NOT part of the family amber: every theme
        // adopts its brand accent for the audio family (a8b3cb2 introduced
        // this for Carbon; the amber-free-audio change extended it to all
        // themes) so knob rings / previews never render amber.
        // catArp == catSeq (Seq + Arp intentionally share the mint
        // sequencer-family hue), so that pair is exempt from the
        // pairwise-distinct guard below (the exact spec-hue match still guards
        // its positional-init alignment).

        const auto opaque = [] (const juce::Colour& c) { return c.getAlpha() == 255; };

        struct ThemeCheck { const char* name; const ParvatiTheme& t; bool expectDark; bool expectSpec; };
        const ThemeCheck themes[] = {
            { "Carbon",   carbonTheme(),   true,  true  },
            { "Midnight", midnightTheme(), true,  true  },
            { "Obsidian", obsidianTheme(), true,  true  },
            { "Paper",    paperTheme(),    false, true  },
            { "Crimson",  crimsonTheme(),  true,  true  },
            { "Legacy",   legacyTheme(),   false, true  },
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
                // catAudio is the theme's BRAND ACCENT, never the family
                // amber (see the comment above the themes table) — encode the
                // expected audio hue PER THEME so the exact-ARGB positional
                // guard keeps full strength on all 5 tokens. Family hues
                // (Env/Lfo/Seq/Arp) are shared across the dark themes; the
                // light themes use their darker 600-tier variants.
                const juce::Colour expAudio = (std::strcmp (tc.name, "Carbon") == 0)
                                                ? juce::Colour (0xff38BDF8)
                                                : (std::strcmp (tc.name, "Midnight") == 0)
                                                    ? juce::Colour (0xff5b9bd5)
                                                    : (std::strcmp (tc.name, "Obsidian") == 0)
                                                        ? juce::Colour (0xff8b5cf6)
                                                        : (std::strcmp (tc.name, "Paper") == 0)
                                                            ? juce::Colour (0xff2563eb)
                                                            : (std::strcmp (tc.name, "Crimson") == 0)
                                                                ? juce::Colour (0xffe5484d)
                                                                : juce::Colour (0xffC8216A);   // Legacy magenta
                const bool paperTheme_  = (std::strcmp (tc.name, "Paper")  == 0);
                const bool legacyTheme_ = (std::strcmp (tc.name, "Legacy") == 0);
                // Paper uses darker 600-tier family hues for light-bg contrast;
                // Legacy adopts the reference module's family hues wholesale.
                const juce::Colour expEnv = paperTheme_  ? juce::Colour (0xff0D9488)
                                            : legacyTheme_ ? juce::Colour (0xff009696)
                                            : specEnv;
                const juce::Colour expLfo = paperTheme_  ? juce::Colour (0xffC026D3)
                                            : legacyTheme_ ? juce::Colour (0xffE5B55C)
                                            : specLfo;
                const juce::Colour expSeq = paperTheme_  ? juce::Colour (0xff059669)
                                            : legacyTheme_ ? juce::Colour (0xffA8C69F)
                                            : specSeq;
                const juce::Colour expArp = expSeq;   // Seq family share
                const juce::Colour spec[] = { expAudio, expEnv, expLfo, expSeq, expArp };
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
        // ---- [6b] T4 scroll safety net: the 6 part rows + the hosted Global
        // page live in a vertical juce::Viewport (previously they clipped
        // unrecoverably in short frames). The body never under-fills the view,
        // and in the minimum 1024x500 frame it OVERFLOWS (natural height >
        // view height) — exactly the content that used to clip is scrollable.
        juce::Viewport* patchViewport = nullptr;
        {
            juce::Array<juce::Component*> nodes;
            nodes.add (patchPage);
            for (int i = 0; i < nodes.size() && patchViewport == nullptr; ++i)
            {
                auto* c = nodes.getUnchecked (i);
                if (auto* v = dynamic_cast<juce::Viewport*> (c)) patchViewport = v;
                for (auto* child : c->getChildren())
                    nodes.add (child);
            }
        }
        char t4msg[160];
        check (patchViewport != nullptr, "PatchPage body scrolls in a juce::Viewport (T4)");
        if (patchViewport != nullptr && patchViewport->getViewedComponent() != nullptr)
        {
            auto* body = patchViewport->getViewedComponent();
            check (body->getHeight() >= patchViewport->getViewHeight(),
                   "PatchPage body never under-fills the view");
            editor->setSize (1024, 500);   // the short-host scenario (minimum frame)
            const bool overflowed = body->getHeight() > patchViewport->getViewHeight();
            editor->setSize (1280, 634);   // restore the default size for later checks
            std::snprintf (t4msg, sizeof (t4msg),
                           "PatchPage body overflows (scrolls) in a 1024x500 frame [body=%d view=%d]",
                           body->getHeight(), patchViewport->getViewHeight());
            check (overflowed, t4msg);
        }
    }

    if (patchPage != nullptr)
    {
        // ---- [7] Arrangements + voice counts via the Patch page ----
        // Drives the REAL UI path (combo onChange -> applyArrangement /
        // setPartVoiceSlots) and the engine->GUI reflection path, then asserts
        // against the engine (the source of truth). The voice-first model: a
        // Part's voice count is 1..16 from the 96-voice pool and the 6 hardware
        // voicecards are DERIVED (contiguous proportional share) — any
        // combination of counts is legal, so there is no per-row cap.
        std::printf ("\n[7] Arrangements + voice counts via PatchPage\n");
        auto& engine = proc.getEngine();
        auto popcount = [] (uint8_t m) { int n = 0; for (int b = 0; b < 6; ++b) if (m & (1u << b)) ++n; return n; };

        // Single: part 0 maxed (16 voices), everything else disabled.
        applyArrangement (engine, Arrangement::Single);
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 16, "Single: part 0 shows 16 voices");
        check (patchPage->getDisplayedVoiceSlots (1) == 0, "Single: part 1 shows 0 (disabled)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Single,
               "Single: arrangement inferred as Single");
        // The derived cards follow the counts: one active part owns all 6.
        check (popcount (engine.getPartVoiceAllocation (0)) == 6,
               "Single: derived mask gives part 0 all 6 cards");

        // TRUE Mono preset: part 0 = 1 voice + MONO polyphony, others disabled.
        applyArrangement (engine, Arrangement::Mono);
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 1, "Mono: part 0 shows 1 voice");
        check (engine.getPartPolyphony (0) == 0, "Mono: part 0 poly is MONO (true mono)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Mono,
               "Mono: arrangement inferred as Mono");

        // Engine -> GUI reflection: load Multi6 (16 each, ch 1..6), refresh,
        // confirm the page mirrors it and re-infers Multi6.
        applyArrangement (engine, Arrangement::Multi6);
        patchPage->refresh();
        bool allSixteen = true;
        for (int p = 0; p < 6; ++p)
            if (patchPage->getDisplayedVoiceSlots (p) != 16) allSixteen = false;
        check (allSixteen, "Multi6: every part shows 16 voices (engine->GUI reflection)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Multi6,
               "Multi6: arrangement inferred as Multi6");

        // ---- [7b] Voice counts (per-part pool allocation) via the Patch page
        // ----
        // Drives the REAL Voices-combo path (onVoicesChanged ->
        // setPartVoiceSlots) and the engine->GUI reflection (refresh re-reads
        // the counts into the combo). The combo offers 1..16 (no "0":
        // disabling is the arrangements'/loaders' job — a real pick always
        // enables).
        std::printf ("\n[7b] Voice counts via PatchPage\n");
        check (patchPage->getDisplayedVoiceSlots (0) == 16,
               "default: part 0 Voices combo shows 16 (Multi6 preset)");
        patchPage->chooseVoiceSlots (0, 10);
        check (engine.getPartVoiceSlots (0) == 10,
               "UI voices: part 0 engine slots == 10");
        check (patchPage->getDisplayedVoiceSlots (0) == 10,
               "UI voices: part 0 combo shows 10");
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 10,
               "UI voices: refresh keeps part 0 at 10 (engine->GUI reflection)");
        // 0 is clamped to 1 (the combo offers no "0" — a pick always enables).
        patchPage->chooseVoiceSlots (0, 0);
        check (engine.getPartVoiceSlots (0) == 1,
               "UI voices: chooseVoiceSlots(0) clamps to 1 (enables, never disables)");

        // ---- [8] The pool has NO per-row cap: every part can be maxed ----
        std::printf ("\n[8] every part can be maxed simultaneously\n");
        applyArrangement (engine, Arrangement::Multi6);   // 6 x 16 = the whole pool
        patchPage->refresh();
        patchPage->chooseVoiceSlots (0, 16);
        patchPage->chooseVoiceSlots (1, 16);
        patchPage->chooseVoiceSlots (2, 16);
        patchPage->chooseVoiceSlots (3, 16);
        patchPage->chooseVoiceSlots (4, 16);
        patchPage->chooseVoiceSlots (5, 16);
        int totalVoices = 0;
        for (int p = 0; p < 6; ++p)
            totalVoices += engine.getPartVoiceSlots (p);
        check (totalVoices == 96, "All six parts maxed: engine total == 96 (whole pool, no cap)");
        // The DERIVED 6-card budget still holds: 6 active parts -> 1 card each,
        // disjoint.
        int totalCards = 0;
        bool disjoint = true;
        uint8_t used = 0;
        for (int p = 0; p < 6; ++p)
        {
            const uint8_t m = engine.getPartVoiceAllocation (p);
            totalCards += popcount (m);
            if (used & m) disjoint = false;
            used = static_cast<uint8_t> (used | m);
        }
        check (totalCards == 6 && disjoint,
               "Derived cards: exactly 6, disjoint, shared across the 6 parts");

        // ---- [9] Per-part tuning via the Patch page (Tune column) ----
        // Drives the REAL combo path (byte-4 write + APVTS re-sync), the
        // engine->GUI reflection, and the Custom… popover (instantiated
        // directly — launch() needs a desktop window).
        std::printf ("\n[9] Per-part tuning via PatchPage\n");
        check (patchPage->getDisplayedTuningMode (0) == 0, "Tune: part 0 starts at 12-EDO");

        // Preset pick via the UI path -> PartData byte 4 + resolved mode.
        patchPage->chooseTuningMode (0, 5);
        check (engine.getPart (0).partBytes[4] == 5, "Tune: preset 5 written to PartData byte 4");
        check (engine.resolvedTuningMode (0) == 5, "Tune: resolved mode reports the preset");
        check (patchPage->getDisplayedTuningMode (0) == 5, "Tune: combo reflects the preset");
        // A preset pick on ANOTHER part must not touch part 0 (byte-4 write is
        // part-addressed through the setCurrentPart idiom).
        patchPage->chooseTuningMode (3, 12);
        check (engine.getPart (0).partBytes[4] == 5 && engine.getPart (3).partBytes[4] == 12,
               "Tune: per-part byte writes stay isolated");

        // Engine -> GUI reflection (a .MUL load landing a preset byte).
        patchPage->refresh();
        check (patchPage->getDisplayedTuningMode (3) == 12, "Tune: refresh mirrors engine mode");

        // Custom… popover (direct instantiation = the same rows/apply paths):
        // prefill from the resolved preset, a row edit activates custom mode,
        // [Clear] zeroes but stays custom, and an explicit 12-EDO pick clears
        // the custom flag (D4).
        {
            int applyCount = 0;
            TuningEditor ed (engine, 0, [&applyCount] { ++applyCount; });
            const auto* just = parvati::tuningPresetTable (5);
            check (ed.rowUnits (0) == just[0], "TuningEditor prefills from the resolved preset");
            ed.setRowUnitsForTest (1, 17);
            check (applyCount >= 1, "TuningEditor applies live (callback fired)");
            check (engine.resolvedTuningMode (0) == 33, "row edit activates custom mode");
            int16_t t[12] = {};
            engine.resolveTuningOffsets (0, t);
            check (t[1] == 17 && t[0] == just[0], "custom table = preset prefill + the edited row");
            check (ed.rowReadout (1) == "+13.28ct", "readout shows quantized cents (17 units)");
            ed.clearForTest();
            engine.resolveTuningOffsets (0, t);
            bool zeros = true;
            for (int c = 0; c < 12; ++c) zeros = zeros && t[c] == 0;
            check (zeros && engine.resolvedTuningMode (0) == 33, "[Clear] zeros the table, stays Custom");
        }

        // Scala import through the popover's conversion path (12tet + a kbm
        // that mutes class 2): table untouched on error, sentinel on success.
        {
            TuningEditor ed (engine, 0, nullptr);
            const auto scl = juce::String ("! 12tet\n12\n")
                + "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";
            const auto kbm = juce::String (
                "! map\n12\n0\n11\n60\n60\n261.6255653\n12\n")
                + "0\nx\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n";
            check (ed.importScalaForTest (scl, kbm), "Scala import accepted (muted class)");
            check (ed.rowUnits (1) == (int) parvati::kTuningSilence, "kbm 'x' class carries the mute sentinel");
            check (ed.rowReadout (1).contains ("\xE2\x80\x94"), "muted row reads as an em dash");
            const auto before = ed.rowUnits (0);
            check (! ed.importScalaForTest (juce::String ("! bad\nnotanumber\n"), {}),
                   "malformed .scl rejected");
            check (ed.rowUnits (0) == before, "rejected import leaves the table untouched");
        }

        // Explicit 12-EDO clears the custom flag; byte 4 returns to 0.
        patchPage->chooseTuningMode (0, 0);
        check (engine.getPart (0).partBytes[4] == 0, "12-EDO writes raga byte 0");
        check (engine.resolvedTuningMode (0) == 0, "12-EDO clears the custom flag (D4)");
        check (patchPage->getDisplayedTuningMode (0) == 0, "Tune: combo back to 12-EDO");
    }

    // ---- [MOD] header toggle: mod-pill bar show/hide ----
    // The toggle collapses the bar SEAM in both workspaces (its height rejoins
    // the content rows). Drive the REAL button (triggerClick + a run-loop pump —
    // a click is asynchronous) and verify the workspace's CentralModBar child
    // hides, then re-shows. Apple-only: the headless pump runs the main
    // CFRunLoop directly (the perf-smoke-test idiom).
#if defined (__APPLE__)
    std::printf ("\n[MOD] mod-pill bar toggle\n");
    {
        auto* ed = dynamic_cast<ParvatiEditor*> (editor);
        std::function<juce::TextButton* (juce::Component*)> findBtn = [&] (juce::Component* c) -> juce::TextButton*
        {
            // Two header buttons are labelled "MOD" after the 2026-08 rename
            // (bar toggle + tap-to-assign); the BAR toggle is identified by its
            // tooltip.
            if (auto* b = dynamic_cast<juce::TextButton*> (c))
                if (b->getButtonText() == "MOD" && b->getTooltip().containsIgnoreCase ("pill bar"))
                    return b;
            for (auto* ch : c->getChildren())
                if (auto* r = findBtn (ch)) return r;
            return nullptr;
        };
        juce::TextButton* mbarBtn = ed != nullptr ? findBtn (ed) : nullptr;
        check (mbarBtn != nullptr, "[MOD] bar-toggle button exists in the header (tooltip: pill bar)");
        check (mbarBtn != nullptr && mbarBtn->getToggleState(),
               "[MOD] bar toggle defaults to ON (bar shown)");
        if (mbarBtn != nullptr)
        {
            std::function<CentralModBar* (juce::Component*)> findBar = [&] (juce::Component* c) -> CentralModBar*
            {
                if (auto* bar = dynamic_cast<CentralModBar*> (c)) return bar;
                for (auto* ch : c->getChildren())
                    if (auto* r = findBar (ch)) return r;
                return nullptr;
            };
            CentralModBar* bar = ed != nullptr ? findBar (ed) : nullptr;
            check (bar != nullptr, "CentralModBar found in the workspace");
            if (bar != nullptr)
            {
                check (bar->isVisible(), "bar visible before the toggle");
                mbarBtn->triggerClick();
                bool hidden = false;
                for (int i = 0; i < 50 && ! hidden; ++i)
                {
                    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
                    hidden = ! bar->isVisible();
                }
                check (hidden, "[MOD] bar toggle OFF hides the bar (seam collapses)");
                mbarBtn->triggerClick();
                bool shown = false;
                for (int i = 0; i < 50 && ! shown; ++i)
                {
                    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
                    shown = bar->isVisible();
                }
                check (shown, "[MOD] bar toggle ON re-shows the bar");
            }
        }
    }
#else
    std::printf ("\n[MOD] mod-pill bar toggle check skipped (non-Apple pump)\n");
#endif

    // ---- teardown ----
    delete editor;

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "EDITOR TEST: FAILURES" : "EDITOR TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}
