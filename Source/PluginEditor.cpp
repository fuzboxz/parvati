// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See PluginEditor.h.

#include "PluginEditor.h"
#include "ParvatiPreset.h"
#include "ui/EnvelopeDisplay.h"
#include "ui/FilterResponseDisplay.h"
#include "ui/ModDestMap.h"
#include "ui/ModMatrixHighlight.h"
#include "ui/OscPreviewDisplay.h"
#include "ui/MulExportDialog.h"
#include "ui/PatchPage.h"
#include "ui/ParamHelp.h"
#include "ui/SynthParamLabels.h"
#include "ui/SynthWorkspace.h"
#include "ui/FxWorkspace.h"
#include "ui/FxRoutingBar.h"
#include "ui/FxSlotCard.h"
#include "ui/NoteStepControl.h"
#include "ui/SeqLengthStepper.h"
#include "ui/FxMatrixView.h"
#include "ui/ModSourceCatalog.h"   // parvati::kNoteSeqSentinel (bar-only NOTE pill)
#include "ui/WheelsComponent.h"
#include "ui/Translations.h"
#include "ui/ChromeRule.h"      // parvati::ChromeRule (the shared separator-rule family)
#include "ui/LiveFeedbackHub.h"   // parvati::LiveFeedbackHub (live mod-feedback pump)
#include "ui/ModTelemetryTypes.h" // parvati::ModTelemetrySnapshot / LiveEnvStage / LiveFilterValues
#include "dsp/patch.h"            // ambika::dsp::MOD_SRC_* (generator-tab drag payloads)

#include <algorithm>   // std::any_of (file-drag extension check)

// Version string from CMake (Parvati target compile def). Fallback for any
// translation unit that does not get the define.
#ifndef PARVATI_VERSION
#define PARVATI_VERSION "0.0.0"
#endif

// parvati_logo.svg is embedded via a dedicated juce_add_binary_data target
// (NAMESPACE ParvatiLogo, see CMakeLists.txt). We resolve its bytes through
// getNamedResource() rather than #include "BinaryData.h": the project already
// links a second binary-data target (parvati_factory_presets, namespace
// FactoryPresets) whose generated header shares the filename "BinaryData.h",
// so an #include here would be ambiguous across the two JuceLibraryCode include
// dirs. getNamedResource() is emitted in the generated BinaryData.cpp (external
// linkage); "parvati_logo_svg" is the resource name derived from the filename
// parvati_logo.svg.
namespace ParvatiLogo {
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);
}

namespace
{
// NATIVE-DIALOG SUPPRESSION (2026-08-21): the editor's "desktop-gated" file
// seams launch REAL NSOpenPanel/NSSavePanel panels whenever desktop components
// exist — correct for a user's plugin window, WRONG for the GUI test binaries,
// whose harness puts the editor ON the desktop (addToDesktop is the only way
// the JUCE timers run for the timer-driven sections) and then pumps the run
// loop. With components on the desktop the old getNumComponents() > 0 gate is
// true, so every export/load seam popped a native picker on the developer's
// screen mid-test. The test binaries set PARVATI_HEADLESS=1 in their main()
// (and a developer can export it for any manual run); every gate below then
// behaves exactly like the console case: the seam fires, no picker launches.
bool nativeDialogsSuppressed()
{
    return juce::SystemStats::getEnvironmentVariable ("PARVATI_HEADLESS", {}) == "1";
}
}  // namespace

// ---- Header logo: [brand icon] + white "Parvati" wordmark -----------------
// The embedded parvati_logo.svg is true vector art (outlined <path>/<g>, no
// raster); it is parsed once into logoDrawable_ via JUCE's SVG renderer and
// drawn as-is (brand colours, NOT theme-tinted). The "Parvati" wordmark is
// painted in the theme `text` token (theme-aware near-white; dark on Paper)
// so it re-colours on theme switch. The logo block width is measured in
// resized() with the SAME font paint() uses so the logo/version/centre/right
// header cluster stays byte-stable.
constexpr const char* kLogoText       = "PARVATI";   // 2026-08-23: ALL CAPS wordmark
constexpr float       kLogoTextHeight = 17.0f;   // 2026-08-23: smaller + letter-spaced + plain ("remove the boldness", lighter read)
constexpr float       kLogoTracking  = 3.0f;    // extra px BETWEEN characters (the airy wordmark look)

// Letter-spaced wordmark width: the sum of the glyph advances plus
// @p tracking between characters. paint() and resized() both measure the
// block with this ONE helper, so the drawn wordmark always fits the block
// resized() reserves for it (the two loops previously carried their own
// copies of the same math).
static float trackedTextWidth (const juce::Font& font, const juce::String& text, float tracking)
{
    float w = 0.0f;
    for (int i = 0; i < text.length(); ++i)
    {
        juce::GlyphArrangement gc;
        gc.addLineOfText (font, text.substring (i, i + 1), 0.0f, 0.0f);
        w += gc.getBoundingBox (0, gc.getNumGlyphs(), true).getWidth();
    }
    return w + tracking * (float) juce::jmax (0, text.length() - 1);
}

// Re-apply each Label's font in the component tree (same height/style, default
// family) so each re-resolves its typeface through the active L&F after a
// font-mode switch (juce::Label caches its font, so a plain repaint would NOT
// pick up the new family).
void refreshFontsIn (juce::Component* c, const ParvatiLookAndFeel& lnf)
{
    if (c == nullptr)
        return;
    if (auto* l = dynamic_cast<juce::Label*> (c))
    {
        const auto f = l->getFont();
        l->setFont (lnf.appFont (f.getHeight(), f.getStyleFlags()));
    }
    for (auto* child : c->getChildren())
        refreshFontsIn (child, lnf);
}

// F-ios-lc-3 (bug hunt 2026-08-19): count of LIVE ParvatiEditor instances in
// THIS process. AUv3 extension processes host MULTIPLE plugin instances (AUM
// can hold several Parvati AUs; JUCE's AUv3 wrapper creates/destroys one
// editor per instance — juce_audio_plugin_client_AUv3.mm createEditorAndMakeActive
// / removeEditor), so teardown side-effects that touch PROCESS-GLOBAL state
// (screensaver policy, the ParamControl tap-assign statics) must be
// reference-counted: closing editor A while editor B is live must NOT undo
// B's global state. Plain int (message-thread-only: every editor ctor/dtor
// runs on the message thread under JUCE).
static int sLiveEditorCount = 0;

//==============================================================================
ParvatiEditor::ParvatiEditor (ParvatiAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef_ (p)
{
    // F-ios-lc-3: count this editor FIRST so every process-global side-effect
    // below can consult the live count (see sLiveEditorCount).
    ++sLiveEditorCount;

    // The UI is landscape-only (no portrait layout exists). Lock the device to
    // the two landscape orientations. The iOS/Android peer consults this live in
    // its supportedInterfaceOrientations (see juce_UIViewComponentPeer_ios.mm);
    // on desktop it is a harmless no-op (no device rotation).
    juce::Desktop::getInstance().setOrientationsEnabled (
        juce::Desktop::rotatedClockwise | juce::Desktop::rotatedAntiClockwise);

    // Install the persisted chrome language BEFORE building the UI, so every
    // TRANS() below resolves to the right language at construction. English (and
    // "auto" on an English locale) clears the mappings => TRANS() is the
    // identity => the UI is byte-identical to the un-localised build.
    installLanguage (processorRef_.getUiLanguage());

    // Theme + LookAndFeel: one L&F on the editor, inherited by the whole control
    // tree, so no per-component palette is needed.
    lnf_.setTheme (themeManager_.getCurrentTheme());
    setLookAndFeel (&lnf_);
    themeManager_.addChangeListener (this);

    // Tooltips: one TooltipWindow parented to (and deleted with) the editor.
    // ParamControl is a TooltipClient returning its parameter's help text.
    tooltipWindow_ = std::make_unique<juce::TooltipWindow> (this);

    // Phase 4a: apply persisted UI preferences. The theme selection may differ
    // from the ThemeManager default (Carbon); selectByName broadcasts a change
    // (caught by changeListenerCallback) if the selection actually moves.
    themeManager_.selectByName (processorRef_.getUiTheme());
    lnf_.setTheme (themeManager_.getCurrentTheme());
    ParamControl::setTooltipsEnabled (processorRef_.getUiTooltips());
    if (patchPage_ != nullptr)
        patchPage_->setTableTooltipsEnabled (processorRef_.getUiTooltips());

    // Apply the persisted parameter-smoothing preference to the engine (the
    // SettingsPanel toggle is seeded from getUiSmoothing() when it is built
    // below; this covers the audio side for hosts that show the editor).
    processorRef_.setParameterSmoothing (processorRef_.getUiSmoothing());

    // Group every descriptor into its section bucket; EVERY part_* param
    // (volume/tuning/spread included, 2026-08-20 completing absorption) lives
    // in the Patch page's part table — no part knob is generated on any page.
    // `part_select` is intentionally skipped here: it has a dedicated top-bar
    // ComboBox (partCombo_) bound to the same APVTS param, so generating a
    // second control for it on a page would be redundant.
    // Sized from the ENUM (Section::FxMatrix + 1), not a literal: only the
    // sections up to Global ever reach the index (the isFx skip below keeps
    // Fx/FxMatrix out), but an enum-sized array can never overflow when a
    // future section is added.
    std::vector<const PatchParamDescriptor*> sec[(size_t) Section::FxMatrix + 1];
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.paramID == "part_select")
            continue;
        // Part settings ABSORBED into the Patch page's per-part table: Octave /
        // Legato / Portamento are TABLE COLUMNS (PartRow), raga (Scale) /
        // polyphony are already covered by the table's Tune / Poly columns,
        // and Volume / Tuning / Spread joined the table as the Vol / Fine /
        // Spr columns (the completing absorption — the hosted page renders
        // ONLY the Global options now). The APVTS PARAMETERS stay fully valid
        // (created by the untouched descriptor table in
        // createParvatiParameterLayout — host automation, state, .parvati);
        // only the page KNOB is not generated, so there is exactly ONE editor
        // surface per setting (the table).
        if (d.paramID == "part_octave" || d.paramID == "part_legato"
            || d.paramID == "part_portamento" || d.paramID == "part_raga"
            || d.paramID == "part_polyphony" || d.paramID == "part_volume"
            || d.paramID == "part_tuning" || d.paramID == "part_spread")
            continue;
        // FX params (fx* / fxmod*) are per-part Parvati-exclusive params routed
        // via applyFxParameter; they are NEVER bucketed into the synth pages.
        // The FX-slot pages are generated separately below, and the FX mod matrix
        // is the editor-owned FxMatrixView (no ParamPage at all).
        if (d.isFx)
            continue;
        sec[(int) sectionForId (d.paramID)].push_back (&d);
    }

    // Integrated workspace hosts the 9 synth ParamPages (built + routed below).
    // Created early so the page-build loop can reparent each page into it.
    synthWorkspace_ = std::make_unique<SynthWorkspace> (themeManager_);

    const ParvatiTheme& theme = themeManager_.getCurrentTheme();

    // ---- Top patch bar: factory patch list + Load .PRO... + name ----
    // (No "Patch:" caption: the preset dropdown follows the brand block
    // directly; see resized().)

    // Cascading patch menu (Templates / User / Factory banks / Multi). The
    // browser scans the dirs live on each open, so there is no pre-populate.
    presetBrowser_ = std::make_unique<PresetBrowser> (
        processorRef_.getTemplatesDir(), processorRef_.getUserPatchDir(),
        processorRef_.getFactoryPatchDir(), processorRef_.getFactoryMultiDir(),
        [this] (const juce::File& f) { applyPatchFile (f); });
    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
    addAndMakeVisible (*presetBrowser_);

    loadButton_.setButtonText (TRANS ("Load"));
    // Button colours from the L&F.
    loadButton_.onClick = [this] { openLoadDialog(); };
    addAndMakeVisible (loadButton_);

    // Save: DIRECT .parvati save (2026-08-20 — no format menu). The Ambika
    // .PRO/.MUL exports moved to dedicated buttons on the Patch page
    // (openSaveDialog / openSaveMultiDialog are now export-only paths);
    // drag-drop import of .PRO/.MUL/.parvati is unchanged (filesDropped).
    saveButton_.setButtonText (TRANS ("Save"));
    saveButton_.onClick = [this] { handleSavePresetShortcut(); };   // desktop-gated direct .parvati
    addAndMakeVisible (saveButton_);

    // Phase 4c: Undo / Redo are Path-drawn IconButtons (curved arrows) — no
    // unicode glyph (the font stack renders U+21B6/21B7 as "..."). The APVTS
    // UndoManager records every parameter change; enable/disable is mirrored on
    // the editor timer.
    undoButton_.setTooltip (TRANS ("Undo"));
    // undoSafe/redoSafe (not the raw UndoManager): they sweep the part-switch
    // boundary first — a recorded action replayed after a part switch would
    // write the OLD part's values into the CURRENT part (cross-part
    // corruption; see ParvatiAudioProcessor::undoSafe).
    undoButton_.onClick = [this] { processorRef_.undoSafe(); };
    // Test seam: the Path-drawn IconButtons carry no text — name them so the
    // layout sweep can locate the primary-set members by name.
    undoButton_.setName ("headerUndo");
    addAndMakeVisible (undoButton_);
    redoButton_.setTooltip (TRANS ("Redo"));
    redoButton_.onClick = [this] { processorRef_.redoSafe(); };
    redoButton_.setName ("headerRedo");
    addAndMakeVisible (redoButton_);

    // Zoom actions: the buttons stay CONSTRUCTED (the '...' overflow popup
    // and the keyboard shortcuts drive the same applyZoom() helper) but are
    // NOT placed and NOT visible. F-ios-touch-3 (bug hunt 2026-08-19): they
    // (The three former top-bar zoom buttons were REMOVED 2026-08-20: zoom
    // lives in the Settings panel now — three buttons + readout replacing the
    // old zoom slider. The Cmd/Ctrl +/-/0 keyboard shortcuts remain.) The
    // "..." overflow button below is the W9 folded-actions host: visible ONLY
    // below the 1024 fold breakpoint, where its popup actually carries the
    // folded Part/page/mod items (see resized()).)
    zoomOverflowButton_.setTooltip (TRANS ("More"));
    zoomOverflowButton_.onClick = [this]
    {
        juce::PopupMenu m;
        m.setLookAndFeel (&lnf_);   // app-themed popup (amber accent, dark fill)
        // SafePointer guards against the editor being deleted while the
        // async menu is still open (host closes the plugin window mid-menu) —
        // the menu outlives onClick's stack frame, so a raw `this` would
        // dangle when the item action finally runs.
        juce::Component::SafePointer<ParvatiEditor> safe (this);
        // ---- W9 folded header actions (AUv3 compact panes): the popup grows
        // the sections whose header controls are now folded away (the
        // SAME breakpoints resized() uses, re-evaluated at click time so a
        // resize between layout and click can never desync the menu). Every
        // item drives the SAME seam as the hidden control: page items call
        // showTopPage (the buttons' onClick entry point), the toggles
        // triggerClick their hidden buttons (toggle state + onClick wiring),
        // the Part items setSelectedId the real combo (attachment -> APVTS).
        // Labels reuse existing keys — no new translation strings.
        if (safe != nullptr)
        {
            if (safe->getWidth() < 1024)   // Part combo + [Synth]/[FX] folded
            {
                m.addSeparator();
                juce::PopupMenu partMenu;
                for (int i = 1; i <= SynthEngine::getNumParts(); ++i)
                    partMenu.addItem (juce::PopupMenu::Item (TRANS ("Part") + " " + juce::String (i))
                                          .setAction ([safe, i] { if (safe != nullptr) safe->partCombo_.setSelectedId (i, juce::sendNotificationSync); }));
                m.addSubMenu (TRANS ("Part"), partMenu);
                m.addItem (juce::PopupMenu::Item (TRANS ("Synth page")).setAction ([safe] { if (safe != nullptr) safe->showTopPage (0); }));
                m.addItem (juce::PopupMenu::Item (TRANS ("FX page")).setAction    ([safe] { if (safe != nullptr) safe->showTopPage (1); }));
            }
            if (safe->getWidth() < 810)    // [MOD]/[MAP]/gear folded
            {
                m.addSeparator();
                m.addItem (juce::PopupMenu::Item (TRANS ("Toggle the modulation pill bar")).setAction ([safe] { if (safe != nullptr) safe->modBarToggleButton_.triggerClick(); }));
                m.addItem (juce::PopupMenu::Item (TRANS ("Tap-to-assign modulation")).setAction      ([safe] { if (safe != nullptr) safe->modAssignButton_.triggerClick(); }));
                m.addItem (juce::PopupMenu::Item (TRANS ("Settings")).setAction                   ([safe] { if (safe != nullptr) safe->settingsButton_.triggerClick(); }));
            }
            if (safe->getWidth() < 650)    // [Patch] page button + Redo folded
            {
                m.addSeparator();
                m.addItem (juce::PopupMenu::Item (TRANS ("Patch / arrangement page")).setAction ([safe] { if (safe != nullptr) safe->showTopPage (2); }));
                m.addItem (juce::PopupMenu::Item (TRANS ("Redo")).setAction                       ([safe] { if (safe != nullptr) safe->redoButton_.triggerClick(); }));
            }
        }
        m.showMenuAsync (juce::PopupMenu::Options()
                             .withTargetComponent (&zoomOverflowButton_)
                             .withStandardItemHeight (ParvatiLookAndFeel::kPopupRowHeight),
                         nullptr);
    };
    addAndMakeVisible (zoomOverflowButton_);

    // ---- Top bar: Part selector (bound to the `part_select` APVTS param) ----
    // (No "Part:" caption: the dropdown alone carries the selection.)
    for (int i = 1; i <= SynthEngine::getNumParts(); ++i)
        partCombo_.addItem (TRANS ("Part") + " " + juce::String (i), i);
    // Combo colours from the L&F.
    addAndMakeVisible (partCombo_);
    partComboAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef_.getApvts(), "part_select", partCombo_);

    // pageSelector_ ([SYNTH | GLOBAL]) tab-bar depth + outline are set after its
    // tabs are populated (below). TabbedComponent / TabbedButtonBar colours come
    // from the inherited editor L&F.

    struct PageInfo { Section s; int cols, cellW, cellH; };
    // Cell heights are kept tight (a 44px knob + its label fits in ~76px) so
    // every page matches the dense SEQ reference instead of the sparse look the
    // 106px rows produced. Mod/Modifier/Seq groups override these in
    // configureGroupLayouts() — their entries here are kept for reference only.
    // (The former name/shortName fields were never read — the loop below uses
    // only s/cols/cellW/cellH.)
    const PageInfo pages[] = {
        { Section::Oscillators, 4, 214, 76 },
        { Section::Mixer,       4, 214, 76 },
        { Section::Filter,      4, 214, 76 },
        { Section::Envelopes,   3, 198, 76 },
        { Section::Lfos,        4, 198, 76 },
        { Section::ModMatrix,   2, 164, 72 },
        { Section::Modifiers,   3, 300, 64 },   // cellH overridden per-group (configureGroupLayouts); ref only
        { Section::Sequencer,   6, 150, 80 },
        { Section::Arp,         3, 214, 76 },
        { Section::Global,      3, 214, 76 },
    };

    // Generator pages captured by section during the loop, then registered with
    // the CentralModBar's active-generator editor (bottom-left host). Each is an
    // editor-owned ParamPage (reparented, never regenerated); ARP shows all its
    // groups (empty setVisibleGroups set).
    ParamPage* envPage = nullptr;
    ParamPage* lfoPage = nullptr;
    ParamPage* modifierPage = nullptr;
    ParamPage* arpPage = nullptr;
    ParamPage* seqPage = nullptr;

    for (const auto& pg : pages)
    {
        // MOD MATRIX is now the editor-owned ModMatrixView (Wave 1), NOT a
        // ParamPage: build + host it here and skip page generation entirely.
        // The mod1_*/mod14_* APVTS params are created independently in
        // createParameterLayout (ParameterLayout.cpp), so removing this ParamPage
        // does NOT touch the byte-bridge / patch / DSP. The view is hosted
        // NON-owned by the MOD MATRIX tab (editor-owned via modMatrixView_).
        if (pg.s == Section::ModMatrix)
        {
            modMatrixView_ = std::make_unique<ModMatrixView> (processorRef_, themeManager_);
            synthWorkspace_->setModMatrixView (modMatrixView_.get());
            continue;
        }

        auto page = std::make_unique<ParamPage> (processorRef_, themeManager_, sec[(int) pg.s],
                                                 pg.cols, pg.cellW, pg.cellH);

        // Live previews: an ADSR curve under each Env group (Envelopes tab) and
        // an LFO waveform under each LFO group (LFOs tab). The getters read the
        // APVTS parameter's NORMALIZED value (getValue() returns 0..1) so the
        // preview tracks the knobs live. (Each env_lfo unit runs BOTH its
        // envelope and its LFO; splitting the halves onto two tabs matches that.)
        auto norm = [this] (const juce::String& id) -> float {
            auto* param = processorRef_.getApvts().getParameter (id);
            return param ? param->getValue() : 0.0f;
        };
        // Register a graph preview for live category re-tinting on theme change:
        // each entry stores a closure calling the concrete component's
        // setCategoryColour + a theme-token pointer so reapplyGraphCategoryColours
        // can re-resolve the NEW theme's value and re-push it.
        auto bindGraph = [this] (GraphTintFn fn, ThemeColourField field) {
            graphCategoryBindings_.emplace_back (std::move (fn), field);
        };
        if (pg.s == Section::Envelopes)
        {
            const juce::String envs[3] = { "env1", "env2", "env3" };
            const juce::String envLabels[3] = { "Env 1 (Mod)", "Env 2 (Filter)", "Env 3 (Amp)" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = envs[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    envLabels[i],
                    [norm, e] { return norm (e + "_attack");  },
                    [norm, e] { return norm (e + "_decay");   },
                    [norm, e] { return norm (e + "_sustain"); },
                    [norm, e] { return norm (e + "_release"); });
                disp->setPreviewMode (0);   // ADSR curve
                // Live stage marker (docs/LIVE_MOD_FEEDBACK_DESIGN.md): while a
                // key is held, a dot + hairline rides the curve through
                // Attack/Decay/Sustain/Release from the engine's REAL envelope
                // telemetry. The provider captures `this` and null-checks
                // liveHub_ at CALL time because the displays are built BEFORE
                // the hub exists in the ctor (and the pages outlive neither).
                disp->setLiveStageProvider ([this, i]
                {
                    return liveHub_ != nullptr ? liveHub_->envStage (i)
                                               : parvati::LiveEnvStage{};
                });
                // Register for the status tick's poll re-assert (the raw
                // pointer follows the graphCategoryBindings_ lifetime rule).
                liveEnvDisplays_.push_back (disp.get());
                // Cyan trace from the Envelopes category token (re-resolved live
                // on theme change via the binding registered below).
                disp->setCategoryColour (theme.catEnv);
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catEnv);
                page->setGroupDecoration (envLabels[i], std::move (disp));
            }
        }
        else if (pg.s == Section::Lfos)
        {
            // LFO 1/2/3: the LFO half of env_lfo[0..2] (shape drives the preview).
            const juce::String lfos[3] = { "env1", "env2", "env3" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = lfos[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    "LFO " + juce::String (i + 1),
                    std::function<float()> {}, std::function<float()> {},
                    std::function<float()> {}, std::function<float()> {},
                    [norm, e] { return norm (e + "_lfo_shape"); });
                disp->setPreviewMode (1);   // LFO waveform
                // Register for the status tick's poll re-assert: LFO previews
                // are EnvelopeDisplays in previewMode 1 — a page built while not
                // yet on screen (the Viewport-swap architecture: pages are built
                // once, shown on demand) starves the component's own visibility
                // hooks, the poll never starts, and the preview freezes on its
                // first-frame waveform while the mod pill (always-on bar timer)
                // tracks every change — the reported "LFO stuck on S&H" bug.
                liveEnvDisplays_.push_back (disp.get());
                disp->setCategoryColour (theme.catLfo);   // magenta trace
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catLfo);
                page->setGroupDecoration ("LFO " + juce::String (i + 1), std::move (disp));
            }
            // Voice LFO (MOD_SRC_LFO_4).
            auto vdisp = std::make_unique<EnvelopeDisplay> (
                "Voice LFO",
                std::function<float()> {}, std::function<float()> {},
                std::function<float()> {}, std::function<float()> {},
                [norm] { return norm ("voice_lfo_shape"); });
            vdisp->setPreviewMode (1);
            vdisp->setCategoryColour (theme.catLfo);   // magenta trace
            // Same status-tick poll re-assert as the LFO displays above.
            liveEnvDisplays_.push_back (vdisp.get());
            bindGraph ([gp = vdisp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                       &ParvatiTheme::catLfo);
            page->setGroupDecoration ("Voice LFO", std::move (vdisp));
        }
        else if (pg.s == Section::Oscillators)
        {
            // INLINE waveform preview beside each OSC Shape dropdown: one
            // OscPreviewDisplay per oscillator, laid out in the reserved column
            // 1 (configureGroupLayouts gives each OSC group 5 columns). Amber
            // (catAudio) trace, re-tinted live on theme change.
            const juce::String oscs[2]  = { "osc1", "osc2" };
            const juce::String labels[2] = { "Osc 1", "Osc 2" };
            for (int i = 0; i < 2; ++i)
            {
                const juce::String o = oscs[i];
                auto disp = std::make_unique<OscPreviewDisplay> (
                    labels[i] + " Wave",
                    [norm, o] { return norm (o + "_shape"); },
                    [norm, o] { return norm (o + "_param"); });
                disp->setCategoryColour (theme.catAudio);   // amber trace
                bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                           &ParvatiTheme::catAudio);
                // Live modulation overlay (2026-08-23 parity pass — same
                // contract as the filter display): while a voice sounds and
                // the EFFECTIVE osc parameter byte is MOVING (env/LFO/matrix/
                // wheel routed to PARAMETER_1/2), the preview's smoothed target
                // follows the engine state instead of the knob, so modulations
                // visibly ride the waveform and settle back at rest. Same
                // call-time null-check pattern (construction precedes the hub).
                disp->setLiveValuesProvider ([this, i]
                {
                    return liveHub_ != nullptr ? liveHub_->liveOsc (i)
                                               : parvati::LiveOscValues{};
                });
                // Register for the status tick's poll re-assert (same starve
                // class as the env/LFO displays: a page built while not on
                // screen can starve the component's own visibility hooks and
                // the 30 Hz poll never starts — the preview freezes on its
                // first painted waveform, e.g. after theme/drawer cycles).
                liveOscDisplays_.push_back (disp.get());
                page->setGroupInlinePreview (labels[i], std::move (disp));
            }
        }
        else if (pg.s == Section::Filter)
        {
            // Magnitude-response curve under the "Filter" group (decoration).
            // Compact height so the Filter column (3 knobs + filter-env/lfo
            // amounts + the curve) fits the main-row half-height at the 1100
            // minimum. Amber (catAudio) trace, re-tinted live on theme change.
            auto disp = std::make_unique<FilterResponseDisplay> (
                "Filter Response",
                [norm] { return norm ("filter1_cutoff"); },
                [norm] { return norm ("filter1_reso"); },
                [norm] { return norm ("filter1_mode"); });
            // Live modulation overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md): when
            // the cutoff/resonance is ACTIVELY being modulated (env-2 sweep,
            // LFO, matrix, wheel...), the display draws the live EFFECTIVE
            // curve + cutoff tick over the opaque base preview. Same call-time
            // null-check pattern as the envelope markers above (construction
            // precedes the hub).
            disp->setLiveValuesProvider ([this]
            {
                return liveHub_ != nullptr ? liveHub_->liveFilter()
                                           : parvati::LiveFilterValues{};
            });
            // Register for the status tick's poll re-assert (same lifetime
            // rule as liveEnvDisplays_ / graphCategoryBindings_).
            liveFilterDisplay_ = disp.get();
            disp->setCategoryColour (theme.catAudio);   // amber trace
            bindGraph ([gp = disp.get()] (const juce::Colour& c) { gp->setCategoryColour (c); },
                       &ParvatiTheme::catAudio);
            page->setGroupDecoration ("Filter", std::move (disp));
            page->setGroupDecorationHeight ("Filter", 42);
        }

        if (pg.s == Section::Global)
            globalPage_ = page.get();   // hosted (reparented) into the Patch page below

        page->setSize (page->getContentWidth(), page->getContentHeight());
        ParamPage* rawPage = page.get();
        generatedPages_.push_back (std::move (page));

        // Route the editor-owned page into the integrated workspace by section.
        // The Global page is hosted inside the Patch page after the loop.
        // Pages are reparented — NOT regenerated — so every APVTS attachment and
        // the checked byte-bridge survive the reorganization unchanged. Dense
        // sections paginate by group via a GroupPager (one sub-tab = one group
        // subset) so each visible slice fits its cell with NO scrollbar. (Patch
        // is never a generated page; if/else avoids switch/enum + branch-clone
        // Route the editor-owned page by section. Main-row pages (MIX/OSC/
        // FILTER) are hosted directly. Generator pages (ENV/LFO/MODIFIERS/ARP/
        // SEQ) are captured here and registered with the CentralModBar's
        // active-generator editor AFTER the loop (one pill -> one page+group).
        // Pages are reparented — NOT regenerated — so every APVTS attachment and
        // the checked byte-bridge survive unchanged. (Patch/ModMatrix never
        // reach here; the if/else avoids switch/enum + branch-clone warnings.)
        if (pg.s == Section::Mixer)
            synthWorkspace_->setMainLeft (rawPage);
        else if (pg.s == Section::Oscillators)
            synthWorkspace_->setOscillators (rawPage);   // both osc panels visible directly
        else if (pg.s == Section::Filter)
            synthWorkspace_->setMainRight (rawPage);
        else if (pg.s == Section::Envelopes)
            envPage = rawPage;
        else if (pg.s == Section::Lfos)
            lfoPage = rawPage;
        else if (pg.s == Section::Modifiers)
            modifierPage = rawPage;
        else if (pg.s == Section::Arp)
            arpPage = rawPage;
        else if (pg.s == Section::Sequencer)
            seqPage = rawPage;
        // Section::ModMatrix is handled by the early-continue above (ModMatrixView).
        // Section::Global (-> hosted inside the Patch page after the loop)
        // intentionally falls through here.
    }

    // ---- Central Modulation Bar wiring (Phase 2) ----
    // The generator pills are registered for BOTH workspaces by ONE shared
    // table further below (after fxWorkspace_ exists — the pages are SHARED,
    // never duplicated; only reparented between workspaces on a mode toggle).
    // The drag-only pill clicks and the Env-1 startup default are wired there
    // too.


    // ---- FX workspace construction (Phase 4) ----
    // FxWorkspace is a structural clone of SynthWorkspace (TOP = 3 FX-slot
    // ParamPages, MIDDLE = its own CentralModBar, BOTTOM-LEFT = the SHARED
    // active-generator host, BOTTOM-RIGHT = the editor-owned FxMatrixView). It
    // reuses the synth's generator ParamPages (shared, never duplicated) so a
    // mode toggle reparents a single active selection between the two
    // workspaces. The FX-slot pages + FxMatrixView are editor-owned
    // (generatedPages_ / fxMatrixView_) and hosted NON-owned by the workspace.
    fxWorkspace_ = std::make_unique<FxWorkspace> (themeManager_);

    // Generate the 3 FX-slot CARDS (FX1/FX2/FX3) — self-contained modular cards
    // (power/bypass toggle + type combo + visualizer + a param knob grid with the
    // dry/wet anchored bottom-right). Each card CREATES + OWNS its 6 full
    // ParamControls from the fx{N}_param1..5 + fx{N}_drywet descriptors (so they
    // keep EVERY modulation behaviour: FX-mod-matrix drag-drop + mod rings +
    // tooltips + category arc). fx_topo / fx_order now ride on the full-width
    // FxRoutingBar (set below), NOT on a slot page. Cards are editor-owned
    // (fxSlotCards_) and hosted NON-owned via setFxSlotCard.
    for (int slot = 0; slot < 3; ++slot)
    {
        const juce::String prefix = "fx" + juce::String (slot + 1) + "_";
        const PatchParamDescriptor *p1 = nullptr, *p2 = nullptr, *p3 = nullptr,
                                   *p4 = nullptr, *p5 = nullptr, *dw = nullptr;
        for (const auto& d : getPatchParamDescriptors())
        {
            if (! (d.isFx && juce::String (d.paramID).startsWith (prefix)))
                continue;
            if      (d.paramID == prefix + "param1") p1 = &d;
            else if (d.paramID == prefix + "param2") p2 = &d;
            else if (d.paramID == prefix + "param3") p3 = &d;
            else if (d.paramID == prefix + "param4") p4 = &d;
            else if (d.paramID == prefix + "param5") p5 = &d;
            else if (d.paramID == prefix + "drywet") dw = &d;
        }
        jassert (p1 != nullptr && p2 != nullptr && p3 != nullptr
                 && p4 != nullptr && p5 != nullptr && dw != nullptr);
        auto card = std::make_unique<FxSlotCard> (processorRef_, slot,
                                                  p1, p2, p3, p4, p5, dw);
        FxSlotCard* raw = card.get();
        fxSlotCards_[slot] = std::move (card);
        fxWorkspace_->setFxSlotCard (slot, raw);
    }

    // The FX routing header bar (topology dropdown + drag-reorderable chain).
    // Editor-owned, hosted NON-owned above the three cards.
    fxRoutingBar_ = std::make_unique<FxRoutingBar> (processorRef_, themeManager_);
    fxWorkspace_->setFxRoutingBar (fxRoutingBar_.get());

    // The FX mod matrix (editor-owned, NON-owned host of the FX workspace).
    fxMatrixView_ = std::make_unique<FxMatrixView> (processorRef_, themeManager_);
    fxWorkspace_->setFxMatrixView (fxMatrixView_.get());

    // ---- Central Modulation Bar wiring (Phase 2, BOTH workspaces) ----
    // Register every GENERATOR pill -> { owning ParamPage, groups-to-show } so
    // each bar's bottom-left active-editor host can reparent +
    // setVisibleGroups the right slice per pill (pages are never regenerated;
    // the SAME pages are shared by both workspaces, only reparented between
    // them on a mode toggle). Group names match the ParamPage groupForId()
    // keys (checked in groupForId). VLFO == per-voice LFO (MOD_SRC_LFO_4,
    // checked in voice.cpp). ARP shows ALL its groups (EMPTY array). The Note
    // Sequencer pill is the bar-only sentinel (parvati::kNoteSeqSentinel ==
    // -1, NOT a real MOD_SRC_*): it reveals its "Note Pitch" group from the
    // Sequencer page (Option A: only the note pitch + gate control; velocity
    // stays in the full Sequencer TAB), and is click-only (the bar skips its
    // drag because enumValue < 0). The drag payload
    // ("parvatiModSrc:<enum>") is emitted by the bar itself, so the
    // destination-side rings / padlock / ModMatrixHighlight need ZERO changes.
    // ONE table drives both workspaces (previously two 15-call copies that
    // had to be kept in sync by hand).
    using namespace ambika::dsp;
    struct GeneratorPill { int source; ParamPage* page; juce::StringArray groups; };
    const GeneratorPill generatorPills[] = {
        { MOD_SRC_ENV_1, envPage,     juce::StringArray{ "Env 1 (Mod)" } },
        { MOD_SRC_ENV_2, envPage,     juce::StringArray{ "Env 2 (Filter)" } },
        { MOD_SRC_ENV_3, envPage,     juce::StringArray{ "Env 3 (Amp)" } },
        { MOD_SRC_LFO_1, lfoPage,     juce::StringArray{ "LFO 1" } },
        { MOD_SRC_LFO_2, lfoPage,     juce::StringArray{ "LFO 2" } },
        { MOD_SRC_LFO_3, lfoPage,     juce::StringArray{ "LFO 3" } },
        { MOD_SRC_LFO_4, lfoPage,     juce::StringArray{ "Voice LFO" } },
        { MOD_SRC_SEQ_1, seqPage,     juce::StringArray{ "Sequencer 1" } },
        { MOD_SRC_SEQ_2, seqPage,     juce::StringArray{ "Sequencer 2" } },
        { MOD_SRC_ARP_STEP, arpPage,  juce::StringArray{} },   // empty => all groups
        // Note Sequencer pill (bar-only sentinel): click-only, NOT draggable;
        // opens the Sequencer page showing ONLY Note Pitch (the remapped note
        // rotary + gate-at-rest). Note Velocity is NOT shown in the generator
        // host — the ~290px non-viewport band cannot fit both 16-step groups,
        // so stacking them clipped velocity ~75%. Velocity stays reachable in
        // the full Sequencer TAB (its knob is unchanged). One group => no clip.
        { parvati::kNoteSeqSentinel, seqPage, juce::StringArray{ "Note Pitch" } },
        { MOD_SRC_OP_1, modifierPage, juce::StringArray{ "Modifier 1" } },
        { MOD_SRC_OP_2, modifierPage, juce::StringArray{ "Modifier 2" } },
        { MOD_SRC_OP_3, modifierPage, juce::StringArray{ "Modifier 3" } },
        { MOD_SRC_OP_4, modifierPage, juce::StringArray{ "Modifier 4" } },
    };
    // Generic lambda: SynthWorkspace and FxWorkspace are unrelated types that
    // expose the same registerGeneratorPage seam.
    const auto registerPills = [&generatorPills] (auto& workspace)
    {
        for (const auto& pill : generatorPills)
            workspace.registerGeneratorPage (pill.source, pill.page, pill.groups);
    };
    registerPills (*synthWorkspace_);
    registerPills (*fxWorkspace_);

    // Drag-only (Perf/Util/Const) pill click: briefly flash the matrix rows
    // routed FROM that source, reusing the existing timed flash. Each
    // workspace flashes ITS OWN matrix (synth mod matrix / FX mod matrix).
    synthWorkspace_->setOnDragOnlyPillClicked ([this] (int src)
    {
        if (modMatrixView_ != nullptr)
            modMatrixView_->flashRowsForSource (src);
    });
    fxWorkspace_->setOnDragOnlyPillClicked ([this] (int src)
    {
        if (fxMatrixView_ != nullptr)
            fxMatrixView_->flashRowsForSource (src);
    });
    // Default to Env 1 visible on startup.
    synthWorkspace_->setActiveGenerator (MOD_SRC_ENV_1);
    // Track the SHARED active generator selection from BOTH workspaces so a mode
    // toggle reparents the right page into the newly-visible workspace.
    synthWorkspace_->setOnActiveGeneratorChanged ([this] (int src) { activeGeneratorModSrc_ = src; });
    fxWorkspace_->setOnActiveGeneratorChanged    ([this] (int src) { activeGeneratorModSrc_ = src; });
    // NOTE: the FX workspace does NOT host the active generator at startup — the
    // shared page can only have ONE parent, and the VISIBLE workspace (SYNTH,
    // default) owns it (set above). setFxMode(true) releases it from SYNTH and
    // reparents it into FX on demand (via activeGeneratorModSrc_).

    // ---- Live modulation feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // ONE poll pump for the whole system: constructed now that both workspaces
    // exist, BEFORE the status timer starts. Its fetcher is the engine's bounded
    // seqlock read; the hub caches ONE consistent frame per tick and every
    // consumer (the two mod bars' pill strips + the envelope/filter display
    // overlays, whose providers were bound during page construction ABOVE and
    // null-check liveHub_ at CALL time) reads the cache — so the engine's lock
    // is taken once per tick no matter how many components animate.
    liveHub_ = std::make_unique<parvati::LiveFeedbackHub> (
        [this] (parvati::ModTelemetrySnapshot& s)
        { return processorRef_.getEngine().readUiTelemetry (s); });
    // Bind the SAME cached-snapshot provider to BOTH workspace bars (each bar
    // owns its own poll timer — both stop while unparented, so only the VISIBLE
    // seam animates).
    const auto barTelemetryProvider = [this] (parvati::ModTelemetrySnapshot& s)
    { return liveHub_ != nullptr ? liveHub_->snapshot (s) : false; };
    if (auto* bar = synthWorkspace_->modBar())
        bar->setTelemetryProvider (barTelemetryProvider);
    if (auto* bar = fxWorkspace_->modBar())
        bar->setTelemetryProvider (barTelemetryProvider);
    // Point the engine's telemetry at the part being EDITED (the frame follows
    // the header Part selector; the processor's load/part-switch reset hooks are
    // the authoritative re-sync, this is the extra startup copy).
    processorRef_.getEngine().setUiTelemetryPart (processorRef_.getEngine().getCurrentPart());
    // Apply the persisted refresh rate once up front (the timerCallback
    // re-checks every tick, so a Settings change lands within one tick).
    applyLiveFeedbackRefreshRate (processorRef_.getUiRefreshHz());

    // ---- Top-level page selector [SYNTH | FX] ----
    // Two NON-owned tab contents (synthWorkspace_ at index 0, fxWorkspace_ at
    // index 1). The tab bar is HIDDEN (depth 0) — the header [Synth]/[FX]
    // buttons are the UI (setFxMode swaps the current tab). PATCH is a header
    // overlay (patchPage_, which hosts globalPage_), not a tab. Both tab
    // contents are editor-owned (synthWorkspace_ / fxWorkspace_), so the teardown
    // order stays deterministic.
    pageSelector_.setTabBarDepth (0);          // hide the tab bar — [Synth]/[FX] header buttons are the UI
    pageSelector_.setOutline (0);
    pageSelector_.addTab (TRANS ("SYNTH"), theme.backgroundBase, synthWorkspace_.get(), false);
    pageSelector_.addTab (TRANS ("FX"),    theme.backgroundBase, fxWorkspace_.get(),     false);
    pageSelector_.setCurrentTabIndex (0, false);   // SYNTH shown first
    // F-ios-touch-3 (bug hunt 2026-08-19): the hidden (0-depth) tab bar still
    // CREATES its TabbedButtons, and they stay enabled+visible at 0x0 extent —
    // juce focus traversal includes them (it filters only visible+enabled,
    // not extent), so an iPad hardware keyboard could Tab onto an INVISIBLE
    // page button. They are never the UI (the header radio buttons drive
    // pageSelector_ via setCurrentTabIndex); disable them to drop them from
    // the traversal list. (Same class as the parked zoom trio — see the ctor.)
    // Hiding the BAR (not the buttons — the bar re-shows them on every
    // layout pass) removes the whole subtree from visibility walks, focus
    // traversal and hit-testing in one step; setCurrentTabIndex still works
    // (it only toggles button state).
    pageSelector_.getTabbedButtonBar().setVisible (false);
    addAndMakeVisible (pageSelector_);

    // ---- Patch page overlay (custom component, not descriptor-generated) ----
    // The Patch page replaces the old separate Multi/Setup + Global pages: it
    // hosts the editor-owned Section::Global ParamPage (patch-wide knobs) with
    // its 6 part rows merged into the Global panel. A header "Patch" button
    // (next to the Part dropdown) toggles this page as a full-page view.
    // globalPage_ ownership stays in generatedPages_; hostParamPage
    // only reparents it into the Patch page.
    patchPage_ = std::make_unique<PatchPage> (processorRef_, themeManager_);
    addChildComponent (patchPage_.get());   // owned here; invisible until toggled
    patchPage_->setVisible (false);
    // Relabel the top-bar Part selector when a part name/alias is edited.
    patchPage_->onPartNamesChanged = [this] { refreshPartComboNames(); };
    // Ambika export seams (desktop-gated: the file pickers + the fallback
    // dialog need a window server; headless tests fire the seam with no
    // picker — the handleLoadPresetShortcut idiom).
    patchPage_->onExportPro = [this] {
        if (! nativeDialogsSuppressed() && juce::Desktop::getInstance().getNumComponents() > 0)
            openSaveDialog();          // .PRO export (current part)
    };
    patchPage_->onExportMul = [this] {
        if (! nativeDialogsSuppressed() && juce::Desktop::getInstance().getNumComponents() > 0)
            openSaveMultiDialog();     // .MUL export (incl. fallback dialog)
    };
    if (globalPage_ != nullptr)
        patchPage_->hostParamPage (globalPage_);   // reparents the Section::Global ParamPage into the Patch page

    // ---- Unified 3-way top-level page selector: [Synth][FX][Patch] ----
    // All three header buttons are radio-group peers; each selects its PAGE via
    // showTopPage(idx), which sets EXCLUSIVE visibility (Patch is now a FULL
    // page — pageSelector_ is hidden while it is active, so it is the sole
    // content, not a floating overlay) and syncs every button. Synth/FX
    // also reparent the shared generator (only on a real Synth<->FX
    // change). NOT APVTS params — view-state only.
    synthModeButton_.setTooltip (TRANS ("Synth page"));
    fxModeButton_.setTooltip    (TRANS ("FX page"));
    globalButton_.setTooltip    (TRANS ("Patch / arrangement page"));
    for (auto* b : { &synthModeButton_, &fxModeButton_, &globalButton_ })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1, juce::dontSendNotification);
        addAndMakeVisible (*b);
    }
    synthModeButton_.onClick = [this] { showTopPage (0); };
    fxModeButton_.onClick    = [this] { showTopPage (1); };
    globalButton_.onClick    = [this] { showTopPage (2); };
    showTopPage (0);   // SYNTH is the default page (sets visibility + button states)

    // ---- [KBD] header toggle: show/hide the bottom virtual keyboard ----
    // The keyboard floats as an OVERLAY over the bottom of the workspace:
    // toggling only shows/hides it. The content area keeps its FULL height
    // whether or not the keyboard is visible, so the synth controls never move
    // (the keyboard bounds are positioned once in resized() and only its
    // visibility toggles here). See resized() for the overlay placement +
    // z-order.
    kbdToggleButton_.setTooltip (TRANS ("Toggle virtual keyboard"));
    kbdToggleButton_.setClickingTogglesState (true);
    kbdToggleButton_.setToggleState (false, juce::dontSendNotification);   // hidden by default: the workspace keeps its full height with the keyboard hidden; toggling [KBD] floats the TALL two-octave strip over the bottom row (it covers the generator editor + matrix; the content never moves)
        kbdToggleButton_.onClick = [this] {
        const bool on = kbdToggleButton_.getToggleState();
        // Musical typing while tweaking (2026-08-21): hand focus to the strip
        // when it appears. Control tweaks mid-performance no longer need a
        // tree-wide focus pass — ParvatiEditor::keyPressed forwards unhandled
        // plain keys to the KeyboardView, so the QWERTY keys keep playing
        // whatever holds the focus.
        if (on && keyboardView_ != nullptr)
            keyboardView_->grabKeyboardFocus();
        // Turning [KBD] ON while the Patch page is showing also needs toFront:
        // the Patch overlay was lifted above the keyboard when the page was
        // entered, so a newly shown keyboard must re-lift itself (and the
        // wheels) above it to actually appear in Patch mode.
        if (keyboardView_ != nullptr)
        {
            keyboardView_->setVisible (on);
            if (on && currentTopPage_ == 2)
                keyboardView_->toFront (false);
        }
        if (wheels_ != nullptr)
        {
            wheels_->setVisible (on);
            if (on && currentTopPage_ == 2)
                wheels_->toFront (false);
        }
    };
    addAndMakeVisible (kbdToggleButton_);

    // ---- [MOD] header toggle: show/hide the central mod-pill bar ----
    // The bar is a fixed-height SEAM in both workspaces (Synth + FX). Hiding
    // collapses the seam — its height rejoins the content rows (a relayout,
    // not an overlay like [KBD], because the bar occupies layout space). Both
    // workspaces are toggled together so switching SYNTH<->FX never reflows on
    // the difference. The bar is NOT torn down: re-showing is a cheap relayout
    // and the pill state (active generator, scroll) survives.
    modBarToggleButton_.setTooltip (TRANS ("Toggle the modulation pill bar"));
    modBarToggleButton_.setClickingTogglesState (true);
    modBarToggleButton_.setToggleState (true, juce::dontSendNotification);   // shown by default (the historical look)
    modBarToggleButton_.onClick = [this] {
        const bool on = modBarToggleButton_.getToggleState();
        if (synthWorkspace_ != nullptr) synthWorkspace_->setModBarVisible (on);
        if (fxWorkspace_    != nullptr) fxWorkspace_->setModBarVisible (on);
    };
    addAndMakeVisible (modBarToggleButton_);

    // ---- [MOD] header toggle: tap-to-assign modulation ----
    // Where there is no drag-and-drop (touch), modulation routing is reached by
    // toggling [MOD] ON, tapping a mod source, then tapping a destination knob —
    // which calls the same requestAssign seam itemDropped uses. ON reuses the
    // drop-zone affordance (ring on dest knobs, dim non-targets) so the tap
    // mode mirrors a drag visually. The toggled button is the "still in assign
    // mode" indicator.
    modAssignButton_.setTooltip (TRANS ("Tap-to-assign modulation"));
    modAssignButton_.setClickingTogglesState (true);
    modAssignButton_.setToggleState (false, juce::dontSendNotification);
    modAssignButton_.onClick = [this] {
        ParamControl::setTapAssignActive (modAssignButton_.getToggleState());
    };
    addAndMakeVisible (modAssignButton_);

    // ---- Header: brand icon + white "Parvati" wordmark (painted, left) ----
    // (The version subtitle is painted inside the brand block — see paint().
    // The former versionLabel_ child was built hidden and never shown, so it
    // is gone.)

    // ---- Phase 4a: settings button + side panel ----
    // Click-toggle feedback reflects whether the Settings panel is open (the
    // "on" colour is the theme accent, via TextButton::buttonOnColourId). The
    // authoritative sync is the panel's onPanelShowHide callback below, which
    // fires on any show/hide (button click, dismiss glyph, click-outside).
    settingsButton_.setClickingTogglesState (true);
    settingsButton_.setTooltip (TRANS ("Settings"));
    settingsButton_.onClick = [this] {
        const bool opening = ! settingsPanelHost_->isPanelShowing();
        // Pre-size BEFORE the slide starts so the drawer's content is in the
        // animation's proxy snapshot (see SettingsScrollTracker::
        // preSizeForOpen — otherwise the first open slides in blank).
        if (opening && settingsScrollTracker_ != nullptr)
            settingsScrollTracker_->preSizeForOpen();
        settingsPanelHost_->showOrHide (! settingsPanelHost_->isPanelShowing());
        settingsButton_.setToggleState (settingsPanelHost_->isPanelShowing(),
                                        juce::dontSendNotification);
        // Patch mode: the overlay's toFront lifted it above the panel when the
        // page was entered, so a newly shown panel re-lifts itself above it
        // (the keyboard overlay stays under the panel either way).
        if (settingsPanelHost_->isPanelShowing() && currentTopPage_ == 2)
            settingsPanelHost_->toFront (false);
    };
    addAndMakeVisible (settingsButton_);

    // ---- Phase 4a: virtual keyboard (bottom strip) ----
    // Click-to-play routes MIDI into the processor's MidiMessageCollector
    // (thread-safe); the timer mirrors sounding notes back as latch highlights.
    keyboardView_ = std::make_unique<KeyboardView>();
    // Ableton-style settings feedback: every Z/X (octave) or C/V (velocity)
    // change surfaces in the status/tooltip bar as a ~2.5 s transient (the
    // same channel tap-to-assign uses), e.g. "Keyboard: octave C4–C6  ·  velocity 100".
    // The initial prime from setSettingsChangedCallback is swallowed (a fresh
    // instance shows nothing until the user actually changes something). The
    // "seen" flag is PER-INSTANCE (a shared_ptr captured by value — no `this`
    // is captured, so the callback is safe even if it ever fired late): a
    // process-wide `static bool` was consumed by the FIRST editor ever opened,
    // so every later instance (or a close+reopen) flashed a spurious
    // initial report in the status strip at open.
    auto primed = std::make_shared<bool> (false);
    keyboardView_->setSettingsChangedCallback ([primed] (int base, int vel)
    {
        if (! *primed) { *primed = true; return; }   // discard the one initial-state report
        ParamControl::postTransientStatus (
            TRANS ("Keyboard: octave ") + midiNoteName (base) + "\u2013" + midiNoteName (base + 24)
                + "  \u00b7  " + TRANS ("velocity") + " " + juce::String (vel),
            75);   // ~2.5 s @ 30 Hz (rapid Z Z Z / V V V re-post one live readout)
    });
    keyboardView_->setNoteCallback ([this] (int note, bool on, float vel) {
        const int ch = currentPartMidiChannel();
        const int status   = on ? (0x90 | ((ch - 1) & 0xf)) : (0x80 | ((ch - 1) & 0xf));
        const int velocity = on ? juce::jlimit (0, 127, juce::roundToInt (vel * 127.0f)) : 0;
        processorRef_.addMidiEvent (juce::MidiMessage (status, note, velocity));
    });
    // Continuous Y-position pressure -> CHANNEL PRESSURE (Aftertouch): moving
    // a held finger up/down a key tracks AT live (SynthEngine::handleChannelPressure
    // routes it per-voice to MOD_SRC_AFTERTOUCH). Fires on every drag move and
    // with each strike; released notes are unaffected (their pressure simply
    // stops updating — the engine holds the last value per voice, like a real
    // channel-pressure stream).
    keyboardView_->setPressureCallback ([this] (float pressure) {
        const int ch = currentPartMidiChannel();
        const int value = juce::jlimit (0, 127, juce::roundToInt (pressure * 127.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::channelPressureChange (ch, value));
    });
    addAndMakeVisible (*keyboardView_);
    keyboardView_->refresh();

    // ---- Pitch + Mod wheels (left of the keyboard) ----
    wheels_ = std::make_unique<WheelsComponent>();
    wheels_->onPitch = [this] (float v) {
        const int ch = currentPartMidiChannel();
        const int pv = juce::jlimit (0, 16383, juce::roundToInt ((v * 0.5f + 0.5f) * 16383.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::pitchWheel (ch, pv));
    };
    wheels_->onMod = [this] (float v) {
        const int ch = currentPartMidiChannel();
        const int mv = juce::jlimit (0, 127, juce::roundToInt (v * 127.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::controllerEvent (ch, 1, mv));   // CC1 = mod wheel
    };
    // Octave [<][>] under the pitch wheel: drive the same Ableton-style octave
    // shift as the Z/X keys. shiftOctave clamps at the MIDI edges, moves the
    // visible window and fires the settings-changed tooltip readout.
    wheels_->onOctaveShift = [this] (int steps)
    {
        if (keyboardView_ != nullptr)
            keyboardView_->shiftOctave (steps * 12);
    };
    addAndMakeVisible (*wheels_);
    // Computer-keyboard (musical-typing) play is a STANDALONE-only affordance.
    // In a plugin host the DAW owns the computer keyboard (e.g. Ableton's
    // "Computer MIDI Keyboard") and routes it as normal MIDI, so capturing keys
    // here would double-trigger and steal keystrokes from the host.
    keyboardView_->setComputerKeyboardEnabled (
        processorRef_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);

    // (No per-voice activity cells exist: the per-part V1..V16 meter was
    // removed from the Global panel and the whole-patch squares from the
    // voice-pool view. Voice activity is carried by the bottom status strip's
    // part-relative count and the Patch page's per-part active/allocated
    // counts.)

    // ---- Bottom status strip: compact active-voice count + tooltip bar ----
    statusCountLabel_.setJustificationType (juce::Justification::centred);
    statusCountLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    statusCountLabel_.setColour (juce::Label::textColourId, theme.accentPrimary);
    statusCountLabel_.setText (
        juce::String (currentPartActiveVoiceCount()) + "/" + juce::String (
            processorRef_.getEngine()
                .getPart (processorRef_.getEngine().getCurrentPart()).voiceCount_.load()),
                               juce::dontSendNotification);
    addAndMakeVisible (statusCountLabel_);
    // Realtime audio-load readout (see ParvatiAudioProcessor::
    // getAudioLoadCurrent). Shows "CPU 42%" in green, amber above 70%, red
    // above 90% (the CURRENT block only — peak/overrun diagnostics are gone
    // from the readout per request). Updated at 30 Hz in timerCallback().
    // Pure read of the processor's atomic (message thread). Fixed tooltip —
    // no interaction on the label.
    statusLoadLabel_.setJustificationType (juce::Justification::centred);
    statusLoadLabel_.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    statusLoadLabel_.setColour (juce::Label::textColourId, theme.textSecondary);
    statusLoadLabel_.setText ("CPU 0%", juce::dontSendNotification);
    statusLoadLabel_.setTooltip (TRANS ("Audio-thread realtime load (current block; "
                                       "near 100% = dropouts/crackle)."));
    addAndMakeVisible (statusLoadLabel_);
    statusTooltipLabel_.setJustificationType (juce::Justification::centredLeft);
    statusTooltipLabel_.setFont (juce::FontOptions (12.0f));
    statusTooltipLabel_.setColour (juce::Label::textColourId, theme.textSecondary);
    addAndMakeVisible (statusTooltipLabel_);

    // ---- Phase 4a: settings side panel (right side, always-on-top) ----
    // The SettingsPanel is owned + deleted by the SidePanel.
    // RIGHT-docked so the panel never covers the left-side Settings button
    // (which the user re-clicks to dismiss it). Was left-docked (true).
    settingsPanelHost_ = std::make_unique<juce::SidePanel> (TRANS ("Settings"), 300, false);
    settingsPanel_ = new SettingsPanel (processorRef_, themeManager_,
        [this] (double z) { setZoom (z); repaint(); },
        [this] (bool b)         { ParamControl::setTooltipsEnabled (b);
                                 if (patchPage_ != nullptr)
                                     patchPage_->setTableTooltipsEnabled (b); },
        [this] (bool b)     { processorRef_.setParameterSmoothing (b); },
        [] (int)            {},   // processor.setOversamplingFactor already applied in the panel
        [this] (const juce::String& code) {
            // Language changed: persist it, install the LocalisedStrings, then
            // re-translate every chrome string live.
            processorRef_.setUiLanguage (code);
            installLanguage (code);
            applyChromeTranslations();
        },
        // Visual Refresh (live mod-feedback cadence): the panel already
        // persists via proc.setUiRefreshHz; route the editor hook straight to
        // the shared applier so the hub + both mod bars re-time IMMEDIATELY
        // (timerCallback's shadow check would otherwise pick it up within one
        // ~30 Hz tick — that poll covers restores / out-of-band writes).
        [this] (int hz) { applyLiveFeedbackRefreshRate (hz); });
    // 2026-08-21: the drawer SCROLLS. The panel's full row budget (theme,
    // zoom, toggles, arp clock, refresh, filter, language) exceeds many host
    // panes (a compact AUv3 drawer / short desktop window) — the old R3
    // behaviour HID rows that did not fit, making bottom settings unreachable.
    // A vertical-only Viewport between the SidePanel and the panel keeps every
    // row reachable; the scrollbar appears only when the pane is shorter than
    // the full budget.
    settingsScroll_ = std::make_unique<juce::Viewport>();
    // Vertical-only, auto-shown — PLUS allowVerticalScrollingWithoutScrollbar:
    // wheel/trackpad scrolling must work even in the window where the
    // auto-bar has not (yet) been made visible. (2026-08-22 "language row
    // unreachable" fix: JUCE's updateVisibleArea has an early-return path —
    // content-position moves return BEFORE the bar's setVisible — so the
    // narrowed content area coexisted with an invisible bar, and with
    // allow-without-bar false the Viewport refused wheel scrolling entirely.
    // The tracker now also sets the bar's visibility explicitly; this flag is
    // the guarantee that scrolling never depends on it.)
    settingsScroll_->setScrollBarsShown (false, true, /*allowVerticalScrollingWithoutScrollbar=*/ true);
    settingsScroll_->setViewedComponent (settingsPanel_, true);   // viewport owns + deletes
    settingsPanelHost_->setContent (settingsScroll_.get(), false);   // SidePanel does NOT delete
    // SIZE THE CONTENT (the bug fix): a juce::Viewport NEVER sizes its viewed
    // component, and the SidePanel now parents the VIEWPORT (not the panel),
    // so without this listener the panel keeps its 0×0 birth size — the
    // drawer rendered blank/mis-sized. Track the viewport: width = the live
    // view width (minus the scrollbar when it auto-shows), height = the FULL
    // row budget (never shorter than the view, so the scroll reads 1:1).
    settingsScrollTracker_ = std::make_unique<SettingsScrollTracker> (
        *settingsScroll_, *settingsPanel_);
    settingsScroll_->addComponentListener (settingsScrollTracker_.get());
    // F-ios-touch-3 focus hygiene: a CLOSED drawer keeps the panel 0×0 (see
    // the tracker note above), and its theme/zoom/toggle controls would sit
    // in the keyboard focus traversal as invisible zero-extent targets (tab
    // could hand focus to an unseen toggle; Space would fire it). A keyboard
    // focus CONTAINER on the panel stops the traversal from descending into
    // it while the drawer is closed; the show/hide hook below re-opens the
    // boundary when the drawer is on screen (by then the tracker has sized
    // every row, so all focusables carry real bounds).
    settingsPanel_->setFocusContainerType (
        juce::Component::FocusContainerType::keyboardFocusContainer);
    // Keep the Settings button's toggle state in sync when the panel is
    // dismissed by other means (the dismiss glyph / clicking outside / ESC) —
    // onPanelShowHide fires after the slide animation on any show/hide.
    settingsPanelHost_->onPanelShowHide = [this] (bool isShown) {
        settingsButton_.setToggleState (isShown, juce::dontSendNotification);
        // The scroll tracker sizes the panel ONLY while the drawer shows (a
        // closed drawer keeps it 0×0 — see the class note): re-apply on both
        // edges so opening sizes it immediately (the slide may not fire a
        // resize) and closing collapses it.
        if (settingsScrollTracker_ != nullptr)
            settingsScrollTracker_->applyFromEditor();
        // Mirror the focus boundary (see the setFocusContainerType note at
        // construction): open drawer = pass-through (its controls join the
        // traversal, all sized), closed drawer = container (the collapsed
        // 0×0 rows leave the traversal again).
        if (settingsPanel_ != nullptr)
            settingsPanel_->setFocusContainerType (
                isShown ? juce::Component::FocusContainerType::none
                        : juce::Component::FocusContainerType::keyboardFocusContainer);
    };
    addAndMakeVisible (*settingsPanelHost_);

    // ---- Chrome separator rules (LAST children => topmost): 1px hair-lines
    //      5px below the header (end-to-end) and 5px above the status strip
    //      (~95% width, centred). Positioned in resized() from the bands. See
    //      ChromeRule for why these are components. ----
    headerRule_ = std::make_unique<parvati::ChromeRule> (true);   // shadow falls below
    statusRule_ = std::make_unique<parvati::ChromeRule> (false);  // shadow falls above
    // Keyboard-overlay top rule: the keyboard strip is chrome raised over the
    // content it covers, so the 1px rule sits at its TOP edge with the depth
    // falloff above it (the footer idiom). Shown/hidden with the strip.
    keyboardRule_ = std::make_unique<parvati::ChromeRule> (false);
    addAndMakeVisible (*headerRule_);
    addAndMakeVisible (*statusRule_);

    // Refresh the Patch page (~30 Hz) so it tracks the edited part.
    startTimerHz (30);

    // Re-apply the UI font family (system default sans-serif) to every cached
    // Label now that all widgets exist (juce::Label caches its font, so the
    // family from getLabelFont needs to be pushed here too). Combos, buttons,
    // tabs, popups and group titles follow via the L&F font getters.
    refreshFontsIn (this, lnf_);

    // Dense integrated layout: header(40) + page tabs(28) + content + status(22).
    // The CentralModBar spans the full content width (== editor width — no
    // horizontal chrome), so the MINIMUM width is its no-clipping preferredWidth()
    // plus a small safety margin: NO pill ever compresses. The DEFAULT size is
    // raised to at least that minimum so the bar is uncompressed at startup too.
    // Min height 600 keeps the 3 rows (top | bar | bottom) usable. (Headless tests
    // call setSize() below the min, which bypasses setResizeLimits.)
    // The CentralModBar scrolls internally (Viewport), so it never widens the
    // editor — the width floor can sit BELOW the old 1280pt so the editor FILLS
    // the screen at 100% zoom on tablets narrower than 1280pt (iPad Pro 11"
    // landscape is 1194pt): no manual zoom-out is needed. 1024 covers every
    // current iPad; min height 500 keeps the 3 rows usable. (Headless tests call
    // setSize() below the min, which bypasses setResizeLimits.)
    setSize (1280, 634);
    setResizable (true, true);
    setResizeLimits (1024, 500, 1800, 1100);

    // Apply persisted zoom (global scale; only if non-default — a rescale at
    // startup is not needed). iOS fullscreen always starts at 100%,
    // ignoring any persisted value.
#if JUCE_IOS
    setZoom (1.0);
#else
    if (processorRef_.getUiZoom() != 1.0)
        setZoom (processorRef_.getUiZoom());
#endif

#if JUCE_IOS
    // T14 (iPadOS audit): keep the display awake while an editor exists — a
    // patch tweak mid-performance must not lock the screen (audio keeps going
    // via UIBackgroundModes, but the UI would vanish mid-drag). Restored in the
    // destructor next to the zoom reset, mirroring that pattern. iOS-only seam
    // (same gate as the zoom default above): desktop screensaver policy is not
    // ours to change.
    // F-ios-lc-3: REFERENCE-COUNTED — an AUv3 extension process hosts MULTIPLE
    // Parvati instances (AUM), each with its own editor. Only the 0 -> 1
    // transition disables the screensaver so closing editor A cannot re-enable
    // sleep while editor B is still open (T14's protection voided per-close).
    if (sLiveEditorCount == 1)
        juce::Desktop::getInstance().setScreenSaverEnabled (false);
#endif

    // Guarantee the full theme-derived colour re-apply runs on first build in
    // EVERY context (standalone, headless screen tool, editor tests).
    // changeListenerCallback is only the theme-CHANGE path: it is NOT invoked at
    // initial construction unless selectByName actually moves the selection
    // (default Carbon => no broadcast), so without this explicit call the
    // category knob arcs / ENV-LFO graph traces / mod-source tints could stay on
    // the L&F default gold until the first manual theme switch. The helper runs
    // the SAME sequence as changeListenerCallback after the whole tree is built
    // + parented, so every control resolves its category colour from the active
    // theme on the very first paint.
    applyAllColoursFromTheme();
}

ParvatiEditor::~ParvatiEditor()
{
    // F-ui-3 (bug hunt 2026-08-18): close any OPEN popup menu deterministically.
    // Open menus (combo dropdowns, the zoom overflow popup, FX type pickers)
    // hold a raw L&F pointer into this editor; JUCE's 20 Hz target-death timer
    // would dismiss them only up to ~50 ms AFTER the target died, leaving a
    // window where an OS-driven paint reads freed memory.
    juce::PopupMenu::dismissAllActiveMenus();

    stopTimer();
    // Release EVERY note the on-screen/computer keyboard still holds BEFORE
    // the callback is nulled: a key physically held at teardown gets no
    // focusLost/mouseUp, so without these note-offs the notes sustain forever
    // in the host (the processor outlives the editor and keeps rendering).
    if (keyboardView_ != nullptr)
        keyboardView_->releaseAllNotes();
    // Clear callbacks that capture `this` before the owning components are
    // destroyed during the reverse-order member teardown (defensive: the
    // components stop their own timers in their destructors, but nulling the
    // callbacks avoids any lingering reference).
    if (keyboardView_ != nullptr)
        keyboardView_->setNoteCallback (nullptr);
    // Reset the PROCESS-GLOBAL tap-assign mode (its statics survive this
    // editor): a [MAP] left ON at teardown would keep every OTHER live
    // instance's knobs in assign affordance (the registry spans instances) —
    // or, after a close+reopen, leave the new editor in assign mode while its
    // [MAP] button shows OFF. Mirrors the zoom-reset teardown below.
    // F-ios-lc-3: ONLY when this is the LAST live editor — the static is
    // process-global and shared with any other open instance's UI; clearing
    // it while editor B is live would silently exit B's assign mode.
    if (sLiveEditorCount == 1)
        ParamControl::setTapAssignActive (false);
    // Detach from the theme broadcaster and release the L&F BEFORE the member
    // objects (themeManager_, lnf_) and the base Component are destroyed, so the
    // ChangeBroadcaster never calls back into a half-dead editor and no child
    // component references a destroyed L&F during teardown.
    themeManager_.removeChangeListener (this);
    setLookAndFeel (nullptr);

    // SF-2: reset the process-wide global scale factor so a non-default zoom
    // does not leak to other JUCE windows / plugin instances after this editor
    // closes. (Global scale is the only zoom path today; per-editor transform
    // zoom is a documented future enhancement — see the setZoom() comment.)
    if (zoom_ != 1.0)
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);

#if JUCE_IOS
    // T14: re-allow screen sleep (pairs with the constructor's disable — see
    // the matching seam there).
    // F-ios-lc-3: reference-counted counterpart of the ctor's 0->1 gate —
    // only the LAST live editor re-enables sleep (an AUv3 process with a
    // still-open sibling editor keeps the display awake).
    if (sLiveEditorCount == 1)
        juce::Desktop::getInstance().setScreenSaverEnabled (true);
#endif

    // F-ios-lc-3: the teardown bookkeeping itself — everything above that
    // consulted the live count (screensaver, tap-assign) must run while this
    // editor still counts as live, so the decrement is the LAST statement.
    --sLiveEditorCount;
}

int ParvatiEditor::liveEditorCountForTest() noexcept { return sLiveEditorCount; }

void ParvatiEditor::dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& details)
{
    // Only a modulation-source drag (payload "parvatiModSrc:<enum>") triggers
    // the drop-zone affordance; any other (defensive — none exist today) leaves
    // the controls untouched. dragOperationStarted fires at the end of
    // startDragging() for the drag that THIS container owns.
    if (details.description.toString().startsWith ("parvatiModSrc"))
        ParamControl::setModDragActive (true);
}

void ParvatiEditor::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&)
{
    // Unconditional restore: dragOperationEnded fires from the DragImageComponent
    // destructor on BOTH a successful drop and a cancel, so the affordance state
    // always clears — no knob is left dimmed or ring-flagged.
    ParamControl::setModDragActive (false);
}

void ParvatiEditor::pollPatchPageMirror()
{
    // Visible-Patch-page mirror under out-of-band engine writes (host
    // automation of part_polyphony / part_raga, MIDI NRPN, host undo): those
    // paths mutate the engine with no editor hook, so a VISIBLE Patch page
    // could keep showing stale rows until the next reveal/load. The engine's
    // display version (bumped by its message-thread mutators) makes the check
    // O(1) and change-only; refresh() is guarded + idempotent (no onChange
    // fires). Also called from timerCallback while the Patch page is shown —
    // this body is the single shared implementation.
    if (currentTopPage_ != 2 || patchPage_ == nullptr)
        return;
    const uint32_t v = processorRef_.getEngine().getDisplayVersion();
    if (v == lastPatchPageDisplayVersion_)
        return;   // nothing mirrored has changed since the last read
    patchPage_->refresh();
    lastPatchPageDisplayVersion_ = v;   // capture AFTER the refresh (dedupes the load/reveal paths below)
}

// Relabel the top-bar Part selector with the current part names/aliases
// (Parvati extension): "3 · Snare" when named, "Part 3" otherwise. Cheap
// (6 string compares); called on name edits + from the poll timer so loads
// (multi/template/DAW state) also refresh the labels.
void ParvatiEditor::refreshPartComboNames()
{
    bool anyChanged = false;
    for (int i = 1; i <= SynthEngine::getNumParts(); ++i)
    {
        const auto n = processorRef_.getEngine().getPartName (i - 1);
        const juce::String label = n.isNotEmpty()
            ? juce::String (i) + " \u00b7 " + n     // e.g. "3 - Snare"
            : TRANS ("Part") + " " + juce::String (i);
        auto& cache = partComboLabelCache_[(size_t) (i - 1)];
        if (cache != label)
        {
            cache = label;
            partCombo_.changeItemText (i, label);
            anyChanged = true;
        }
    }
    // changeItemText updates only the MENU entry — the combo's inline label
    // keeps painting the OLD text until the next selection change (JUCE's
    // ComboBox does not refresh its label from item text). Re-applying the
    // current selection re-renders the label, so renaming the SELECTED part
    // (Patch-page name edit -> onPartNamesChanged -> here) shows immediately;
    // dontSendNotification keeps this a pure display fix (no parameter or
    // part-switch side effects). Skipped entirely when nothing changed (the
    // 30 Hz poll path is now a pure 6-string compare).
    if (anyChanged)
    {
        partCombo_.setSelectedId (processorRef_.getEngine().getCurrentPart() + 1,
                                  juce::dontSendNotification);
        partCombo_.repaint();
    }
}

int ParvatiEditor::currentPartActiveVoiceCount() const
{
    auto& engine = processorRef_.getEngine();
    const int curPart = engine.getCurrentPart();
    int active = 0;
    for (int i = 0; i < engine.getNumVoices(); ++i)
        if (auto* av = engine.getAmbikaVoice (i);
            av != nullptr && av->getPartIndex() == curPart && av->isDisplayedActive())
            ++active;
    return active;
}

ParvatiEditor::ThermalStatusAction ParvatiEditor::thermalStatusForTransition (int oldHint,
                                                                              int newHint) noexcept
{
    // F-ios-perf-2 label surfacing (2026-08-19 follow-up) — the PURE policy
    // half; the 30 Hz timer's iOS-only block applies it. Hints are
    // ThermalAction ints (0=None, 1=Hint, 2=StrongHint); clamp defensively so
    // a corrupt atomic can never invent a stronger action than StrongHint.
    const int o = juce::jlimit (0, 2, oldHint);
    const int n = juce::jlimit (0, 2, newHint);
    if (n > o)   // escalation: 0->1, 0->2, 1->2 (all three cells)
        return n >= 2 ? ThermalStatusAction::ShowStrong : ThermalStatusAction::ShowHint;
    if (n < o)   // de-escalation: 1->0, 2->0, 2->1 — hand back to the expiry
        return ThermalStatusAction::Clear;
    return ThermalStatusAction::NoOp;   // same->same (incl. the idle 0->0 tick)
}

#if JUCE_IOS
// The last hint seen by the 30 Hz thermal check (F-ios-perf-2 follow-up).
// File-scope STATIC, matching the seam it drives: the transient status is
// process-global (ParamControl::postTransientStatus) and the hint itself is
// processor-global — so per-EDITOR copies would only multiply identical
// transitions (and double-post the status in multi-editor AUv3 processes).
// Lives INSIDE the gate so desktop builds compile none of it (-Werror: an
// iOS-only class member would be an unused private field on desktop).
static int sLastThermalHint = 0;
#endif

void ParvatiEditor::timerCallback()
{
    tickSettingsScrollbar();
    const bool popupOpen = tickTooltipPopupGuard();

    // Part-name labels follow engine state (edits made on the Patch page fire
    // onPartNamesChanged directly; this also catches file loads + DAW restores).
    refreshPartComboNames();

    // Visible-Patch-page mirror: out-of-band engine writes (host automation /
    // NRPN / undo) have no editor hook, so while the Patch page is shown the
    // poll re-reads it whenever the engine's display version moved (change-
    // only; see pollPatchPageMirror). Cheap no-op on every other page.
    pollPatchPageMirror();

    // ---- Live mod-feedback refresh-rate application (~30 Hz, change-only) ----
    // The persisted ui_refresh_hz pref is the single source of truth; this
    // shadow check re-times the hub + BOTH mod bars within one tick of a
    // Settings-panel change (the panel persists via proc.setUiRefreshHz and
    // ALSO routes its onRefreshChanged callback straight to the same helper,
    // so the effect is immediate; this poll covers host-state restores and any
    // other out-of-band pref write). Static pref => one int compare, nothing
    // else. NOTE on PART tracking: every part switch (combo, Cmd+1..6, context
    // menu, file loads, state restores) funnels through the processor's
    // part_select -> onPartSelect seam, whose engine telemetry reset hook is
    // the AUTHORITATIVE re-sync — the ctor's setUiTelemetryPart is the startup
    // extra startup copy; nothing further is needed here.
    applyLiveFeedbackRefreshRate (processorRef_.getUiRefreshHz());

    tickTelemetryReasserts();
    tickMouseActivity();
    tickThermalHint();
    tickHostTempoHint();

    // Single drain of the tap-to-assign transient status this tick (it has a
    // frame budget — calling it twice would double-drain). The result feeds
    // BOTH the tooltip priority below and the adaptive-rate activity signal.
    const juce::String transientStatus = drainTransientStatus();

    const int activeVoices = tickStatusStrip (transientStatus, popupOpen);

    // NOTE: the Patch page shows ALL 6 parts (it is not part-relative), so there
    // is nothing to re-sync on a part switch here. External state changes (a
    // .MUL load) are covered by the forced refresh in applyPatchFile.

    tickKeyboardLatching();
    tickAdaptiveRate (activeVoices, popupOpen, transientStatus);
}

// ---- Settings drawer scrollbar determinism (~30 Hz, ~one compare) ----
// JUCE's Viewport::updateVisibleArea has an early-return path (content
// repositioning) that can leave the auto scrollbar HIDDEN while the
// content area stays narrowed for it — and it re-hides the bar on some
// view-position changes even after our tracker set it visible. Wheel
// scrolling no longer depends on the bar (allowVerticalScrollingWithout
// scrollbar), but the affordance must not flicker: re-assert the intended
// state every tick while the drawer shows (idempotent; dirt cheap).
void ParvatiEditor::tickSettingsScrollbar()
{
    if (settingsPanelHost_ != nullptr && settingsPanelHost_->isPanelShowing()
        && settingsScroll_ != nullptr && settingsPanel_ != nullptr
        && settingsScroll_->isShowing())
        settingsScroll_->getVerticalScrollBar().setVisible (
            settingsPanel_->getHeight() > settingsScroll_->getHeight());
}

// ---- Tooltip bleed-through fix (~30 Hz) ----
// ROOT CAUSE: the editor's TooltipWindow is parented to the editor (so it
// inherits the ParvatiLookAndFeel and scales with the editor / DAW), but a
// popup menu (a ComboBox drop-down OR a right-click context menu) lives in
// its OWN top-level window with a different ComponentPeer. The base
// juce::TooltipWindow::timerCallback only processes components that share
// ITS peer, so while a popup is open it SKIPS its show/hide logic and
// FREEZES the underlying control's tip on screen (the reported bleed).
// A popup always enters the modal state (juce::PopupMenu::showMenuAsync ->
// MenuWindow::enterModalState), so while any modal component is active we
// hide the editor tooltip. (tooltipWindow_'s own timer then does nothing —
// it skips its block while a different-peer popup is open — so the hide
// sticks until the popup closes.) The context-menu items show their OWN
// tooltips via the desktop TooltipWindow created in
// ParamControl::showContextMenu; ComboBox drop-down items simply show
// nothing. No-op when no popup is open.
bool ParvatiEditor::tickTooltipPopupGuard()
{
    const bool popupOpen = juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    if (popupOpen && tooltipWindow_ != nullptr)
        tooltipWindow_->hideTip();
    return popupOpen;
}

// CONSUMER poll re-asserts (every tick, idempotent): the bars' strip polls
// start unconditionally now (see updateTelemetryTimer), but re-asserting
// costs nothing and covers a provider/rate arriving after construction;
// the display polls re-evaluate their own visibility gates (their per-tick
// work is change-gated, so a started-but-hidden display is cheap).
void ParvatiEditor::tickTelemetryReasserts()
{
    if (synthWorkspace_ != nullptr)
        if (auto* b = synthWorkspace_->modBar())
            b->reassertTelemetryTimer();
    if (fxWorkspace_ != nullptr)
        if (auto* b = fxWorkspace_->modBar())
            b->reassertTelemetryTimer();
    for (auto* d : liveEnvDisplays_)
        d->reassertPollTimer();
    for (auto* d : liveOscDisplays_)
        d->reassertPollTimer();
    if (liveFilterDisplay_ != nullptr)
        liveFilterDisplay_->reassertPollTimer();
}

// Mouse-activity tracking for the adaptive poll rate (see
// tickAdaptiveRate): getMouseXYRelative() is the peer-cached position
// (cheap), so a delta vs the last tick means the mouse moved. A mouse
// parked OUTSIDE the window also reports a constant position — no false
// activity.
void ParvatiEditor::tickMouseActivity()
{
    if (const auto mousePos = getMouseXYRelative(); mousePos != lastMousePos_)
    {
        lastMousePos_ = mousePos;
        lastMouseActivity_ = juce::Time::getCurrentTime();
    }
}

// Empty on non-iOS builds: the processor hint sampler and the state
// behind sLastThermalHint compile only for iOS.
void ParvatiEditor::tickThermalHint()
{
#if JUCE_IOS
    // ---- Thermal-hint surfacing (F-ios-perf-2, 2026-08-19 follow-up) ----
    // One RELAXED atomic read per 30 Hz tick (the processor's iOS-only ~1 Hz
    // sampler writes it); ONLY a level TRANSITION arms the transient status
    // (pure matrix: ParvatiEditor::thermalStatusForTransition), never every
    // tick — an idle thermal state costs the status strip nothing. Placed
    // BEFORE the single drain below so an escalation is visible THIS tick.
    {
        const int thermalNow = processorRef_.getThermalHint();
        switch (thermalStatusForTransition (sLastThermalHint, thermalNow))
        {
            case ThermalStatusAction::ShowHint:   // Serious: suggest
                ParamControl::postTransientStatus (
                    TRANS ("Thermal: reduce Filter Quality"), 90);   // ~3 s @ 30 Hz
                break;
            case ThermalStatusAction::ShowStrong:   // Critical: insist
                ParamControl::postTransientStatus (
                    TRANS ("Thermal: lower Filter Quality now"), 150);   // ~5 s
                break;
            case ThermalStatusAction::Clear:
                // De-escalation: the seam is frame-budget based with no
                // explicit clear API — expiry handles it (advisory-only
                // policy: nothing else to undo, the load readout keeps the
                // strip alive).
                break;
            case ThermalStatusAction::NoOp:
            default:
                break;
        }
        sLastThermalHint = thermalNow;
    }
#endif
}

// ---- No-host-tempo hint (2026-08-19 AUv3 wave) ----
// The processor publishes whether the last block resolved the arp clock
// from a HOST tempo or the MANUAL fallback (Settings > Arp Clock — hosts
// that expose no musical context, e.g. GarageBand-class AUv3 hosts and
// the Standalone app, would otherwise silently run the arp at a fixed
// tempo). Fires ONCE per Host->Manual transition; process-static for the
// same reason as sLastThermalHint (an AUv3 process hosts several
// editors/instances — per-editor state would multiply identical posts).
// NOT iOS-gated: the desktop Standalone benefits identically. The
// optimistic `true` default means a DAW session (tempo present in every
// block) never sees it — the flip only fires once a block has ACTUALLY
// rendered without a host tempo.
void ParvatiEditor::tickHostTempoHint()
{
    static bool sLastHostTempoPresent = true;
    const bool hostNow = processorRef_.isHostTempoPresent();
    if (hostNow != sLastHostTempoPresent)
    {
        if (! hostNow)
            ParamControl::postTransientStatus (
                TRANS ("No host tempo - arp clock: manual BPM (Settings)"), 150);   // ~5 s @ 30 Hz
        sLastHostTempoPresent = hostNow;
    }
}

// Single per-tick drain of the tap-to-assign transient status. The
// dispatcher calls it exactly once; a second call would double-drain
// the frame budget.
juce::String ParvatiEditor::drainTransientStatus()
{
    return ParamControl::tickTransientStatus();
}

// Bottom status strip: current-part voice count, audio-load readout,
// tooltip bar (hover help, transient priority, keyboard idle readout)
// and the undo/redo button mirror. Returns the active-voice count for
// tickAdaptiveRate.
int ParvatiEditor::tickStatusStrip (const juce::String& transientStatus, bool popupOpen)
{
    // Voice count for the status strip AND the adaptive-rate activity signal.
    int activeVoices = 0;
    // Mirror the UndoManager's undo/redo availability onto the top-bar buttons
    // (~30 Hz, same cadence as the Patch-page refresh below). Cheap O(1)
    // canUndo/canRedo checks; setEnabled() is a no-op when unchanged.
    undoButton_.setEnabled (processorRef_.getUndoManager().canUndo());
    redoButton_.setEnabled (processorRef_.getUndoManager().canRedo());

    // ---- Bottom status strip: active-voice count + hover tooltip (~30 Hz) ----
    {
        auto& engine = processorRef_.getEngine();
        // PART-RELATIVE count: active voices of the CURRENT part only. The
        // numerator used to sweep the whole 96-voice pool while the denominator
        // was the current part's allocation — mixed fractions like "23/16".
        // (The keyboard latching further below intentionally stays GLOBAL
        // across all parts.)
        const int active = currentPartActiveVoiceCount();
        activeVoices = active;
        const int denom = engine
            .getPart (engine.getCurrentPart()).voiceCount_.load();
        const juce::String countText = juce::String (active) + "/" + juce::String (denom);
        if (statusCountLabel_.getText() != countText)
            statusCountLabel_.setText (countText, juce::dontSendNotification);

        // ---- Realtime audio-load readout ----
        // Shows the current block's CPU% (render-time / real-time budget).
        // Colour flips to amber above 70%, red above 90% — so a glance at the
        // strip tells you whether audible crackle coincides with the audio
        // thread being starved (e.g. by GUI render load on a shared core).
        // (The peak/overrun diagnostics were dropped from the READOUT per
        // request; the processor's probe APIs remain available for tooling.)
        {
            const double cur = processorRef_.getAudioLoadCurrent();
            const int curPct = juce::jlimit (0, 999, juce::roundToInt (cur * 100.0));
            // Anti-flicker hold gate: the per-block probe jitters 0<->1% from
            // render-timing noise, which used to re-set this label ~20x/sec at
            // idle (each text change repaints the whole status strip, and with
            // it the editor). The readout now updates only when the value MOVES
            // MEANINGFULLY (>=2 percentage points) or 500 ms elapsed since the
            // last refresh (a genuinely drifting load still tracks). The
            // existing text-comparison guard stays as the final gate below.
            const bool movedEnough = std::abs (curPct - lastLoadPct_) >= 2;
            const bool holdElapsed = juce::Time::getCurrentTime() - lastLoadTextUpdate_
                                     > juce::RelativeTime::milliseconds (500);
            if (movedEnough || holdElapsed)
            {
                const juce::String loadText = "CPU " + juce::String (curPct) + "%";
                if (statusLoadLabel_.getText() != loadText)
                    statusLoadLabel_.setText (loadText, juce::dontSendNotification);
                lastLoadPct_ = curPct;
                lastLoadTextUpdate_ = juce::Time::getCurrentTime();
            }
            // Colour by headroom (the CURRENT percentage drives the colour).
            auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
            const ParvatiTheme* th = lnf ? lnf->getTheme() : nullptr;
            const juce::Colour ok     = th ? th->textSecondary : juce::Colour (0xff9a9aa8);
            const juce::Colour warn   = th ? th->accentPrimary  : parvati::parvatiFallbackAccent;
            const juce::Colour danger = juce::Colour (0xffe0584a);
            const juce::Colour c = cur >= 0.90 ? danger : cur >= 0.70 ? warn : ok;
            // setColour marks the label dirty even when the colour is unchanged
            // — gate on an actual change so an idle tick never repaints.
            if (c != lastLoadColour_)
            {
                statusLoadLabel_.setColour (juce::Label::textColourId, c);
                lastLoadColour_ = c;
            }
            // The tooltip is a fixed string (set once in the ctor) — the live
            // value is the label text itself, so there is no per-tick tooltip
            // rebuild to guard.
        }

        // Tooltip bar: the help text of the control under the mouse (walks up
        // to the first ancestor carrying a tooltip). Empty when tooltips are
        // disabled in Settings or the mouse is over dead space. Suppressed while
        // a modal popup is open too: getComponentAt() would otherwise return the
        // editor control physically under the popup and leak its help text
        // (same bleed-through class of bug, in the status strip).
        juce::String tip;
        if (ParamControl::tooltipsEnabled() && ! popupOpen)
        {
            const auto rel = getMouseXYRelative();
            if (getLocalBounds().contains (rel))
                for (auto* c = getComponentAt (rel); c != nullptr; c = c->getParentComponent())
                {
                    const juce::String t = (dynamic_cast<juce::TooltipClient*> (c) != nullptr)
                        ? dynamic_cast<juce::TooltipClient*> (c)->getTooltip() : juce::String();
                    if (t.isNotEmpty()) { tip = t; break; }
                }
        }
        // Tap-to-assign transient status (e.g. "Mod Matrix full") takes priority
        // over the hover tooltip for a short time after requestAssign returns
        // false (full matrix). Drains back to the normal hover tip afterwards.
        // (Drained once at the top of this callback — transientStatus.)
        if (transientStatus.isNotEmpty())
            tip = transientStatus;

        // Keyboard idle readout: when nothing else is showing and the pointer
        // hovers the virtual keyboard (or its [KBD] toggle), display the
        // CURRENT Ableton-style musical-typing settings (octave window +
        // velocity) so they are discoverable without pressing Z/X/C/V first.
        if (tip.isEmpty() && keyboardView_ != nullptr)
        {
            const auto rel = getMouseXYRelative();
            for (auto* c = getComponentAt (rel); c != nullptr; c = c->getParentComponent())
            {
                if (c == keyboardView_.get() || c == wheels_.get() || c == &kbdToggleButton_)
                {
                    const int base = keyboardView_->qwertyOctaveBase();
                    tip = TRANS ("Keyboard: octave ") + midiNoteName (base) + "\u2013"
                            + midiNoteName (base + 24) + "  \u00b7  "
                            + TRANS ("velocity") + " "
                            + juce::String (keyboardView_->qwertyVelocity127());
                    break;
                }
            }
        }
        if (statusTooltipLabel_.getText() != tip)
            statusTooltipLabel_.setText (tip, juce::dontSendNotification);
    }
    return activeVoices;
}

// ---- Keyboard latching: mirror sounding notes across all voices ----
// The count stays GLOBAL across all parts. tickAdaptiveRate runs after
// this stage every tick, so this method performs no early exit.
void ParvatiEditor::tickKeyboardLatching()
{
    if (keyboardView_ != nullptr)
    {
    const int curPart = processorRef_.getEngine().getCurrentPart();
    if (curPart != lastLatchPart_)
    {
        // Edited part changed: clear all latched notes to avoid stuck lamps.
        for (int n = 0; n < 128; ++n)
            keyboardView_->latchNoteOff (n);
        latchedNotes_.clear();
        lastLatchPart_ = curPart;
    }
    else
    {

    // Collect the set of active notes across ALL voices.
    juce::Array<int> activeNotes;
    auto& engine = processorRef_.getEngine();
    for (int i = 0; i < engine.getNumVoices(); ++i)
    {
        // SF-1: read the lock-free atomic snapshot instead of the
        // non-atomic SynthesiserVoice::currentlyPlayingNote.
        auto* voice = engine.getAmbikaVoice (i);
        if (voice != nullptr && voice->isDisplayedActive())
        {
            const int note = voice->getDisplayedNote();
            if (note >= 0 && ! activeNotes.contains (note))
                activeNotes.add (note);
        }
    }

    // New notes: latch on.
    for (int note : activeNotes)
    {
        if (! latchedNotes_.contains (note))
        {
            keyboardView_->latchNoteOn (note, 1.0f);
            latchedNotes_.add (note);
        }
    }

    // Released notes: latch off.
    for (int i = latchedNotes_.size() - 1; i >= 0; --i)
    {
        if (! activeNotes.contains (latchedNotes_[i]))
        {
            keyboardView_->latchNoteOff (latchedNotes_[i]);
            latchedNotes_.remove (i);
        }
    }
    }   // else: same-part latch mirror
    }   // keyboardView_ != nullptr
}

// ---- Adaptive poll rate: 30 Hz while anything is happening, 4 Hz idle ----
// The timer drives the status strip, the tooltip hover walk, the undo/redo
// mirror and the keyboard latching. At true idle — no sounding voices, no
// transient status draining, no modal popup, no latched keyboard lamps and
// the mouse parked for >3 s — none of those displays can change, so the
// poll drops to 4 Hz and the idle repaint/CPU churn collapses. Any
// activity flips back to 30 Hz on the next tick (<=250 ms later at worst):
// tooltips only matter while the mouse moves (a moving mouse keeps the
// 30 Hz rate, well inside the hover delay), voice lamps update while voices
// sound, and a transient status drains at full rate while visible. The rate
// is recomputed at the END of the tick so this tick's own work (voice
// counts, latch state, drained status) already feeds the decision.
void ParvatiEditor::tickAdaptiveRate (int activeVoices, bool popupOpen,
                                      const juce::String& transientStatus)
{
    {
        const bool mouseRecentlyMoved = juce::Time::getCurrentTime() - lastMouseActivity_
                                        < juce::RelativeTime::seconds (3.0);
        const bool keyboardBusy = keyboardView_ != nullptr
                               && keyboardView_->isVisible()
                               && ! latchedNotes_.isEmpty();
        const bool busy = activeVoices > 0
                       || transientStatus.isNotEmpty()
                       || popupOpen
                       || keyboardBusy
                       || mouseRecentlyMoved;
        const int desiredHz = busy ? 30 : 4;
        if (desiredHz != timerHz_)
        {
            timerHz_ = desiredHz;
            startTimerHz (desiredHz);
        }
    }
}

void ParvatiEditor::reparentGeneratorTo (bool toFx)
{
    // The generator ParamPages are SHARED (editor-owned, registered into BOTH
    // workspaces). Only the VISIBLE workspace may host the active page: release
    // it from the outgoing workspace first (detach + forget), then reparent it
    // into the destination workspace. This guarantees a single parent — no
    // double-parent / dangling (a JUCE Component can only have one parent, and
    // addAndMakeVisible re-parents cleanly once the outgoing host has released
    // its stale activePage_ reference).
    if (toFx)
    {
        if (synthWorkspace_ != nullptr) synthWorkspace_->releaseActiveEditor();
        if (fxWorkspace_    != nullptr) fxWorkspace_->setActiveGenerator (activeGeneratorModSrc_);
    }
    else
    {
        if (fxWorkspace_    != nullptr) fxWorkspace_->releaseActiveEditor();
        if (synthWorkspace_ != nullptr) synthWorkspace_->setActiveGenerator (activeGeneratorModSrc_);
    }
}

void ParvatiEditor::setFxMode (bool fx)
{
    // Public entry for the screen-shot tool / tests: select SYNTH (false) or FX
    // (true) via the unified page selector.
    showTopPage (fx ? 1 : 0);
}

void ParvatiEditor::setCurrentTopPage (int pageIndex)
{
    // Public 3-way entry for the layout / screenshot tools: 0=Synth, 1=FX,
    // 2=Patch — the same path the header page buttons take.
    showTopPage (juce::jlimit (0, 2, pageIndex));
}

void ParvatiEditor::showTopPage (int idx)
{
    // idx: 0=Synth 1=FX 2=Patch — three PEER top-level pages. Patch is a FULL
    // page (pageSelector_ is hidden while it is active so it is the sole
    // content), not a floating overlay.
    currentTopPage_ = idx;

    // Reparent the shared generator only when landing on Synth/FX and it is
    // now hosted by the OTHER workspace. Going to/from Patch leaves the
    // generator where it was (its workspace is just hidden, not torn down).
    if (idx == 0 && fxModeActive_)        { fxModeActive_ = false; reparentGeneratorTo (false); }
    else if (idx == 1 && ! fxModeActive_) { fxModeActive_ = true;  reparentGeneratorTo (true); }

    // Exclusive page visibility: exactly one of the synth/fx tabbed selector or
    // the Patch full-page child is shown.
    pageSelector_.setVisible (idx == 0 || idx == 1);
    if (idx == 0 || idx == 1)
        pageSelector_.setCurrentTabIndex (idx, false);
    if (patchPage_ != nullptr) patchPage_->setVisible (idx == 2);

    // The full-page child covers the content area; bring it to the front
    // (above the keyboard overlay). PatchPage::resized lays out its rows + hosts /
    // reflows the globalPage_, so no reflow is needed here. The keyboard overlay
    // must stay visible in Patch mode too: after the Patch overlay is lifted, a
    // [KBD]-on keyboard (+ wheels) is re-lifted above it, and a showing Settings
    // side panel is re-lifted LAST so it still covers the keyboard
    // (patchPage_->toFront raised it above everything, including the panel).
    if (idx == 2 && patchPage_ != nullptr)
    {
        patchPage_->toFront (true);
        // The page re-reads the engine every time it is REVEALED. Engine state
        // can change under a hidden page with no editor notification — a host
        // state restore (setStateInformation) rewrites the engine directly and
        // has no editor hook, and engine-direct loads from tools/tests bypass
        // applyPatchFile too. refresh() is idempotent + guarded (no onChange
        // fires), so this is cheap on the common no-change path.
        patchPage_->refresh();
        // Capture the display version the rows now reflect so the poll mirror
        // (timerCallback) does not immediately re-run this refresh.
        lastPatchPageDisplayVersion_ = processorRef_.getEngine().getDisplayVersion();
        if (keyboardView_ != nullptr && keyboardView_->isVisible())
            keyboardView_->toFront (false);
        if (wheels_ != nullptr && wheels_->isVisible())
            wheels_->toFront (false);
        if (settingsPanelHost_ != nullptr && settingsPanelHost_->isPanelShowing())
            settingsPanelHost_->toFront (false);
    }

    // Sync all three header page buttons to the active page.
    synthModeButton_.setToggleState (idx == 0, juce::dontSendNotification);
    fxModeButton_.setToggleState    (idx == 1, juce::dontSendNotification);
    globalButton_.setToggleState    (idx == 2, juce::dontSendNotification);
}

void ParvatiEditor::applyAllColoursFromTheme()
{
    // Force every descendant to re-run lookAndFeelChanged(): ComboBox only
    // re-syncs its internal label's text colour (ComboBox::textColourId) in
    // colourChanged()/lookAndFeelChanged(), which a plain L&F colour change
    // does NOT trigger — so a combo themed under a dark theme would otherwise
    // keep near-white label text after switching to the light Paper theme.
    // This also re-applies the per-widget fonts (combo/button/tab/popup) and
    // (crucially) makes each ParamControl::lookAndFeelChanged() re-push its
    // category arc / mod tint once the editor's ParvatiLookAndFeel is attached.
    sendLookAndFeelChange();
    // (2026-08-22) The settings-drawer scrollbar override is REMOVED: it
    // muted the thumb to backgroundInput — which read as an unthemed gray
    // bar (the reported "give the scrollbar a theme-appropriate color").
    // The ParvatiLookAndFeel already themes ScrollBar correctly for every
    // theme (accent thumb, base track, hover brighten, rounded — see
    // drawScrollbar) and re-setColour's on every theme switch, so the drawer
    // scrollbar now simply inherits it.
    for (auto& page : generatedPages_)
        page->applyThemeColors();
    if (synthWorkspace_ != nullptr)
        synthWorkspace_->applyThemeColors();   // 3-row workspace: top pages + bar + active editor + matrix
    if (modMatrixView_ != nullptr)
        modMatrixView_->applyThemeColors();    // bottom-right ModMatrixView (direct child of the workspace)
    if (fxWorkspace_ != nullptr)
        fxWorkspace_->applyThemeColors();      // FX workspace: slot pages + bar + active editor + FxMatrixView
    if (fxMatrixView_ != nullptr)
        fxMatrixView_->applyThemeColors();     // bottom-right FxMatrixView (direct child of the FX workspace)
    if (patchPage_ != nullptr)
        patchPage_->applyThemeColors();
    // Re-resolve + re-push every control's category arc colour / mod-source tint
    // and the ENV/LFO graph trace from the active theme. Component-level
    // setColour overrides survive a theme switch but keep the OLD theme's value
    // otherwise, so they must be re-resolved (sliders, source combos, graph
    // traces). sendLookAndFeelChange() above also re-applies the arcs, but this
    // explicit pass guarantees them regardless of any L&F-resolution timing.
    ParamControl::reapplyCategoryColours();
    reapplyGraphCategoryColours();
    // [20] Top-bar clickable affordance (re-resolved from the new theme).
    applyHeaderButtonChrome();
    statusCountLabel_.setColour (juce::Label::textColourId,
                                 themeManager_.getCurrentTheme().accentPrimary);
    statusTooltipLabel_.setColour (juce::Label::textColourId,
                                   themeManager_.getCurrentTheme().textSecondary);
    // Phase 4a: refresh visualization components so they pick up the new colours.
    if (keyboardView_ != nullptr)
        keyboardView_->refresh();
    repaint();
}

void ParvatiEditor::applyHeaderButtonChrome()
{
    // [20] Clickable affordance for the top bar (user feedback: "introduce
    // some color to the unselected items to signal that they are clickable").
    // The L&F default off-state fill is the flat backgroundPanel — visually
    // identical to the surrounding chrome, so unselected header buttons read
    // as dead captions. Every header TextButton now gets:
    //   - fill:   accentSecondary at ~16% alpha (a subtle themed colour wash;
    //             alpha kept <= 0.35 so it stays subordinate to the on state),
    //   - text:   textPrimary (the bright value tier).
    // Toggled-on keeps the L&F's buttonOnColourId (solid accent fill + dark
    // text) so SELECTED stays clearly stronger than the wash; hover/press
    // derive automatically in ParvatiLookAndFeel::drawButtonBackground
    // (brighter on hover, darker on press). Component-level setColour wins
    // over the L&F defaults by design; this helper re-resolves from the
    // ACTIVE theme on every switch (called from applyAllColoursFromTheme).
    const auto& t = themeManager_.getCurrentTheme();
    const auto wash = t.accentSecondary.withAlpha ((juce::uint8) 0x2A);   // 42/255 ~= 16%
    for (auto* b : { &synthModeButton_, &fxModeButton_, &globalButton_,
                     &kbdToggleButton_, &modBarToggleButton_, &modAssignButton_,
                     &loadButton_, &saveButton_, &zoomOverflowButton_ })
    {
        b->setColour (juce::TextButton::buttonColourId, wash);
        b->setColour (juce::TextButton::textColourOffId, t.textPrimary);
    }
    // The patch indicator: PresetBrowser's name button (its only child
    // TextButton) gets the same treatment + the brighter text tier (user
    // feedback: the patch indicator must match the other header contrast).
    if (presetBrowser_ != nullptr)
        if (auto* pb = dynamic_cast<juce::TextButton*> (presetBrowser_->getChildComponent (0)))
        {
            pb->setColour (juce::TextButton::buttonColourId, wash);
            pb->setColour (juce::TextButton::textColourOffId, t.textPrimary);
        }
}

void ParvatiEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A new theme was selected: install it on the L&F, then re-apply every
    // theme-derived colour across the whole tree (shared helper, also used at
    // ctor end for the first-paint guarantee).
    lnf_.setTheme (themeManager_.getCurrentTheme());
    applyAllColoursFromTheme();
}

void ParvatiEditor::reapplyGraphCategoryColours()
{
    // Re-resolve each graph preview's category token from the current theme and
    // re-push it (a snapshot Colour would otherwise freeze on the old theme).
    const auto& theme = themeManager_.getCurrentTheme();
    for (auto& binding : graphCategoryBindings_)
        binding.first (theme.*binding.second);
}

void ParvatiEditor::setZoom (double zoom)
{
    zoom_ = juce::jlimit (0.75, 2.0, zoom);
    juce::Desktop::getInstance().setGlobalScaleFactor (static_cast<float> (zoom_));
}

void ParvatiEditor::applyZoom (double zoom)
{
    // Shared by the Cmd/Ctrl +/-/0 shortcuts and the on-screen zoom buttons so
    // both use one clamping + persist + Settings-mirror path.
    setZoom (zoom);                               // clamps to [0.75, 2.0] + applies global scale
    processorRef_.setUiZoom (zoom_);              // persist the clamped value
    if (settingsPanel_ != nullptr)
        settingsPanel_->setZoomValue (zoom_);     // mirror into the slider (no re-fire)
}

void ParvatiEditor::applyLiveFeedbackRefreshRate (int hz)
{
    // Live mod-feedback cadence (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the ONE
    // hub poll + both mod-bar strip timers follow the persisted pref
    // (clamped 5..60 by the processor setter / the hub itself). Idempotent
    // through lastAppliedRefreshHz_ so a static pref costs one compare per
    // 30 Hz status tick and nothing else.
    if (hz == lastAppliedRefreshHz_)
        return;
    lastAppliedRefreshHz_ = hz;
    if (liveHub_ != nullptr)
        liveHub_->setRateHz (hz);
    if (synthWorkspace_ != nullptr)
        if (auto* bar = synthWorkspace_->modBar())
            bar->setTelemetryRateHz (hz);
    if (fxWorkspace_ != nullptr)
        if (auto* bar = fxWorkspace_->modBar())
            bar->setTelemetryRateHz (hz);
}

std::vector<ParamPage*> ParvatiEditor::allGeneratedPages() const
{
    std::vector<ParamPage*> out;
    out.reserve (generatedPages_.size());
    for (const auto& p : generatedPages_)
        out.push_back (p.get());
    return out;
}

bool ParvatiEditor::keyPressed (const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    const bool cmdOrCtrl = key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown();

    // ---- Preset stepping: [ / ] — plain OR Cmd/Ctrl. Plain [ ] were
    // previously unclaimed everywhere in the focus chain (KeyboardView's
    // musical typing uses letters only; ComboBox consumes navigation keys
    // only; a focused TextEditor consumes them itself and this handler never
    // runs), so they are safe to claim at the editor level. The Cmd/Ctrl
    // variants exist for hosts/layouts where a child grabs plain brackets.
    // Guard: never step while a text field has the focus (extra safety —
    // a focused TextEditor consumes the key first; this covers text-entry
    // children that forward keypresses, e.g. the preset menu's find field
    // if one is ever added).
    if (code == '[' || code == ']')
    {
        const bool typing = dynamic_cast<juce::TextEditor*> (
            juce::Component::getCurrentlyFocusedComponent()) != nullptr;
        if (! typing)
            return handleStepPresetShortcut (code == ']' ? +1 : -1);
        return false;
    }

    // ---- Musical typing while tweaking (2026-08-21 user report): the
    // KeyboardView handles QWERTY notes only while IT holds focus, but a
    // clicked knob/combo/button GRABS focus mid-performance and a focused
    // sibling never sees the keys. Unhandled plain keys bubble up the parent
    // chain to HERE, so forward them to the strip instead of mutating
    // wantsKeyboardFocus tree-wide (the previous approach left the focus
    // traversal empty). Guards: the strip must be on screen and the focused
    // component must not be a text entry (a TextEditor consumes keys itself
    // and never reaches this handler; the check is extra safety).
    // KeyboardView::keyPressed re-checks the modifier/computer-keyboard rules
    // itself and returns false for anything it does not own (including
    // Cmd/Ctrl combos), so this forward can never consume a shortcut.
    if (! cmdOrCtrl && keyboardView_ != nullptr && keyboardView_->isShowing())
    {
        const bool typing = dynamic_cast<juce::TextEditor*> (
            juce::Component::getCurrentlyFocusedComponent()) != nullptr;
        if (! typing && keyboardView_->keyPressed (key))
            return true;
    }

    // Everything below carries Cmd/Ctrl; plain keys pass through so typing
    // in combos / text boxes is never swallowed.
    if (! cmdOrCtrl)
        return false;

    // Accept both '=' (un-shifted) and '+' for zoom-in across keyboard layouts.
    if (code == '+' || code == '=')
    {
        applyZoom (zoom_ + 0.1);
        return true;
    }
    if (code == '-')
    {
        applyZoom (zoom_ - 0.1);
        return true;
    }
    if (code == '0')
    {
        applyZoom (1.0);
        return true;
    }

    // ---- File shortcuts: Cmd/Ctrl+O = Load picker; Cmd/Ctrl+S = Save
    // PARVATI format. Choice for S: the .parvati save is the FULL-FIDELITY
    // format (vca_curve / filter_card / arp/seq all round-trip; .PRO drops
    // them) — the lossless default. The Ambika .PRO / .MUL saves stay
    // reachable from the Save button's own menu.
    if (code == 'o' || code == 'O')
        return handleLoadPresetShortcut();
    if (code == 's' || code == 'S')
        return handleSavePresetShortcut();

    // ---- Part select: Cmd/Ctrl+1..6 switches the edited Part through the
    // same partCombo_ seam the part context menu uses (sendNotificationSync
    // fires the ComboBox listener -> part_select APVTS param -> engine). The
    // bare digit keys stay unclaimed (free for future recall slots).
    if (code >= '1' && code <= '6')
        return handlePartSelectShortcut (code - '1');

    // Phase 4c: Undo / Redo. Cmd/Ctrl+Z = undo; Cmd/Ctrl+Shift+Z or
    // Cmd/Ctrl+Y = redo. These carry the Cmd/Ctrl modifier (already required to
    // reach here), so they never collide with KeyboardView's plain-key musical
    // typing — that view returns false for modifier-key combos, letting these
    // keypresses bubble up to the editor. The keyCode is the bare letter on
    // both shifted and un-shifted presses (JUCE tracks shift in the modifiers),
    // so check 'z'/'Z' both and decide undo-vs-redo from isShiftDown().
    if (code == 'z' || code == 'Z')
    {
        if (key.getModifiers().isShiftDown())
            processorRef_.redoSafe();
        else
            processorRef_.undoSafe();
        return true;
    }
    if (code == 'y' || code == 'Y')
    {
        processorRef_.redoSafe();
        return true;
    }

    return false;
}

bool ParvatiEditor::keyStateChanged (bool isKeyDown)
{
    // Musical-typing release path (see keyPressed's forward): while a knob /
    // combo holds the focus, key events bubble here — forward the state
    // change so the strip releases a computer-key note the moment its key
    // comes up (KeyboardView::keyStateChanged is a no-op walk when no
    // computer-key notes are held). Returning its result keeps the normal
    // propagation semantics for anything above the editor.
    if (keyboardView_ != nullptr && keyboardView_->isShowing())
        return keyboardView_->keyStateChanged (isKeyDown);
    return false;
}

bool ParvatiEditor::handleStepPresetShortcut (int direction)
{
    // No browser (this cannot happen — it is editor-owned) => not consumed.
    if (presetBrowser_ == nullptr)
        return false;
    const juce::File next = direction >= 0 ? presetBrowser_->selectNext()
                                            : presetBrowser_->selectPrev();
    if (! next.existsAsFile())
        return false;   // empty tree: nothing to step to — let the key pass on
    // The selection fired the editor's onSelect (load) seam; applyPatchFile
    // updates the browser label with the PARSED program name (a factory .PRO
    // leaf's menu label is the patch name inside the file, not the filename).
    return true;
}

bool ParvatiEditor::handleLoadPresetShortcut()
{
    // Desktop-gated: a native file picker needs a window-server session. The
    // headless tests assert the SEAM fired via this true — no picker opens.
    // PARVATI_HEADLESS=1 suppresses the picker even when the test harness has
    // components on the desktop (see nativeDialogsSuppressed above).
    if (! nativeDialogsSuppressed() && juce::Desktop::getInstance().getNumComponents() > 0)
        openLoadDialog();
    return true;
}

bool ParvatiEditor::handleSavePresetShortcut()
{
    // Parvati format (see keyPressed's choice note). Desktop-gated as above
    // (incl. the PARVATI_HEADLESS suppression for the GUI test binaries).
    if (! nativeDialogsSuppressed() && juce::Desktop::getInstance().getNumComponents() > 0)
        openSaveParvatiDialog();
    return true;
}

bool ParvatiEditor::handlePartSelectShortcut (int part0Based)
{
    if (part0Based < 0 || part0Based >= 6)
        return false;
    // The exact seam the part context menu uses (sendNotificationSync fires
    // the ComboBox listener synchronously -> part_select -> engine + APVTS
    // reload of the new part's parameters).
    partCombo_.setSelectedId (part0Based + 1, juce::sendNotificationSync);
    return true;
}

void ParvatiEditor::applyChromeTranslations()
{
    // Re-translate every editor-chrome string through the active LocalisedStrings
    // so a live language switch updates immediately. The top-level page-selector
    // labels (SYNTH/GLOBAL) and the chrome strings below are translated; the
    // CentralModBar pill/cluster labels are short fixed codes (E1/L1/ARP/...),
    // so they need no translation. With no mappings installed (English) TRANS()
    // is the identity, so this is a no-op for the default.
    loadButton_.setButtonText (TRANS ("Load"));
    saveButton_.setButtonText (TRANS ("Save"));
    loadButton_.setTooltip (TRANS ("Load a patch (Cmd/Ctrl+O)"));
    saveButton_.setTooltip (TRANS ("Save the current patch (Cmd/Ctrl+S)"));
    undoButton_.setTooltip (TRANS ("Undo"));
    redoButton_.setTooltip (TRANS ("Redo"));
    settingsButton_.setTooltip (TRANS ("Settings"));
    globalButton_.setButtonText (TRANS ("Patch"));
    globalButton_.setTooltip (TRANS ("Patch / arrangement"));
    synthModeButton_.setButtonText (TRANS ("Synth"));
    fxModeButton_.setButtonText (TRANS ("FX"));

    pageSelector_.setTabName (0, TRANS ("SYNTH"));
    if (pageSelector_.getNumTabs() > 1)
        pageSelector_.setTabName (1, TRANS ("FX"));
    // The CentralModBar pill/cluster labels are language-neutral short codes
    // (E1/L1/ARP/ENV...), so there are no tab labels to re-apply on a language
    // switch (the old nested ENV/LFO/MOD-MATRIX tab strip is gone).

    for (auto& page : generatedPages_)
        page->refreshLanguage();

    if (patchPage_ != nullptr)
        patchPage_->refreshLanguage();
    if (settingsPanel_ != nullptr)
        settingsPanel_->refreshLanguage();
    // NOTE: the SidePanel's own title-bar text ("Settings") has no public setter,
    // so it updates on the next editor open (set via TRANS at construction) but
    // not live. The in-panel chrome (Language combo etc.) DOES update live.

    repaint();
}

void ParvatiEditor::loadLogoIcon()
{
    if (logoDrawable_ != nullptr)
        return;

    int svgBytes = 0;
    const char* const svgData = ParvatiLogo::getNamedResource ("parvati_logo_svg", svgBytes);
    if (svgData == nullptr || svgBytes <= 0)
        return;

    // parvati_logo.svg is now TRUE vector art (outlined <path>/<g>, no raster),
    // so parse it with JUCE's SVG renderer and cache the resulting Drawable.
    // No PNG/base64/<image> decode anywhere. (JUCE 9 exposes the string-based
    // parser createFromSVGString; the older createFromSVG(XmlElement) is gone.)
    logoDrawable_ = juce::Drawable::createFromSVGString (
        juce::String (svgData, (size_t) svgBytes));
}

void ParvatiEditor::paint (juce::Graphics& g)
{
    const auto& theme = themeManager_.getCurrentTheme();
    // The whole UI (header included) is one flat windowBackground — no tinted
    // band. The chrome separators are ChromeRule CHILD components (the editor's
    // own paint is overdrawn by the content children — see ChromeRule).
    g.fillAll (theme.backgroundBase);

    // Header logo cluster: [brand icon] [gap] [white "Parvati" text], painted
    // into the reserved left logo block (the version label sits inline to its
    // right). The icon is a fixed brand asset drawn as-is (own colours); the
    // "Parvati" text uses the theme `text` token so it re-colours each paint().
    if (! logoArea_.isEmpty())
    {
        // Two-line brand (2026-08-23 revision 3): "PARVATI" — ALL CAPS, PLAIN,
        // 17pt, LETTER-SPACED — centred over the SUBTITLE TEXT's own span
        // (not the whole block: the block carries breathing slack that would
        // offset the optical centre), above "by 805Labs \xc2\xb7 v<ver>"
        // (10px, dim, LEFT-aligned at the block's left edge). The airy
        // tracking + smaller plain weight is the "lighter font" read.
        auto block = logoArea_;
        const juce::Font subFont = lnf_.appFont (10.0f, juce::Font::plain);
        const juce::String subText (juce::CharPointer_UTF8 ("by 805Labs \xc2\xb7 v" PARVATI_VERSION));
        {
            juce::GlyphArrangement gs;
            gs.addLineOfText (subFont, subText, 0.0f, 0.0f);
            const float subW = gs.getBoundingBox (0, gs.getNumGlyphs(), true).getWidth();

            const juce::Font markFont = lnf_.appFont (kLogoTextHeight, juce::Font::plain);
            const juce::String mark (kLogoText);
            const float markW = trackedTextWidth (markFont, mark, kLogoTracking);

            const auto band = block.removeFromTop (juce::roundToInt (static_cast<float> (block.getHeight()) * 0.62f));
            g.setColour (theme.textPrimary);
            // Draw each character at its tracked x, baseline-centred in the
            // band (optical middle ~= band centre + ~0.3 of the font height).
            float x = block.getX() + (subW - markW) * 0.5f;   // centred OVER THE SUBTITLE SPAN
            if (x < block.getX()) x = block.getX();
            const float baseline = band.getCentre().y + kLogoTextHeight * 0.30f;
            for (int i = 0; i < mark.length(); ++i)
            {
                const auto ch = mark.substring (i, i + 1);
                juce::GlyphArrangement gc;
                gc.addLineOfText (markFont, ch, 0.0f, 0.0f);
                const float chW = gc.getBoundingBox (0, gc.getNumGlyphs(), true).getWidth();
                g.drawSingleLineText (ch, juce::roundToInt (x), juce::roundToInt (baseline));
                x += chW + kLogoTracking;
            }
        }
        g.setFont (subFont);
        g.setColour (theme.textSecondary);
        // CharPointer_UTF8: the \xc2\xb7 middle dot makes this a UTF-8 literal;
        // the implicit juce::String (const char*) conversion would assert
        // (CharPointer_ASCII::isValidString) on EVERY editor paint — the paint
        // is not clipped before the String is constructed — and in Release the
        // ASCII path is ambiguous for bytes >127.
        g.drawText (subText, block, juce::Justification::centredLeft, false);

    }
}

void ParvatiEditor::resized()
{
    auto area = getLocalBounds();

    // Keep the UI out of the OS safe area (iOS: status bar / home indicator /
    // landscape camera stub). Without the TOP inset the header is laid out at
    // y=0 directly under the iOS status bar, where iOS consumes/defers the
    // first touch — the reported double-tap / non-recognized-tap issue on the
    // top row. iOS ONLY: desktop displays can report non-zero insets too (e.g.
    // a MacBook notch), which would pad the WINDOWED desktop UI — desktop
    // builds apply no safe-area padding (the window chrome already insets it).
    // safeAreaInsets are in DISPLAY (physical) points, but `area` is in the
    // editor's LOCAL logical coords. The global scale factor maps local->physical,
    // so divide each inset by it: at any zoom the rendered inset then lands on the
    // real safe-area edge (without this, zooming out shrank the inset and the UI
    // slid back under the status bar).
#if JUCE_IOS
    // W9: trim ONLY the display edges the editor actually SPANS. The old
    // unconditional full-inset trim wasted up to ~47-59pt/side on a centred
    // AUv3 pane that touches no safe-area edge (the round-3 lane-C
    // aggravator). A full-screen Standalone (editor == display) still trims
    // every side, so the T-fix notch/home-indicator behaviour is unchanged.
    // Guards: headless (no peer) and offscreen (no display found) skip the
    // trim entirely. Comparison in DISPLAY points with a small tolerance so
    // a pixel-level border still counts as spanning the edge.
    if (auto* peer = getPeer())
    {
        const auto screen = peer->localToGlobal (getLocalBounds());
        if (auto* d = juce::Desktop::getInstance().getDisplays()
                          .getDisplayForPoint (screen.getCentre(), false))
        {
            const auto total = d->totalArea;
            if (total.getWidth() > 0 && total.getHeight() > 0)
            {
                constexpr int kEdgeTolerance = 4;
                const double z = juce::jmax (0.1, (double) juce::Desktop::getInstance().getGlobalScaleFactor());
                const auto& s = d->safeAreaInsets;
                if (screen.getY() <= total.getY() + kEdgeTolerance)
                    area = area.withTrimmedTop (juce::roundToInt (s.getTop() / z));
                if (screen.getBottom() >= total.getBottom() - kEdgeTolerance)
                    area = area.withTrimmedBottom (juce::roundToInt (s.getBottom() / z));
                if (screen.getX() <= total.getX() + kEdgeTolerance)
                    area = area.withTrimmedLeft (juce::roundToInt (s.getLeft() / z));
                if (screen.getRight() >= total.getRight() - kEdgeTolerance)
                    area = area.withTrimmedRight (juce::roundToInt (s.getRight() / z));
            }
        }
    }
#endif

#if ! JUCE_IOS
    // Desktop (non-iPadOS): a little air between the window's top edge and
    // the header. iPadOS already gets its safe-area top inset above; a
    // borderless plugin window shows the header kissing the window frame
    // otherwise.
    area = area.withTrimmedTop (kDesktopTopPad);
#endif

    // ---- Bottom status strip = LOWEST band: [tooltip bar] + [n/denom] +
    //      [CPU %] on the RIGHT (indicators sit at the right edge; the hover
    //      tooltip fills the left). ----
    {
        statusBand_ = area.removeFromBottom (kVoiceStripH);
        auto strip = statusBand_.reduced (6, 1);
        statusLoadLabel_.setBounds (strip.removeFromRight (96));
        strip.removeFromRight (8);
        statusCountLabel_.setBounds (strip.removeFromRight (48));
        statusTooltipLabel_.setBounds (strip);
    }

    // NOTE: the virtual keyboard is NO LONGER part of the layout flow. It floats
    // as an OVERLAY over the bottom of the content area (positioned at the end of
    // resized()), so the workspace + overlays keep the FULL content height and
    // toggling [KBD] never moves the controls.

    // ---- Header (44px row): [logo+version] (left) | Patch/Part menu (centre) | icons+[KBD] (right) ----
    auto header = area.removeFromTop (kHeaderH);
    headerBand_ = header;   // geometry source for the headerRule_ separator
    // A kBarHeight-tall strip vertically centred in the 44px header holds every
    // header control (the logo block uses the same strip height).
    auto bar = header.withTrimmedTop ((kHeaderH - kBarHeight) / 2)
                     .withTrimmedBottom ((kHeaderH - kBarHeight) / 2)
                     .reduced (6, 0);

    // Right cluster (removeFromRight => first item ends up rightmost): system
    // icons, then the [KBD] toggle at the far right.
    // Right cluster = a coherent toolbar grouped [Load][Save] | [Undo][Redo] |
    // [Zoom +/0/-] | [Gear] | [MOD] | [MAP] | [KBD]. The icon/zoom buttons
    // share a uniform gap; an 8px gap separates the history/zoom/view icons
    // from the file group. Save/Load are trimmed (100/80 -> 84/70) so the
    // cluster stays compact and never collides with the centred Patch/Part
    // cluster at the default width. Every icon is a 44x44 touch target with
    // >=8pt gaps, and the three zoom buttons (+/-/0) are folded into one "..."
    // overflow popup so the grown cluster still fits the 1280pt editor width.
    // [KBD] is already 44pt wide.
    // ---- W9 adaptive header folding (AUv3 compact panes) ----
    // The AUv3 wrapper force-resizes the editor to the host pane
    // (setResizeLimits is desktop-only advice), and juce::Rectangle
    // removeFromLeft/Right CLIP to the remaining width — an unfolded fixed
    // budget drives controls to 0px (invisible AND untouchable; the round-3
    // lane-C finding: AUM keyboard-open ~570pt, GarageBand panes ~700pt).
    // Secondary controls fold into the existing "..." overflow popup at
    // MEASURED budget breakpoints (derived from the fixed budgets below:
    // right cluster 466pt full (7x44 icons + gaps + slim Load 48/Save 52) /
    // 310 without the view trio / 258 without Redo; fixed left overhead 123pt
    // (insets 12 + edge 8 + logo ~103 = the WIDER version-subtitle line + 18px
    // breathing margin, per the version/patch-separation fix);
    // preset 156 + Patch 64 + the Part/Synth/FX cluster 212):
    //   < 1024: Part combo + [Synth]/[FX] fold (the cluster's designed budget
    //           is exactly 1024 — below it SYNTH/FX historically collapsed;
    //           at the floor the budget closes at ~1015, 9pt slack);
    //   < 810:  [MOD]/[MAP]/gear also fold (the preset+Patch cluster
    //           + full right cluster needs ~815);
    //   < 650:  Redo AND the [Patch] page button also fold (with both
    //           placed, 560pt runs ~46pt over; the Patch page stays reachable
    //           via the popup's page items).
    // Primary controls (preset browser, Load, Save, Undo, [KBD], "...") NEVER
    // fold. At >= 1024 every flag is false and the sequence below is
    // byte-identical to the pre-W9 layout (the desktop designed-width gates
    // are unchanged). Pure resized() math + visibility — no timers.
    const int editorW = getWidth();
    const bool foldPartCluster = editorW < 1024;
    const bool foldViewCluster = editorW < 810;
    const bool foldHistoryBand = editorW < 650;

    // ---- Header-button visual height (user feedback: "a tiny bit less
    //      tall") ----
    // The header STRIP stays the full 44pt (kHeaderH/kBarHeight unchanged;
    // pinned by ipad_hig_sizing_test). On desktop the header BUTTONS now draw
    // at 36pt, vertically centred in their strip cell — a slimmer chrome. On
    // iOS the button fills the whole 44pt cell so every touch target keeps
    // the HIG 44pt minimum (the shrink is visual-only and desktop-only).
    // (Preprocessor, not a ternary: JUCE_IOS is undefined on non-iOS builds,
    // which an expression would reject as an undeclared identifier.)
#if JUCE_IOS
    constexpr int kHeaderBtnH = 44;
#else
    constexpr int kHeaderBtnH = 36;
#endif
    const auto slimCell = [] (juce::Rectangle<int> cell)
    { return cell.withSizeKeepingCentre (cell.getWidth(), kHeaderBtnH); };

    kbdToggleButton_.setVisible (true);   // primary: never folds
    kbdToggleButton_.setBounds (slimCell (bar.removeFromRight (44)));     // [KBD] keyboard-overlay toggle (far right)
    bar.removeFromRight (8);
    modBarToggleButton_.setVisible (! foldViewCluster);
    if (! foldViewCluster)
    {
        modBarToggleButton_.setBounds (slimCell (bar.removeFromRight (44))); // [MOD] mod-pill bar seam toggle (left of [KBD])
        bar.removeFromRight (8);
    }
    modAssignButton_.setVisible (! foldViewCluster);
    if (! foldViewCluster)
    {
        modAssignButton_.setBounds (slimCell (bar.removeFromRight (44)));     // [MOD] tap-to-assign toggle
        bar.removeFromRight (8);
    }
    settingsButton_.setVisible (! foldViewCluster);
    if (! foldViewCluster)
    {
        settingsButton_.setBounds (slimCell (bar.removeFromRight (44)));      // gear
        bar.removeFromRight (8);
    }
    // The "..." overflow host: visible ONLY when something is actually
    // folded away (foldPartCluster is the LARGEST breakpoint — below it the
    // menu carries the Part/page/folded-action items). At >= 1024 nothing is
    // folded, the menu would open EMPTY (the zoom items moved to Settings in
    // 6ed8463 — the "still visible but no longer working" report), so the
    // button hides entirely and its 52pt of header space is reclaimed.
    zoomOverflowButton_.setVisible (foldPartCluster);
    if (foldPartCluster)
    {
        zoomOverflowButton_.setBounds (slimCell (bar.removeFromRight (44)));  // "..." overflow (popup)
        bar.removeFromRight (8);
    }
    redoButton_.setVisible (! foldHistoryBand);
    if (! foldHistoryBand)
    {
        redoButton_.setBounds (slimCell (bar.removeFromRight (44)));          // redo
        bar.removeFromRight (8);
    }
    undoButton_.setVisible (true);   // primary: never folds
    undoButton_.setBounds (slimCell (bar.removeFromRight (44)));          // undo
    bar.removeFromRight (4);   // separates the history/view icons from the file group
    saveButton_.setBounds (slimCell (bar.removeFromRight (52)));          // Save (direct .parvati; .PRO/.MUL export lives on the Patch page)
    bar.removeFromRight (6);
    loadButton_.setBounds (slimCell (bar.removeFromRight (48)));          // Load


    // Left: brand icon + white "Parvati" wordmark (painted) + version label
    // inline to its right. Layout: [~14px edge] "Parvati" [6px] [icon] [6px] [version]
    // (equal 6px gaps; text width measured with the SAME font paint() uses).
    {
        bar.removeFromLeft (8);   // extra left edge whitespace (6 from bar.reduced + 8 = ~14px)
        const juce::Font textFont = lnf_.appFont (kLogoTextHeight, juce::Font::plain);   // SAME weight/size paint() uses
        const int textW = juce::roundToInt (trackedTextWidth (textFont, kLogoText, kLogoTracking));
        // ---- Version/patch separation (user feedback: "more distance between
        //      the version and the patch indicator") ----
        // The version subtitle ("by 805Labs · v<ver>", 10px) is painted inside
        // the SAME brand block and is WIDER than the bold wordmark — the old
        // block sized to the wordmark alone (+16 slack), so the subtitle nearly
        // touched the preset dropdown. Size the block to fit BOTH lines (the
        // wider of the two) plus an 18px breathing margin, so the visible gap
        // from the version text to the patch indicator is a comfortable ~18px.
        // The version stays in the brand block (moving it far-right would
        // collide with the folding icon cluster); the extra ~17px is reclaimed
        // from Save/Load below so the 1024px minimum-width budget still holds.
        const juce::Font subFont = lnf_.appFont (10.0f, juce::Font::plain);
        juce::GlyphArrangement gaSub;
        gaSub.addLineOfText (subFont,
                             juce::String (juce::CharPointer_UTF8 ("by 805Labs \xc2\xb7 v" PARVATI_VERSION)),
                             0.0f, 0.0f);
        const int subW = juce::roundToInt (gaSub.getBoundingBox (0, gaSub.getNumGlyphs(), true).getWidth());
        const int brandW = juce::jmax (textW, subW);
        // logoArea_ carries the breathing-room slack for the left-aligned
        // preset dropdown that follows it.
        logoArea_ = bar.removeFromLeft (brandW + 18);

    }

    // Patch/Part menu cluster. The "Patch:" caption is removed and the preset
    // dropdown is LEFT-aligned right after the logo block (logoArea_ carries
    // breathing-room slack) so it sits close to the wordmark; the preset browser
    // is narrowed. The toolbar sits at the right edge, so the menus pack from the
    // left of the remaining bar. Layout: [preset][gap][Patch][Part n][Synth][FX]
    // W9: the secondary tail controls fold at the measured breakpoints (see
    // the fold block above) and are hidden; the preset browser is PRIMARY —
    // it never folds, but below the floor its width is ELASTIC (whatever the
    // leftover affords, clamped to a 60pt floor so it stays functional)
    // instead of the fixed 156pt natural width.
    {
        const int partComboW = 88;
        const int gapW = 6;
        const int globalW = 64;
        const int modeW = 50;   // [Synth]/[FX] toggle buttons (radio group)

        // Left-cluster budget at the 1024px MINIMUM editor width
        // (setResizeLimits): right cluster 486 (7x44 icons + gaps + Load/Save)
        // + insets 12 + edge 8 + logo ~70+16 + this cluster 156+6+64+6+88+6+50+6+50
        // = 1024. presetW is 156 (was 168): at 168 the cluster ran ~12px over,
        // truncating [FX] to ~38px; 156 lands exactly on the budget so every
        // control keeps its designed width at the minimum frame.

        globalButton_.setVisible (! foldHistoryBand);
        partCombo_.setVisible (! foldPartCluster);
        synthModeButton_.setVisible (! foldPartCluster);
        fxModeButton_.setVisible (! foldPartCluster);

        auto cluster = bar;   // left-aligned: follows the logo block directly
        if (presetBrowser_ != nullptr)
        {
            // Elastic preset width: at/above the floor this clamps to the
            // natural 156 (identical to the old layout); below it the preset
            // absorbs the squeeze FIRST (a 60pt floor) so the placed primary
            // neighbours keep their designed widths as long as possible.
            const int availForPreset = cluster.getWidth()
                                           - (foldHistoryBand ? 0 : (globalW + gapW));
            const int presetW = juce::jlimit (60, 156, availForPreset);
            presetBrowser_->setBounds (cluster.removeFromLeft (presetW));
            cluster.removeFromLeft (gapW);
        }
        if (! foldHistoryBand)
        {
            globalButton_.setBounds (slimCell (cluster.removeFromLeft (globalW)));   // Patch page overlay toggle (between Patch dropdown and Part)
            cluster.removeFromLeft (gapW);
        }
        if (! foldPartCluster)
        {
            partCombo_.setBounds (slimCell (cluster.removeFromLeft (partComboW)));
            cluster.removeFromLeft (gapW);
            // Synth/FX mode toggle (radio group) after Part.
            synthModeButton_.setBounds (slimCell (cluster.removeFromLeft (modeW)));
            cluster.removeFromLeft (gapW);
            fxModeButton_.setBounds (slimCell (cluster.removeFromLeft (modeW)));
        }
    }

    // ---- Chrome-rule clearance: the separator rules (topmost ChromeRule
    //      components) sit 5px below the header and 5px above the status
    //      strip. The content area starts BELOW the header rule and stops
    //      ABOVE the status rule (5 gap + 1 rule on each side), so nothing —
    //      including the workspace's top-row scrollbar — starts above/behind
    //      the rules, and the chrome reads as an evenly inset frame. ----
    area = area.withTrimmedTop (kChromeRuleGap + 1)
               .withTrimmedBottom (kChromeRuleGap + 1);

    // ---- Page selector [SYNTH] + integrated content (no void) ----
    // pageSelector_ (a single-tab TabbedComponent, bar hidden via depth 0) fills
    // the remaining area and sizes SYNTH (SynthWorkspace) into all of it — butted
    // directly under the header rule's clearance. SynthWorkspace lays out its 3
    // columns + nested tab groups in its own resized().
    pageSelector_.setBounds (area);

    // The Patch page overlay covers exactly the content area when toggled on.
    // PatchPage::resized lays out its rows + hosts / reflows the globalPage_, so
    // only its bounds are set here (no direct globalPage_ setBounds / reflow).
    if (patchPage_ != nullptr)
        patchPage_->setBounds (area);

    // ---- Keyboard OVERLAY: floats over the bottom of the content area ----
    // `area` is the full content rect (status strip + header already trimmed);
    // the workspace + overlays above were given ALL of it, so they keep their
    // full height. The keyboard (incl. the pitch/mod wheels to its left) is
    // positioned absolutely over the bottom kKeyboardH pixels of that rect and
    // shown/hidden purely via setVisible() — toggling [KBD] never resizes the
    // content above. The strip is TALL (kKeyboardH == the workspace bottom-row
    // cap), so [KBD]-on covers the ENTIRE bottom row — the active generator
    // editor + the mod/FX matrix hide underneath it. Z-order: keyboardView_ is
    // added AFTER pageSelector_ so it already paints above the workspace; the
    // Patch overlay lifts itself toFront when shown and the keyboard overlay is
    // then re-lifted above it (see showTopPage / the [KBD] toggle) so [KBD]
    // stays visible in Patch mode.
    // The Settings side panel (added last) stays above it too — showTopPage /
    // the gear click re-lift it above the patch overlay. No toFront() is called
    // here so a resize while a modal is open never lifts the keyboard above it.
    if (keyboardView_ != nullptr)
    {
        // The wheels panel widens with the tall strip (was 76 at the flat
        // 76px strip) so the pitch/mod wheel arcs stay proportionate at the
        // full two-octave keyboard height.
        constexpr int kWheelsW = 100;
        const bool kbdVisible = kbdToggleButton_.getToggleState();
        auto bottomStrip = area.withHeight (juce::jmin (kKeyboardH, area.getHeight()))
                               .withY (area.getBottom() - juce::jmin (kKeyboardH, area.getHeight()));
        if (wheels_ != nullptr)
            wheels_->setBounds (bottomStrip.removeFromLeft (kWheelsW));
        keyboardView_->setBounds (bottomStrip);
        keyboardView_->setVisible (kbdVisible);
        if (wheels_ != nullptr)
            wheels_->setVisible (kbdVisible);
        // Keyboard TOP RULE (2026-08-20): a full-width separator at the strip's
        // top edge so the overlay reads as a raised chrome band over the
        // content it covers (the same family as the header/status rules; the
        // depth falloff points UP into the covered content). Added after
        // keyboardView_ in the child order, so it paints above the strip.
        if (keyboardRule_ != nullptr)
        {
            keyboardRule_->setVisible (kbdVisible);
            keyboardRule_->setBounds (area.getX(), bottomStrip.getY() - kChromeShadowH,
                                      area.getWidth(), 1 + kChromeShadowH);
        }
    }

    // ---- Chrome separator rules (components; geometry from the bands):
    //      LAST in resized() so the bands are final. Header rule: 1px rule +
    //      a 5px depth falloff BELOW it (bounds include the shadow room);
    //      status rule: 1px rule + a 5px falloff ABOVE it. The rules sit 5px
    //      from their bands; the falloff extends into the already-reserved
    //      kChromeRuleGap+1 content clearance, so nothing is overlapped. ----
    if (headerRule_ != nullptr)
        headerRule_->setBounds (headerBand_.getX(), headerBand_.getBottom() + kChromeRuleGap,
                                headerBand_.getWidth(), 1 + kChromeShadowH);
    if (statusRule_ != nullptr)
    {
        // FULL WIDTH (2026-08-20 user request): the former 2.5%-of-band inset
        // stopped the rule short of the window edges; the bottom separator now
        // spans the band edge-to-edge (which is the editor's full width on
        // desktop; on iOS the safe-area-trimmed band edge).
        statusRule_->setBounds (statusBand_.getX(),
                                statusBand_.getY() - kChromeRuleGap - 1 - kChromeShadowH,
                                statusBand_.getWidth(), 1 + kChromeShadowH);
    }
}

//==========================================================================
#if JUCE_IOS
// iOS: make a successful user-area save visible in the Files app. The
// Standalone plist advertises UIFileSharingEnabled, which browses
// <sandbox>/Documents — but the USER patch area lives in the shared App-Group
// container (Source/ui/SharedContainer.h), which Files cannot browse at all.
// COPY, not move: the group container stays the single source of truth for
// the PresetBrowser and the AUv3 extension (Standalone + AUv3 keep one tree);
// the Documents copy is a plain export. Only saves that land INSIDE the USER
// area are mirrored — a picker navigated elsewhere is an explicit export
// already — and the USER/ sub-path is preserved (Documents/Parvati/USER/...)
// so bank folders survive. A failed copy is non-fatal (the save itself
// already succeeded) and the next successful save re-mirrors; stale mirrors
// are intentionally never deleted (silently removing user-visible files
// would be surprising, and the group tree remains authoritative).
// Note: when this editor runs inside the AUv3 extension in a host, Documents
// is the EXTENSION's sandbox, which Files does not browse — the copy is
// harmless there; the Standalone app's own saves are the visible ones.
static void mirrorUserSaveToDocumentsIOS (const juce::File& saved)
{
    const auto userDir = ParvatiAudioProcessor::getUserPatchDir();
    if (! saved.isAChildOf (userDir))
        return;
    const auto dest = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile ("Parvati/USER")
                          .getChildFile (saved.getRelativePathFrom (userDir));
    // F-ios-files-5 (iOS hunt 2026-08-19): ATOMIC copy. copyFileTo streamed
    // straight onto the Files-visible destination, so an iOS suspension
    // mid-copy could leave a TORN .parvati/.PRO/.MUL in Documents that the
    // user could open. The TemporaryFile + rename pattern (the house idiom,
    // PatchFile.cpp / FactoryPresetInstaller.cpp) makes the visible file
    // appear only complete: an interrupted copy leaves the PREVIOUS mirror
    // intact and no fragment behind. A failed copy stays non-fatal (the save
    // itself already succeeded) and the next successful save re-mirrors.
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile temp (dest);
    if (saved.copyFileTo (temp.getFile()))
        temp.overwriteTargetFileWithTemporary();
}
#endif

void ParvatiEditor::openLoadDialog()
{
    // .parvati-first (2026-08-20): the Load button/shortcut default to the
    // native format; Ambika .PRO/.MUL remain importable via drag-drop and
    // the PresetBrowser (both route through applyPatchFile — a separate
    // seam from this picker). Starts nowhere in particular (empty start
    // file): on iOS the document picker opens at its browse root, from
    // which the mirrored Documents/Parvati/USER saves are reachable (On My
    // iPad > Parvati); on desktop the browser starts at the OS default.
    fileChooser_ = std::make_unique<juce::FileChooser> (TRANS ("Load Parvati Patch (.parvati)"),
                                                       juce::File(), "*.parvati");
    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
            applyPatchFile (fc.getResult());
        fileChooser_ = nullptr;
    });
}

// Minimal, non-blocking failure feedback for the file save/load paths: a
// save that cannot write (unwritable location / full disk) previously
// returned false into silence — the user believed the file existed (data
// loss), and a failed load did nothing at all. NativeMessageBox (async) so
// the FileChooser callback never blocks the message thread; headless tests
// never reach these branches (their save/load paths all succeed).
namespace
{
void showFileOpFailure (const juce::String& title, const juce::String& path)
{
    // HEADLESS GUARD: a native alert needs a window-server session; the
    // headless test/editor-coverage binaries (console, no desktop windows)
    // would block forever inside the OS alert once the message loop is
    // pumped — silently hanging the run. Skip when no desktop windows exist
    // OR the PARVATI_HEADLESS override is set (the GUI test binaries: their
    // editor IS on the desktop, but no human is present to dismiss a native
    // alert — the same hazard the file-picker gates solve). Tests never rely
    // on the dialog; they assert the return codes.
    if (nativeDialogsSuppressed()
        || juce::Desktop::getInstance().getNumComponents() == 0)
        return;
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                 title, path);
}
}  // namespace

// Shared tail for the three save pickers (.PRO / .parvati / .MUL). See the
// header declaration; each dialog below keeps ONLY its saver lambda (and the
// .MUL fallback branch) — the default name, USER/ dir, default file, save-mode
// flags, extension forcing and the fileChooser_ teardown live here once.
void ParvatiEditor::launchSavePicker (const char* titleKey, const char* ext,
                                      const std::function<void (const juce::File&)>& saver)
{
    auto defaultName = processorRef_.getLoadedProgramName();
    if (defaultName.isEmpty())
        defaultName = "Parvati";
    const juce::File defaultDir = processorRef_.getUserPatchDir();
    defaultDir.createDirectory();   // make sure USER/ exists
    const juce::File defaultFile (defaultDir.getChildFile (defaultName + ext));
    fileChooser_ = std::make_unique<juce::FileChooser> (TRANS (titleKey),
                                                       defaultFile, juce::String ("*") + ext);
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync (flags, [this, ext, saver] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
            saver (fc.getResult().withFileExtension (ext));
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::openSaveDialog()
{
    // EXPORT PATH (.PRO — Patch-page button only, 2026-08-20): save the
    // CURRENT part as an Ambika .PRO (byte-faithful; shareable with Ambika
    // hardware; drops vca_curve / filter_card / arp). Desktop-gated by the
    // PatchPage wiring. Defaults to the user's preset area.
    launchSavePicker ("Save Ambika Patch (.PRO)", ".PRO", [this] (const juce::File& f) {
        if (processorRef_.saveProgramFile (f))
            afterMultiSaved (f);
        else
            showFileOpFailure (TRANS ("Could not save file:"), f.getFullPathName());
    });
}

void ParvatiEditor::openSaveParvatiDialog()
{
    // Save a full-fidelity Parvati-native patch (.parvati, YAML) that carries
    // EVERYTHING — including vca_curve / filter_card / arp, which the Ambika
    // .PRO byte format drops. Defaults to the user's preset area.
    launchSavePicker ("Save Parvati Patch (.parvati)", ".parvati", [this] (const juce::File& f) {
        if (processorRef_.saveParvatiPatchFile (f))
            afterMultiSaved (f);
        else
            showFileOpFailure (TRANS ("Could not save file:"), f.getFullPathName());
    });
}

void ParvatiEditor::openSaveMultiDialog()
{
    // Save the whole 6-Part multitimbral setup as an Ambika .MUL (byte-faithful;
    // shareable with Ambika hardware). When a Part requests more voices than
    // its voicecards (the voice-slot extension), the .MUL cannot express the
    // setup faithfully -> the export-fallback dialog picks a strategy for
    // mapping the voices onto the 6 hardware cards (MulExport solver).
    launchSavePicker ("Save Ambika Multi (.MUL)", ".MUL", [this] (const juce::File& f) {
        const auto setup = processorRef_.getMulExportSetup();
        if (! parvati::mul_export::needsFallback (setup))
        {
            if (processorRef_.saveMultiFile (f))
                afterMultiSaved (f);
            else
                showFileOpFailure (TRANS ("Could not save file:"), f.getFullPathName());
            return;
        }
        // Needs a strategy: show the fallback dialog (with the part names for
        // the preview), then save with the choice.
        std::vector<juce::String> names;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
            names.push_back (processorRef_.getEngine().getPartName (i));
        // SafePointer guard: MulExportDialog opens its OWN desktop window
        // (launchAsync), so its DoneCallback can fire after the host has torn
        // the editor down — a raw `this` would dangle (use-after-free on
        // processorRef_/afterMultiSaved).
        juce::Component::SafePointer<ParvatiEditor> safe (this);
        MulExportDialog::launch (this, setup, names, [safe, f] (int strategy)
        {
            if (safe == nullptr) return;
            if (strategy >= 0 && safe->processorRef_.saveMultiFile (f, strategy))
                safe->afterMultiSaved (f);
            else if (strategy >= 0)
                showFileOpFailure (TRANS ("Could not save file:"), f.getFullPathName());
        });
    });
}

void ParvatiEditor::afterMultiSaved (const juce::File& f)
{
    juce::ignoreUnused (f);   // iOS-only use below; silences -Wunused-parameter on desktop
#if JUCE_IOS
    mirrorUserSaveToDocumentsIOS (f);   // Files-app export (see helper)
#endif
    if (presetBrowser_ != nullptr)
    {
        presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
        presetBrowser_->invalidate();   // W10: a save changed the preset tree -> rescan at the next open
    }
}

int ParvatiEditor::currentPartMidiChannel()
{
    // Omni (0) -> 1: the editor's virtual keyboard, wheels and pressure
    // callbacks inject on a concrete channel, and channel 1 always reaches
    // the Part (every MIDI consumer folds Omni the same way).
    int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
    if (ch == 0) ch = 1;
    return ch;
}
void ParvatiEditor::applyPatchFile (const juce::File& f)
{
    // .MUL -> multitimbral multi (all 6 Parts); .PRO -> single program;
    // .parvati -> Parvati-native YAML (patch or multi, sniffed by format:).
    // Both kinds refresh the Patch page on success (see the tail).
    bool ok = false;

    if (f.hasFileExtension (".parvati"))
    {
        juce::String text;
        if (juce::FileInputStream in (f); in.openedOk())
            text = in.readEntireStreamAsString();
        const juce::String fmt = parvati::preset::detectParvatiFormat (text);
        if (fmt == parvati::preset::kFormatMulti)
            ok = processorRef_.loadParvatiMultiFile (f);
        else
            ok = processorRef_.loadParvatiPatchFile (f);
    }
    else
    {
        ok = f.hasFileExtension (".mul") ? processorRef_.loadMultiFile (f)
                                         : processorRef_.loadProgramFile (f);
    }

    if (ok)
    {
#if JUCE_IOS
        // F-ios-files-1 (iOS hunt 2026-08-19): import-on-load. The iOS save
        // picker can only write into document-provider locations (On My iPad /
        // iCloud / third-party) — the shared App-Group USER tree this editor's
        // PresetBrowser scans is NOT part of any provider tree, so a preset
        // saved through the picker NEVER appeared in the preset menu. Compensate
        // on the LOAD side: after a successful load of a file that lives OUTSIDE
        // the USER tree, atomically import a copy into USER and drop the browser
        // cache, so the just-loaded preset is selectable from the menu from now
        // on. Desktop is deliberately unchanged (its users organize files on
        // disk; behavior stays byte-identical).
        if (const juce::File imported = PresetBrowser::importIntoUserTree (
                f, processorRef_.getUserPatchDir());
            imported.existsAsFile())
        {
            // F-ios-files-1c: mirror the imported copy to Documents as well, so
            // the Files app shows a picker-location save (same atomic helper as
            // the USER import; Documents visibility is the T6 story).
            mirrorUserSaveToDocumentsIOS (imported);
            if (presetBrowser_ != nullptr)
                presetBrowser_->invalidate();   // the next open shows the new leaf
        }
#endif
        if (presetBrowser_ != nullptr)
        {
            presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
            // Anchor prev/next stepping at this load (a load can arrive from
            // the menu — which already set it — or from Load... / drag-drop /
            // open-in, which did not; idempotent either way).
            presetBrowser_->setCurrentFile (f);
        }
        // EVERY successful load refreshes the Patch page, not just multis. A
        // multi rewrites every part's channel / key zone / voice allocation /
        // polyphony, and a SINGLE-patch load (.PRO / .parvati patch) rewrites
        // the current part's PartData bytes — poly (byte 15), raga preset
        // (byte 4), spread (byte 3) — which the page's Poly / Tune combos
        // mirror. refresh() is a cheap guarded engine re-read (no onChange
        // fires), so calling it on the patch-only path too keeps the rows
        // honest without touching the engine.
        if (patchPage_ != nullptr)
        {
            patchPage_->refresh();
            // Same dedupe capture as the reveal path: the rows now reflect this
            // display version (the poll mirror skips the redundant re-read).
            lastPatchPageDisplayVersion_ = processorRef_.getEngine().getDisplayVersion();
        }
    }
    else
    {
        // A failed load (corrupt file / wrong format / unreadable) previously
        // vanished into silence — the drop or menu click did nothing, with no
        // hint why. The engine was left untouched (validate-before-mutate),
        // so a non-blocking notice is the whole remedy.
        showFileOpFailure (TRANS ("Could not load file:"), f.getFullPathName());
    }
}

bool ParvatiEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return std::any_of (files.begin(), files.end(), [] (const juce::String& fn) {
        return fn.endsWithIgnoreCase (".pro") || fn.endsWithIgnoreCase (".mul")
               || fn.endsWithIgnoreCase (".parvati");
    });
}

void ParvatiEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& fn : files)
    {
        juce::File f (fn);
        if (f.hasFileExtension (".pro") || f.hasFileExtension (".mul") || f.hasFileExtension (".parvati"))
        {
            applyPatchFile (f);
            break;
        }
    }
}
