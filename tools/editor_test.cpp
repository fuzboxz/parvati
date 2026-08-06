// tools/editor_test.cpp
// Headless GUI coverage check for the Parvati editor (integrated, Serum-style layout).
//
// The editor is no longer a flat 10-tab TabbedComponent. It is a top-level
// [SYNTH | GLOBAL] page selector whose SYNTH page is a SynthWorkspace hosting the
// 9 synth ParamPages (reparented — NOT regenerated — so every APVTS attachment +
// the byte-bridge survive) in signal-chain columns (Oscillators | Mixer | Filter)
// above two nested tab groups ([ENV | LFO] and [MOD MATRIX | MODIFIERS | ARP |
// SEQ]); the dense sections paginate by group via GroupPager sub-tabs, with NO
// per-page Viewports/scrollbars. The GLOBAL page shows the Global ParamPage. This test verifies:
//   - createEditor() returns a non-null AudioProcessorEditor
//   - the top-level page selector has exactly 2 tabs ([SYNTH | GLOBAL])
//   - every patch/part descriptor EXCEPT `part_select` gets exactly one
//     ParamControl cell (pages are surfaced across ALL nesting levels to count)
//   - the Oscillators page has exactly 8 controls; the Global page has 10
//   - the top-bar Part selector is wired: setting `part_select` switches the part
//   - default editor size is 1280 x 620 (dense integrated layout)
//   - every surfaced ParamPage reports a sane (overlap-free, width-filling) layout
//   - Sequencer: exactly 3 marked Length controls + correct step dimming
//   - Voice activity meter (cells) exists on the Global page
//   - the editor is deleted cleanly (JUCE leak detector validates Parvati classes)
//
// Build: cmake --build build_release --target parvati_editor_test && ./build_release/parvati_editor_test

#include <algorithm>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

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

// First TabbedComponent in the subtree (DFS). With the integrated layout the
// top-level page selector is the first TC discovered (it is added before the
// nested workspace tab groups).
juce::TabbedComponent* findTabs (juce::Component* c)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c))
        return t;
    for (auto* child : c->getChildren())
        if (auto* t = findTabs (child))
            return t;
    return nullptr;
}

// Surface every tab at every nesting level (switching each current in turn) so
// every ParamPage becomes reachable, collecting the ParamPage objects. A
// ParamPage owns its ParamControls whether parented or not, so a collected page
// can be inspected directly. Each page is content of exactly one tab in exactly
// one TabbedComponent, so it is surfaced exactly once.
void surfacePages (juce::Component* c, std::vector<ParamPage*>& out)
{
    if (auto* page = dynamic_cast<ParamPage*> (c))
    {
        out.push_back (page);
        return;   // pages never nest pages
    }
    if (auto* tc = dynamic_cast<juce::TabbedComponent*> (c))
    {
        for (int i = 0; i < tc->getNumTabs(); ++i)
        {
            tc->setCurrentTabIndex (i, false);   // parent this tab's content
            if (auto* content = tc->getTabContentComponent (i))
                surfacePages (content, out);
        }
        return;
    }
    for (auto* child : c->getChildren())
        surfacePages (child, out);
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
    // (part_select has the dedicated top-bar ComboBox).
    int expectedCells = 0;
    for (const auto& d : descs)
        if (d.paramID != "part_select")
            ++expectedCells;

    {
        ParvatiAudioProcessor processor;
        processor.prepareToPlay (48000.0, 256);

        juce::AudioProcessorEditor* ed = processor.createEditor();
        if (ed == nullptr)
        {
            std::printf ("FAIL: createEditor() returned null\n");
            return 1;
        }

        // Surface every ParamPage across all nesting levels. (Leaves the top
        // selector on its last tab; reset to SYNTH once checks are done.)
        std::vector<ParamPage*> pages;
        surfacePages (ed, pages);
        std::sort (pages.begin(), pages.end());
        pages.erase (std::unique (pages.begin(), pages.end()), pages.end());

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

        // Global page: 10 controls including filter_card + vca_curve.
        const auto globalPage = std::find_if (pages.begin(), pages.end(), [] (ParamPage* p)
        {
            auto cs = pageControls (p);
            if (cs.size() != 10) return false;
            bool hasCard = false, hasCurve = false;
            for (auto* c : cs)
            {
                if (c->getParamID() == "filter_card") hasCard = true;
                if (c->getParamID() == "vca_curve")   hasCurve = true;
            }
            return hasCard && hasCurve;
        });

        auto* topTabs = findTabs (ed);
        const int numTopTabs = topTabs ? topTabs->getNumTabs() : 0;

        char msg[96];

        std::printf ("[1] Editor construction\n");
        check (dynamic_cast<juce::AudioProcessorEditor*> (ed) != nullptr,
               "createEditor() returns an AudioProcessorEditor");

        std::printf ("\n[2] Top-level page selector (expected 2: SYNTH, GLOBAL)\n");
        std::printf ("     top-level tabs = %d\n", numTopTabs);
        check (numTopTabs == 2, "exactly 2 top-level page tabs ([SYNTH | GLOBAL])");

        std::printf ("\n[3] ParamControl coverage (surfaced pages = %zu)\n", pages.size());
        std::printf ("     descriptors = %zu, expected cells = %d, found = %d\n",
                     descs.size(), expectedCells, cells);
        check (cells == expectedCells,
               "one ParamControl per descriptor (except part_select), across all nesting");

        std::printf ("\n[3b] Page grouping (Oscillators / Global)\n");
        std::printf ("     Oscillators page found = %d (8 osc controls); Global page found = %d (10 controls)\n",
                     oscPage != pages.end(), globalPage != pages.end());
        check (oscPage != pages.end(), "Oscillators page has exactly 8 osc_* controls");
        check (globalPage != pages.end(), "Global page has 10 controls incl. filter_card + vca_curve");

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
        std::printf ("     %d x %d\n", ed->getWidth(), ed->getHeight());
        check (ed->getWidth() == 1280 && ed->getHeight() == 620,
               "default editor size is 1280 x 620 (integrated dense layout)");

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
        // [10] Voice activity CELLS live on the Global page (a decoration).
        // ------------------------------------------------------------------
        std::printf ("\n[10] Voice meter (cells) on the Global page\n");
        // JUCE only parents the CURRENT tab's content, so switch to GLOBAL so the
        // Global page (and its VoiceMeter decoration) is in the component tree.
        if (topTabs != nullptr)
        {
            const auto names = topTabs->getTabNames();
            for (int i = 0; i < names.size(); ++i)
                if (names[i] == "GLOBAL") { topTabs->setCurrentTabIndex (i, false); break; }
        }
        auto* meter = findFirst<VoiceMeter> (ed);
        check (meter != nullptr, "voice meter (cells) exists on the Global page");
        if (meter != nullptr)
            check (meter->getActiveVoiceCount() >= 0,
                   "voice meter reports an active-voice count (6-cell view)");

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
