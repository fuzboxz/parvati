// Multitimbral GUI smoke test: the editor builds, a Part selector exists, and
// per-part MIDI-channel editing reaches the engine. Headless (bare create /
// resize / teardown; no real message loop).

#include <cmath>
#include "unified_test_runner.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>   // std::getenv (PARVATI_HEADLESS reads below)
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setInt (host-path helper)
#include "PluginEditor.h"
#include "PatchFile.h"              // AmbikaProgram parse (expected .PRO PartData bytes)
#include "ui/CentralModBar.h"     // [MOD] toggle check (dynamic_cast target)
#include "ui/EnvelopeDisplay.h"    // [19] preview-generation regression
#include "ui/FilterResponseDisplay.h"
#include "ui/OscPreviewDisplay.h"

// Headless run-loop pump for the asynchronous triggerClick (Apple-only; the
// JUCE MessageQueue IS a CFRunLoopSource on the main loop — the
// perf-smoke-test idiom). defined(__APPLE__), not JUCE_MAC: this precedes the
// JUCE includes, which are what defines the JUCE_MAC macro.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif
#include "TuningTables.h"              // tuningPresetTable (Tune combo assertions)
#include "ui/ParvatiTheme.h"
#include "ui/SynthWorkspace.h"          // [25] getSynthWorkspaceForTest()->modBar()
#include "ui/ModTelemetryTypes.h"       // [25] ModTelemetrySnapshot / LiveEnvStage / LiveFilterValues
#include "dsp/patch.h"                  // [25] MOD_SRC_* (synthetic telemetry history)
#include "ui/PatchPage.h"
#include "ui/PatchArrangement.h"
#include "ui/SettingsPanel.h"           // language-switch no-op check
#include "ui/FxSlotCard.h"             // [12b] the seeding seam (W10)
#include "ui/PresetBrowser.h"          // [17] the scan-cache seams (W10)
#include "ui/ThemeManager.h"

// Exact float comparison is deliberate: these asserts pin values,
// not ranges.
#pragma clang diagnostic ignored "-Wfloat-equal"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }
}

// [19] preview-display update regression (defined after main; see its comment).
static int runPreviewRegression (bool windowed);

#if defined (__APPLE__)
// PUMP-WITH-DEADLINE (2026-08-21): macOS sometimes throttles a background
// test process's run loop so a FIXED-time CFRunLoop pump delivers ZERO 30 Hz
// timer ticks (the flaky "x=0.000 / gen 0 -> 0" failures across the pumped
// sections — random 1-in-4 runs). Every pumped assertion instead pumps in
// 40 ms slices UNTIL its observable condition holds, with a generous 3 s
// deadline; a healthy run satisfies the condition in the first slice or two.
template <typename Pred>
static void pumpUntil25 (Pred&& pred, double maxSec = 3.0)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (! pred())
    {
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.040, false);
        if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > maxSec)
            break;
    }
}
#endif

TEST(editor_test)
{
    // NATIVE-DIALOG SUPPRESSION (PluginEditor.cpp nativeDialogsSuppressed):
    // this harness puts the editor ON the desktop (addToDesktop is required
    // for the JUCE timers to run), which makes the editor's desktop-gated
    // file-picker seams think a human is present — every export/load seam
    // would pop a REAL NSOpenPanel/NSSavePanel on the developer's screen while
    // the pump runs. Setting the override keeps the seam semantics (handlers
    // still fire) with zero native chrome. A developer can export the same
    // variable to suppress pickers in ANY manual binary run.
    // (setEnvVar from test_utils.h wraps ::setenv on POSIX and _putenv_s on
    // Windows, so every platform that builds the suite gets the override.)
    setEnvVar ("PARVATI_HEADLESS", "1");

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
    // The 7 factories use POSITIONAL brace init, so a missed/extra/misordered
    // value silently misaligns every later field with no compile error. This
    // guard catches that: every category colour is opaque + pairwise-distinct,
    // isDark is correct per theme, and every theme matches its exact expected hues.
    std::printf ("\n[4] Theme category tokens (positional-init guard)\n");
    {
        const juce::Colour specEnv (0xff2DD4BF),
            specLfo (0xffE879F9), specSeq (0xff34D399);
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

        // Per-theme EXACT expected category hues (full-strength positional guard
        // for every theme, including the palette-deviating ones): audio, env,
        // lfo, seq. The dark spec themes share the family hues (teal / magenta /
        // mint); Paper uses its darker 600-tier variants; Immutable adopts the
        // reference module's hues wholesale; Swedish Red drives its DISPLAY
        // families with monochrome LCD greens (the green-screen theme identity).
        // catArp is always the seq hue (the sequencer family share).
        struct ThemeCheck
        {
            const char* name;
            const ParvatiTheme& t;
            bool expectDark;
            juce::Colour expAudio, expEnv, expLfo, expSeq;
        };
        const ThemeCheck themes[] = {
            { "Carbon",      carbonTheme(),      true,  juce::Colour (0xff38BDF8), specEnv, specLfo, specSeq },
            { "Midnight",    midnightTheme(),    true,  juce::Colour (0xff5b9bd5), specEnv, specLfo, specSeq },
            { "Obsidian",    obsidianTheme(),    true,  juce::Colour (0xff8b5cf6), specEnv, specLfo, specSeq },
            { "Paper",       paperTheme(),       false, juce::Colour (0xff2563eb),
              juce::Colour (0xff0D9488), juce::Colour (0xffC026D3), juce::Colour (0xff059669) },
            { "Crimson",     crimsonTheme(),     true,  juce::Colour (0xffe5484d), specEnv, specLfo, specSeq },
            { "Immutable",   immutableTheme(),   false, juce::Colour (0xffC8216A),
              juce::Colour (0xff009696), juce::Colour (0xffE5B55C), juce::Colour (0xffA8C69F) },
            { "Swedish Red", swedishRedTheme(), true,  juce::Colour (0xff9BE24A),
              juce::Colour (0xff57E05C), juce::Colour (0xff2FD98C), juce::Colour (0xffD6D2C4) },
            { "Y2K",          y2kTheme(),         false, juce::Colour (0xff3FBF3F),
              juce::Colour (0xffC45AB8), juce::Colour (0xffD9A441), juce::Colour (0xff5577CC) },
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

            {
                // catAudio is the theme's BRAND ACCENT, never the family
                // amber (see the comment above the themes table) — the expected
                // hues are encoded PER THEME so the exact-ARGB positional
                // guard keeps full strength on all 5 tokens.
                const juce::Colour spec[] = { tc.expAudio, tc.expEnv, tc.expLfo, tc.expSeq, tc.expSeq };
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
            // byte 15 + byte 4 of the current part). The factory .PRO carries
            // raga byte 0 (verified below from the parsed file), which must
            // leave resolvedTuningMode at 0 and the row must show it.
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
                               "drop(.PRO): 12-EDO file resolves mode 0 (raga byte is the whole state)");
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
        // Drives the REAL combo path (byte-4 write + APVTS re-sync) and the
        // engine->GUI reflection. (The Custom… popover was removed with the
        // custom-tuning subsystem, 2026-08-19.)
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

        // Explicit 12-EDO: byte 4 returns to 0.
        patchPage->chooseTuningMode (0, 0);
        check (engine.getPart (0).partBytes[4] == 0, "12-EDO writes raga byte 0");
        check (engine.resolvedTuningMode (0) == 0, "12-EDO resolves mode 0");
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

        // (d) W10b keyboard seam (F-w10-1): the focused combo's ARROW KEYS
        //     must seed exactly like a popup pick. JUCE's base keyPressed
        //     nudges the selection directly (no popup -> the showPopup
        //     override never ran), so a keyboard type change used to land with
        //     NO engagement defaults (a silent effect). FxTypeCombo now routes
        //     the arrows through the guarded pick seam.
        if (fxCard1 != nullptr)
        {
            juce::ComboBox* combo = nullptr;
            for (auto* c : fxCard1->getChildren())
                if ((combo = dynamic_cast<juce::ComboBox*> (c)))
                    break;
            check (combo != nullptr, "[12d] FX1 type combo found");
            if (combo != nullptr)
            {
                um.beginNewTransaction();
                const float typeBefore = raw ("fx1_type");
                const bool moved = combo->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
                check (moved, "[12d] down-arrow consumed by the combo");
                check (raw ("fx1_type") == typeBefore + 1.0f,
                       "[12d] keyboard arrow advances the type");
                check (raw ("fx1_enabled") == 1.0f,
                       "[12d] keyboard pick SEEDS engagement defaults (was: silent effect)");
            }
        }

        // (e) W10b same-item guard (F-w10-2): re-picking the CURRENT type
        //     (a natural way to dismiss the picker) must be a NO-OP — the
        //     unguarded seam re-seeded all 7 engagement defaults and clobbered
        //     the user's knob tweaks with no type change.
        if (fxCard1 != nullptr)
        {
            um.beginNewTransaction();
            setP ("fx1_param1", 21.0f);   // the user's tweak on the CURRENT type
            um.beginNewTransaction();
            const float typeNow = raw ("fx1_type");
            fxCard1->simulateUserTypePickForTest (juce::roundToInt (typeNow));   // same item
            check (raw ("fx1_type") == typeNow, "[12e] same-item pick keeps the type");
            check (raw ("fx1_param1") == 21.0f,
                   "[12e] same-item pick does NOT re-seed (user knobs survive)");
            // Contrast: picking a DIFFERENT item still seeds.
            const int other = juce::roundToInt (typeNow) == 1 ? 2 : 1;
            fxCard1->simulateUserTypePickForTest (other);
            check (raw ("fx1_type") == (float) other, "[12e] different-item pick switches the type");
            check (raw ("fx1_param1") != 21.0f, "[12e] different-item pick seeds its defaults");
        }

        // (f) W7 undo pin: undo across a type change keeps the restored params
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

        // External add (no invalidate): the directory mtime must move. The
        // sleep alone was filesystem-dependent (25 ms is invisible on 1-second-
        // granularity filesystems like HFS+/FAT) — bump the directory mtime
        // EXPLICITLY so the pickup is deterministic everywhere (W11).
        juce::Thread::sleep (25);
        check (userDir.getChildFile ("newleaf.parvati").replaceWithText ("x"),
               "test setup: external file added to USER");
        userDir.setLastModificationTime (juce::Time::getCurrentTime()
                                             + juce::RelativeTime::seconds (2));
        juce::PopupMenu m4;
        browser.buildMenu (m4);
        check (browser.debugScanCount() == 3, "external USER change self-invalidates via dir mtime");
        check (browser.debugTreeHasLeafLabel ("newleaf"), "the external file appears at the next open");

        // DOCUMENTED RESIDUAL (W11, F-w10-3): a write that PRESERVES the
        // directory mtime exactly is accepted-stale (same-ms granularity /
        // timestamp-preserving restores). Pin that deterministically: add a
        // file but restore the recorded mtime -> NO rescan. (invalidate()
        // after a save covers the in-app writer.)
        {
            const juce::Time frozen = userDir.getLastModificationTime();
            check (userDir.getChildFile ("ghost.parvati").replaceWithText ("x"),
                   "test setup: mtime-preserving external file added");
            userDir.setLastModificationTime (frozen);
            juce::PopupMenu m5;
            browser.buildMenu (m5);
            check (browser.debugScanCount() == 3,
                   "RESIDUAL pinned: mtime-preserving external write stays cached (by design)");
            check (! browser.debugTreeHasLeafLabel ("ghost"),
                   "RESIDUAL pinned: the ghost file is not in the cached tree (by design)");
        }

        // W11 (F-w10-4): a watch ROOT that was ABSENT at scan time and is
        // created externally mid-session must invalidate the cache (the
        // recorded `present` flag flips). Pre-fix, never-recorded roots left
        // the new subtree invisible until a save-invalidate.
        {
            const auto lateUser = tmp.getChildFile ("LATE_USER");   // absent so far
            PresetBrowser lateBrowser (templatesDir, lateUser, factoryDir, multiDir,
                                       [] (const juce::File&) {});
            juce::PopupMenu l1;
            lateBrowser.buildMenu (l1);
            check (lateBrowser.debugScanCount() == 1, "late-root: first open scans once");

            lateUser.createDirectory();
            lateUser.getChildFile ("appeared.parvati").replaceWithText ("x");
            lateUser.setLastModificationTime (juce::Time::getCurrentTime()
                                                 + juce::RelativeTime::seconds (2));
            juce::PopupMenu l2;
            lateBrowser.buildMenu (l2);
            check (lateBrowser.debugScanCount() == 2,
                   "late-root: external creation of an absent root invalidates the cache");
            check (lateBrowser.debugTreeHasLeafLabel ("appeared"),
                   "late-root: the externally created root's preset appears");
        }

        tmp.deleteRecursively();
    }

    // ------------------------------------------------------------------
    // [18] Header keyboard shortcuts + preset stepping + host-context menu
    // degradation. The shortcut handlers are small testable seams
    // (handleStepPresetShortcut / handlePartSelectShortcut / ...) reached
    // exactly as the keyboard reaches them: editor->keyPressed(...).
    // ------------------------------------------------------------------
    std::printf ("\n[18] keyboard shortcuts + preset stepping\n");
    {
        // ---- (a) PresetBrowser step semantics on a fully deterministic tree.
        // Flattened order mirrors the menu: Factory bank A first, then Multi,
        // User, Templates; wrap at the ends; setCurrentFile anchors an
        // out-of-menu load.
        const auto tmp18 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("parvati_preset_step_test");
        tmp18.deleteRecursively();
        auto mkDir18 = [] (const juce::File& d) { d.createDirectory(); return d; };
        const auto factoryA18  = mkDir18 (tmp18.getChildFile ("FACTORY/A"));
        const auto multiDir18  = mkDir18 (tmp18.getChildFile ("FACTORY_MULTI"));
        const auto userDir18   = mkDir18 (tmp18.getChildFile ("USER"));
        const auto tplDir18    = mkDir18 (tmp18.getChildFile ("TEMPLATES"));
        check (factoryA18.getChildFile ("a1.PRO").replaceWithText ("x", false), "setup: A/a1");
        check (factoryA18.getChildFile ("a2.PRO").replaceWithText ("x", false), "setup: A/a2");
        check (multiDir18.getChildFile ("m1.MUL").replaceWithText ("x"), "setup: multi");
        check (userDir18.getChildFile ("u1.parvati").replaceWithText ("x"), "setup: user");
        check (tplDir18.getChildFile ("t1.parvati").replaceWithText ("x"), "setup: template");

        juce::Array<juce::File> steppedFiles;   // every file onSelect delivers
        PresetBrowser stepper (tplDir18, userDir18, tmp18.getChildFile ("FACTORY"),
                               multiDir18, [&steppedFiles] (const juce::File& f) { steppedFiles.add (f); });
        const juce::File first = stepper.selectNext();   // not-anchored -> FIRST leaf
        check (first.getFileName() == "a1.PRO", "step(next) unanchored starts at Factory A leaf 0");
        const juce::File second = stepper.selectNext();
        check (second.getFileName() == "a2.PRO", "step(next) advances within the bank");
        const juce::File third = stepper.selectNext();
        check (third.getFileName() == "m1.MUL", "step order: Multi follows the factory banks");
        const juce::File fourth = stepper.selectNext();
        check (fourth.getFileName() == "u1.parvati", "step order: User follows Multi");
        const juce::File fifth = stepper.selectNext();
        check (fifth.getFileName() == "t1.parvati", "step order: Templates last");
        const juce::File wrapped = stepper.selectNext();
        check (wrapped == first, "step(next) WRAPS back to the first leaf");
        const juce::File prev = stepper.selectPrev();
        check (prev == fifth, "step(prev) returns to the last leaf");
        const juce::File prevUser = stepper.selectPrev();
        check (prevUser.getFileName() == "u1.parvati", "step(prev) walks backwards (User)");
        check (steppedFiles.size() == 8, "every step fires the onSelect (load) seam");
        // Anchor an out-of-menu load (drag-drop / Load... path): stepping
        // continues from THAT file, not from wherever stepping left off.
        stepper.setCurrentFile (userDir18.getChildFile ("u1.parvati"));
        const juce::File fromAnchor = stepper.selectPrev();
        check (fromAnchor.getFileName() == "m1.MUL", "setCurrentFile anchors stepping");
        // An empty tree degrades to an invalid File (the editor then passes
        // the key on instead of consuming it).
        PresetBrowser empty (mkDir18 (tmp18.getChildFile ("E_TPL")),
                             mkDir18 (tmp18.getChildFile ("E_USER")),
                             mkDir18 (tmp18.getChildFile ("E_FACTORY")),
                             mkDir18 (tmp18.getChildFile ("E_MULTI")), [] (const juce::File&) {});
        check (! empty.selectNext().existsAsFile(), "empty tree: step returns an invalid File");
        tmp18.deleteRecursively();

        // ---- (a2) Multi-bank factory interleave + nested USER directories.
        // The four factory banks step in kBanks order (A -> B -> F -> S),
        // then Multi, then the recursive USER tree, then Templates; inside
        // the USER tree a SUBDIRECTORY's leaves come before the parent's own
        // leaves (the menu's subs-then-leaves traversal).
        const auto tmp18b = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("parvati_preset_step_multibank");
        tmp18b.deleteRecursively();
        const auto fact18b = mkDir18 (tmp18b.getChildFile ("FACTORY"));
        const char* const kBankNames18b[] = { "A", "B", "F", "S" };
        bool bankSetupOk = true;
        for (const char* bank : kBankNames18b)
        {
            const auto bankDir = mkDir18 (fact18b.getChildFile (bank));
            if (! bankDir.getChildFile (juce::String (bank).toLowerCase() + ".PRO")
                      .replaceWithText ("x", false))
                bankSetupOk = false;
        }
        check (bankSetupOk, "setup: one .PRO per factory bank (A/B/F/S)");
        const auto multi18b  = mkDir18 (tmp18b.getChildFile ("FACTORY_MULTI"));
        const auto user18b   = mkDir18 (tmp18b.getChildFile ("USER"));
        const auto tpl18b    = mkDir18 (tmp18b.getChildFile ("TEMPLATES"));
        check (multi18b.getChildFile ("m1.MUL").replaceWithText ("x"), "setup: multi");
        // Nested user tree: Sub/ holds u0, the user root holds z — "Sub"
        // must be visited BEFORE the root's own leaf z.
        const auto userSub18b = mkDir18 (user18b.getChildFile ("Sub"));
        check (userSub18b.getChildFile ("u0.parvati").replaceWithText ("x"), "setup: user/Sub/u0");
        check (user18b.getChildFile ("z.parvati").replaceWithText ("x"), "setup: user/z");
        check (tpl18b.getChildFile ("t1.parvati").replaceWithText ("x"), "setup: template");
        {
            PresetBrowser multi (tpl18b, user18b, fact18b, multi18b,
                                 [] (const juce::File&) {});
            const char* const expectedOrder[] = {
                "a.PRO", "b.PRO", "f.PRO", "s.PRO",   // factory banks A B F S
                "m1.MUL",                                  // multi bank
                "u0.parvati", "z.parvati",               // user: Sub before root leaves
                "t1.parvati" };                           // templates last
            bool orderOk = true;
            for (int i = 0; i < 8; ++i)
            {
                const auto leaf = multi.selectNext();
                if (leaf.getFileName() != expectedOrder[i])
                {
                    std::printf ("     step %d: got \"%s\" want \"%s\"\n", i,
                                 leaf.getFileName().toRawUTF8(), expectedOrder[i]);
                    orderOk = false;
                }
            }
            check (orderOk, "multi-bank + nested-user step order (A,B,F,S,Multi,Sub,z,tpl)");
            // Wrap from the last leaf back to the first bank.
            check (multi.selectNext().getFileName() == "a.PRO",
                   "step wraps from Templates back to bank A");
        }
        tmp18b.deleteRecursively();

        // ---- (b) Editor-level shortcuts through the REAL keyPressed path.
        auto* parEd18 = dynamic_cast<ParvatiEditor*> (editor);
        if (parEd18 != nullptr)
        {
            // Host-context degradation: headless/standalone has NO host
            // context — getHostContext() is null and showContextMenu takes
            // the local-only path (ParamControl reads it via the same call;
            // the null check IS the degradation).
            check (parEd18->getHostContext() == nullptr,
                   "headless: getHostContext() is null (local menu path)");

            // Preset stepping via the editor's REAL browser (its factory tree
            // was installed to app-data by the processor ctor — deterministic,
            // non-empty). Anchor first via a real drop, then step.
            const juce::File repoFactA = juce::File::getCurrentWorkingDirectory()
                                            .getChildFile ("presets/FACTORY/A");
            juce::Array<juce::File> repoPros = repoFactA.findChildFiles (
                juce::File::findFiles, false, "*.pro");
            if (! repoPros.isEmpty())
            {
                parEd18->filesDropped (juce::StringArray (repoPros[0].getFullPathName()), 0, 0);
                const bool stepped = parEd18->keyPressed (juce::KeyPress (']', 0, ']'));
                check (stepped, "keyPressed(']') consumes the key (step fired)");
                const bool steppedPlain = parEd18->keyPressed (juce::KeyPress ('[', 0, '['));
                check (steppedPlain, "keyPressed('[') (plain) consumes the key");
                // Cmd/Ctrl variants must behave identically.
                const bool steppedCmd = parEd18->keyPressed (
                    juce::KeyPress (']', juce::ModifierKeys::commandModifier, ']'));
                check (steppedCmd, "keyPressed(Cmd+']') consumes the key");
            }
            else
            {
                check (false, "presets/FACTORY/A present (editor step assertions)");
            }

            // Part select: Cmd/Ctrl+1..6 through the partCombo_ seam. Part 3
            // (Cmd+3) must land the engine on 0-based part 2.
            const int before18 = proc.getEngine().getCurrentPart();
            check (parEd18->keyPressed (
                      juce::KeyPress ('3', juce::ModifierKeys::commandModifier, '3')),
                  "keyPressed(Cmd+3) consumes the key");
            check (proc.getEngine().getCurrentPart() == 2,
                   "Cmd+3 selects Part 3 (engine current part 0-based 2)");
            (void) before18;
            // Out-of-range digit (Cmd+7): not consumed (no part 7).
            check (! parEd18->keyPressed (
                      juce::KeyPress ('7', juce::ModifierKeys::commandModifier, '7')),
                  "keyPressed(Cmd+7) passes through (no Part 7)");
            // Restore Part 1.
            (void) parEd18->keyPressed (
                juce::KeyPress ('1', juce::ModifierKeys::commandModifier, '1'));

            // File shortcuts: headless, the picker launch is desktop-gated —
            // the SEAM fires (true) with no dialog (a hang here would mean
            // the gate regressed; a native picker needs a window server).
            check (parEd18->keyPressed (
                      juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 'o')),
                  "keyPressed(Cmd+O) consumed headless (desktop-gated picker)");
            check (parEd18->keyPressed (
                      juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 's')),
                  "keyPressed(Cmd+S) consumed headless (desktop-gated picker)");
            // Zoom keys still work alongside the new bindings.
            check (parEd18->keyPressed (
                      juce::KeyPress ('0', juce::ModifierKeys::commandModifier, '0')),
                  "keyPressed(Cmd+0) still consumed (zoom reset)");
        }
        else
        {
            check (false, "editor casts to ParvatiEditor (shortcut assertions)");
        }
    }

    // ------------------------------------------------------------------
    // [20] Top-bar chrome polish (user feedback round 2):
    //   (a) version/patch separation — the brand block is sized to its WIDEST
    //       line (the "by 805Labs · v<ver>" subtitle, wider than the bold
    //       wordmark) + a breathing margin, so the patch indicator no longer
    //       jams against the version text;
    //   (b) slimmer header buttons on desktop (36pt visual height, vertically
    //       centred in the unchanged 44pt strip; iOS keeps full 44pt cells —
    //       HIG, not compiled here);
    //   (c) unselected-state colour affordance — accent wash (alpha <= 0.35)
    //       + textPrimary text on every header TextButton + the preset
    //       indicator, distinct from the stronger selected state.
    // ------------------------------------------------------------------
    std::printf ("\n[20] top-bar chrome polish\n");
    {
        auto* parEd = dynamic_cast<ParvatiEditor*> (editor);
        check (parEd != nullptr, "[20] editor casts to ParvatiEditor");
        if (parEd != nullptr)
        {
            editor->setSize (1280, 634);

            // ---- (a) version/patch separation ----
            const auto logo = parEd->getLogoAreaForTest();
            // Measure the subtitle with the SAME font paint()/resized() use.
            const ParvatiLookAndFeel measureLnf;
            juce::GlyphArrangement ga;
            ga.addLineOfText (measureLnf.appFont (10.0f, juce::Font::plain),
                              juce::String (juce::CharPointer_UTF8 ("by 805Labs \xc2\xb7 v" PARVATI_VERSION)),
                              0.0f, 0.0f);
            const int subW = juce::roundToInt (
                ga.getBoundingBox (0, ga.getNumGlyphs(), true).getWidth());
            check (logo.getWidth() >= subW + 12,
                   "[20] brand block fits the version subtitle + >=12px margin");
            // The preset indicator must start at/after the brand block's right
            // edge, and the VISIBLE gap (from the subtitle text end) is >= 10px.
            const juce::Component* browser = nullptr;
            for (auto* c : editor->getChildren())
                if (auto* b = dynamic_cast<const PresetBrowser*> (c)) { browser = b; break; }
            check (browser != nullptr, "[20] preset browser found");
            if (browser != nullptr)
            {
                const int visibleGap = browser->getBounds().getX()
                                       - (logo.getX() + subW);
                check (visibleGap >= 10,
                       "[20] version text -> patch indicator gap >= 10px");
            }

            // ---- (b) desktop-slimmer header buttons ----
            // Find every direct-child TextButton + the part combo and assert
            // the visual height band (36 on this desktop build) with unchanged
            // widths (>= 44pt cells).
            int checkedH = 0;
            for (auto* c : editor->getChildren())
            {
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    if (! b->isShowing() && b->getWidth() == 0) continue;
                    check (b->getHeight() >= 30 && b->getHeight() <= 38,
                           "[20] header TextButton visual height in [30,38] (slim, desktop)");
                    check (b->getHeight() < ParvatiEditor::kBarHeight,
                           "[20] header TextButton shorter than the 44pt strip");
                    ++checkedH;
                }
                else if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
                {
                    if (combo->getWidth() == 0) continue;
                    check (combo->getHeight() >= 30 && combo->getHeight() <= 38,
                           "[20] part combo visual height in [30,38] (slim, desktop)");
                    ++checkedH;
                }
            }
            check (checkedH >= 8, "[20] found >= 8 header controls to pin");

            // ---- (c) unselected-state colour affordance ----
            const ThemeManager tm;   // default Carbon == the editor's startup theme
            const auto& th = tm.getCurrentTheme();
            const juce::Colour wash = th.accentSecondary.withAlpha ((juce::uint8) 0x2A);
            int checkedC = 0;
            for (const char* name : { "Synth", "FX", "Patch", "Load", "Save" })
            {
                juce::TextButton* b = nullptr;
                for (auto* c : editor->getChildren())
                    if (auto* tb = dynamic_cast<juce::TextButton*> (c))
                        if (tb->getButtonText() == name) { b = tb; break; }
                if (b == nullptr) { check (false, "[20] header button found (colour pin)"); continue; }
                const auto fill = b->findColour (juce::TextButton::buttonColourId);
                const float a = fill.getFloatAlpha();
                check (a > 0.0f && a <= 0.35f,
                       "[20] unselected header fill is a translucent wash (0 < a <= 0.35)");
                check (fill != th.backgroundPanel,
                       "[20] unselected fill differs from the flat panel default");
                check (fill != b->findColour (juce::TextButton::buttonOnColourId),
                       "[20] unselected fill differs from the selected (on) fill");
                check (b->findColour (juce::TextButton::textColourOffId) == th.textPrimary,
                       "[20] unselected text uses the bright textPrimary tier");
                check (fill == wash, "[20] unselected fill is the accentSecondary wash");
                ++checkedC;
            }
            check (checkedC == 5, "[20] pinned the colour treatment on 5 header buttons");
            // Patch indicator (PresetBrowser's name button): bright text tier.
            if (browser != nullptr)
                if (auto* pb = dynamic_cast<juce::TextButton*> (
                        const_cast<juce::Component*> (browser)->getChildComponent (0)))
                {
                    check (pb->findColour (juce::TextButton::textColourOffId) == th.textPrimary,
                           "[20] patch indicator text uses textPrimary");
                    const float a = pb->findColour (juce::TextButton::buttonColourId).getFloatAlpha();
                    check (a > 0.0f && a <= 0.35f,
                           "[20] patch indicator fill is a translucent wash");
                }
        }
    }

    // ---- [21] Patch-page simplification: part settings placement ----
    // (a) NO page shows ANY part knob (octave/legato/portamento/raga/
    //     polyphony/volume/tuning/spread are ALL table columns now — the
    //     completing absorption);
    // (b) the Global (Patch-hosted) page carries ONLY the Global group knobs
    //     (the compact "Part / Play" row is gone);
    // (c) no page renders a "Part / Play" group;
    // (d) the table cells drive the engine bytes through the real seams
    //     (incl. the SIGNED bytes 1 / 2).
    std::printf ("\n[21] Patch-page simplification (part settings placement)\n");
    {
        struct PageScan { juce::StringArray groups; juce::StringArray paramIds; };
        juce::Array<PageScan> scans;
        auto* parvatiEd = dynamic_cast<ParvatiEditor*> (editor);
        check (parvatiEd != nullptr, "[21] ParvatiEditor cast for allGeneratedPages");
        if (parvatiEd != nullptr)
        for (auto* page : parvatiEd->allGeneratedPages())
        {
            PageScan ps;
            for (auto* child : page->getChildren())
            {
                if (auto* pc = dynamic_cast<ParamControl*> (child))
                    ps.paramIds.add (pc->getParamID());
                if (auto* gc = dynamic_cast<juce::GroupComponent*> (child))
                    ps.groups.add (gc->getName());
            }
            scans.add (std::move (ps));
        }

        const PageScan* globalPage = nullptr;
        const PageScan* mixerPage  = nullptr;
        int absorbedKnobsAnywhere = 0;
        for (const auto& ps : scans)
        {
            if (ps.groups.contains ("Global"))
                globalPage = &ps;
            if (ps.groups.contains ("Mixer"))
                mixerPage = &ps;
            for (const char* id : { "part_octave", "part_legato", "part_portamento",
                                    "part_raga", "part_polyphony", "part_volume",
                                    "part_tuning", "part_spread" })
                if (ps.paramIds.contains (id)) ++absorbedKnobsAnywhere;
        }

        // Option-(2) placement (see audit/work_patch_page.md): volume /
        // tuning / spread stay on the PATCH-hosted Global page (the per-part
        // output stage sits directly above the part table); the Mixer page
        // gained NO part knobs (measured: no top-row slack for a panel).
        if (globalPage != nullptr)
        {
            check (globalPage->paramIds.contains ("vca_curve")
                   && globalPage->paramIds.contains ("filter_card")
                   && globalPage->paramIds.contains ("filter_drive"),
                   "[21] Global page keeps the three global-option knobs");
            int stray = 0;
            for (const auto& id : globalPage->paramIds)
                if (id.startsWith ("part_"))
                    ++stray;
            check (stray == 0,
                   "[21] Global page carries ONLY the global options (every part knob absorbed into the table)");
            check (globalPage->paramIds.size() == 3,
                   "[21] Global page is exactly the 3 global knobs (Part / Play row gone)");
        }
        if (mixerPage != nullptr)
        {
            int partKnobs = 0;
            for (const auto& id : mixerPage->paramIds)
                if (id.startsWith ("part_")) ++partKnobs;
            check (partKnobs == 0, "[21] Mixer page carries NO part_* knobs (top-row budget preserved)");
        }
        check (absorbedKnobsAnywhere == 0,
               "[21] no page generates the absorbed part knobs (table covers them)");
        // The dead "Part / Play" group must be gone from every page.
        int partPlayGroups = 0;
        for (const auto& ps : scans)
            if (ps.groups.contains ("Part / Play")) ++partPlayGroups;
        check (partPlayGroups == 0,
               "[21] no page renders a Part / Play group (the row is gone)");

        // (d) table cells drive the engine bytes (PartData 1 / 5 / 6).
        if (patchPage != nullptr)
        {
            auto& eng = proc.getEngine();
            patchPage->chooseOctave (0, 2);
            check (static_cast<int8_t> (eng.getPart (0).partBytes[1]) == 2,
                   "[21] Oct column writes PartData byte 1 (+2)");
            check (patchPage->getDisplayedOctave (0) == 2,
                   "[21] Oct column shows +2");
            patchPage->chooseOctave (1, -2);
            check (static_cast<int8_t> (eng.getPart (1).partBytes[1]) == -2,
                   "[21] Oct column writes the SIGNED byte (-2 -> 0xFE)");
            patchPage->chooseLegato (0, 1);
            check (eng.getPart (0).partBytes[5] == 1,
                   "[21] Lgo column writes PartData byte 5 (on)");
            check (patchPage->getDisplayedLegato (0) == 1,
                   "[21] Lgo column shows On");
            patchPage->choosePortamento (0, 40);
            check (eng.getPart (0).partBytes[6] == 40,
                   "[21] Porta column writes PartData byte 6 (40)");
            check (patchPage->getDisplayedPortamento (0) == 40,
                   "[21] Porta column shows 40");
            // APVTS re-sync: the current part's knobs must reflect the write
            // (the write path calls loadPartIntoApvts(currentPart)).
            check (juce::roundToInt (proc.getApvts().getRawParameterValue ("part_portamento")->load()) == 40,
                   "[21] Porta write re-syncs the part_portamento APVTS value");

            // (e) the output columns drive PartData bytes 0 / 2 / 3 (the
            // completing absorption; byte 2 is SIGNED int8).
            patchPage->chooseVolume (0, 96);
            check (eng.getPart (0).partBytes[0] == 96,
                   "[21] Vol column writes PartData byte 0 (96)");
            check (patchPage->getDisplayedVolume (0) == 96,
                   "[21] Vol column shows 96");
            check (juce::roundToInt (proc.getApvts().getRawParameterValue ("part_volume")->load()) == 96,
                   "[21] Vol write re-syncs the part_volume APVTS value");
            patchPage->chooseFineTune (2, 64);
            check (static_cast<int8_t> (eng.getPart (2).partBytes[2]) == 64,
                   "[21] Fine column writes SIGNED byte 2 (+64)");
            patchPage->chooseFineTune (3, -127);
            check (static_cast<int8_t> (eng.getPart (3).partBytes[2]) == -127,
                   "[21] Fine column writes the SIGNED byte (-127 -> 0x81)");
            check (patchPage->getDisplayedFineTune (3) == -127,
                   "[21] Fine column shows -127");
            patchPage->chooseSpread (1, 40);
            check (eng.getPart (1).partBytes[3] == 40,
                   "[21] Spr column writes PartData byte 3 (40 = max)");
            check (patchPage->getDisplayedSpread (1) == 40,
                   "[21] Spr column shows 40");
        }

        // The Patch-hosted Global page stays well-formed with the Global
        // panel + the merged table (all part knobs are table columns now).
        if (parvatiEd != nullptr)
        for (auto* page : parvatiEd->allGeneratedPages())
        {
            bool isGlobal = false;
            for (auto* child : page->getChildren())
                if (auto* gc = dynamic_cast<juce::GroupComponent*> (child))
                    if (gc->getName() == "Global") isGlobal = true;
            if (isGlobal)
                check (page->layoutIsSane(), "[21] Global (Patch-hosted) page layoutIsSane after slimming");
        }
    }

    // ---- [22] Patch-table column headers + tooltips ----
    // (a) the header strip exists with one localized caption per column, in
    //     column order (shared geometry — partColumnRects is the single source
    //     of truth, so this pins the caption count + order);
    // (b) every interactive cell of every row carries a NON-empty tooltip
    //     (the "patch table tooltips seem empty" regression class);
    // (c) the editor-wide tooltips gate blanks them (the ParamControl
    //     contract extended to the table).
    std::printf ("\n[22] Patch-table column headers + tooltips\n");
    if (patchPage != nullptr)
    {
        const auto labels = patchPage->headerLabelsForTest();
        // Voice tab (default): 9 visible columns after the 2026-08-20
        // follow-up regrouping (Part, Voices, Porta, Lgo, Vol, Fine, Spr,
        // Tune, Polyphony).
        check (labels.size() == 9, "[22] Voice-tab header has 9 column captions");
        check (labels[0]  == TRANS ("Part") && labels[1]  == TRANS ("Voices")
            && labels[7]  == TRANS ("Tune") && labels[8] == TRANS ("Polyphony"),
               "[22] Voice-tab header captions in column order");

        check (patchPage->tableTooltipsCompleteForTest(),
               "[22] every interactive table cell has a tooltip");

        ParamControl::setTooltipsEnabled (false);
        patchPage->setTableTooltipsEnabled (false);
        check (! patchPage->tableTooltipsCompleteForTest(),
               "[22] tooltips gate blanks the table cells");
        ParamControl::setTooltipsEnabled (true);
        patchPage->setTableTooltipsEnabled (true);
        check (patchPage->tableTooltipsCompleteForTest(),
               "[22] gate restore re-applies the tooltips");
    }
    else
        check (false, "[22] PatchPage reachable for header/tooltip checks");

    // ---- [23] Patch-table tabs: alignment + regrouped split + left tabs ----
    // (a) default tab = Voice; MIDI = the note-routing tab (Ch, zone, Oct,
    //     Polyphony) and Voice = the sound-shaping tab (Voices, Porta, Lgo,
    //     Vol, Fine, Spr, Tune) — the 2026-08-20 regrouping;
    // (b) header captions ALIGN with the row cells (per-column x equality:
    //     both consume partColumnRects over the same kTableContentInset
    //     band — the pre-fix header painted from the un-inset band and sat
    //     4px left of every column);
    // (c) the [Voice|MIDI] strip is the LEFTMOST control of the summary row;
    // (d) all choose*/getDisplayed* seams work on BOTH tabs (cells stay
    //     state-readable when hidden).
    std::printf ("\n[23] Patch-table tabs: alignment + regrouping\n");
    if (patchPage != nullptr)
    {
        check (patchPage->activeTableTabForTest() == 0, "[23] default tab is Voice");
        {
            const bool* m = patchPage->tableVisibleMaskForTest();
            //            Name    Voices  Ch      ZoneLo  ZoneHi
            const bool want[] = { true,  true,   false,  false,  false,
            //                    Oct     Porta   Lgo     Vol     Fine
                                  false,  true,   true,   true,   true,
            //                    Spr     Tune    Poly
                                  true,   true,   true };
            bool ok = true;
            for (int i = 0; i < 13; ++i) ok = ok && (m[i] == want[i]);
            check (ok, "[23] Voice-tab mask = sound-shaping columns");
        }

        // ---- (b) ALIGNMENT: per visible column, header x == row-0 cell x.
        {
            const bool* m = patchPage->tableVisibleMaskForTest();
            bool ok = true; int checked = 0;
            for (int i = 0; i < 13; ++i)
                if (m[i])
                {
                    ++checked;
                    ok = ok && (patchPage->headerColumnXForTest (i)
                                == patchPage->rowColumnXForTest (i));
                }
            check (checked >= 8, "[23] alignment pin covers the visible columns");
            check (ok, "[23] every Voice-tab caption x == its cell x (0px drift)");
        }

        // ---- (b2) LABEL BINDING: the caption PAINTED over each column is
        // that column's own caption (the 2026-08-20 regression indexed the
        // filtered caption list by the raw column index — "legato has tune
        // as the column header").
        {
            struct ColCap { int col; const char* key; };
            const ColCap voiceCols[] = {
                { 0, "Part" }, { 1, "Voices" }, { 6, "Portamento" }, { 7, "Legato" },
                { 8, "Volume" }, { 9, "Fine Tune" }, { 10, "Spread" }, { 11, "Tune" },
                { 12, "Polyphony" } };
            bool ok = true;
            for (const auto& cc : voiceCols)
                ok = ok && (patchPage->headerCaptionForTest (cc.col) == TRANS (cc.key));
            check (ok, "[23] Voice-tab caption over each column is its own (Lgo != Tune)");
        }

        // ---- (c) the tab strip leads the summary row (INSIDE the centred
        // table band: expected x = inset + centre-offset + 220 combo + 12
        // gap, within a small tolerance for the flex rounding).
        {
            const int centreOff = (patchPage->tableBandWidthForTest()
                                   - patchPage->tableContentWidthForTest()) / 2;
            const int expectX  = 4 + centreOff + 220 + 12;
            check (std::abs (patchPage->tabStripXForTest() - expectX) <= 4,
                   "[23] [Voice|MIDI] strip sits right of the arrangement combo");
        }

        // Seams on the Voice tab (the default): a write + read-back.
        patchPage->choosePortamento (0, 33);
        check (patchPage->getDisplayedPortamento (0) == 33,
               "[23] Porta seam works on the Voice tab");

        patchPage->chooseTableTabForTest (1);   // -> MIDI
        check (patchPage->activeTableTabForTest() == 1, "[23] toggle switches to MIDI");
        {
            const bool* m = patchPage->tableVisibleMaskForTest();
            //            Name    Voices  Ch      ZoneLo  ZoneHi
            const bool want[] = { true,  false,  true,   true,   true,
            //                    Oct     Porta   Lgo     Vol     Fine
                                  true,   false,  false,  false,  false,
            //                    Spr     Tune    Poly
                                  false,  false,  false };
            bool ok = true;
            for (int i = 0; i < 13; ++i) ok = ok && (m[i] == want[i]);
            check (ok, "[23] MIDI-tab mask = note-routing columns (Ch/Zones/Oct)");
        }
        const auto midiLabels = patchPage->headerLabelsForTest();
        check (midiLabels.size() == 5, "[23] MIDI-tab header has 5 captions");
        check (midiLabels[1] == TRANS ("Channel") && midiLabels[4] == TRANS ("Octave"),
               "[23] MIDI-tab header order Part/Ch/Zone/Zone/Oct");

        // Alignment pin on the MIDI tab too (the geometry re-distributes).
        {
            const bool* m = patchPage->tableVisibleMaskForTest();
            bool ok = true;
            for (int i = 0; i < 13; ++i)
                if (m[i])
                    ok = ok && (patchPage->headerColumnXForTest (i)
                                == patchPage->rowColumnXForTest (i));
            check (ok, "[23] every MIDI-tab caption x == its cell x (0px drift)");
        }

        // ---- (c) FIXED CENTRED WIDTHS: on a wide editor the table content
        // stops at the column maximums and CENTRES (the combos never stretch
        // to full width; at/below the 1024 floor the flex minimums still fill
        // the band, so nothing shrinks).
        const int prevW = editor->getWidth();
        const int prevH = editor->getHeight();
        editor->setSize (1600, 900);
        for (int i = 0; i < 4; ++i) { juce::Thread::sleep (2); juce::Timer::callPendingTimersSynchronously(); }
        {
            const int bandW = patchPage->tableBandWidthForTest();
            const int contW = patchPage->tableContentWidthForTest();
            // Voice tab maxes (kColumnSpecs): Name 160 + Voices 90 + Porta 88
            // + Lgo 90 + Vol 72 + Fine 80 + Spr 72 + Tune 170 + Poly 150
            // + 8 gaps x 4 — a PIN: changing a max without updating this
            // fails loudly so the policy cannot silently drift.
            constexpr int kVoiceTabContentMax = 160 + 90 + 88 + 90 + 72 + 80
                                                + 72 + 170 + 150 + 8 * 4;
            check (bandW > contW, "[23] at 1600pt the table band is wider than the content");
            check (contW <= kVoiceTabContentMax,
                   "[23] content width <= sum of column maximums + gaps");
            check (contW > 0 && (bandW - contW) / 2 >= 0,
                   "[23] centring offset >= 0 (table centred in the band)");
        }
        editor->setSize (prevW, prevH);
        for (int i = 0; i < 4; ++i) { juce::Thread::sleep (2); juce::Timer::callPendingTimersSynchronously(); }

        // MIDI-tab label binding (the same regression class).
        {
            struct ColCap { int col; const char* key; };
            const ColCap midiCols[] = {
                { 0, "Part" }, { 2, "Channel" }, { 3, "Zone Low" },
                { 4, "Zone High" }, { 5, "Octave" } };
            bool ok = true;
            for (const auto& cc : midiCols)
                ok = ok && (patchPage->headerCaptionForTest (cc.col) == TRANS (cc.key));
            check (ok, "[23] MIDI-tab caption over each column is its own");
        }

        // Hidden-cell seam: Porta is HIDDEN on the MIDI tab but still writable.
        patchPage->choosePortamento (0, 21);
        check (patchPage->getDisplayedPortamento (0) == 21,
               "[23] hidden-cell seam still works on the MIDI tab");
        // Hidden-cell seam on the other side: Oct is Voice-hidden here.
        patchPage->chooseOctave (1, -1);
        check (static_cast<int8_t> (proc.getEngine().getPart (1).partBytes[1]) == -1,
               "[23] hidden Oct column still writes the engine byte");
        patchPage->chooseTableTabForTest (0);   // back to Voice
        check (patchPage->getDisplayedPortamento (0) == 21,
               "[23] back on Voice, the MIDI-tab write persisted");
        check (patchPage->getDisplayedOctave (1) == -1,
               "[23] back on Voice, the MIDI-tab Oct write shows");
    }
    else
        check (false, "[23] PatchPage reachable for tab checks");

    // ---- [24] Load/Save default to .parvati; Patch-page Ambika export ----
    // (a) the top-bar Load/Save buttons exist with the direct .parvati
    //     semantics — clicking headless completes synchronously (the desktop
    //     gate means no picker/menu launches; an un-gated menu/picker would
    //     either hang the pump or create desktop chrome);
    // (b) the Patch page carries the two export buttons (findable, enabled,
    //     tooltiped) and the click seams fire the editor's wiring.
    std::printf ("\n[24] parvati-first Load/Save + Ambika export buttons\n");
    {
        juce::TextButton* loadBtn = nullptr;
        juce::TextButton* saveBtn = nullptr;
        {
            juce::Array<juce::Component*> nodes { editor };
            for (int i = 0; i < nodes.size(); ++i)
            {
                auto* c = nodes.getUnchecked (i);
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    if (b->getButtonText() == TRANS ("Load")) loadBtn = b;
                    if (b->getButtonText() == TRANS ("Save")) saveBtn = b;
                }
                for (auto* ch : c->getChildren()) nodes.add (ch);
            }
        }
        check (loadBtn != nullptr && saveBtn != nullptr,
               "[24] Load + Save buttons found");
        if (loadBtn != nullptr && loadBtn->onClick != nullptr)
        {
            loadBtn->onClick();   // headless: desktop-gated -> returns, no picker
            check (true, "[24] Load click completes headless (desktop-gated picker)");
        }
        else check (false, "[24] Load button has an onClick");
        if (saveBtn != nullptr && saveBtn->onClick != nullptr)
        {
            saveBtn->onClick();   // direct .parvati save, desktop-gated
            check (true, "[24] Save click completes headless (direct .parvati, no menu)");
        }
        else check (false, "[24] Save button has an onClick");

        if (patchPage != nullptr)
        {
            juce::TextButton* proBtn = nullptr;
            juce::TextButton* mulBtn = nullptr;
            juce::Array<juce::Component*> nodes { patchPage };
            for (int i = 0; i < nodes.size(); ++i)
            {
                auto* c = nodes.getUnchecked (i);
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    if (b->getButtonText() == TRANS ("Export .PRO")) proBtn = b;
                    if (b->getButtonText() == TRANS ("Export .MUL")) mulBtn = b;
                }
                for (auto* ch : c->getChildren()) nodes.add (ch);
            }
            check (proBtn != nullptr && mulBtn != nullptr,
                   "[24] Export .PRO + Export .MUL buttons on the Patch page");
            check (proBtn == nullptr || (proBtn->isEnabled() && proBtn->getWidth() >= 44 && proBtn->getHeight() >= 20),
                   "[24] .PRO button enabled + sized");
            check (mulBtn == nullptr || (mulBtn->isEnabled() && mulBtn->getWidth() >= 44 && mulBtn->getHeight() >= 20),
                   "[24] .MUL button enabled + sized");
            check (patchPage->exportProTooltipForTest().isNotEmpty()
                   && patchPage->exportMulTooltipForTest().isNotEmpty(),
                   "[24] export tooltips set (format trade-offs)");
            check (patchPage->exportMulTooltipForTest().contains ("fallback"),
                   "[24] .MUL tooltip mentions the fallback dialog");

            // The editor wired the seams: fire them via the test hooks with
            // SWAPPED callbacks (flag pattern), then restore the editor's.
            check ((bool) patchPage->onExportPro && (bool) patchPage->onExportMul,
                   "[24] editor wired the export callbacks");
            int proFired = 0, mulFired = 0;
            auto savedPro = patchPage->onExportPro;
            auto savedMul = patchPage->onExportMul;
            patchPage->onExportPro = [&proFired] { ++proFired; };
            patchPage->onExportMul = [&mulFired] { ++mulFired; };
            patchPage->clickExportProForTest();
            patchPage->clickExportMulForTest();
            patchPage->onExportPro = savedPro;
            patchPage->onExportMul = savedMul;
            check (proFired == 1 && mulFired == 1,
                   "[24] export click seams fire exactly once");
            // The real (restored) wiring is desktop-gated: firing headless is a
            // no-op pick — proves no un-gated picker path hangs the run.
            patchPage->clickExportProForTest();
            patchPage->clickExportMulForTest();
            check (true, "[24] real export wiring completes headless (gated pickers)");
        }
        else
            check (false, "[24] PatchPage reachable for export checks");
    }

    // ==================================================================
    // [25] Live mod-feedback UI: mod-pill telemetry strips, preview stage
    //      markers / live filter overlay, and the Visual Refresh preference
    //      (docs/LIVE_MOD_FEEDBACK_DESIGN.md, the UI half of the contract).
    //
    //      (a) CentralModBar: the editor wired the strip poll (its rate
    //          follows the persisted pref); the synthetic-provider path feeds
    //          the per-pill DIFF GATE (moving history -> a bounded strip
    //          repaint / generation bump; constant history -> ZERO repaints —
    //          the idle GPU-cost control), an invalid frame clears the strips
    //          exactly once, and setTelemetryRateHz clamps (0 = disable,
    //          5..60 otherwise).
    //      (b) EnvelopeDisplay: the live stage marker appears INSIDE the
    //          attack span, rests at the SUSTAIN plateau start (the pin), and
    //          hides on inactive.
    //      (c) FilterResponseDisplay: the live overlay curve appears only
    //          when the effective bytes DEPART from the base knob bytes
    //          (>= 2), and its cutoff tick follows the live frequency.
    //      (d) The Visual Refresh preference round-trips + clamps, and the
    //          Settings combo mirrors it and routes a pick through onChange.
    //
    //      Timer reality headless: the bar's strip poll is gated on
    //      isShowing() (the F-ios-perf-3 dual-hook gate), so its data-driven
    //      path runs on a short-lived OFF-SCREEN desktop peer (created only
    //      when the environment can; otherwise the check degrades to the
    //      deterministic seams below). The preview displays' ctor-started
    //      30 Hz polls keep running for a NEVER-PARENTED component (the gate
    //      re-evaluates only on visibility/hierarchy CHANGES, and neither
    //      hook fires without a parent), so (b)/(c) drive them through the
    //      CFRunLoop pump with no desktop chrome at all.
    // ==================================================================
    std::printf ("\n[25] live mod-feedback UI (strips / markers / live curve / refresh pref)\n");
    {
        auto* ed25 = dynamic_cast<ParvatiEditor*> (editor);
        CentralModBar* editorBar = (ed25 != nullptr && ed25->getSynthWorkspaceForTest() != nullptr)
            ? ed25->getSynthWorkspaceForTest()->modBar() : nullptr;
        check (editorBar != nullptr, "[25] editor mod bar reachable via the synth workspace");
        {
            char msg25[160];
            std::snprintf (msg25, sizeof (msg25),
                           "[25] editor bar strip rate follows the persisted pref (got %d, want %d)",
                           editorBar != nullptr ? editorBar->telemetryRateHz() : -1,
                           proc.getUiRefreshHz());
            check (editorBar != nullptr && editorBar->telemetryRateHz() == proc.getUiRefreshHz(), msg25);
        }

        // -- (a1) deterministic seams: rate clamping + clear idempotence --
        {
            ThemeManager themeMgr25;
            CentralModBar bar (themeMgr25);
            bar.setBounds (0, 0, 900, CentralModBar::kBarHeight);
            check (bar.telemetryRateHz() == 30, "[25] standalone bar defaults to a 30 Hz strip poll");
            bar.setTelemetryRateHz (200);
            check (bar.telemetryRateHz() == 60, "[25] setTelemetryRateHz(200) clamps to 60");
            bar.setTelemetryRateHz (1);
            check (bar.telemetryRateHz() == 5,  "[25] setTelemetryRateHz(1) clamps to 5");
            bar.setTelemetryRateHz (0);
            check (bar.telemetryRateHz() == 0,  "[25] setTelemetryRateHz(0) is the disable sentinel");
            bar.setTelemetryRateHz (30);
            const int genClear = bar.telemetryGeneration();
            bar.clearTelemetry();
            check (bar.telemetryGeneration() == genClear,
                   "[25] clearTelemetry on an already-clear bar bumps no generation");
        }

#if defined (__APPLE__)
        // -- (a2) data-driven strip path: a synthetic provider feeds the diff
        //    gate through the REAL bar timer on an off-screen peer --
        struct SyntheticTelemetry
        {
            int  call   = 0;
            bool moving = true;
            bool valid  = true;
            bool noteMoving = false;   // the NOTE-sentinel melody (slot 31) scrolls
            parvati::ModTelemetrySnapshot snap {};

            SyntheticTelemetry()
            {
                snap.epoch = 1;
                snap.part  = 0;
                snap.historyCount = 64;
                // VELOCITY: a constant pattern — it must NEVER trip the gate.
                uint8_t* vel = snap.history
                    + (size_t) ambika::dsp::MOD_SRC_VELOCITY * parvati::ModTelemetrySnapshot::kHistoryLen;
                for (int i = 0; i < 64; ++i) vel[(size_t) i] = 200;
            }

            bool operator() (parvati::ModTelemetrySnapshot& out)
            {
                if (! valid)
                    return false;   // a torn read / stale reset epoch
                // LFO 1: a scrolling ramp — the downsampled first/last/min/max
                // signature moves on every call while `moving`.
                uint8_t* lfo = snap.history
                    + (size_t) ambika::dsp::MOD_SRC_LFO_1 * parvati::ModTelemetrySnapshot::kHistoryLen;
                for (int i = 0; i < 64; ++i)
                    lfo[(size_t) i] = static_cast<uint8_t> ((i * 4 + (moving ? call * 8 : 0)) & 0xff);
                snap.sources[ambika::dsp::MOD_SRC_LFO_1] = lfo[63];
                // NOTE-SEQ (spare slot): a scrolling melody while noteMoving —
                // proves the bar-only sentinel pill consumes kNoteSeqSlot.
                uint8_t* nseq = snap.history
                    + (size_t) parvati::ModTelemetrySnapshot::kNoteSeqSlot * parvati::ModTelemetrySnapshot::kHistoryLen;
                for (int i = 0; i < 64; ++i)
                    nseq[(size_t) i] = static_cast<uint8_t> ((i * 4 + (noteMoving ? call * 8 : 0)) & 0xff);
                snap.sources[(size_t) parvati::ModTelemetrySnapshot::kNoteSeqSlot] = nseq[63];
                ++call;
                out = snap;
                return true;
            }
        };
        {
            ThemeManager themeMgr25;
            CentralModBar bar (themeMgr25);
            bar.setBounds (-1400, -1400, 900, CentralModBar::kBarHeight);   // off-screen probe peer
            bar.setVisible (true);   // a fresh JUCE Component is born HIDDEN — isShowing() needs this
            SyntheticTelemetry synth;
            bar.setTelemetryProvider ([&synth] (parvati::ModTelemetrySnapshot& s) { return synth (s); });
            bar.setTelemetryRateHz (60);
            // A headless host cannot create the off-screen probe peer; the
            // isShowing() gate below then skips the checks. macOS always
            // reports a display.
            if (displayAvailable())
                bar.addToDesktop (0);   // borderless, taskbar-less — parentHierarchyChanged starts the poll
            if (bar.isShowing())
            {
                // Moving history: the strip data changes each tick, so the
                // generation counter climbs (bounded strip-rect repaints).
                const int gen0 = bar.telemetryGeneration();
                pumpUntil25 ([&] { return bar.telemetryGeneration() > gen0; });
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] moving history animates the strips (gen %d -> %d)",
                               gen0, bar.telemetryGeneration());
                check (bar.telemetryGeneration() > gen0, msg25);

                // Constant history: after the LAST moving frame's signature
                // settles, the diff gate must go COMPLETELY silent.
                synth.moving = false;
                pumpUntil25 ([&] { return false; }, 0.25);   // let any transition tick land
                const int gen1 = bar.telemetryGeneration();
                pumpUntil25 ([&] { return false; }, 0.45);   // parked window: must stay silent
                std::snprintf (msg25, sizeof (msg25),
                               "[25] constant history repaints NOTHING (gen %d -> %d)",
                               gen1, bar.telemetryGeneration());
                check (bar.telemetryGeneration() == gen1, msg25);

                // Note-Sequencer sentinel: with EVERY real source parked, only
                // the spare kNoteSeqSlot scrolls (a melody trace). A climbing
                // generation proves the bar-only NOTE pill consumes the slot.
                synth.noteMoving = true;
                const int genN0 = bar.telemetryGeneration();
                pumpUntil25 ([&] { return bar.telemetryGeneration() > genN0; });
                std::snprintf (msg25, sizeof (msg25),
                               "[25] note-seq melody on the spare slot animates the NOTE pill (gen %d -> %d)",
                               genN0, bar.telemetryGeneration());
                check (bar.telemetryGeneration() > genN0, msg25);
                synth.noteMoving = false;   // park again for the invalid-frame stage
                pumpUntil25 ([&] { return false; }, 0.20);   // settle

                // An invalid frame (a torn seqlock read / a stale reset epoch)
                // hides the strips ONCE; further invalid frames stay silent.
                synth.valid = false;
                const int gen2 = bar.telemetryGeneration();
                pumpUntil25 ([&] { return bar.telemetryGeneration() > gen2; });
                check (bar.telemetryGeneration() > gen2,
                       "[25] invalid frame clears the strips (one generation bump)");
                const int gen3 = bar.telemetryGeneration();
                pumpUntil25 ([&] { return false; }, 0.30);
                check (bar.telemetryGeneration() == gen3,
                       "[25] already-cleared strips stay clear (no repeated bumps)");
            }
            else
            {
                std::printf ("  skip: [25] no desktop peer available — strip data-driven path not exercised\n");
            }
            bar.setVisible (false);   // hide BEFORE removing the peer (a visible off-screen window can steal focus on some hosts)
            bar.removeFromDesktop();
        }

        // -- (a3) END-TO-END: real processor + real editor in a REAL window,
        //    engine rendered with a held note — the whole pipeline
        //    (renderPartFx telemetry -> LiveFeedbackHub -> bar strip diff
        //    gate) must animate the EDITOR'S OWN bar. This is the Standalone
        //    scenario verbatim; the earlier synthetic checks fed the bar
        //    directly and could not catch a dead hub/pump between engine and
        //    bar (the exact bug class that shipped invisibly: the hub was
        //    gated on the editor's visibilityChanged alone, which never fires
        //    again once the window gains its peer). --
        {
            ParvatiAudioProcessor e2eProc;
            e2eProc.prepareToPlay (48000.0, 256);
            setInt (e2eProc, "env1_lfo_rate", 60);      // free-running LFO 1: the strip data moves
            e2eProc.syncAllParamsToEngine();
            auto* e2eEd = dynamic_cast<ParvatiEditor*> (e2eProc.createEditor());
            check (e2eEd != nullptr, "[25] e2e: editor created");
            CentralModBar* e2eBar = nullptr;
            // The e2e animation check needs the real peer (the hub pump
            // starts on visibility). A headless host cannot create one.
            const bool e2eDesktop = displayAvailable();
            if (! e2eDesktop)
                std::printf ("  SKIP [25] e2e: no display server (headless host)\n");
            if (e2eEd != nullptr && e2eDesktop)
            {
                e2eEd->setSize (1280, 634);
                auto win2 = std::make_unique<juce::DocumentWindow> ("E2E",
                    juce::Colours::black, juce::DocumentWindow::allButtons);
                win2->setUsingNativeTitleBar (true);
                win2->setContentNonOwned (e2eEd, false);   // content parented while the window has no peer yet
                win2->centreWithSize (1280, 700);
                // The REAL standalone ordering (juce_StandaloneFilterWindow): the
                // DocumentWindow base ctor desktops the window (peer exists,
                // still invisible), then initialise() calls setVisible(true) —
                // which recurses visibilityChanged down through the content with
                // the peer already present, so isShowing() flips true inside the
                // hooks. (The REVERSED order — visible-then-desktop — fires the
                // children's hooks pre-peer and never again: a known JUCE gap,
                // and exactly why the harness must mirror the real order.)
                win2->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
                win2->setVisible (true);
                e2eBar = (e2eEd->getSynthWorkspaceForTest() != nullptr)
                    ? e2eEd->getSynthWorkspaceForTest()->modBar() : nullptr;
            }
            if (e2eDesktop)
                check (e2eBar != nullptr, "[25] e2e: editor bar reachable");
            else
                std::printf ("       (bar-reachable check skipped: no display)\n");
            if (e2eBar != nullptr)
            {
                // Render a held note through the REAL audio path (the message
                // thread calling processBlock is the established test idiom).
                const auto on2 = juce::MidiMessage::noteOn (1, 60, 0.9f);
                juce::AudioBuffer<float> e2eBuf (2, 256);
                const int gen0 = e2eBar->telemetryGeneration();
                for (int b = 0; b < 160; ++b)   // ~850 ms held
                {
                    juce::MidiBuffer m;
                    if (b == 0) m.addEvent (on2, 0);
                    e2eBuf.clear();
                    e2eProc.processBlock (e2eBuf, m);
                    if (b % 16 == 0)
                    {
                        // Pump-with-deadline INSIDE the render window (the
                        // telemetry only scrolls while blocks flow): stop as
                        // soon as the bar has visibly animated — immune to the
                        // run-loop throttling that starved fixed pumps.
                        pumpUntil25 ([&] { return e2eBar->telemetryGeneration() > gen0; }, 0.080);
                        if (e2eBar->telemetryGeneration() > gen0)
                            break;
                    }
                }
                pumpUntil25 ([&] { return e2eBar->telemetryGeneration() > gen0; });   // hub + bar ticks at 30 Hz
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] e2e: engine -> hub -> editor bar animates (gen %d -> %d)",
                               gen0, e2eBar->telemetryGeneration());
                check (e2eBar->telemetryGeneration() > gen0, msg25);
            }
            if (e2eEd != nullptr)
            {
                e2eEd->removeFromDesktop();
                delete e2eEd;
            }
        }

        // -- (b) EnvelopeDisplay live stage marker (standalone, never parented:
        //    the ctor-started 30 Hz poll keeps ticking; the pump delivers it) --
        {
            // Boundaries DERIVED from the display's own span definition (the
            // 2026-08-22 time-honest geometry: LUT-duration spans + sustain
            // share, remapped through the edge padding) so this pin cannot
            // drift when the geometry changes.
            float wA25 = 0.0f, wD25 = 0.0f, wS25 = 0.0f, wR25 = 0.0f;
            EnvelopeDisplay::adsrSegmentSpansForTest (0.5f, 0.3f, 0.7f, 0.4f,
                                                      &wA25, &wD25, &wS25, &wR25);
            const float kTotal25   = wA25 + wD25 + wS25 + wR25;
            const float kPad25     = EnvelopeDisplay::kEdgePad;
            const float kSpan25    = 1.0f - 2.0f * kPad25;
            const float kAttackEnd = kPad25 + (wA25 / kTotal25) * kSpan25;
            const float kPlateau25 = kPad25 + ((wA25 + wD25) / kTotal25) * kSpan25;

            parvati::LiveEnvStage stage;   // provider-owned state, mutated between phases
            EnvelopeDisplay disp ("Env 25",
                [] { return 0.5f; }, [] { return 0.3f; }, [] { return 0.7f; }, [] { return 0.4f; });
            disp.setBounds (0, 0, 200, 80);
            disp.setLiveStageProvider ([&stage] { return stage; });

            // Each phase pumps UNTIL the marker reaches the expected span
            // (deadlines absorb the throttled-run-loop jitter; a healthy run
            // satisfies each within a tick or two).
            stage = { true, 0, 0.25f };   // ATTACK, a quarter through
            pumpUntil25 ([&] { return disp.liveMarkerVisibleForTest()
                                  && disp.liveMarkerXForTest() > 0.0f
                                  && disp.liveMarkerXForTest() < kAttackEnd; });
            {
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] env marker visible in ATTACK (visible=%d)",
                               (int) disp.liveMarkerVisibleForTest());
                check (disp.liveMarkerVisibleForTest(), msg25);
                std::snprintf (msg25, sizeof (msg25),
                               "[25] env marker x inside the attack span (x=%.3f, span 0..%.3f)",
                               disp.liveMarkerXForTest(), kAttackEnd);
                check (disp.liveMarkerXForTest() > 0.0f && disp.liveMarkerXForTest() < kAttackEnd, msg25);
            }

            stage = { true, 1, 0.5f };    // DECAY, halfway: attack-end..plateau
            pumpUntil25 ([&] { return disp.liveMarkerVisibleForTest()
                                  && disp.liveMarkerXForTest() > kAttackEnd
                                  && disp.liveMarkerXForTest() < kPlateau25; });
            {
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] env marker inside the decay span (x=%.3f, span %.3f..%.3f)",
                               disp.liveMarkerXForTest(), kAttackEnd, kPlateau25);
                check (disp.liveMarkerXForTest() > kAttackEnd
                       && disp.liveMarkerXForTest() < kPlateau25, msg25);
            }

            stage = { true, 2, 1.0f };    // SUSTAIN: pinned at the plateau START
            pumpUntil25 ([&] { return disp.liveMarkerVisibleForTest()
                                  && std::fabs (disp.liveMarkerXForTest() - kPlateau25) < 0.01f; });
            {
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] env marker rests at the sustain plateau start (x=%.3f, want %.3f)",
                               disp.liveMarkerXForTest(), kPlateau25);
                check (std::fabs (disp.liveMarkerXForTest() - kPlateau25) < 0.01f, msg25);
            }

            stage = { false, 2, 1.0f };   // inactive (key released): hidden
            pumpUntil25 ([&] { return ! disp.liveMarkerVisibleForTest(); });
            check (! disp.liveMarkerVisibleForTest(), "[25] env marker hides when the provider goes inactive");
        }

        // -- (c) FilterResponseDisplay live modulation overlay (same
        //    never-parented poll technique) --
        {
            // PROVIDER (pump-starvation-proof, 2026-08-21): the harness's
            // CFRunLoop pumps occasionally deliver ZERO 30 Hz ticks in a 150 ms
            // window (a run-loop scheduling quirk under load — this section
            // flaked with the x=0.000 "never ticked" signature). Every
            // "visible" phase therefore drives a CONTINUOUSLY MOVING value
            // (the temporal hold can never expire mid-pump), and every
            // "settled" phase pumps well past the hold window.
            struct LiveFilt25
            {
                bool  active = true;
                bool  moving = false;
                float frozen = 0.8f;      // the settled value while !moving
                int   call   = 0;
                parvati::LiveFilterValues operator()()
                {
                    if (! moving) return { active, frozen, 0.2f };
                    // A slow cutoff sweep around 0.7: bytes move EVERY tick.
                    const float c = 0.7f + 0.15f * std::sin (0.4f * (float) (++call));
                    return { active, c, 0.2f };
                }
            };
            LiveFilt25 lv;
            FilterResponseDisplay disp ("Filter 25",
                [] { return 0.5f; },   // base cutoff  -> byte 128
                [] { return 0.2f; },   // base resonance
                [] { return 0.0f; });  // LP
            disp.setBounds (0, 0, 220, 64);
            disp.setLiveValuesProvider ([&lv] { return lv(); });

            // (1) Modulated (moving): the overlay shows and STAYS shown while
            //     the bytes move every tick (the temporal gate re-arms each
            //     tick; pump length no longer matters).
            lv.moving = true;
            pumpUntil25 ([&] { return disp.liveCurveVisibleForTest(); });
            check (disp.liveCurveVisibleForTest(),
                   "[25] filter live curve visible under modulation (moving)");

            // (2) Frozen at a known byte (204 = 0.8): the tick-x mapping is
            //     exact (the freeze itself is a movement that re-arms the hold,
            //     so the exact-x assert lands inside the window).
            lv.moving = false;   // frozen = 0.8 -> byte 204
            pumpUntil25 ([&] { return std::fabs (disp.liveCutoffXForTest() - 204.0f / 255.0f) < 0.01f; });
            {
                char msg25[160];
                std::snprintf (msg25, sizeof (msg25),
                               "[25] live cutoff tick right of the base tick (x=%.3f > %.3f)",
                               disp.liveCutoffXForTest(), 128.0f / 255.0f);
                check (disp.liveCutoffXForTest() > 128.0f / 255.0f
                       && std::fabs (disp.liveCutoffXForTest() - 204.0f / 255.0f) < 0.01f, msg25);
            }

            // (3) Settled (TEMPORAL gate): static bytes -> the hold expires and
            //     the overlay hides (pump well past the ~270 ms budget).
            lv.frozen = 0.5f;    // a fresh one-step movement, then still
            pumpUntil25 ([&] { return ! disp.liveCurveVisibleForTest(); }, 2.0);
            check (! disp.liveCurveVisibleForTest(),
                   "[25] filter overlay hides once the live values settle (hold expired)");

            // (4) The KEY-TRACKING regression pin: a live value STATICALLY far
            //     from the knob base (what key tracking does to every held
            //     note) shows only while MOVING, never while settled (the old
            //     spatial >= 2-byte threshold tripped on exactly this).
            const bool visibleAfterStatic = [&]
            {
                lv.moving  = true;   // movement: visible...
                pumpUntil25 ([&] { return disp.liveCurveVisibleForTest(); });
                const bool duringHold = disp.liveCurveVisibleForTest();
                lv.moving  = false;   // ...then STATICALLY far from the base: hidden
                lv.frozen  = 0.8f;
                pumpUntil25 ([&] { return ! disp.liveCurveVisibleForTest(); }, 2.0);
                return duringHold && ! disp.liveCurveVisibleForTest();
            }();
            check (visibleAfterStatic,
                   "[25] static live-vs-base offset (key tracking) shows only during the movement, not while settled");
        }
#endif  // __APPLE__ (pump-driven [25] sub-checks; the seams above ran everywhere)

        // -- (d) Visual Refresh preference: round-trip + clamp + Settings combo --
        {
            proc.setUiRefreshHz (60);
            check (proc.getUiRefreshHz() == 60, "[25] setUiRefreshHz(60) round-trips");
            proc.setUiRefreshHz (0);
            check (proc.getUiRefreshHz() == 5,  "[25] setUiRefreshHz(0) clamps to 5");
            proc.setUiRefreshHz (500);
            check (proc.getUiRefreshHz() == 60, "[25] setUiRefreshHz(500) clamps to 60");
            proc.setUiRefreshHz (30);   // restore the default for everything after

            ThemeManager themeMgr25;
            ParvatiAudioProcessor settingsProc25;
            settingsProc25.prepareToPlay (48000.0, 256);
            int refreshCbSum = 0;
            SettingsPanel panel25 (settingsProc25, themeMgr25,
                                   [] (double) {}, [] (bool) {}, [] (bool) {}, [] (int) {},
                                   [] (const juce::String&) {},
                                   [&] (int hz) { refreshCbSum += hz; });
            panel25.setBounds (0, 0, 420, 360);

            // Locate the refresh combo by its stable item IDs (10/15/30/60 —
            // the Hz values themselves; theme/lang/os combos use 1..N).
            juce::ComboBox* refreshCombo = nullptr;
            {
                juce::Array<juce::Component*> nodes { &panel25 };
                for (int i = 0; i < nodes.size(); ++i)
                {
                    auto* c = nodes.getUnchecked (i);
                    if (auto* cb = dynamic_cast<juce::ComboBox*> (c))
                        if (cb->getNumItems() == 4 && cb->getItemId (0) == 10 && cb->getItemId (3) == 60)
                            refreshCombo = cb;
                    for (auto* ch : c->getChildren()) nodes.add (ch);
                }
            }
            check (refreshCombo != nullptr, "[25] Visual Refresh combo found (item ids 10/15/30/60)");
            if (refreshCombo != nullptr)
            {
                check (refreshCombo->getSelectedId() == 30,
                       "[25] refresh combo mirrors the persisted 30 Hz default");
                refreshCombo->setSelectedId (15, juce::sendNotificationSync);
                check (settingsProc25.getUiRefreshHz() == 15,
                       "[25] picking 15 Hz persists through the processor");
                check (refreshCbSum == 15,
                       "[25] picking 15 Hz fires the editor hook exactly once");
            }
        }
    }

    // ---- teardown ----
    delete editor;

    // ---- [19] preview-display update regression (headless always; windowed
    // via --windowed for the real on-desktop visibility path) ----
    std::printf ("\n[19] Preview-update regression (headless)\n");
    runPreviewRegression (false);
    // (--windowed was argv[1] in the standalone binary; the unified runner
    // runs the headless path only.)
    std::printf ("\n[19] Preview-update regression (headless)\n");
    runPreviewRegression (false);

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "EDITOR TEST: FAILURES" : "EDITOR TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

// ============================================================================
// [19] Preview-display update regression (osc waveform / ADSR / filter curve)
// ----------------------------------------------------------------------------
// Each preview owns a 30 Hz poll timer with an eps-diff gate. A param change
// MUST produce a refresh (generation bump) within ~1 s of message pumping.
// Run WINDOWED (argv[1] == "--windowed") to exercise the real on-desktop
// visibility path as well (the Standalone scenario).
// ============================================================================
static int runPreviewRegression (bool windowed)
{
    juce::ScopedJuceInitialiser_GUI gui;
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    auto* ed = dynamic_cast<ParvatiEditor*> (proc.createEditor());
    if (ed == nullptr)
    {
        std::printf ("  FAIL: createEditor() -> ParvatiEditor\n");
        return 1;
    }
    ed->setSize (1280, 634);

    // Optional: host the editor in a real on-desktop window (the Standalone
    // scenario — a REAL peer changes isShowing() semantics for the preview
    // timers gated in visibilityChanged). A headless host cannot create the
    // peer, so the windowed path needs a display.
    std::unique_ptr<juce::DocumentWindow> win;
    if (windowed && displayAvailable())
    {
        win = std::make_unique<juce::DocumentWindow> ("PreviewProbe",
            juce::Colours::black, juce::DocumentWindow::allButtons);
        win->setUsingNativeTitleBar (true);
        win->setContentNonOwned (ed, false);
        win->centreWithSize (1280, 700);
        win->setVisible (true);
        win->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
    }

    auto pump = [] (int ms)
    {
#if defined (__APPLE__)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001 * ms, false);
#else
        juce::Thread::sleep (ms);
#endif
    };

    // Locate the preview components through the generated pages.
    OscPreviewDisplay*  oscPrev[2]  = {};
    EnvelopeDisplay*    envPrev     = nullptr;
    FilterResponseDisplay* filtPrev = nullptr;
    for (auto* page : ed->allGeneratedPages())
    {
        if (auto* c = dynamic_cast<OscPreviewDisplay*> (page->getGroupInlinePreviewForTest ("Osc 1")))
            oscPrev[0] = c;
        if (auto* c = dynamic_cast<OscPreviewDisplay*> (page->getGroupInlinePreviewForTest ("Osc 2")))
            oscPrev[1] = c;
        if (auto* c = dynamic_cast<EnvelopeDisplay*> (page->getGroupDecorationForTest ("Env 1 (Mod)")))
            envPrev = c;
        if (auto* c = dynamic_cast<FilterResponseDisplay*> (page->getGroupDecorationForTest ("Filter")))
            filtPrev = c;
    }

    const char* mode = windowed ? "windowed" : "headless";
    check (oscPrev[0] != nullptr && oscPrev[1] != nullptr,
           (std::string ("[19] ") + mode + ": both Osc previews found").c_str());
    check (envPrev != nullptr,
           (std::string ("[19] ") + mode + ": Env 1 ADSR preview found").c_str());
    check (filtPrev != nullptr,
           (std::string ("[19] ") + mode + ": Filter response preview found").c_str());

    // HEADLESS semantics pin: with no desktop peer, isShowing() is false for
    // the whole hierarchy, so the F-ios-perf-3 gate keeps the 30 Hz poll
    // STOPPED (getTimerInterval()==0) — no battery/CPU burn for an invisible
    // UI. (This is the deliberate behaviour, not the frozen-preview bug: the
    // bug was that the timers never RESTARTED once the editor became visible.)
    if (! windowed)
    {
        char msg[128];
        std::snprintf (msg, sizeof (msg), "[19] headless: osc preview poll stopped (running=%d)",
                       oscPrev[0] ? (int) oscPrev[0]->isPollRunningForTest() : -1);
        check (oscPrev[0] != nullptr && ! oscPrev[0]->isPollRunningForTest(), msg);
        std::snprintf (msg, sizeof (msg), "[19] headless: env preview poll stopped (running=%d)",
                       envPrev ? (int) envPrev->isPollRunningForTest() : -1);
        check (envPrev != nullptr && ! envPrev->isPollRunningForTest(), msg);
        std::snprintf (msg, sizeof (msg), "[19] headless: filter preview poll stopped (running=%d)",
                       filtPrev ? (int) filtPrev->isPollRunningForTest() : -1);
        check (filtPrev != nullptr && ! filtPrev->isPollRunningForTest(), msg);
    }
    else
    {
        // THE REGRESSION: give the editor hierarchy a real desktop peer (the
        // Standalone scenario). Before the parentHierarchyChanged fix the
        // polls stayed dead even here — visibilityChanged never re-fires
        // after construction — so the generation counters froze and the
        // previews never updated on any param change.
        ed->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
        pump (300);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "[19] windowed: osc preview poll RUNNING after peer attach (running=%d)",
                       oscPrev[0] ? (int) oscPrev[0]->isPollRunningForTest() : -1);
        check (oscPrev[0] != nullptr && oscPrev[0]->isPollRunningForTest(), msg);
        std::snprintf (msg, sizeof (msg), "[19] windowed: env preview poll RUNNING after peer attach (running=%d)",
                       envPrev ? (int) envPrev->isPollRunningForTest() : -1);
        check (envPrev != nullptr && envPrev->isPollRunningForTest(), msg);
        std::snprintf (msg, sizeof (msg), "[19] windowed: filter preview poll RUNNING after peer attach (running=%d)",
                       filtPrev ? (int) filtPrev->isPollRunningForTest() : -1);
        check (filtPrev != nullptr && filtPrev->isPollRunningForTest(), msg);
    }

    auto& apvts = proc.getApvts();
    auto setParam = [&apvts] (const char* id, float v)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (v);
    };

    if (windowed && displayAvailable())
    {
        pump (700);   // let the 30 Hz polls settle past the initial build

        const int failuresBefore = g_failures;

        // osc1 shape dropdown: switch Saw -> Square (choice index 1 -> 2 of 17).
        if (oscPrev[0] != nullptr)
        {
            const int gen0 = oscPrev[0]->previewGeneration();
            setParam ("osc1_shape", 2.0f / 16.0f);
            pump (700);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "[19] windowed: osc1 shape change -> preview rebuilt (gen %d -> %d)",
                           gen0, oscPrev[0]->previewGeneration());
            check (oscPrev[0]->previewGeneration() > gen0, msg);

            const int gen1 = oscPrev[0]->previewGeneration();
            setParam ("osc1_param", 0.75f);
            pump (700);
            std::snprintf (msg, sizeof (msg), "[19] windowed: osc1 param change -> preview rebuilt (gen %d -> %d)",
                           gen1, oscPrev[0]->previewGeneration());
            check (oscPrev[0]->previewGeneration() > gen1, msg);
        }

        if (envPrev != nullptr)
        {
            const int gen0 = envPrev->previewGeneration();
            setParam ("env1_attack", 0.8f);
            pump (700);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "[19] windowed: env1_attack change -> ADSR refreshed (gen %d -> %d)",
                           gen0, envPrev->previewGeneration());
            check (envPrev->previewGeneration() > gen0, msg);
        }

        // ---- Envelope SHAPE pin (2026-08-22 revision): TIME-HONEST spans +
        // EDGE PADDING. Segment widths are proportional to the ACTUAL engine
        // durations (the same LUT the envelopes run on), so a ~1 ms attack
        // renders as a near-vertical ramp — and a few pixels of silence pad
        // the plot before the attack / after the release so the steepness is
        // readable (supersedes the 2026-08-20 attack floor, which made fast
        // attacks read as "plenty of attack").
        if (envPrev != nullptr)
        {
            using ED = EnvelopeDisplay;
            const float d = 0.4f, s = 0.6f, r = 0.3f;   // typical A/D/S/R backdrop

            // (a) instant attack (a == 0, ~1 tick ~ 1 ms): silence pads the
            // left edge, then the curve jumps to ~peak within ~1.5% of width
            // — a near-vertical ramp (steep must look steep).
            check (ED::adsrCurveLevelForTest (0.0f, d, s, r, 0.02f) < 0.05f,
                   "[19] a=0: pre-attack padding is silence");
            check (ED::adsrCurveLevelForTest (0.0f, d, s, r, 0.05f) > 0.9f,
                   "[19] a=0: peak reached within ~1.5% of width (near-vertical)");

            // (b) a very fast but non-zero attack (~1-2 ms knob position):
            // same near-vertical read.
            check (ED::adsrCurveLevelForTest (0.02f, d, s, r, 0.02f) < 0.05f
                   && ED::adsrCurveLevelForTest (0.02f, d, s, r, 0.05f) > 0.9f,
                   "[19] a=0.02 (fast): near-vertical attack after the padding");

            // (c) a moderate attack keeps a gradual ramp: the midpoint of the
            // attack segment sits below the peak (not instant).
            const float midRamp = ED::adsrCurveLevelForTest (0.45f, d, s, r, 0.20f);
            check (midRamp > 0.1f && midRamp < 0.99f,
                   "[19] a=0.45 (moderate): the attack ramp is still gradual");

            // (d) MINIMAL THEORETICAL SUSTAIN WIDTH (2026-08-22 user request):
            // attack/decay/release all ~1 ms with sustain 100% renders as an
            // instant jump, a LONG plateau, an instant drop — NOT three ~1/3-
            // width fades. The plateau must dominate >= 90% of the curve.
            {
                float wA = 0, wD = 0, wS = 0, wR = 0;
                ED::adsrSegmentSpansForTest (0.0f, 0.0f, 1.0f, 0.0f, &wA, &wD, &wS, &wR);
                const float sustainShare = wS / (wA + wD + wS + wR);
                char m19[128];
                std::snprintf (m19, sizeof (m19),
                               "[19] 1ms/1ms/sust/1ms: plateau dominates (share=%.3f >= 0.9)", sustainShare);
                check (sustainShare >= 0.9f, m19);
                // and the curve is at the sustain level through the middle.
                check (std::fabs (ED::adsrCurveLevelForTest (0.0f, 0.0f, 1.0f, 0.0f, 0.5f) - 1.0f) < 0.02f,
                       "[19] 1ms/1ms/sust/1ms: curve sits at the plateau mid-plot");
            }
        }

        if (filtPrev != nullptr)
        {
            const int gen0 = filtPrev->previewGeneration();
            setParam ("filter1_cutoff", 0.9f);
            pump (700);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "[19] windowed: filter1_cutoff change -> response refreshed (gen %d -> %d)",
                           gen0, filtPrev->previewGeneration());
            check (filtPrev->previewGeneration() > gen0, msg);
        }

        (void) failuresBefore;
        ed->removeFromDesktop();
    }

    if (win != nullptr)
    {
        win->clearContentComponent();   // detach the editor before teardown
        win.reset();
    }
    delete ed;
    return 0;
}
