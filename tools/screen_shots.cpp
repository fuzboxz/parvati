// tools/screen_shots.cpp
//
// Renders the full Parvati editor screen for each page (Oscillators, Mixer,
// Filter, Envelopes, LFOs, Mod Matrix, Modifiers, Arp, Sequencer, Global) to
// individual PNGs — exactly as a user sees the plugin — so they can be fed to a
// UI-redesign agent.
//
// How: instantiate the real ParvatiAudioProcessor + editor (so the
// ParvatiLookAndFeel — embedded Unifont font, colours, layout — is byte-for-byte
// the shipped appearance), switch to each tab, and paint the whole editor
// offscreen via paintEntireComponent at 2x for AI-readable crispness. No display
// or screen-recording permission required; fully deterministic.
//
// Build:   cmake --build build --target parvati_screen_shots
// Run:     ./build/parvati_screen_shots [outputDir] [scale]   (defaults: ./screens, 2)
//
// The font assertions printed to stderr are identical to those the project's own
// headless editor_test emits (set-a-style-on-a-typeface); they are benign and
// do not affect the rendered output.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <cstdio>

namespace
{
juce::TabbedComponent* findTabs (juce::Component* c)
{
    if (auto* t = dynamic_cast<juce::TabbedComponent*> (c)) return t;
    for (auto* child : c->getChildren())
        if (auto* t = findTabs (child)) return t;
    return nullptr;
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

    auto* tabs = findTabs (ed);
    if (tabs == nullptr)
    {
        std::printf ("FAIL: no TabbedComponent found in editor\n");
        return 1;
    }

    std::printf ("Rendering page screens @ %.0fx -> %s\n", (double) scale, outDir.getFullPathName().toRawUTF8());

    const int numTabs = tabs->getNumTabs();
    for (int t = 0; t < numTabs; ++t)
    {
        tabs->setCurrentTabIndex (t, false);
        const juce::String name = fullTabName (tabs->getTabNames()[t]);

        // Force the page to lay out at the tab width (headless: JUCE may defer
        // the resize otherwise) — same guard as tools/editor_test.cpp.
        if (auto* content = tabs->getTabContentComponent (t))
            if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                if (auto* page = dynamic_cast<ParamPage*> (vp->getViewedComponent()))
                    if (page->getWidth() <= 0)
                        page->reflowToWidth (juce::jmax (400, vp->getWidth() > 0 ? vp->getWidth() : 940));

        const juce::String fname = juce::String::formatted ("%02d_", t + 1) + name + ".png";
        savePng (renderScreen (*ed, scale), outDir.getChildFile (fname));
    }

    processor.editorBeingDeleted (ed);
    delete ed;

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\nDone. %d screens in: %s\n", numTabs, outDir.getFullPathName().toRawUTF8());
    return 0;
}
