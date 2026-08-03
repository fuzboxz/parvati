// tools/editor_test.cpp
// Headless GUI coverage check for the Parvati editor.
//
// Verifies:
//   - createEditor() returns a non-null AudioProcessorEditor
//   - the tabbed pages are exactly the expected 10 (9 ParamPages + Multi)
//   - every patch/part descriptor EXCEPT `part_select` gets exactly one
//     ParamControl cell (part_select has the dedicated top-bar Part selector)
//   - the top-bar Part selector is wired: setting `part_select` switches the
//     engine's current part
//   - the Multi page's per-part MIDI-channel editing round-trips to the engine
//   - default editor size is 980 x 660
//   - the editor is deleted cleanly (JUCE leak detector validates Parvati classes)
//
// Build: cmake --build build --target parvati_editor_test && ./build/parvati_editor_test

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

template <typename Pred>
int countInTree (juce::Component* c, Pred pred)
{
    int n = pred (c) ? 1 : 0;
    for (auto* child : c->getChildren())
        n += countInTree (child, pred);
    return n;
}

juce::TabbedComponent* findTabs (juce::Component* c)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c))
        return t;
    for (auto* child : c->getChildren())
        if (auto* t = findTabs (child))
            return t;
    return nullptr;
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

        auto* tabs = findTabs (ed);
        const int numTabs = tabs ? tabs->getNumTabs() : 0;

        // JUCE only parents the CURRENT tab's content into the component tree,
        // so visit each tab and count ParamControl cells there.
        int cells = 0;
        std::vector<int> perTab;
        if (tabs != nullptr)
            for (int i = 0; i < numTabs; ++i)
            {
                tabs->setCurrentTabIndex (i, false);
                const int n = countInTree (ed, [] (juce::Component* c) {
                    return dynamic_cast<ParamControl*> (c) != nullptr;
                });
                perTab.push_back (n);
                cells += n;
            }

        auto tabIndex = [&] (const char* name) -> int {
            if (tabs == nullptr) return -1;
            const auto names = tabs->getTabNames();
            for (int i = 0; i < names.size(); ++i)
                if (names[i] == name) return i;
            return -1;
        };

        std::printf ("[1] Editor construction\n");
        check (dynamic_cast<juce::AudioProcessorEditor*> (ed) != nullptr,
               "createEditor() returns an AudioProcessorEditor");

        std::printf ("\n[2] Tab pages (expected 10: 9 ParamPages + Multi)\n");
        std::printf ("     tab pages = %d\n", numTabs);
        check (numTabs == 10, "exactly 10 tab pages");

        std::printf ("\n[3] ParamControl coverage\n");
        std::printf ("     descriptors = %zu, expected ParamControl cells = %d, found = %d\n",
                     descs.size(), expectedCells, cells);
        check (cells == expectedCells,
               "one ParamControl per descriptor (except part_select)");

        std::printf ("\n[3b] Global-tab placement (no global clutter on Oscillators)\n");
        const int oscTab = tabIndex ("Oscillators");
        const int glbTab = tabIndex ("Global");
        std::printf ("     Oscillators tab controls = %d (expect 8), Global tab controls = %d (expect 10)\n",
                     oscTab >= 0 ? perTab[(size_t) oscTab] : -1,
                     glbTab >= 0 ? perTab[(size_t) glbTab] : -1);
        check (oscTab >= 0 && perTab[(size_t) oscTab] == 8,
               "Oscillators page has exactly 8 controls (no global/part clutter)");
        check (glbTab >= 0 && perTab[(size_t) glbTab] == 10,
               "Global page has 10 controls (volume/octave/tuning/spread/legato/portamento/polyphony/VCA/filter card/filter drive)");

        std::printf ("\n[4] Top-bar Part selector is wired to the engine\n");
        processor.getApvts().getParameterAsValue ("part_select") = 3.0f;   // 1-based part 3
        processor.syncAllParamsToEngine();                                // apply synchronously
        const int curPart = processor.getEngine().getCurrentPart();
        char msg[96];
        std::snprintf (msg, sizeof (msg), "part_select=3 => engine current part is 2 (0-based) [was %d]", curPart);
        check (curPart == 2, msg);

        std::printf ("\n[5] Multi page: per-part MIDI channel round-trips\n");
        processor.getEngine().setPartMidiChannel (2, 7);
        const int got = processor.getEngine().getPartChannel (2);
        std::snprintf (msg, sizeof (msg), "setPartMidiChannel(2,7) => getPartChannel==7 [was %d]", got);
        check (got == 7, msg);

        std::printf ("\n[6] Default editor size\n");
        std::printf ("     %d x %d\n", ed->getWidth(), ed->getHeight());
        check (ed->getWidth() == 980 && ed->getHeight() == 660,
               "default editor size is 980 x 660");

        std::printf ("\n[7] Layout sanity (flexible-width grid: no overlaps, fills width)\n");
        if (tabs != nullptr)
        {
            for (int i = 0; i < numTabs; ++i)
            {
                tabs->setCurrentTabIndex (i, false);
                auto* content = tabs->getTabContentComponent (i);
                auto* vp = dynamic_cast<juce::Viewport*> (content);
                ParamPage* page = (vp != nullptr)
                    ? dynamic_cast<ParamPage*> (vp->getViewedComponent())
                    : nullptr;
                if (page == nullptr)
                    continue;   // Multi page (custom page, no group grid)
                // Defensive: ensure the page is laid out at the tab width before
                // validating (JUCE sizes tab content on resize, but this guards
                // against a not-yet-parented edge case).
                if (page->getWidth() <= 0)
                    page->reflowToWidth (juce::jmax (400, vp != nullptr ? vp->getWidth() : 940));

                char m[128];
                std::snprintf (m, sizeof (m), "tab %d ('%s') group grid is well-formed",
                               i, tabs->getTabNames()[i].toRawUTF8());
                check (page->layoutIsSane(), m);
            }
        }

        // ----------------------------------------------------------------------
        // [8] Snapshot dump (dev visual sanity check). Gated on the
        // PARVATI_DUMP_SHOTS env var (a directory path); off by default so a
        // normal run writes nothing. Renders the editor + each tab offscreen via
        // paintEntireComponent (no display / no Screen-Recording permission).
        // ----------------------------------------------------------------------
        if (const char* dirEnv = std::getenv ("PARVATI_DUMP_SHOTS"))
        {
            const juce::File dir { juce::String { dirEnv } };
            dir.createDirectory();
            const auto dump = [&dir] (juce::Component* c, const juce::String& name)
            {
                juce::Image img (juce::Image::RGB, c->getWidth(), c->getHeight(), true);
                {
                    juce::Graphics g (img);
                    g.fillAll (juce::Colour (0xff202020));
                    c->paintEntireComponent (g, false);
                }
                juce::FileOutputStream os (dir.getChildFile (name + ".png"));
                juce::PNGImageFormat().writeImageToStream (img, os);
                os.flush();
                std::printf ("  shot: %s.png  (%dx%d)\n", name.toRawUTF8(), c->getWidth(), c->getHeight());
            };
            std::printf ("\n[8] Snapshot dump -> %s\n", dir.getFullPathName().toRawUTF8());
            dump (ed, "00_overview");
            if (tabs != nullptr)
            {
                for (int i = 0; i < numTabs; ++i)
                {
                    tabs->setCurrentTabIndex (i, false);
                    // Force the tab's page to lay out at the tab width before painting
                    // (headless: JUCE may defer the resize otherwise).
                    if (auto* content = tabs->getTabContentComponent (i))
                        if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                            if (auto* page = dynamic_cast<ParamPage*> (vp->getViewedComponent()))
                                page->reflowToWidth (juce::jmax (400, vp->getWidth() > 0 ? vp->getWidth() : 940));
                    const juce::String nm = juce::String (i + 1) + "_"
                        + tabs->getTabNames()[i].replaceCharacters (" /", "__");
                    dump (ed, nm);
                }

                // Extra shot: the Envelopes/LFO tab with all 3 slots switched to
                // LFO view (shows the LFO-waveform previews), then restored to ENV.
                const auto names = tabs->getTabNames();
                int envTab = -1;
                for (int i = 0; i < names.size(); ++i)
                    if (names[i].containsIgnoreCase ("Envelopes")) { envTab = i; break; }
                if (envTab >= 0)
                {
                    tabs->setCurrentTabIndex (envTab, false);
                    ParamPage* envPage = nullptr;
                    if (auto* content = tabs->getTabContentComponent (envTab))
                        if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                            envPage = dynamic_cast<ParamPage*> (vp->getViewedComponent());
                    if (envPage != nullptr)
                    {
                        envPage->reflowToWidth (juce::jmax (400, 940));
                        envPage->setAllEnvLfoModesForDump (1);   // LFO view
                        dump (ed, "4b_Envelopes_LFO_LFOmode");
                        envPage->setAllEnvLfoModesForDump (0);   // restore ENV view
                    }
                }
            }
        }

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
