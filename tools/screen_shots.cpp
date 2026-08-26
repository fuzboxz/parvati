// tools/screen_shots.cpp
//
// Renders the Hellcat editor to PNGs for the integrated (Serum-style) layout:
// a SYNTH overview (the 3-row SynthWorkspace: top OSC|MIX|FILTER direct pages,
// the full-width CentralModBar, and the bottom active-editor host + ModMatrixView),
// one in-context shot per generator (each generator pill clicked via the
// catalogue so its editor is surfaced in the live workspace), one standalone
// shot per generated ParamPage (via HellcatEditor::allGeneratedPages()), and the
// GLOBAL overlay (header button) — exactly as a user sees the plugin.
//
// How: instantiate the real HellcatAudioProcessor + editor (so the
// HellcatLookAndFeel — system sans-serif font, colours, layout — is byte-for-byte
// the shipped appearance), surface each page, and paint it offscreen via
// paintEntireComponent at 2x for AI-readable crispness. No display or
// screen-recording permission required; fully deterministic.
//
// Build:   cmake --build build --target hellcat_screen_shots
// Run:     ./build/hellcat_screen_shots [outputDir] [scale]   (defaults: ./screens, 2)
//
// CANONICAL (builds the tool from latest source, then runs it — never stale):
//   cmake --build build --target screens
//
// The font assertions printed to stderr are identical to those the project's own
// headless editor_test emits (set-a-style-on-a-typeface); they are benign and
// do not affect the rendered output.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/ModSourceCatalog.h"   // hellcat::kAllSources (generator pill set)
#include "ui/SynthWorkspace.h"     // complete type for findFirst<SynthWorkspace>
#include "ui/FxWorkspace.h"        // complete type for findFirst<FxWorkspace>

#include <cstdio>
#include <vector>

namespace
{
// First component of type T in the subtree (DFS).
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

// Collect every ParamControl in c's subtree (c included).
void collectParamControls (juce::Component* c, std::vector<ParamControl*>& out)
{
    if (auto* p = dynamic_cast<ParamControl*> (c))
        out.push_back (p);
    for (auto* child : c->getChildren())
        collectParamControls (child, out);
}

// Ensure a page is laid out at a readable width before painting (headless: JUCE
// may defer the resize). A ParamPage owns its controls whether parented or not,
// so a standalone (unparented generator) page renders fine here too.
void ensureLaidOut (ParamPage* page, int width = 940)
{
    if (page == nullptr)
        return;
    if (page->getWidth() <= 0)
        page->reflowToWidth (width);
}

// Map a page's dominant control prefix to a clear output filename. Returns an
// empty string for the Global page (it is captured separately as the header-
// button overlay) and for any page without a recognised control.
juce::String pageName (ParamPage* page)
{
    if (page == nullptr)
        return {};
    std::vector<ParamControl*> cs;
    collectParamControls (page, cs);
    bool hasCard = false, hasCurve = false;
    bool isLfo = false;   // envN_lfo_* / voice_lfo_* live on the LFO page
    juce::String name;
    for (auto* c : cs)
    {
        const auto id = c->getParamID();
        if (id == "filter_card") hasCard = true;
        if (id == "vca_curve")   hasCurve = true;
        if (id.contains ("_lfo_")) isLfo = true;
        if (name.isEmpty())
        {
            // "modif" before "mod" (modifiers vs nothing-on-a-ParamPage); the
            // mod-matrix mod{N}_* params live on ModMatrixView, not a ParamPage.
            if      (id.startsWith ("modif")) name = "Modifiers";
            else if (id.startsWith ("osc"))   name = "Oscillators";
            else if (id.startsWith ("mix"))   name = "Mixer";
            else if (id.startsWith ("filter"))name = "Filter";
            else if (id.startsWith ("env"))   name = "Envelopes";
            else if (id.startsWith ("seq"))   name = "Sequencer";
            else if (id.startsWith ("arp"))   name = "Arp";
            // FX-slot pages (fx1_/fx2_/fx3_ + fx_topo/fx_order). Standalone shots
            // of each of the 3 FX slot panels (type/enabled/drywet/4 params).
            else if (id.startsWith ("fx1"))   name = "FX_Slot1";
            else if (id.startsWith ("fx2"))   name = "FX_Slot2";
            else if (id.startsWith ("fx3"))   name = "FX_Slot3";
        }
    }
    if (hasCard && hasCurve)
        return {};   // Global page -> handled by the overlay capture
    if (isLfo)
        return "LFOs";   // overrides the "env" prefix (env1_lfo_* is an LFO control)
    return name;
}

// Paint a component (and all children) into a fresh image at @p scale.
juce::Image renderScreen (juce::Component& c, float scale)
{
    const int w = juce::roundToInt ((float) c.getWidth()  * scale);
    const int h = juce::roundToInt ((float) c.getHeight() * scale);
    juce::Image img (juce::Image::RGB, juce::jmax (1, w), juce::jmax (1, h), true);
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
    // JUCE's FileOutputStream opens an existing file at EOF (append mode — see
    // juce_SharedCode_posix.h openHandle: lseek SEEK_END). Without this reset a
    // re-render would concatenate the new PNG after the old one, so viewers kept
    // showing the stale first image and the file grew on every run.
    os.setPosition (0);
    os.truncate();
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

    HellcatAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    auto* ed = processor.createEditor();
    if (ed == nullptr)
    {
        std::printf ("FAIL: createEditor() returned null\n");
        return 1;
    }

    // Optional theme override: HELLCAT_SHOT_THEME="Y2K" renders every shot
    // in that theme through the REAL selection path (the Settings combo's
    // seam — selectThemeForTest), so a theme can be reviewed headlessly.
    // Unset (or unknown name) keeps the DEFAULT theme: the canonical
    // 40-screen byte-identity baseline is untouched.
    if (const char* themeName = std::getenv ("HELLCAT_SHOT_THEME"); themeName != nullptr)
        if (auto* editor = dynamic_cast<HellcatEditor*> (ed); editor != nullptr)
            if (! editor->switchThemeSynchronousForTest (juce::String { themeName }))
                std::printf ("WARN: unknown HELLCAT_SHOT_THEME '%s' (using the default)\n", themeName);

    auto* workspace = findFirst<SynthWorkspace> (ed);

    std::printf ("Rendering screens @ %.0fx -> %s\n", (double) scale, outDir.getFullPathName().toRawUTF8());

    int n = 0;
    juce::MemoryBlock lastPng;   // consecutive-duplicate suppression

    // Render a component to a PNG, skipping if byte-identical to the previous
    // capture. Keeps the output set clean and sequentially numbered (only saved
    // shots consume a number).
    auto capture = [&] (juce::Component& c, const juce::String& desc)
    {
        if (c.getWidth() <= 0 || c.getHeight() <= 0)
        {
            std::printf ("  skip  %-24s (zero size)\n", desc.toRawUTF8());
            return;
        }
        const juce::Image img = renderScreen (c, scale);
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

    // 1) SYNTH overview: the default 3-row workspace (ENV 1 is the default
    //    active generator).
    capture (*ed, "Synth_overview.png");

    // 2) One in-context shot per generator: click each GENERATOR pill via its
    //    catalogue enum through SynthWorkspace::setActiveGenerator (the real
    //    click path) — the generator's page is reparented into the active-editor
    //    host in the live 3-row layout, then the whole editor is captured exactly
    //    as a user sees that generator selected. The catalogue drives the same
    //    generator pages HellcatEditor::allGeneratedPages() owns; one generator
    //    page can back several pills (Env1/2/3, LFO1..4), so a per-pill capture
    //    shows each group subset, and the bar-only NOTE sentinel is included.
    if (workspace != nullptr)
    {
        for (const auto& src : hellcat::kAllSources)
        {
            if (! src.isGenerator)
                continue;
            workspace->setActiveGenerator (src.enumValue);
            capture (*ed, juce::String (src.fullName).replace (" ", "_") + ".png");
        }
    }

    // 2b) FX workspace (Hellcat-exclusive). Switch the top-level page selector to
    //     the FX tab and capture the 3-row FX layout (3 slot panels | CentralModBar
    //     | shared generator editor + FxMatrixView), exactly as a user sees it.
    //     The modulator editor is SHARED with SYNTH, so each generator pill is
    //     also captured in the FX context. A populated variant (FX1 = Reverb,
    //     enabled, + an FX-matrix routing) shows the FX + matrix in action.
    auto* editor = dynamic_cast<HellcatEditor*> (ed);
    if (editor != nullptr)
    {
        editor->setFxMode (true);
        auto* fxs = findFirst<FxWorkspace> (ed);

        // FX overview (default active generator = ENV 1).
        capture (*ed, "FX_overview.png");

        // Each generator surfaced in the FX workspace (shared modulator editor).
        if (fxs != nullptr)
            for (const auto& src : hellcat::kAllSources)
            {
                if (! src.isGenerator)
                    continue;
                fxs->setActiveGenerator (src.enumValue);
                capture (*ed, "FX_" + juce::String (src.fullName).replace (" ", "_") + ".png");
            }

        // Populated FX: enable FX1 = Reverb with dry/wet + an FX-matrix routing
        // (Env 1 -> FX1 Dry/Wet) so the matrix list + an active slot are visible.
        auto setFxParam = [&processor] (const char* id, float v)
        { processor.getApvts().getParameterAsValue (juce::String (id)) = v; };
        setFxParam ("fx1_type",    3.0f);   // Reverb
        setFxParam ("fx1_enabled", 1.0f);
        setFxParam ("fx1_drywet",  72.0f);
        setFxParam ("fx1_param1",  90.0f);
        setFxParam ("fx1_param2",  60.0f);
        setFxParam ("fx1_param3",  80.0f);
        setFxParam ("fx1_param4",  64.0f);
        setFxParam ("fxmod1_source", 0.0f);  // Env 1
        setFxParam ("fxmod1_dest",   0.0f);  // FX1 Dry/Wet
        setFxParam ("fxmod1_amount", 28.0f);
        // ENV 1 is the default active generator after setFxMode, so the shared
        // modulator editor already shows Env 1 in the FX context.
        capture (*ed, "FX_reverb_active.png");

        // Reset FX params to defaults so the standalone FX-slot page captures
        // (below) render clean.
        for (const char* id : { "fx1_type", "fx1_enabled", "fx1_drywet",
                                "fx1_param1", "fx1_param2", "fx1_param3", "fx1_param4",
                                "fxmod1_source", "fxmod1_dest", "fxmod1_amount" })
            setFxParam (id, 0.0f);

        editor->setFxMode (false);   // back to SYNTH for the remaining captures
    }

    // 3) One standalone shot per generated ParamPage via allGeneratedPages()
    //    (the editor-owned pages). Each page is laid out and rendered directly —
    //    this covers the top-row direct pages (Oscillators/Mixer/Filter), which
    //    are not generator pills, and re-emits each generator page as a clean
    //    standalone editor. A ParamPage owns its controls whether parented or
    //    not, so unparented generator pages render here too. The FX-slot pages
    //    (fx1_/fx2_/fx3_) are also editor-owned, so they are captured here too.
    if (editor != nullptr)
    {
        for (auto* page : editor->allGeneratedPages())
        {
            ensureLaidOut (page);
            const juce::String name = pageName (page);
            if (name.isEmpty())
                continue;   // Global page -> overlay capture below
            capture (*page, name + ".png");
        }
    }

    // 4) GLOBAL overlay (the header "Global" button toggles it; render the
    //    overlay page directly — it is a direct child of the editor).
    for (auto* child : ed->getChildren())
        if (auto* gp = dynamic_cast<ParamPage*> (child))
        {
            gp->setVisible (true);
            gp->toFront (false);
            ensureLaidOut (gp);
            capture (*gp, "Global.png");
            gp->setVisible (false);
            break;
        }

    processor.editorBeingDeleted (ed);
    delete ed;

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf ("\nDone. %d screens in: %s\n", n, outDir.getFullPathName().toRawUTF8());
    return 0;
}
