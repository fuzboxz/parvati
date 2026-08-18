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
#include "PatchFile.h"              // AmbikaProgram parse (expected .PRO PartData bytes)
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
#include "ui/SettingsPanel.h"           // language-switch no-op check
#include "ui/FxSlotCard.h"             // [12b] the seeding seam (W10)
#include "ui/PresetBrowser.h"          // [17] the scan-cache seams (W10)
#include "ui/ThemeManager.h"
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

        // Poly: part 0 maxed (16 voices), everything else disabled.
        applyArrangement (engine, Arrangement::Poly);
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 16, "Poly: part 0 shows 16 voices");
        check (patchPage->getDisplayedVoiceSlots (1) == 0, "Poly: part 1 shows 0 (disabled)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Poly,
               "Poly: arrangement inferred as Poly");
        // The derived cards follow the counts: one active part owns all 6.
        check (popcount (engine.getPartVoiceAllocation (0)) == 6,
               "Poly: derived mask gives part 0 all 6 cards");

        // TRUE Mono preset: part 0 = 1 voice + MONO polyphony, others disabled.
        applyArrangement (engine, Arrangement::Mono);
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 1, "Mono: part 0 shows 1 voice");
        check (engine.getPartPolyphony (0) == 0, "Mono: part 0 poly is MONO (true mono)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Mono,
               "Mono: arrangement inferred as Mono");

        // Engine -> GUI reflection: load Multitimbral (16 each, ch 1..6),
        // refresh, confirm the page mirrors it and re-infers Multitimbral.
        applyArrangement (engine, Arrangement::Multitimbral);
        patchPage->refresh();
        bool allSixteen = true;
        for (int p = 0; p < 6; ++p)
            if (patchPage->getDisplayedVoiceSlots (p) != 16) allSixteen = false;
        check (allSixteen, "Multitimbral: every part shows 16 voices (engine->GUI reflection)");
        check (patchPage->getDisplayedArrangement() == Arrangement::Multitimbral,
               "Multitimbral: arrangement inferred as Multitimbral");

        // Drum Kit: six 1-voice Omni parts on single GM note zones.
        applyArrangement (engine, Arrangement::DrumKit);
        patchPage->refresh();
        bool oneEach = true;
        for (int p = 0; p < 6; ++p)
            if (patchPage->getDisplayedVoiceSlots (p) != 1) oneEach = false;
        check (oneEach, "Drum Kit: every part shows 1 voice (engine->GUI reflection)");
        check (patchPage->getDisplayedArrangement() == Arrangement::DrumKit,
               "Drum Kit: arrangement inferred as Drum Kit");

        // ---- [7b] Voice counts (per-part pool allocation) via the Patch page
        // ----
        // Drives the REAL Voices-combo path (onVoicesChanged ->
        // setPartVoiceSlots / the 0-disable) and the engine->GUI reflection
        // (refresh re-reads the counts into the combo). The combo offers
        // 0..16: 0 DISABLES the part (the "0" item — a real pick, not a
        // ghosted placeholder).
        std::printf ("\n[7b] Voice counts via PatchPage\n");
        applyArrangement (engine, Arrangement::Multitimbral);
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 16,
               "default: part 0 Voices combo shows 16 (Multitimbral preset)");
        patchPage->chooseVoiceSlots (0, 10);
        check (engine.getPartVoiceSlots (0) == 10,
               "UI voices: part 0 engine slots == 10");
        check (patchPage->getDisplayedVoiceSlots (0) == 10,
               "UI voices: part 0 combo shows 10");
        patchPage->refresh();
        check (patchPage->getDisplayedVoiceSlots (0) == 10,
               "UI voices: refresh keeps part 0 at 10 (engine->GUI reflection)");
        // 0 is a REAL pick now: it disables the part (0 engine slots, the
        // "0" combo item selected at full strength — not a placeholder), and
        // the non-template state re-infers Custom (the combo's real disabled
        // id-6 item, never ghosted text).
        patchPage->chooseVoiceSlots (0, 0);
        check (engine.getPartVoiceSlots (0) == 0,
               "UI voices: chooseVoiceSlots(0) disables the part (0 slots)");
        check (patchPage->getDisplayedVoiceSlots (0) == 0,
               "UI voices: part 0 combo shows the real \"0\" item");
        check (patchPage->getDisplayedArrangement() == Arrangement::Custom,
               "UI voices: 10/0/16x4 mix re-infers Custom (real combo item, no ghost)");
        // Re-enable: a positive pick revives the part.
        patchPage->chooseVoiceSlots (0, 4);
        check (engine.getPartVoiceSlots (0) == 4,
               "UI voices: re-picking 4 re-enables the part");

        // ---- [7c] Stock template FILES through the REAL load path ----
        // Mirrors ParvatiEditor::applyPatchFile exactly (loadParvatiMultiFile
        // + patchPage->refresh()): loads the shipped presets/TEMPLATES/*.parvati
        // and asserts the Patch page's rows/arrangement reflect the loaded
        // multi — the end-to-end engine->GUI reflection after a file load.
        std::printf ("\n[7c] stock templates via the real load path\n");
        const juce::File tplDir = juce::File::getCurrentWorkingDirectory()
                                      .getChildFile ("presets/TEMPLATES");
        const juce::File polyFile  = tplDir.getChildFile ("Poly.parvati");
        const juce::File multiFile = tplDir.getChildFile ("Multitimbral.parvati");
        const juce::File drumFile  = tplDir.getChildFile ("Drum Kit (GM).parvati");
        check (polyFile.existsAsFile() && multiFile.existsAsFile() && drumFile.existsAsFile(),
               "stock templates present (run parvati_gen_templates first)");
        if (polyFile.existsAsFile())
        {
            check (proc.loadParvatiMultiFile (polyFile), "load path: Poly.parvati loads");
            patchPage->refresh();
            const int polyRows[6] = { 16, 0, 0, 0, 0, 0 };
            bool polyMirrored = true;
            for (int p = 0; p < 6; ++p)
                if (patchPage->getDisplayedVoiceSlots (p) != polyRows[p]) polyMirrored = false;
            check (polyMirrored,
                   "load path: Poly rows mirror 16/0/0/0/0/0 (Custom-style load no longer mis-styled)");
            check (patchPage->getDisplayedArrangement() == Arrangement::Poly,
                   "load path: Poly.parvati re-infers as Poly");
        }
        if (multiFile.existsAsFile())
        {
            check (proc.loadParvatiMultiFile (multiFile), "load path: Multitimbral.parvati loads");
            patchPage->refresh();
            bool sixMaxed = true;
            for (int p = 0; p < 6; ++p)
                if (patchPage->getDisplayedVoiceSlots (p) != 16) sixMaxed = false;
            check (sixMaxed, "load path: Multitimbral rows mirror 6 x 16");
            check (patchPage->getDisplayedArrangement() == Arrangement::Multitimbral,
                   "load path: Multitimbral.parvati re-infers as Multitimbral");
        }
        if (drumFile.existsAsFile())
        {
            // The shipped GM kit matches the built-in Drum Kit arrangement
            // preset exactly (6 x 1 mono voice, GM single-note zones) plus its
            // bespoke drum content (names + tuned patches, which inferArrangement
            // ignores) — so the page must show "Drum Kit", not Custom.
            check (proc.loadParvatiMultiFile (drumFile), "load path: Drum Kit (GM).parvati loads");
            patchPage->refresh();
            bool allOne = true;
            for (int p = 0; p < 6; ++p)
                if (patchPage->getDisplayedVoiceSlots (p) != 1) allOne = false;
            check (allOne, "load path: Drum Kit rows mirror 1 voice/part");
            check (patchPage->getDisplayedArrangement() == Arrangement::DrumKit,
                   "load path: Drum Kit (GM).parvati re-infers as Drum Kit");
        }

        // ---- [7d] EDITOR-LEVEL load wiring: the REAL user entry points ----
        // filesDropped is the actual drag-drop path (not the processor method
        // the [7c] block called directly): drop -> applyPatchFile -> load ->
        // patchPage->refresh(). Every check here asserts the page reflects the
        // load WITHOUT any manual refresh call from the test.
        std::printf ("\n[7d] editor-level load wiring (filesDropped / page reveal)\n");
        auto* parEd = dynamic_cast<ParvatiEditor*> (editor);
        check (parEd != nullptr, "editor casts to ParvatiEditor (drop-target seam)");
        if (parEd != nullptr)
        {
            // (a) Multi drop: Poly.parvati through filesDropped alone.
            if (polyFile.existsAsFile())
            {
                parEd->filesDropped (juce::StringArray (polyFile.getFullPathName()), 0, 0);
                bool polyMirrored = true;
                for (int p = 0; p < 6; ++p)
                    if (patchPage->getDisplayedVoiceSlots (p) != (p == 0 ? 16 : 0)) polyMirrored = false;
                check (polyMirrored,
                       "drop(multi): rows mirror 16/0/0/0/0/0 with NO manual refresh");
                check (patchPage->getDisplayedArrangement() == Arrangement::Poly,
                       "drop(multi): arrangement re-infers Poly with NO manual refresh");
            }

            // (b) Single-patch (.PRO) drop: the CURRENT part's Poly + Tune
            // combos must mirror the loaded PartData (a .PRO load rewrites
            // byte 15 + byte 4 of the current part). A custom tuning is armed
            // FIRST so the 12-EDO case also pins the stale-custom clear: the
            // factory .PRO carries raga byte 0 (verified below from the parsed
            // file), which must leave resolvedTuningMode at 0 — not the armed
            // 33 — and the row must show it.
            {
                const juce::File factDir = juce::File::getCurrentWorkingDirectory()
                                               .getChildFile ("presets/FACTORY/A");
                juce::Array<juce::File> pros = factDir.findChildFiles (
                    juce::File::findFiles, false, "*.pro");   // case-insensitive
                check (! pros.isEmpty(), "factory .PRO bank present (presets/FACTORY/A)");
                if (! pros.isEmpty())
                {
                    AmbikaProgram expect;
                    check (parseAmbikaProgramFile (pros[0], expect) && expect.hasPart,
                           "factory .PRO parses (PartData present)");
                    // Arm a custom tuning so a stale flag would show as 33.
                    int16_t offs[12] = { 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
                    engine.setPartTuningCustom (0, offs);
                    check (engine.resolvedTuningMode (0) == 33,
                           "precondition: custom tuning armed (resolved mode 33)");

                    parEd->filesDropped (juce::StringArray (pros[0].getFullPathName()), 0, 0);
                    const int cur = engine.getCurrentPart();
                    std::snprintf (msg, sizeof (msg),
                                  "drop(.PRO): Poly combo mirrors engine byte 15 [combo=%d engine=%d]",
                                  patchPage->getDisplayedPolyphony (cur),
                                  (int) engine.getPartPolyphony (cur));
                    check (patchPage->getDisplayedPolyphony (cur)
                               == (int) engine.getPartPolyphony (cur), msg);
                    std::snprintf (msg, sizeof (msg),
                                  "drop(.PRO): Tune combo mirrors resolved mode [combo=%d engine=%d fileByte4=%d]",
                                  patchPage->getDisplayedTuningMode (0),
                                  engine.resolvedTuningMode (0), (int) expect.part[4]);
                    check (patchPage->getDisplayedTuningMode (0)
                               == engine.resolvedTuningMode (0), msg);
                    if (expect.part[4] == 0)
                        check (engine.resolvedTuningMode (0) == 0,
                               "drop(.PRO): stale custom tuning cleared (12-EDO file resolves 0, not 33)");
                }
            }

            // (c) CORRUPT .parvati multi drop: validation must fail BEFORE any
            // engine mutation (the old path ran resetAllVoices + slot resets
            // first, so a failed load silently re-partitioned the pool).
            {
                engine.setPartVoiceSlots (0, 11);   // distinctive state to detect mutation
                patchPage->refresh();
                juce::File corrupt = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                         .getChildFile ("parvati_corrupt_test.parvati");
                check (corrupt.replaceWithText (
                           "format: parvati-multi\nparts: [\n"),
                       "corrupt .parvati test file written");
                const int slotsBefore[6] = { engine.getPartVoiceSlots (0),
                                             engine.getPartVoiceSlots (1),
                                             engine.getPartVoiceSlots (2),
                                             engine.getPartVoiceSlots (3),
                                             engine.getPartVoiceSlots (4),
                                             engine.getPartVoiceSlots (5) };
                parEd->filesDropped (juce::StringArray (corrupt.getFullPathName()), 0, 0);
                bool unchanged = true;
                for (int p = 0; p < 6; ++p)
                    if (engine.getPartVoiceSlots (p) != slotsBefore[p]) unchanged = false;
                check (unchanged,
                       "drop(corrupt): failed multi load leaves voice slots untouched");
                check (engine.getPartVoiceSlots (0) == 11,
                       "drop(corrupt): part 0 keeps its 11 slots (no init reset)");
                corrupt.deleteFile();
            }

            // (d) Page-reveal refresh: an engine-direct edit behind the UI's
            // back (the host state restore stand-in) must surface when the
            // Patch page is revealed — setCurrentTopPage is the public page
            // switch the header [Patch] button drives.
            {
                parEd->setCurrentTopPage (0);   // leave the Patch page first
                engine.setPartVoiceSlots (0, 7);
                parEd->setCurrentTopPage (2);   // reveal -> showTopPage refresh
                check (patchPage->getDisplayedVoiceSlots (0) == 7,
                       "page reveal: hidden-page engine edit (7 slots) surfaces on reveal");
                parEd->setCurrentTopPage (0);   // restore the SYNTH page for later sections
            }

            // (e) Header min-width fit: at the 1024px minimum editor width the
            // [FX] mode button keeps its full 50px width (the left cluster
            // budget: presetW was trimmed 168 -> 156 so the cluster fits).
            {
                const int prevW = editor->getWidth(), prevH = editor->getHeight();
                editor->setSize (1024, 500);
                std::function<juce::TextButton* (juce::Component*)> findFx =
                    [&] (juce::Component* c) -> juce::TextButton*
                {
                    // The [FX] mode toggle is a DIRECT header child of the editor
                    // (y inside the 44px header band); nested pages have no "FX"
                    // TextButton, but stay defensive via the parent check.
                    if (auto* b = dynamic_cast<juce::TextButton*> (c))
                        if (b->getButtonText() == "FX" && b->getParentComponent() == dynamic_cast<juce::Component*> (parEd))
                            return b;
                    for (auto* ch : c->getChildren())
                        if (auto* r = findFx (ch)) return r;
                    return nullptr;
                };
                if (auto* fxBtn = findFx (parEd))
                {
                    std::snprintf (msg, sizeof (msg),
                                  "header fit @1024: [FX] keeps full 50px width [got %d]",
                                  fxBtn->getWidth());
                    check (fxBtn->getWidth() >= 50, msg);
                }
                else
                    check (false, "header fit @1024: [FX] button found");
                editor->setSize (prevW, prevH);   // restore
            }

            // (f) Part-combo label refresh: renaming the SELECTED part must
            // relabel the header combo's INLINE text immediately —
            // changeItemText only updates the menu entry, so the combo used to
            // keep painting the old label until the next part switch.
            {
                const juce::String savedName = engine.getPartName (0);
                engine.setPartName (0, "Snare");
                parEd->refreshPartComboNames();
                // Find the header part combo by its text ("1 · Snare").
                std::function<juce::ComboBox* (juce::Component*)> findPartCombo =
                    [&] (juce::Component* c) -> juce::ComboBox*
                {
                    if (auto* cb = dynamic_cast<juce::ComboBox*> (c))
                        if (cb->getText().contains ("Snare"))
                            return cb;
                    for (auto* ch : c->getChildren())
                        if (auto* r = findPartCombo (ch)) return r;
                    return nullptr;
                };
                if (auto* pcb = findPartCombo (parEd))
                    check (pcb->getText().contains ("Snare"),
                           "partCombo label: renamed part shows immediately (no part switch)");
                else
                    check (false, "partCombo label: header combo found after rename");
                engine.setPartName (0, savedName);
                parEd->refreshPartComboNames();
            }

            // (g) VISIBLE-page mirror under out-of-band engine writes: with
            // the Patch page on screen, an engine mutation that has no editor
            // hook (host automation / MIDI NRPN / host undo — the message-
            // thread engine mutators) must surface in the rows via the poll
            // timer's display-version mirror (pollPatchPageMirror is the
            // exact timer code path). Before the mirror, a VISIBLE page kept
            // showing stale rows until the next reveal/load.
            {
                parEd->setCurrentTopPage (2);   // make the Patch page VISIBLE

                // Out-of-band write 1: engine-direct voice slots (the
                // load/state-restore class of writes — no editor notification).
                engine.setPartVoiceSlots (0, 11);
                parEd->pollPatchPageMirror();
                check (patchPage->getDisplayedVoiceSlots (0) == 11,
                       "visible mirror: out-of-band slots write surfaces with NO manual refresh");

                // Idempotence: a second poll with no further change is a no-op
                // (the version capture after the refresh dedupes it).
                parEd->pollPatchPageMirror();
                check (patchPage->getDisplayedVoiceSlots (0) == 11,
                       "visible mirror: second poll with no change is a no-op");

                // Out-of-band write 2: the APVTS host-automation path — a
                // part_raga write drives applyPartByte(4) with no editor hook;
                // the row's Tune combo must mirror the resolved preset after
                // the poll.
                proc.getApvts().getParameterAsValue ("part_raga") = 5.0f;   // a raga preset
                parEd->pollPatchPageMirror();
                check (patchPage->getDisplayedTuningMode (0) == 5,
                       "visible mirror: host-automation raga write surfaces in the Tune combo");
                proc.getApvts().getParameterAsValue ("part_raga") = 0.0f;   // restore 12-EDO
                parEd->pollPatchPageMirror();
                check (patchPage->getDisplayedTuningMode (0) == 0,
                       "visible mirror: raga back to 12-EDO surfaces too");

                parEd->setCurrentTopPage (0);   // restore the SYNTH page for later sections
            }
        }

        // ---- [8] The pool has NO per-row cap: every part can be maxed ----
        std::printf ("\n[8] every part can be maxed simultaneously\n");
        applyArrangement (engine, Arrangement::Multitimbral);   // 6 x 16 = the whole pool
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

        // Custom… popover's POST-EDIT notification re-syncs the APVTS: the
        // popover writes the part's PartData ENGINE-DIRECT (byte 4 = 0 + custom
        // flag armed), so without the re-sync the hosted part_raga combo — and
        // an APVTS-based save — kept the STALE preset byte while the engine
        // played the custom table. Wire a directly-instantiated TuningEditor's
        // change callback to PatchPage::tuningEditorApplied (the exact body
        // openTuningEditor's launched popover runs) so the headless test drives
        // the real post-edit path without opening the modal dialog.
        patchPage->chooseTuningMode (0, 12);   // part 0 is CURRENT: preset 12 lands in the APVTS
        check (proc.getApvts().getRawParameterValue ("part_raga")->load() == 12.0f,
               "precondition: APVTS part_raga == 12 (stale-preset scenario armed)");
        {
            TuningEditor ed (engine, 0, [&patchPage] { patchPage->tuningEditorApplied (0); });
            ed.setRowUnitsForTest (2, 19);   // a user-style row edit -> applyTable -> onChanged
            check (engine.resolvedTuningMode (0) == 33,
                   "popover edit arms the custom table (mode 33)");
            check (proc.getApvts().getRawParameterValue ("part_raga")->load() == 0.0f,
                   "popover edit re-syncs the APVTS part_raga to 0 (no stale preset byte)");
        }
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

    // ------------------------------------------------------------------
    // [16] Settings language switch is a UI no-op: refreshLanguage() rebuilds
    // the Filter Quality combo, and the rebuild must NEVER fire its own
    // onChange (the ComboBox::clear() default queues an ASYNC change that
    // lands after the selection restore — an uncommanded engine write). The
    // Patch page got the same fix; this locks the Settings instance in.
    // ------------------------------------------------------------------
    std::printf ("\n[16] SettingsPanel: language switch fires no uncommanded writes\n");
    {
        ThemeManager themeMgr;
        ParvatiAudioProcessor settingsProc;
        settingsProc.prepareToPlay (48000.0, 256);
        settingsProc.setOversamplingFactor (4);   // NON-default selection
        int osWrites = 0, langWrites = 0;
        SettingsPanel panel (settingsProc, themeMgr,
                             [] (double) {}, [] (bool) {}, [] (bool) {},
                             [&] (int) { ++osWrites; },
                             [&] (const juce::String&) { ++langWrites; });
        panel.setBounds (0, 0, 420, 320);
        panel.refreshLanguage();
#if defined (__APPLE__)
        // Pump the run loop so any pending ASYNC ComboBox update (the
        // pre-fix clear() hazard) would be delivered.
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.050, false);
#endif
        check (osWrites == 0,
               "Settings: refreshLanguage fires NO oversampling write (async clear purged)");
        check (langWrites == 0,
               "Settings: refreshLanguage fires NO language write");
        check (settingsProc.getUiOversampling() == 4,
               "Settings: non-default Filter Quality selection preserved");
    }

    // ------------------------------------------------------------------
    // [12] FX type change: engagement defaults seed ONLY on a UI pick (W10,
    // lane-A finding 1b). parameterChanged fires identically for host
    // automation / NRPN writes of fx{N}_type — those must NOT clobber the
    // current param values with the incoming type's defaults. The seeding
    // seam (FxSlotCard::seedEngagementDefaultsForType) is invoked by the UI
    // paths (the type-combo popup pick + stepType) BEFORE the param write;
    // the listener never seeds. The W7 undo case is subsumed (nothing seeds
    // in the listener, so an undo replay can never seed) and still pinned.
    // (FxSlotCard lives in the FX workspace, built at editor construction —
    // its APVTS listener + the cards are live regardless of the page shown.)
    // ------------------------------------------------------------------
    std::printf ("\n[12] FX type seeds only on UI picks (automation-safe)\n");
    {
        auto& apvts = proc.getApvts();
        auto& um = proc.getUndoManager();
        auto setP = [&apvts] (const char* id, float v) { apvts.getParameterAsValue (id) = v; };
        const auto raw = [&apvts] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

        // Locate the FX1 card in the component tree (for the seam call).
        // The pageSelector_ TabbedComponent UNPARENTS non-current tab contents
        // (juce_TabbedComponent.cpp removeChildComponent on tab switch), so the
        // FX workspace + its slot cards are only IN the tree while the FX page
        // is the current one — switch there for the hunt, restore SYNTH after.
        FxSlotCard* fxCard1 = nullptr;
        {
            auto* parEd = dynamic_cast<ParvatiEditor*> (editor);
            if (parEd != nullptr) parEd->setCurrentTopPage (1);
            std::function<void (juce::Component*)> hunt = [&] (juce::Component* c)
            {
                if (c == nullptr || fxCard1 != nullptr) return;
                if (auto* card = dynamic_cast<FxSlotCard*> (c)) { fxCard1 = card; return; }
                for (int i = 0; i < c->getNumChildComponents(); ++i)
                    hunt (c->getChildComponent (i));
            };
            hunt (editor);
            if (parEd != nullptr) parEd->setCurrentTopPage (0);
        }
        check (fxCard1 != nullptr, "FX1 card found in the component tree");

        // Drain the part-switch undo invalidation armed during editor startup
        // (undoSafe would otherwise CLEAR the history on its first call — the
        // W2 cross-part-corruption guard doing its job).
        proc.undoSafe();
        um.clearUndoHistory();

        // (a) Baseline: pick type 1 via a plain param write (the automation
        //     stand-in — no seed now), then set a distinct user value.
        setP ("fx1_type", 1.0f);
        um.beginNewTransaction();
        setP ("fx1_param1", 9.0f);    // the user's custom value on type 1
        um.beginNewTransaction();

        // (b) AUTOMATION STAND-IN: a direct fx1_type write must NOT clobber.
        setP ("fx1_type", 5.0f);      // host automation lane / NRPN equivalent
        check (raw ("fx1_type") == 5.0f, "automation: type switched (5)");
        check (raw ("fx1_param1") == 9.0f,
               "automation: param1 NOT clobbered by the type write (listener never seeds)");

        // (c) UI SEAM: the explicit seam seeds the type's engagement defaults.
        if (fxCard1 != nullptr)
        {
            um.beginNewTransaction();
            fxCard1->seedEngagementDefaultsForType (5);   // same type: its defaults
            check (raw ("fx1_param1") != 9.0f,
                   "UI seam: seeding lands (param1 now the type-5 engagement default)");
            check (raw ("fx1_enabled") == 1.0f && raw ("fx1_drywet") != 0.0f,
                   "UI seam: enabled + an audible drywet seeded");
            check (raw ("fx1_type") == 5.0f,
                   "UI seam: seeding does not touch the type param itself");
        }

        // (d) W7 undo pin: undo across a type change keeps the restored params
        //     (nothing seeds in parameterChanged anymore; this pins it — a
        //     future re-introduction of listener-side seeding fails here).
        um.beginNewTransaction();
        setP ("fx1_param1", 7.0f);    // the user's value on type 5
        um.beginNewTransaction();
        setP ("fx1_type", 1.0f);      // switch back (automation-style: no seed)
        check (raw ("fx1_param1") == 7.0f,
               "undo precondition: params survive the type switch");
        proc.undoSafe();              // undo the type switch only (synchronous replay)
        check (raw ("fx1_type") == 5.0f, "undo restores the type (5)");
        check (raw ("fx1_param1") == 7.0f,
               "undo keeps the restored param1 (no seed-during-replay clobber)");

        um.clearUndoHistory();
        setP ("fx1_type", 0.0f);      // None: leave FX idle for later sections
    }

    // ------------------------------------------------------------------
    // [17] PresetBrowser menu cache (W10, lane-A finding 5): the directory
    // scan + the per-.PRO name parse run ONCE per generation — a cached
    // rebuild costs no disk scan and no re-parse (the old code rescanned and
    // re-parsed every factory .PRO on EVERY open, synchronously on the
    // message thread). invalidate() — the editor's save seam — forces the
    // rescan; an externally changed directory (mtime) also self-heals.
    // ------------------------------------------------------------------
    std::printf ("\n[17] PresetBrowser caches the scan + .PRO name parse\n");
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_presetbrowser_cache_test");
        tmp.deleteRecursively();
        auto mkDir = [] (const juce::File& d) { d.createDirectory(); return d; };
        const auto templatesDir = mkDir (tmp.getChildFile ("TEMPLATES"));
        const auto userDir      = mkDir (tmp.getChildFile ("USER"));
        const auto factoryDir   = mkDir (tmp.getChildFile ("FACTORY"));
        const auto factoryA     = mkDir (factoryDir.getChildFile ("A"));
        const auto multiDir     = mkDir (tmp.getChildFile ("FACTORY_MULTI"));
        check (userDir.getChildFile ("zeta.parvati").replaceWithText ("format: parvati-multi\nparts: []\n"),
               "test setup: user preset file written");
        check (factoryA.getChildFile ("000.PRO").replaceWithText ("not-a-pro", false),
               "test setup: factory .PRO written (parse fails -> filename label)");
        check (multiDir.getChildFile ("00.MUL").replaceWithText ("x"), "test setup: factory .MUL written");
        check (templatesDir.getChildFile ("tpl").replaceWithText ("x"), "test setup: template written");

        PresetBrowser browser (templatesDir, userDir, factoryDir, multiDir,
                               [] (const juce::File&) {});
        juce::PopupMenu m;
        browser.buildMenu (m);
        check (browser.debugScanCount() == 1, "first open scans once");
        const int parses = browser.debugParseCount();
        check (parses >= 1, ".PRO name parsed on the first scan");
        check (browser.debugTreeHasLeafLabel ("zeta"), "user preset present in the cached tree");
        check (browser.debugTreeHasLeafLabel ("000"), "factory .PRO leaf present (filename fallback label)");

        juce::PopupMenu m2;
        browser.buildMenu (m2);
        check (browser.debugScanCount() == 1, "second open is served from the cache (no rescan)");
        check (browser.debugParseCount() == parses, "cached rebuild re-parses no .PRO names");

        browser.invalidate();
        juce::PopupMenu m3;
        browser.buildMenu (m3);
        check (browser.debugScanCount() == 2, "invalidate() forces the rescan (the save seam)");
        check (browser.debugParseCount() > parses, "rescan re-parses the .PRO names");

        // External add (no invalidate): the directory mtime must move — sleep
        // past the millisecond resolution juce::Time keeps so the delta is
        // deterministic on every filesystem.
        juce::Thread::sleep (25);
        check (userDir.getChildFile ("newleaf.parvati").replaceWithText ("x"),
               "test setup: external file added to USER");
        juce::PopupMenu m4;
        browser.buildMenu (m4);
        check (browser.debugScanCount() == 3, "external USER change self-invalidates via dir mtime");
        check (browser.debugTreeHasLeafLabel ("newleaf"), "the external file appears at the next open");

        tmp.deleteRecursively();
    }

    // ---- teardown ----
    delete editor;

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "EDITOR TEST: FAILURES" : "EDITOR TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}
