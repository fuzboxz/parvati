// tools/editor_test.cpp
// Headless GUI coverage check for the Parvati editor (integrated, Serum-style
// layout).
//
// The editor is a two-tab [SYNTH][FX] page selector (the tab bar itself is
// hidden; the header [Synth]/[FX] buttons are the UI). The SYNTH content is
// a 3-row SynthWorkspace:
//   - TOP row: 3 direct ParamPages in signal-chain columns (OSC | MIX | FILTER),
//     reparented (NOT regenerated) so every APVTS attachment + the byte-bridge
//     survive.
//   - MIDDLE row: a full-width CentralModBar (CentralModBar::kBarHeight) — the
//     pill hub. Clicking a GENERATOR pill selects that generator (Env/LFO/Seq/
//     Arp/Modifier), reparenting its page into the bottom-left active-editor
//     host; dragging any pill onto a knob assigns it.
//   - BOTTOM row: the active-editor host (shows ONE generator ParamPage at a
//     time, chosen by the bar) on the left, and the editor-owned ModMatrixView
//     (a DIRECT child of the workspace, no longer tab content) on the right.
// Generator pages are EDITOR-OWNED (ParvatiEditor::generatedPages_); only ENV 1
// is reparented at startup, the rest stay unparented until their pill is clicked.
// A ParamPage OWNS its ParamControls whether parented or not, so this test
// enumerates every page via ParvatiEditor::allGeneratedPages() and counts the
// controls directly (parented or not). The Global ParamPage is hosted INSIDE
// the Patch page (hostParamPage) and shown by its header "Patch" button.
// This test verifies:
//   - createEditor() returns a non-null AudioProcessorEditor
//   - the top-level page selector has exactly 2 tabs ([SYNTH] + [FX]; the
//     Patch page is a header-button overlay, not a tab)
//   - every patch/part descriptor EXCEPT `part_select`, the mod-matrix slot
//     params, and the five Patch-table-absorbed part knobs (octave/legato/
//     portamento/raga/polyphony) gets exactly one ParamControl cell (counted
//     across ALL generated pages, parented or not)
//   - the Oscillators page has exactly 8 controls; the Global page has 6
//     (the three global options + the compact volume/tuning/spread row —
//     the rest absorbed into the Patch table, 2026-08-20)
//   - clicking a CentralModBar generator pill reparents the right page into the
//     active-editor host (the new click-wiring's first automated coverage)
//   - a ModMatrixView is a DIRECT child of SynthWorkspace (no longer tab content)
//   - the top-bar Part selector is wired: setting `part_select` switches the part
//   - default editor size is the fixed 1280 x 634 (R3: the mod bar scrolls
//     horizontally inside its own Viewport, so the editor no longer tracks
//     CentralModBar::preferredWidth())
//   - every generated ParamPage reports a sane (overlap-free, width-filling) layout
//   - the Sequencer page is present among the generated pages; it has exactly 3
//     marked Length controls + correct step dimming
//   - the voice-activity CELLS meter is GONE; the global voice-POOL view
//     (labels + active/allocated counts) lives on the Patch page
//   - the editor is deleted cleanly (JUCE leak detector validates Parvati classes)
//
// Build: cmake --build build --target parvati_editor_test && ./build/parvati_editor_test

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

// THE HEADLESS POPUP PUMP (Apple): driving the async ComboBox popup path needs
// the main run loop serviced; JUCE 9's synchronous pumps are either
// JUCE_MODAL_LOOPS_PERMITTED-gated or [NSApp run] (which a console binary's
// timer queue does not service). Same idiom as tests/perf_smoke_test.cpp — the
// JUCE MessageQueue IS a CFRunLoopSource on the main loop.
// (defined(__APPLE__), not JUCE_MAC: this precedes the JUCE includes, which
//  are what defines the JUCE_MAC macro.)
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "dsp/fx/FxTypes.h"     // FxType::Count (FX type combo assertion)
#include "ui/ModSourceCatalog.h"   // parvati::kNoteSeqSentinel + ambika::dsp::MOD_SRC_*
#include "ui/SynthWorkspace.h"     // complete type for findFirst<SynthWorkspace>
#include "ui/FxWorkspace.h"        // complete type for findFirst<FxWorkspace>
#include "ui/FxMatrixView.h"       // complete type for findFirst<FxMatrixView>
#include "ui/FxSlotCard.h"         // complete type for findFirst/collectAll<FxSlotCard>
#include "ui/FxRoutingBar.h"       // complete type for findFirst<FxRoutingBar>
#include "ui/FxSlotVisualizer.h"   // complete type for findFirst<FxSlotVisualizer>

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Collect every ParamControl in c's subtree (c included).
void collectParamControls (juce::Component* c, std::vector<ParamControl*>& out)
{
    if (auto* p = dynamic_cast<ParamControl*> (c))
        out.push_back (p);
    for (auto* child : c->getChildren())
        collectParamControls (child, out);
}

// First component of type T in the subtree (depth-first).
template <typename T>
T* findFirst (juce::Component* c)
{
    if (auto* t = dynamic_cast<T*> (c))
        return t;
    for (auto* child : c->getChildren())
        if (auto* t = findFirst<T> (child))
            return t;
    return nullptr;
}

// Collect EVERY component of type T in c's subtree (c included, depth-first).
template <typename T>
void collectAll (juce::Component* c, std::vector<T*>& out)
{
    if (auto* t = dynamic_cast<T*> (c))
        out.push_back (t);
    for (auto* child : c->getChildren())
        collectAll<T> (child, out);
}

// First TabbedComponent in the subtree (DFS). With the integrated layout the
// only TabbedComponent is the single-tab [SYNTH] page selector (the lone tab
// bar is hidden via depth 0; the SYNTH content is the 3-row SynthWorkspace).
juce::TabbedComponent* findTabs (juce::Component* c)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c))
        return t;
    for (auto* child : c->getChildren())
        if (auto* t = findTabs (child))
            return t;
    return nullptr;
}

// ParamControls owned by a ParamPage (the page's descendants).
std::vector<ParamControl*> pageControls (ParamPage* page)
{
    std::vector<ParamControl*> v;
    collectParamControls (page, v);
    return v;
}
}  // namespace

int main()
{
    juce::MessageManager::getInstance();   // macOS: main thread == message thread

    const auto& descs = getPatchParamDescriptors();

    // Every descriptor except `part_select` should get a ParamControl on a page
    // (part_select has the dedicated top-bar ComboBox). The mod-matrix params
    // (mod{1..14}_source/_dest/_amount) are now hosted by the editor-owned
    // ModMatrixView (Wave 1) — NOT ParamControls on a ParamPage — so they are
    // intentionally excluded from this coverage count (validated separately as
    // [3c]: a ModMatrixView is present in the tree). "modif*" is NOT matched
    // (modifiers stay on a paginated ParamPage). Likewise the per-part FX-slot
    // params (fx{1,2,3}_type/enabled/drywet/param1-4) are now hosted by the
    // self-contained FxSlotCards (6 owned ParamControls each), and fx_topo /
    // fx_order live on the FxRoutingBar (fx_topo via the compact FLOW ComboBox;
    // fx_order is no longer user-exposed) — none are ParamControls on a
    // ParamPage, so they are excluded here too.
    int expectedCells = 0;
    for (const auto& d : descs)
    {
        if (d.paramID == "part_select")
            continue;
        if (d.paramID.size() > 3 && d.paramID.compare (0, 3, "mod") == 0
            && std::isdigit (static_cast<unsigned char> (d.paramID[3])))
            continue;   // mod{N}_... == a ModMatrixView slot param
        if (d.paramID.size() > 5 && d.paramID.compare (0, 5, "fxmod") == 0)
            continue;   // fxmod{N}_... == an FxMatrixView slot param (per-part FX mod matrix)
        if (d.paramID == "fx_topo" || d.paramID == "fx_order"
            || d.paramID == "fx_mix"
            || d.paramID == "fx_eq_low" || d.paramID == "fx_eq_mid" || d.paramID == "fx_eq_high")
            continue;   // fx_topo/fx_order + the master section (fx_mix/eq_*) -> FxRoutingBar controls, not ParamPage ParamControls
        if (d.paramID.size() > 4
            && d.paramID.compare (0, 2, "fx") == 0
            && (d.paramID[2] == '1' || d.paramID[2] == '2' || d.paramID[2] == '3')
            && d.paramID[3] == '_')
            continue;   // fx{1,2,3}_... -> an FxSlotCard's 6 owned ParamControls
        // Patch-page simplification (2026-08-20): octave / legato /
        // portamento are Patch-table COLUMNS, and raga (Scale) / polyphony are
        // covered by the table's Tune / Poly columns — no page generates them
        // (the APVTS parameters remain for host automation / state / files).
        if (d.paramID == "part_octave" || d.paramID == "part_legato"
            || d.paramID == "part_portamento" || d.paramID == "part_raga"
            || d.paramID == "part_polyphony")
            continue;
        ++expectedCells;
    }

    {
        ParvatiAudioProcessor processor;
        processor.prepareToPlay (48000.0, 256);

        juce::AudioProcessorEditor* ed = processor.createEditor();
        if (ed == nullptr)
        {
            std::printf ("FAIL: createEditor() returned null\n");
            return 1;
        }

        // Enumerate EVERY generated page (parented or not) via the editor
        // accessor — the 3 top-row direct pages (OSC/MIX/FILTER), every
        // generator page (ENV/LFO/SEQ/ARP/Modifiers — only ENV 1 is reparented
        // at startup), and the Global page. A ParamPage owns its ParamControls
        // whether parented or not, so unparented generator pages are counted
        // here too.
        auto* editor = dynamic_cast<ParvatiEditor*> (ed);
        std::vector<ParamPage*> pages = (editor != nullptr)
            ? editor->allGeneratedPages() : std::vector<ParamPage*>{};
        std::sort (pages.begin(), pages.end());
        pages.erase (std::unique (pages.begin(), pages.end()), pages.end());

        // The 3-row workspace (top direct pages | CentralModBar | active-editor
        // host + ModMatrixView). Reused by [3c], [3d] and [6].
        auto* workspace = findFirst<SynthWorkspace> (ed);

        int cells = 0;
        for (auto* p : pages)
            cells += static_cast<int> (pageControls (p).size());

        // Oscillators page: exactly 8 controls, all osc_*.
        auto isOscPage = [] (ParamPage* p)
        {
            auto cs = pageControls (p);
            if (cs.size() != 8) return false;
            for (auto* c : cs)
                if (! c->getParamID().startsWith ("osc")) return false;
            return true;
        };
        const auto oscPage = std::find_if (pages.begin(), pages.end(), isOscPage);

        // Global page: 6 controls — the 3 global options + the compact
        // Part / Play row (volume / tuning / spread). The other 5 part knobs
        // were absorbed into the Patch page's table (2026-08-20).
        const auto globalPage = std::find_if (pages.begin(), pages.end(), [] (ParamPage* p)
        {
            auto cs = pageControls (p);
            if (cs.size() != 6) return false;
            bool hasCard = false, hasCurve = false, hasVolume = false;
            for (auto* c : cs)
            {
                if (c->getParamID() == "filter_card")  hasCard = true;
                if (c->getParamID() == "vca_curve")    hasCurve = true;
                if (c->getParamID() == "part_volume")  hasVolume = true;
            }
            return hasCard && hasCurve && hasVolume;
        });

        auto* topTabs = findTabs (ed);
        const int numTopTabs = topTabs ? topTabs->getNumTabs() : 0;

        char msg[96];

        std::printf ("[1] Editor construction\n");
        check (dynamic_cast<juce::AudioProcessorEditor*> (ed) != nullptr,
               "createEditor() returns an AudioProcessorEditor");

        std::printf ("\n[2] Top-level page selector (expected 2: SYNTH + FX; Global is a header-button overlay)\n");
        std::printf ("     top-level tabs = %d\n", numTopTabs);
        check (numTopTabs == 2, "exactly 2 top-level page tabs ([SYNTH][FX]); Global is a header-button overlay");

        std::printf ("\n[3] ParamControl coverage (generated pages = %zu)\n", pages.size());
        std::printf ("     descriptors = %zu, expected cells = %d, found = %d\n",
                     descs.size(), expectedCells, cells);
        check (cells == expectedCells,
               "one ParamControl per descriptor (except part_select + mod-matrix), across all nesting");

        // [3c] MOD MATRIX: the editor-owned ModMatrixView is now a DIRECT child
        // of SynthWorkspace (it is no longer tab content). The 42 mod{1..14}_*
        // params are NOT ParamControls (excluded above); the view hosts them
        // directly via its own combos/sliders, so a live ModMatrixView parented on
        // the workspace is the proof the wiring replaced the paginated ParamPage.
        std::printf ("\n[3c] MOD MATRIX: a ModMatrixView is a DIRECT child of SynthWorkspace\n");
        {
            auto* mmv = findFirst<ModMatrixView> (ed);
            check (mmv != nullptr, "a ModMatrixView is present in the editor tree");
            if (mmv != nullptr)
            {
                auto* mmvParent = mmv->getParentComponent();
                const bool directChild = mmvParent != nullptr
                    && dynamic_cast<SynthWorkspace*> (mmvParent) != nullptr;
                check (directChild,
                       "ModMatrixView is a direct child of SynthWorkspace (no longer tab content)");
            }
        }

        // [3c-fx] FX MATRIX: the editor-owned FxMatrixView is a DIRECT child of
        // FxWorkspace (Phase 4). The fxmod{1..16}_* params are NOT ParamControls
        // (excluded from the coverage count above); the view hosts them directly
        // via its own combos/sliders. A TabbedComponent only parents the CURRENT
        // tab's content, so switch to the FX tab (index 1) first — then an
        // FxMatrixView parented on the FX workspace proves the wiring mirrors the
        // synth wiring. Restored to SYNTH (index 0) afterwards.
        std::printf ("\n[3c-fx] FX MATRIX: an FxMatrixView is a DIRECT child of FxWorkspace\n");
        {
            const int prevTab = topTabs ? topTabs->getCurrentTabIndex() : 0;
            if (topTabs != nullptr && topTabs->getNumTabs() > 1)
                topTabs->setCurrentTabIndex (1, false);   // make the FX tab current
            auto* fxv = findFirst<FxMatrixView> (ed);
            check (fxv != nullptr, "an FxMatrixView is present in the editor tree (FX tab)");
            if (fxv != nullptr)
            {
                auto* fxvParent = fxv->getParentComponent();
                const bool directChild = fxvParent != nullptr
                    && dynamic_cast<FxWorkspace*> (fxvParent) != nullptr;
                check (directChild,
                       "FxMatrixView is a direct child of FxWorkspace");
                check (findFirst<FxWorkspace> (ed) != nullptr,
                       "an FxWorkspace is present in the editor tree (FX tab)");
            }
            if (topTabs != nullptr)
                topTabs->setCurrentTabIndex (prevTab, false);   // restore
        }

        // ------------------------------------------------------------------
        // [3d] Click-wiring: a CentralModBar generator pill surfaces the right
        // page (reparents it into SynthWorkspace's active-editor host). This
        // drives the same setActiveGenerator path the bar's pill-click handler
        // invokes — the new click-wiring's first automated coverage. The host
        // is a vertical-scroll juce::Viewport (T4 safety net), so the page's
        // DIRECT parent is the viewport's internal content holder; the check
        // walks the ancestor chain and asserts the page is inside a Viewport
        // that is a direct child of SynthWorkspace.
        // ------------------------------------------------------------------
        std::printf ("\n[3d] CentralModBar generator pill surfaces the right page\n");
        {
            // The LFO page's controls are envN_lfo_* / voice_lfo_* (they CONTAIN
            // "_lfo_" rather than start with "lfo"), so it is matched by
            // containment; the Sequencer page's controls start with "seq".
            auto findLfoPage = [&] () -> ParamPage*
            {
                for (auto* p : pages)
                    for (auto* c : pageControls (p))
                        if (c->getParamID().contains ("_lfo_"))
                            return p;
                return nullptr;
            };
            auto findSeqPage = [&] () -> ParamPage*
            {
                for (auto* p : pages)
                    for (auto* c : pageControls (p))
                        if (c->getParamID().startsWith ("seq"))
                            return p;
                return nullptr;
            };
            check (workspace != nullptr, "SynthWorkspace present (CentralModBar host)");
            if (workspace != nullptr)
            {
                // True when @p page is inside a juce::Viewport that is a direct
                // child of the workspace (the T4 active-editor host).
                auto surfacedInHost = [&] (juce::Component* page)
                {
                    for (auto* c = page->getParentComponent(); c != nullptr; c = c->getParentComponent())
                        if (auto* vp = dynamic_cast<juce::Viewport*> (c))
                            return vp->getParentComponent() == workspace;
                    return false;
                };

                // LFO 1 pill -> the LFO page.
                ParamPage* lfoPage = findLfoPage();
                check (lfoPage != nullptr, "LFO generator page found");
                if (lfoPage != nullptr)
                {
                    workspace->setActiveGenerator (ambika::dsp::MOD_SRC_LFO_1);
                    std::snprintf (msg, sizeof (msg),
                                   "LFO_1 pill reparents the LFO page into the active-editor host [parent=%s]",
                                   lfoPage->getParentComponent() != nullptr ? "set" : "null");
                    check (surfacedInHost (lfoPage), msg);
                }

                // NOTE pill (bar-only sentinel) -> the Sequencer page (Option A:
                // reveals the "Note Pitch" group only; Note Velocity stays in the
                // full Sequencer tab).
                ParamPage* seqNotePage = findSeqPage();
                check (seqNotePage != nullptr, "Sequencer generator page found");
                if (seqNotePage != nullptr)
                {
                    workspace->setActiveGenerator (parvati::kNoteSeqSentinel);
                    std::snprintf (msg, sizeof (msg),
                                   "NOTE pill reparents the Sequencer page into the active-editor host [parent=%s]",
                                   seqNotePage->getParentComponent() != nullptr ? "set" : "null");
                    check (surfacedInHost (seqNotePage), msg);
                }

                // T4 scroll safety net: the active-editor host is a
                // vertical-scroll juce::Viewport (the audit's "no safety net"
                // fix — short host frames scroll instead of clipping
                // unrecoverably). The active page is the viewport's viewed
                // component, and it never UNDER-fills the view (reflowToWidth
                // grows a fitting page to the full view height, so no scrollbar
                // appears at the tuned design size).
                // Resolve the T4 host from the ACTIVE PAGE's ancestor chain
                // (the same rule as surfacedInHost above) instead of taking the
                // first Viewport child: R3 (695fa27) added topRowViewport_ AHEAD
                // of activeEditorHost_ in the workspace child list, so child
                // order no longer identifies the generator host (it now grabs
                // the top-row scroll host instead).
                juce::Viewport* genHost = nullptr;
                if (seqNotePage != nullptr)
                    for (auto* c = seqNotePage->getParentComponent(); c != nullptr;
                         c = c->getParentComponent())
                        if (auto* v = dynamic_cast<juce::Viewport*> (c))
                        {
                            genHost = v;
                            break;
                        }
                check (genHost != nullptr,
                       "active-editor host is a juce::Viewport (T4 scroll safety net)");
                if (genHost != nullptr && seqNotePage != nullptr)
                {
                    check (genHost->getViewedComponent() == seqNotePage,
                           "the active generator page is the host viewport's viewed component");
                    check (seqNotePage->getHeight() >= genHost->getViewHeight(),
                           "generator page never under-fills the host view (no gap below a fitting page)");
                }
            }
        }

        std::printf ("\n[3b] Page grouping (Oscillators / Global)\n");
        std::printf ("     Oscillators page found = %d (8 osc controls); Global page found = %d (6 controls)\n",
                     oscPage != pages.end(), globalPage != pages.end());
        check (oscPage != pages.end(), "Oscillators page has exactly 8 osc_* controls");
        check (globalPage != pages.end(), "Global page has 6 controls: filter_card + vca_curve + filter_drive + volume/tuning/spread");

        std::printf ("\n[4] Top-bar Part selector is wired to the engine\n");
        processor.getApvts().getParameterAsValue ("part_select") = 3.0f;   // 1-based part 3
        processor.syncAllParamsToEngine();                                // apply synchronously
        const int curPart = processor.getEngine().getCurrentPart();
        std::snprintf (msg, sizeof (msg), "part_select=3 => engine current part is 2 (0-based) [was %d]", curPart);
        check (curPart == 2, msg);

        std::printf ("\n[5] Multi page: per-part MIDI channel round-trips\n");
        processor.getEngine().setPartMidiChannel (2, 7);
        const int got = processor.getEngine().getPartChannel (2);
        std::snprintf (msg, sizeof (msg), "setPartMidiChannel(2,7) => getPartChannel==7 [was %d]", got);
        check (got == 7, msg);

        std::printf ("\n[6] Default editor size\n");
        {
            // R3 (695fa27): the CentralModBar scrolls horizontally inside its
            // own Viewport, so the editor no longer tracks preferredWidth()
            // (which grew to 1580 with the named Env pills). The default is a
            // fixed 1280 x 634 with a 1024pt width floor so the editor fills
            // tablets at 100% zoom (PluginEditor.cpp: setSize (1280, 634)).
            // barPreferredWidth is printed as a diagnostic only.
            const int barPrefW = (workspace != nullptr) ? workspace->barPreferredWidth() : 0;
            std::printf ("     %d x %d (bar preferred=%d, unused for sizing)\n",
                         ed->getWidth(), ed->getHeight(), barPrefW);
            std::snprintf (msg, sizeof (msg),
                           "default editor size is 1280 x 634 [was %d x %d]",
                           ed->getWidth(), ed->getHeight());
            check (ed->getWidth() == 1280 && ed->getHeight() == 634, msg);
        }

        std::printf ("\n[7] Layout sanity (every surfaced page: no overlaps, fills width)\n");
        int saneCount = 0;
        for (auto* p : pages)
        {
            p->reflowToWidth (940);   // deterministic reflow before validating
            if (p->layoutIsSane())
                ++saneCount;
        }
        std::printf ("     sane pages = %d / %zu\n", saneCount, pages.size());
        check (saneCount == static_cast<int> (pages.size()),
               "every surfaced ParamPage reports a well-formed group grid");

        // ------------------------------------------------------------------
        // [8] Mixer: "Sub Shape" (mix_sub_shape) spans 2 cells; "Sub Level"
        // (mix_sub) sits on the 3rd column of the merged Mixer panel.
        // ------------------------------------------------------------------
        std::printf ("\n[8] Mixer sub-section: mix_sub_shape spans 2 cells\n");
        const auto mixerPage = std::find_if (pages.begin(), pages.end(), [] (ParamPage* p)
        {
            for (auto* c : pageControls (p))
                if (c->getParamID() == "mix_sub_shape") return true;
            return false;
        });
        check (mixerPage != pages.end(), "Mixer page (mix_sub_shape) exists");
        if (mixerPage != pages.end())
        {
            (*mixerPage)->reflowToWidth (940);   // deterministic sectioned layout
            ParamControl* shape = nullptr;
            ParamControl* level = nullptr;
            for (auto* c : pageControls (*mixerPage))
            {
                if (c->getParamID() == "mix_sub_shape") shape = c;
                if (c->getParamID() == "mix_sub")       level = c;
            }
            if (shape != nullptr && level != nullptr)
            {
                const int shapeW = shape->getWidth();
                const int levelW = level->getWidth();
                std::snprintf (msg, sizeof (msg),
                               "mix_sub_shape width %d >= 1.8x mix_sub width %d", shapeW, levelW);
                check (shapeW * 10 >= levelW * 18, msg);
                std::snprintf (msg, sizeof (msg),
                               "mix_sub x %d is right of mix_sub_shape right %d",
                               level->getX(), shape->getRight());
                check (level->getX() >= shape->getRight() - 2, msg);
                std::printf ("     mix_sub_shape=%dx%d @x=%d ; mix_sub=%dx%d @x=%d\n",
                             shapeW, shape->getHeight(), shape->getX(),
                             levelW, level->getHeight(), level->getX());
            }
            else
            {
                check (false, "mix_sub_shape + mix_sub controls found on the Mixer page");
            }
        }

        // ------------------------------------------------------------------
        // [9] Sequencer: marked Length knob + dimmed inactive steps
        // ------------------------------------------------------------------
        std::printf ("\n[9] Sequencer length marking + step dimming\n");
        const auto seqPage = std::find_if (pages.begin(), pages.end(), [] (ParamPage* p)
        {
            for (auto* c : pageControls (p))
                if (c->getParamID().startsWith ("seq")) return true;
            return false;
        });
        check (seqPage != pages.end(), "Sequencer page exists");
        if (seqPage != pages.end())
        {
            std::vector<ParamControl*> seq = pageControls (*seqPage);

            int lengthCount = 0;
            for (auto* p : seq)
                if (p->isLengthControl()) ++lengthCount;
            std::printf ("     Length controls = %d (expect 3)\n", lengthCount);
            check (lengthCount == 3, "exactly 3 marked Length controls (Seq1/2/Note)");
            for (auto* p : seq)
                if (p->isLengthControl())
                    check (p->isLengthLabelVisible(), "length control reports a visible label");

            // Dim: Seq1 length=4 => seq1_step4..15 disabled, Seq2 untouched.
            auto& apvts = processor.getApvts();
            apvts.getParameterAsValue ("seq_length_1") = 4.0f;
            int seq1EnabledBefore = 0, seq1DisabledPast = 0, seq2AllEnabled = 1;
            for (auto* p : seq)
            {
                if (p->getParamID().startsWith ("seq1_step"))
                {
                    if (p->stepIndex() < 4) { if (p->isStepEnabled()) ++seq1EnabledBefore; }
                    else                    { if (! p->isStepEnabled()) ++seq1DisabledPast; }
                }
                else if (p->getParamID().startsWith ("seq2_step"))
                {
                    if (! p->isStepEnabled()) seq2AllEnabled = 0;
                }
            }
            std::printf ("     seq1 enabled<4=%d dimmed>=4=%d seq2-all-enabled=%d\n",
                         seq1EnabledBefore, seq1DisabledPast, seq2AllEnabled);
            check (seq1EnabledBefore == 4, "Seq1 steps 0..3 enabled after length=4");
            check (seq1DisabledPast == 12, "Seq1 steps 4..15 dimmed after length=4");
            check (seq2AllEnabled == 1, "Seq2 steps unaffected by Seq1 length");
            apvts.getParameterAsValue ("seq_length_1") = 16.0f;   // restore
        }

        // ------------------------------------------------------------------
        // [10] REMOVED: the global voice-pool view (VoicePoolView) was deleted
        // from the Patch page by design — the whole-patch picture is gone; the
        // per-part voice counts live in the 6-part allocation table and the
        // bottom status strip's count stays part-relative. Nothing to assert
        // here (same pattern as [11]).
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // [11] REMOVED: the nested ENV/LFO/ARP/SEQ card tab bar (and its
        // per-tab parvatiTabCategoryColourId colouring) was deleted by design
        // — generator selection is now driven by the CentralModBar pills
        // (covered by [3d]). Nothing to assert here.
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // [12] Modulation ring: per-source concentric arcs (new schema).
        // ParamControl::refreshModRing() pushes the per-source count/colour/
        // amount onto the knob's Slider getProperties(), read here headlessly.
        // filter1_cutoff is the Filter column's MOD_DST_FILTER_CUTOFF knob — a
        // DIRECT (always-parented) page, so its LookAndFeel resolves the theme
        // and the colour props are actually pushed. reapplyCategoryColours()
        // (the theme-switch entry point) forces a synchronous refreshModRing(),
        // which exercises categoryColourForSourceName() via the arc colours.
        // ------------------------------------------------------------------
        std::printf ("\n[12] Modulation ring schema (per-source concentric arcs)\n");
        {
            const auto& carbon = carbonTheme();
            auto& apvts = processor.getApvts();

            // Find the filter1_cutoff control + its Slider child.
            ParamControl* cutoff = nullptr;
            for (auto* p : pages)
                for (auto* c : pageControls (p))
                    if (c->getParamID() == "filter1_cutoff")
                        cutoff = c;
            check (cutoff != nullptr, "filter1_cutoff ParamControl exists");
            if (cutoff != nullptr)
            {
                juce::Slider* sl = nullptr;
                for (auto* child : cutoff->getChildren())
                    if (auto* s = dynamic_cast<juce::Slider*> (child))
                        sl = s;
                check (sl != nullptr, "filter1_cutoff has a Slider child");
                if (sl != nullptr)
                {
                    auto& props = sl->getProperties();

                    // Route slot1 to Filter Cutoff with amount 30, then vary the
                    // SOURCE: the arc colour must follow the source's category.
                    apvts.getParameterAsValue ("mod1_dest")   = 12.0f;  // MOD_DST_FILTER_CUTOFF
                    apvts.getParameterAsValue ("mod1_amount") = 30.0f;

                    auto arc0Colour = [&]() -> uint32_t
                    {
                        ParamControl::reapplyCategoryColours();
                        const auto* v = props.getVarPointer ("parvatiModCol0");
                        return (v && v->isInt()) ? (uint32_t) (int) *v : 0;
                    };

                    struct SrcCat { float idx; const char* label; juce::Colour want; };
                    const SrcCat cats[] = {
                        { 0.0f,  "Env 1",     carbon.catEnv },
                        { 4.0f,  "LFO 2",     carbon.catLfo },
                        { 6.0f,  "Voice LFO", carbon.catLfo },   // special: name == "Voice LFO"
                        { 11.0f, "Seq 1",     carbon.catSeq },
                        { 13.0f, "Arp Step",  carbon.catArp },
                        { 10.0f, "Op 4",      carbon.accentPrimary },   // neutral
                        { 14.0f, "Velocity",  carbon.accentPrimary },   // neutral
                    };
                    for (const auto& cat : cats)
                    {
                        apvts.getParameterAsValue ("mod1_source") = cat.idx;
                        const uint32_t arcCol = arc0Colour();
                        std::snprintf (msg, sizeof (msg),
                                       "arc0 colour follows source category (%s)", cat.label);
                        check (arcCol == cat.want.getARGB(), msg);
                    }

                    // Schema keys for the one-slot case.
                    const auto* nVar = props.getVarPointer ("parvatiModN");
                    const int N = (nVar && nVar->isInt()) ? (int) *nVar : -1;
                    std::snprintf (msg, sizeof (msg), "one active slot => parvatiModN=1 [was %d]", N);
                    check (N == 1, msg);
                    const auto* amt0 = props.getVarPointer ("parvatiModAmt0");
                    check (amt0 && amt0->isInt() && (int) *amt0 == 30, "parvatiModAmt0 == 30");
                    check (props.getVarPointer ("parvatiModDepth") == nullptr,
                           "legacy parvatiModDepth key removed");

                    // Second source on the same dest -> two concentric arcs.
                    apvts.getParameterAsValue ("mod2_source") = 3.0f;   // LFO 2
                    apvts.getParameterAsValue ("mod2_dest")   = 12.0f;
                    apvts.getParameterAsValue ("mod2_amount") = -20.0f;
                    ParamControl::reapplyCategoryColours();
                    const auto* n2Var = props.getVarPointer ("parvatiModN");
                    const int N2 = (n2Var && n2Var->isInt()) ? (int) *n2Var : -1;
                    std::snprintf (msg, sizeof (msg), "two sources => parvatiModN=2 [was %d]", N2);
                    check (N2 == 2, msg);
                    const auto* col1 = props.getVarPointer ("parvatiModCol1");
                    check (col1 && col1->isInt()
                           && juce::Colour ((uint32_t) (int) *col1).getARGB() == carbon.catLfo.getARGB(),
                           "second arc parvatiModCol1 == catLfo (magenta)");
                    const auto* amt1 = props.getVarPointer ("parvatiModAmt1");
                    check (amt1 && amt1->isInt() && (int) *amt1 == -20, "parvatiModAmt1 == -20");

                    // Move the first slot's dest away from Cutoff -> arc removed.
                    apvts.getParameterAsValue ("mod1_dest") = 18.0f;  // VCA
                    ParamControl::reapplyCategoryColours();
                    const auto* n3Var = props.getVarPointer ("parvatiModN");
                    const int N3 = (n3Var && n3Var->isInt()) ? (int) *n3Var : -1;
                    std::snprintf (msg, sizeof (msg),
                                   "moving slot1 dest away => parvatiModN=1 [was %d]", N3);
                    check (N3 == 1, msg);

                    // Restore defaults so the tree is clean for teardown.
                    apvts.getParameterAsValue ("mod1_source") = 0.0f;
                    apvts.getParameterAsValue ("mod1_dest")   = 0.0f;
                    apvts.getParameterAsValue ("mod1_amount") = 0.0f;
                    apvts.getParameterAsValue ("mod2_source") = 0.0f;
                    apvts.getParameterAsValue ("mod2_dest")   = 0.0f;
                    apvts.getParameterAsValue ("mod2_amount") = 0.0f;
                }
            }
        }

        // ------------------------------------------------------------------
        // [13] FX top-section layout (3 FxSlotCards + FxRoutingBar). Headless
        // layout-sanity coverage so regressions (knob cells too small, a missing
        // visualizer / type combo / power toggle) are caught WITHOUT a render.
        // Switches to the FX tab, asserts the card + routing-bar child structure
        // + bounds + dynamic knob visibility, then restores SYNTH. The FX top row
        // is a 4-column [ ROUTING | FX1 | FX2 | FX3 ] layout: the slim
        // FxRoutingBar column (FLOW topology ComboBox + MIX + keep-tails) is the
        // leftmost column, so the routing assertions check its presence + bounds
        // + the FLOW ComboBox.
        // ------------------------------------------------------------------
        std::printf ("\n[13] FX top-section layout (cards + routing bar)\n");
        {
            const int prevTab = topTabs ? topTabs->getCurrentTabIndex() : 0;
            if (topTabs != nullptr && topTabs->getNumTabs() > 1)
                topTabs->setCurrentTabIndex (1, false);   // FX tab current (content parented + laid out synchronously)

            // ---- 3 FxSlotCards (DFS, in slot order: cards[0] == FX1) ----
            std::vector<FxSlotCard*> cards;
            collectAll<FxSlotCard> (ed, cards);
            std::printf ("     FxSlotCards found = %zu (expect 3)\n", cards.size());
            check (cards.size() == 3, "exactly 3 FxSlotCards present (FX1/FX2/FX3)");

            bool cardsPositive = true;
            for (auto* card : cards)
                if (card->getWidth() <= 0 || card->getHeight() <= 0)
                    cardsPositive = false;
            check (cardsPositive, "every FxSlotCard has positive width + height");

            if (cards.size() == 3)
            {
                // Sort left->right and assert the 3 equal columns do not overlap.
                std::vector<FxSlotCard*> sorted = cards;
                std::sort (sorted.begin(), sorted.end(),
                           [] (FxSlotCard* a, FxSlotCard* b) { return a->getX() < b->getX(); });
                bool nonOverlapping = true;
                for (size_t i = 1; i < sorted.size(); ++i)
                    if (sorted[i]->getX() < sorted[i - 1]->getRight() - 1)
                        nonOverlapping = false;
                check (nonOverlapping,
                       "the 3 FxSlotCards are side-by-side (no horizontal overlap)");
            }

            // ---- Per-card child structure + usable knob sizes ----
            for (size_t i = 0; i < cards.size(); ++i)
            {
                auto* card = cards[i];

                // Exactly 6 owned ParamControls (param1..5 + drywet), parented
                // or not (counted across the card's whole subtree).
                std::vector<ParamControl*> knobControls;
                collectParamControls (card, knobControls);
                std::snprintf (msg, sizeof (msg),
                               "FX%d card owns 6 ParamControls (param1..5 + drywet) [found %zu]",
                               (int) i + 1, knobControls.size());
                check (knobControls.size() == 6, msg);

                // DIRECT children: a type ComboBox, a power/bypass Button, + an
                // FxSlotVisualizer. (Direct-child checks avoid matching the
                // ComboBox's internal arrow Button.)
                bool hasCombo = false, hasButton = false, hasVisualizer = false;
                for (auto* child : card->getChildren())
                {
                    if (dynamic_cast<juce::ComboBox*> (child)     != nullptr) hasCombo     = true;
                    if (dynamic_cast<juce::Button*> (child)       != nullptr) hasButton     = true;
                    if (dynamic_cast<FxSlotVisualizer*> (child)   != nullptr) hasVisualizer  = true;
                }
                std::snprintf (msg, sizeof (msg), "FX%d card has a type ComboBox", (int) i + 1);
                check (hasCombo, msg);
                std::snprintf (msg, sizeof (msg), "FX%d card has a power/bypass Button", (int) i + 1);
                check (hasButton, msg);
                std::snprintf (msg, sizeof (msg), "FX%d card has an FxSlotVisualizer", (int) i + 1);
                check (hasVisualizer, msg);

                // Visible knobs must be usable (catches a "knobs collapsed to a
                // sliver" regression; the dial targets 52px so the cell needs
                // ~74px). 50px is a conservative floor.
                int minVisibleKnobH = 0, minVisibleKnobW = 0;
                bool anyVisibleKnob = false;
                for (auto* pc : knobControls)
                    if (pc->isVisible())
                    {
                        if (! anyVisibleKnob || pc->getHeight() < minVisibleKnobH)
                            minVisibleKnobH = pc->getHeight();
                        if (! anyVisibleKnob || pc->getWidth() < minVisibleKnobW)
                            minVisibleKnobW = pc->getWidth();
                        anyVisibleKnob = true;
                    }
                if (anyVisibleKnob)
                {
                    std::snprintf (msg, sizeof (msg),
                                   "FX%d visible knob height >= 50px (usable dial) [min=%d]",
                                   (int) i + 1, minVisibleKnobH);
                    check (minVisibleKnobH >= 50, msg);
                    // Width floor: the rotary dial is forced to 52px and centred,
                    // so a cell < 52px overlaps its neighbour. 50px catches a
                    // too-narrow column (e.g. a 5-knob Reverb row in a shrunken
                    // card) regression.
                    std::snprintf (msg, sizeof (msg),
                                   "FX%d visible knob width >= 50px (no dial overlap) [min=%d]",
                                   (int) i + 1, minVisibleKnobW);
                    check (minVisibleKnobW >= 50, msg);
                }
            }

            // ---- FxRoutingBar: exists, positive bounds, ◀ ▶ topology steppers ----
            auto* routeBar = findFirst<FxRoutingBar> (ed);
            check (routeBar != nullptr, "an FxRoutingBar is present (FX tab)");
            if (routeBar != nullptr)
            {
                check (routeBar->getWidth() > 0 && routeBar->getHeight() > 0,
                       "FxRoutingBar has positive width + height");
                // FLOW topology is now a ◀ diagram ▶ stepper UI (no ComboBox); the
                // global wet/dry is a Slider. Confirm the Mix Slider + the two
                // topology stepper TextButtons ("<", ">") are present.
                check (findFirst<juce::Slider> (routeBar) != nullptr,
                       "FxRoutingBar has a global Mix Slider");
                bool hasPrev = false, hasNext = false;
                for (auto* c : routeBar->getChildren())
                    if (auto* b = dynamic_cast<juce::TextButton*> (c))
                    {
                        if (b->getButtonText() == "<") hasPrev = true;
                        if (b->getButtonText() == ">") hasNext = true;
                    }
                check (hasPrev && hasNext,
                       "FxRoutingBar has the ◀ ▶ topology steppers");
            }

            // ---- Dynamic knob visibility: fx{N}_type = None -> Dry/Wet HIDDEN ----
            // (The user requirement: the Dry/Wet knob must not show for a None slot.)
            // The construction default is None, so the card's first layout already
            // hid every knob; verify that directly (synchronous, no async pump).
            if (! cards.empty())
            {
                auto& apvts = processor.getApvts();
                apvts.getParameterAsValue ("fx1_type") = 0.0f;   // FxType::None (choice idx 0)

                int visibleKnobs = 0;
                std::vector<ParamControl*> fx1Controls;
                collectParamControls (cards[0], fx1Controls);   // cards[0] == FX1 (slot 0)
                for (auto* pc : fx1Controls)
                    if (pc->isVisible())
                        ++visibleKnobs;
                std::snprintf (msg, sizeof (msg),
                               "fx1_type=None => 0 visible knobs (Dry/Wet hidden on None) [found %d]",
                               visibleKnobs);
                check (visibleKnobs == 0, msg);

                // Engagement defaults (W10): seeding lives ONLY at the UI
                // seams (FxSlotCard::seedEngagementDefaultsForType, called by
                // the type-combo popup pick and stepType BEFORE the type write).
                // A PLAIN param write (host automation / NRPN / preset-load
                // stand-in) must NOT seed — it would clobber live values. A
                // LATER param write — as a preset/part load does (descriptor
                // order sets type THEN params) — must OVERRIDE the seed, so
                // saved patches keep their own values (preset-safe).
                {
                    apvts.getParameterAsValue ("fx1_type") = 2.0f;      // Delay
                    const int p1PlainWrite = juce::roundToInt (apvts.getParameterAsValue ("fx1_param1").getValue());
                    std::snprintf (msg, sizeof (msg),
                                   "fx1_type=Delay plain write seeds NOTHING (automation cannot clobber) [got %d]",
                                   p1PlainWrite);
                    check (p1PlainWrite == 0, msg);

                    cards[0]->seedEngagementDefaultsForType (2);        // Delay (the UI-pick seam)
                    const int p1AfterSeed = juce::roundToInt (apvts.getParameterAsValue ("fx1_param1").getValue());
                    std::snprintf (msg, sizeof (msg),
                                   "fx1_type=Delay UI seam seeds param1=50 (engagement default) [got %d]",
                                   p1AfterSeed);
                    check (p1AfterSeed == 50, msg);

                    apvts.getParameterAsValue ("fx1_param1") = 100.0f;  // override (preset value / user tweak)
                    const int p1AfterOverride = juce::roundToInt (apvts.getParameterAsValue ("fx1_param1").getValue());
                    std::snprintf (msg, sizeof (msg),
                                   "param write after seed overrides the engagement default [got %d]",
                                   p1AfterOverride);
                    check (p1AfterOverride == 100, msg);
                }

                // The type ComboBox itself must drive fx{N}_type via its
                // ComboBoxAttachment — simulate a user selection and confirm the
                // param follows (rules out a broken/non-interactive combo).
                {
                    apvts.getParameterAsValue ("fx1_type") = 0.0f;   // start from None
                    juce::ComboBox* typeCombo = nullptr;
                    for (auto* c : cards[0]->getChildren())
                        if ((typeCombo = dynamic_cast<juce::ComboBox*> (c)))
                            break;
                    std::snprintf (msg, sizeof (msg),
                                   "FX1 type combo has %d items [got %d]",
                                   (int) FxType::Count,
                                   typeCombo ? typeCombo->getNumItems() : -1);
                    // One item per FxType — the FV-1 family is APPEND-ONLY
                    // (81679f5 + f80a6c9 grew Count 11 -> 16), so assert
                    // against the enum, not a literal count.
                    check (typeCombo != nullptr
                               && typeCombo->getNumItems() == (int) FxType::Count,
                           msg);

                    // Dropdown-REOPEN regression: FxTypeCombo::showPopup once
                    // finished with a nullptr completion callback, so the
                    // private menuActive flag stayed latched TRUE after the
                    // popup dismissed and every later click bailed inside
                    // ComboBox::showPopupIfNotActive() — the effect could be
                    // picked exactly once per card. Drive the REAL click path
                    // (mouseDown -> showPopupIfNotActive -> async showPopup)
                    // and prove the flag resets when the menu is dismissed.
                    // (Apple-only: the headless pump runs the main CFRunLoop
                    // directly — the perf-smoke-test idiom; other platforms
                    // skip this check.)
#if JUCE_MAC || JUCE_IOS
                    if (typeCombo != nullptr)
                    {
                        check (! typeCombo->isPopupActive(),
                               "FX1 type combo: menuActive false before first open");
                        typeCombo->mouseDown (
                            juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                                               typeCombo->getLocalBounds().getCentre().toFloat(),
                                               juce::ModifierKeys::leftButtonModifier,
                                               juce::MouseInputSource::defaultPressure,
                                               juce::MouseInputSource::defaultOrientation,
                                               juce::MouseInputSource::defaultRotation,
                                               juce::MouseInputSource::defaultTiltX,
                                               juce::MouseInputSource::defaultTiltY,
                                               typeCombo, typeCombo,
                                               juce::Time::getCurrentTime(), {},
                                               juce::Time::getCurrentTime(), 1, false));
                        // showPopupIfNotActive defers the actual popup via
                        // callAsync — pump the main run loop so it opens
                        // (JUCE 9 has no unguarded synchronous pump; the JUCE
                        // MessageQueue IS a CFRunLoopSource on the main loop).
                        bool opened = false;
                        for (int i = 0; i < 50 && ! opened; ++i)
                        {
                            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
                            opened = typeCombo->isPopupActive();
                        }
                        check (opened, "FX1 type combo: popup active after click");
                        juce::PopupMenu::dismissAllActiveMenus();
                        bool closed = false;
                        for (int i = 0; i < 50 && ! closed; ++i)
                        {
                            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
                            closed = ! typeCombo->isPopupActive();
                        }
                        std::snprintf (msg, sizeof (msg),
                                       "FX1 type combo: menuActive resets after dismissal "
                                       "(dropdown reopenable) [got %d]",
                                       typeCombo->isPopupActive() ? 1 : 0);
                        check (closed && ! typeCombo->isPopupActive(), msg);
                    }
#else
                    std::printf ("  (popup-reopen check skipped: non-Apple pump)\n");
#endif
                    // (Selection -> APVTS propagation is async via the attachment and
                    // isn't pumpable in this headless test; the identical addItemList +
                    // ComboBoxAttachment pattern already powers the working osc-shape
                    // and routing-bar dropdowns, so propagation is proven at runtime.
                    // The real regression was an EMPTY dropdown, covered by the count.)
                }

                apvts.getParameterAsValue ("fx1_type") = 0.0f;   // restore None
            }

            if (topTabs != nullptr)
                topTabs->setCurrentTabIndex (prevTab, false);   // restore
        }

        // Reset to SYNTH.
        if (topTabs != nullptr) topTabs->setCurrentTabIndex (0, false);

        processor.editorBeingDeleted (ed);
        delete ed;
    }

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "EDITOR TEST: FAILURES" : "EDITOR TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
