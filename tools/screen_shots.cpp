// tools/screen_shots.cpp
//
// Renders the Parvati editor to PNGs for the integrated (Serum-style) layout:
// a SYNTH overview (3-column workspace: Mixer | Oscillators | Filter, plus the
// default nested ENV/LFO + MOD tabs), one shot per nested tab (Envelopes, LFOs,
// Mod Matrix, Modifiers, Arp, Sequencer), and the GLOBAL page — exactly as a user
// sees the plugin. The main-row pages (Mixer/Oscillators/Filter) are always
// visible on the SYNTH tab, so they appear in every SYNTH shot.
//
// How: instantiate the real ParvatiAudioProcessor + editor (so the
// ParvatiLookAndFeel — embedded Unifont font, colours, layout — is byte-for-byte
// the shipped appearance), switch tabs, and paint the whole editor offscreen via
// paintEntireComponent at 2x for AI-readable crispness. No display or
// screen-recording permission required; fully deterministic.
//
// Build:   cmake --build build_release --target parvati_screen_shots
// Run:     ./build_release/parvati_screen_shots [outputDir] [scale]   (defaults: ./screens, 2)
//
// The font assertions printed to stderr are identical to those the project's own
// headless editor_test emits (set-a-style-on-a-typeface); they are benign and
// do not affect the rendered output.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/GroupPager.h"

#include <cstdio>
#include <vector>

namespace
{
// Collect every TabbedComponent in c's subtree (DFS). With the integrated layout
// the SYNTH tab content (SynthWorkspace) holds the two nested tab groups
// ([ENV|LFO] and [MOD MATRIX|MODIFIERS|ARP|SEQ]); the top-level page selector is
// the first TC found from the editor root.
void collectTabbedComponents (juce::Component* c, std::vector<juce::TabbedComponent*>& out)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c)) out.push_back (t);
    for (auto* child : c->getChildren())
        collectTabbedComponents (child, out);
}

// Ensure a tab's content is laid out at the tab width before painting (headless:
// JUCE may defer the resize). No Viewports remain: a tab's content is a GroupPager
// (whose resized() reflows its page) or a direct ParamPage.
void ensureLaidOut (juce::Component* tabContent)
{
    if (auto* page = dynamic_cast<ParamPage*> (tabContent))
    {
        if (page->getWidth() <= 0)
            page->reflowToWidth (940);
        return;
    }
    if (auto* pager = dynamic_cast<GroupPager*> (tabContent))
        if (auto* page = pager->getPage())
            if (page->getWidth() <= 0)
                page->reflowToWidth (940);
}

// The tabs are created with their short label ("OSC", "MOD MATRIX", ...). Map
// those to the full section names for clearer output filenames.
juce::String fullTabName (const juce::String& shortName)
{
    static const std::pair<juce::String, juce::String> map[] = {
        { "OSC",        "Oscillators" },
        { "MIX",        "Mixer"       },
        { "FILTER",     "Filter"      },
        { "ENV",        "Envelopes"   },
        { "LFO",        "LFOs"        },
        { "MOD MATRIX", "Mod_Matrix"  },
        { "MODIFIERS",  "Modifiers"   },
        { "ARP",        "Arp"         },
        { "SEQ",        "Sequencer"   },
        { "GLOBAL",     "Global"      },
    };
    for (const auto& [k, v] : map)
        if (shortName == k) return v;
    return shortName;
}

// Paint a component (and all children) into a fresh image at @p scale.
juce::Image renderScreen (juce::Component& c, float scale)
{
    const int w = juce::roundToInt ((float) c.getWidth()  * scale);
    const int h = juce::roundToInt ((float) c.getHeight() * scale);
    juce::Image img (juce::Image::RGB, w, h, true);
    {
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (juce::Colour (0xff202020));
        c.paintEntireComponent (g, false);
    }
    return img;
}

void savePng (const juce::Image& img, const juce::File& file)
{
    juce::FileOutputStream os (file);
    if (! os.openedOk())
    {
        std::printf ("  SKIP  %s (cannot open output)\n", file.getFileName().toRawUTF8());
        return;
    }
    juce::PNGImageFormat().writeImageToStream (img, os);
    os.flush();
    std::printf ("  wrote %-28s (%dx%d)\n", file.getFileName().toRawUTF8(), img.getWidth(), img.getHeight());
}
}  // namespace

int main (int argc, char** argv)
{
    juce::MessageManager::getInstance();   // macOS: main thread == message thread

    const juce::File outDir { argc > 1 ? juce::String { argv[1] } : juce::String { "screens" } };
    const float scale      = argc > 2 ? std::max (1.0f, (float) std::atof (argv[2])) : 2.0f;
    outDir.createDirectory();

    ParvatiAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    auto* ed = processor.createEditor();
    if (ed == nullptr)
    {
        std::printf ("FAIL: createEditor() returned null\n");
        return 1;
    }

    // Top-level page selector [SYNTH | GLOBAL] = the first TabbedComponent in
    // the editor tree (it is added before the nested workspace tab groups).
    std::vector<juce::TabbedComponent*> topLevel;
    collectTabbedComponents (ed, topLevel);
    if (topLevel.empty())
    {
        std::printf ("FAIL: no TabbedComponent found in editor\n");
        return 1;
    }
    auto* pageSelector = topLevel.front();

    auto tabIndex = [] (juce::TabbedComponent* tc, const char* name) -> int
    {
        if (tc == nullptr) return -1;
        const auto names = tc->getTabNames();
        for (int i = 0; i < names.size(); ++i)
            if (names[i] == name) return i;
        return -1;
    };
    const int synthIdx  = juce::jmax (0, tabIndex (pageSelector, "SYNTH"));
    const int globalIdx = tabIndex (pageSelector, "GLOBAL");

    std::printf ("Rendering screens @ %.0fx -> %s\n", (double) scale, outDir.getFullPathName().toRawUTF8());

    int n = 0;
    juce::MemoryBlock lastPng;   // consecutive-duplicate suppression

    // Render the current editor state to a PNG, skipping if it is byte-identical
    // to the previous capture. The integrated layout's default nested tab repeats
    // the overview state, so this keeps the output set clean and sequentially
    // numbered (only saved shots consume a number).
    auto capture = [&] (const juce::String& desc)
    {
        const juce::Image img = renderScreen (*ed, scale);
        juce::MemoryBlock mb;
        {
            juce::MemoryOutputStream mos (mb, false);
            juce::PNGImageFormat().writeImageToStream (img, mos);
        }
        if (mb == lastPng)
        {
            std::printf ("  skip  %-24s (duplicate of previous)\n", desc.toRawUTF8());
            return;
        }
        lastPng = mb;
        savePng (img, outDir.getChildFile (juce::String::formatted ("%02d_", ++n) + desc));
    };

    // 1) SYNTH overview: the 3-column workspace (Mixer | Oscillators | Filter)
    //    plus the default nested ENV/LFO + MOD tabs.
    pageSelector->setCurrentTabIndex (synthIdx, false);
    capture ("Synth_overview.png");

    // 2) One shot per nested tab inside the SYNTH workspace. The main-row pages
    //    (Mixer/Oscillators/Filter) stay visible in every shot; only the nested
    //    tab content changes.
    if (auto* synth = pageSelector->getTabContentComponent (synthIdx))
    {
        std::vector<juce::TabbedComponent*> nested;
        collectTabbedComponents (synth, nested);
        for (auto* tc : nested)
        {
            const auto names = tc->getTabNames();
            for (int t = 0; t < tc->getNumTabs(); ++t)
            {
                tc->setCurrentTabIndex (t, false);
                ensureLaidOut (tc->getTabContentComponent (t));
                capture (fullTabName (names[t]) + ".png");
            }
        }
    }

    // 3) GLOBAL page (synth options + voice-activity cells).
    if (globalIdx >= 0)
    {
        pageSelector->setCurrentTabIndex (globalIdx, false);
        ensureLaidOut (pageSelector->getTabContentComponent (globalIdx));
        capture ("Global.png");
    }

    processor.editorBeingDeleted (ed);
    delete ed;

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\nDone. %d screens in: %s\n", n, outDir.getFullPathName().toRawUTF8());
    return 0;
}
