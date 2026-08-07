// tools/editor_test.cpp
// Headless GUI coverage check for the Parvati editor (integrated, Serum-style
// layout).
//
// The editor is a single [SYNTH] page selector (the lone tab bar is hidden).
// The SYNTH content is a 3-row SynthWorkspace:
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
// controls directly (parented or not). The Global ParamPage is a direct-child
// overlay toggled by the header "Global" button. This test verifies:
//   - createEditor() returns a non-null AudioProcessorEditor
//   - the top-level page selector has exactly 1 tab ([SYNTH])
//   - every patch/part descriptor EXCEPT `part_select` and the mod-matrix slot
//     params gets exactly one ParamControl cell (counted across ALL generated
//     pages, parented or not)
//   - the Oscillators page has exactly 8 controls; the Global page has 10
//   - clicking a CentralModBar generator pill reparents the right page into the
//     active-editor host (the new click-wiring's first automated coverage)
//   - a ModMatrixView is a DIRECT child of SynthWorkspace (no longer tab content)
//   - the top-bar Part selector is wired: setting `part_select` switches the part
//   - default editor size matches CentralModBar::preferredWidth()+8 x 620
//     (~1443 x 620, not the old 1280 x 620)
//   - every generated ParamPage reports a sane (overlap-free, width-filling) layout
//   - the Sequencer page is present among the generated pages; it has exactly 3
//     marked Length controls + correct step dimming
//   - Voice activity meter (cells) exists on the Global page
//   - the editor is deleted cleanly (JUCE leak detector validates Parvati classes)
//
// Build: cmake --build build --target parvati_editor_test && ./build/parvati_editor_test

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/ModSourceCatalog.h"   // parvati::kNoteSeqSentinel + ambika::dsp::MOD_SRC_*
#include "ui/SynthWorkspace.h"     // complete type for findFirst<SynthWorkspace>

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
    // (modifiers stay on a paginated ParamPage).
    int expectedCells = 0;
    for (const auto& d : descs)
    {
        if (d.paramID == "part_select")
            continue;
        if (d.paramID.size() > 3 && d.paramID.compare (0, 3, "mod") == 0
            && std::isdigit (static_cast<unsigned char> (d.paramID[3])))
            continue;   // mod{N}_... == a ModMatrixView slot param
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

        std::printf ("\n[2] Top-level page selector (expected 1: SYNTH; Global is a header-button overlay)\n");
        std::printf ("     top-level tabs = %d\n", numTopTabs);
        check (numTopTabs == 1, "exactly 1 top-level page tab ([SYNTH]); Global is a header-button overlay");

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

        // ------------------------------------------------------------------
        // [3d] Click-wiring: a CentralModBar generator pill surfaces the right
        // page (reparents it into SynthWorkspace's active-editor host). This
        // drives the same setActiveGenerator path the bar's pill-click handler
        // invokes — the new click-wiring's first automated coverage. The page's
        // parent becomes the host, and the host is a direct child of
        // SynthWorkspace, so the page's grand-parent is the workspace.
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
                // LFO 1 pill -> the LFO page.
                ParamPage* lfoPage = findLfoPage();
                check (lfoPage != nullptr, "LFO generator page found");
                if (lfoPage != nullptr)
                {
                    workspace->setActiveGenerator (ambika::dsp::MOD_SRC_LFO_1);
                    juce::Component* host = lfoPage->getParentComponent();
                    const bool surfaced = host != nullptr && host->getParentComponent() == workspace;
                    std::snprintf (msg, sizeof (msg),
                                   "LFO_1 pill reparents the LFO page into the active-editor host [parent=%s]",
                                   host != nullptr ? "set" : "null");
                    check (surfaced, msg);
                }

                // NOTE pill (bar-only sentinel) -> the Sequencer page (reveals
                // BOTH "Note Pitch" + "Note Velocity" groups).
                ParamPage* seqNotePage = findSeqPage();
                check (seqNotePage != nullptr, "Sequencer generator page found");
                if (seqNotePage != nullptr)
                {
                    workspace->setActiveGenerator (parvati::kNoteSeqSentinel);
                    juce::Component* host = seqNotePage->getParentComponent();
                    const bool surfaced = host != nullptr && host->getParentComponent() == workspace;
                    std::snprintf (msg, sizeof (msg),
                                   "NOTE pill reparents the Sequencer page into the active-editor host [parent=%s]",
                                   host != nullptr ? "set" : "null");
                    check (surfaced, msg);
                }
            }
        }

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
        {
            const int barPrefW = (workspace != nullptr) ? workspace->barPreferredWidth() : 0;
            const int expectedW = juce::jmax (1280, barPrefW + 8);   // == CentralModBar::preferredWidth()+8 (~1443)
            std::printf ("     %d x %d (bar preferred=%d, expected width=%d)\n",
                         ed->getWidth(), ed->getHeight(), barPrefW, expectedW);
            std::snprintf (msg, sizeof (msg),
                           "default editor size is %d x 620 [was %d x %d]",
                           expectedW, ed->getWidth(), ed->getHeight());
            check (ed->getWidth() == expectedW && ed->getHeight() == 620, msg);
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
        // [10] Voice activity CELLS live on the Global page (a decoration).
        // ------------------------------------------------------------------
        std::printf ("\n[10] Voice meter (cells) on the Global page\n");
        // globalPage_ is now a permanent (invisible) direct-child overlay, so its
        // VoiceMeter decoration is always in the component tree — no tab switch
        // is needed to surface it.
        auto* meter = findFirst<VoiceMeter> (ed);
        check (meter != nullptr, "voice meter (cells) exists on the Global page");
        if (meter != nullptr)
            check (meter->getActiveVoiceCount() >= 0,
                   "voice meter reports an active-voice count (6-cell view)");

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
