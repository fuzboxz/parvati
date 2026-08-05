// tools/menu_shots.cpp
//
// Renders Parvati's right-click / popup menus to PNGs — exactly as they appear
// (themed colours + active font) — so they can be fed to an LLM for review.
//
// Why a dedicated tool: juce::PopupMenu shows itself as a live OS window, and
// its item list is private API, so it cannot be captured offscreen directly.
// Instead we reconstruct each menu's items and paint them through the SAME
// ParvatiLookAndFeel the live menu uses (the editor's), calling the public L&F
// hooks the live PopupMenu calls: drawPopupMenuBackground + drawPopupMenuItem +
// getIdealPopupMenuItemSize + getPopupMenuFont. The result is byte-faithful to
// the shipped appearance (Carbon theme, embedded Unifont), at 2x for crispness.
//
// Each menu is rendered in two states: resting (no highlight) and with the first
// item highlighted, so both the text colour and the accent highlight are visible.
//
// Build: cmake --build build_release --target parvati_menu_shots
// Run:   ./build_release/parvati_menu_shots [outputDir] [scale]   (defaults: ./screens/menus, 2)

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/ParvatiLookAndFeel.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
// One menu to render: a label (output filename stem) + its item texts.
struct MenuSpec
{
    const char* stem;
    std::vector<juce::String> items;
};

// Paint a single themed menu into an Image at @p scale and write it to @p file.
// @p highlightIdx is the 0-based item to show highlighted, or -1 for none.
void renderMenu (ParvatiLookAndFeel& lnf,
                 const juce::String& fileStem,
                 const std::vector<juce::String>& items,
                 int highlightIdx,
                 const juce::File& outDir,
                 int scale)
{
    // Measure every item with the active L&F (matches the live PopupMenu).
    constexpr int standardItemHeight = 0;   // 0 => natural (font height * 1.3)
    int maxItemW = 0, totalH = 0;
    std::vector<int> itemH;
    itemH.reserve (items.size());
    for (const auto& text : items)
    {
        int iw = 0, ih = 0;
        lnf.getIdealPopupMenuItemSize (text, false, standardItemHeight, iw, ih);
        ih = juce::jmax (ih, 16);   // a touch of breathing room
        itemH.push_back (ih);
        totalH += ih;
        maxItemW = juce::jmax (maxItemW, iw);
    }

    const int padX = 2, padY = 2;
    const int w = maxItemW + padX * 2;
    const int h = totalH + padY * 2;

    juce::Image img (juce::Image::RGB, w * scale, h * scale, true);
    {
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale ((float) scale));

        // Background fill + themed scanline overlay (whatever the live L&F draws).
        lnf.drawPopupMenuBackground (g, w, h);

        // Items top-to-bottom, full menu width (matches the live layout).
        int y = padY;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const juce::Rectangle<int> itemRect (padX, y, maxItemW, itemH[i]);
            lnf.drawPopupMenuItem (g, itemRect,
                                   false,                      // isSeparator
                                   true,                       // isActive
                                   static_cast<int> (i) == highlightIdx,  // isHighlighted
                                   false,                      // isTicked
                                   false,                      // hasSubMenu
                                   items[i],                   // text
                                   juce::String(),             // shortcutKeyText
                                   nullptr,                    // icon
                                   nullptr);                   // textColourToUse
            y += itemH[i];
        }
    }

    const juce::File file = outDir.getChildFile (fileStem + ".png");
    juce::FileOutputStream os (file);
    if (! os.openedOk())
    {
        std::printf ("  FAIL: could not open %s\n", file.getFullPathName().toRawUTF8());
        return;
    }
    juce::PNGImageFormat().writeImageToStream (img, os);
    os.flush();
    std::printf ("  wrote %-34s (%dx%d)\n", (file.getFileName()).toRawUTF8(), w * scale, h * scale);
}

} // namespace

int main (int argc, char** argv)
{
    using namespace juce;

    const std::string outDirArg = (argc > 1) ? argv[1] : "screens/menus";
    const int scale = (argc > 2) ? std::max (1, std::atoi (argv[2])) : 2;

    ScopedJuceInitialiser_GUI gui;
    const File outDir { String { outDirArg } };
    outDir.createDirectory();

    ParvatiAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    AudioProcessorEditor* ed = processor.createEditor();
    if (ed == nullptr)
    {
        std::printf ("FAIL: createEditor() returned null\n");
        return 1;
    }

    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&ed->getLookAndFeel());
    if (lnf == nullptr)
    {
        std::printf ("FAIL: editor LookAndFeel is not a ParvatiLookAndFeel\n");
        delete ed;
        return 1;
    }

    std::printf ("Rendering themed popup menus @ %dx -> %s\n", scale, outDir.getFullPathName().toRawUTF8());

    // The real right-click / popup menus (verbatim item text from
    // ParamControl::showContextMenu + the Save format menu in PluginEditor).
    const std::vector<MenuSpec> menus {
        { "01_context_menu",            { TRANS ("Reset to default"), TRANS ("Randomize") } },
        { "03_save_format",             { TRANS ("Ambika Patch (.PRO)"), TRANS ("Parvati Patch (.parvati)") } },
    };

    for (const auto& m : menus)
    {
        // Resting state (no highlight) — how the menu looks when first opened.
        renderMenu (*lnf, m.stem, m.items, -1, outDir, scale);
        // First item highlighted — shows the accent highlight colour + on-colour text.
        renderMenu (*lnf, juce::String (m.stem) + "_highlight", m.items, 0, outDir, scale);
    }

    processor.editorBeingDeleted (ed);
    delete ed;

    std::printf ("\nDone. %d menu renders in: %s\n",
                 static_cast<int> (menus.size() * 2), outDir.getFullPathName().toRawUTF8());
    return 0;
}
