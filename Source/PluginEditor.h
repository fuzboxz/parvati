// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// HellcatEditor — the full Ambika GUI. An integrated (Serum-style dense)
// editor whose controls are generated entirely from the PatchParamDescriptor
// table (ParameterLayout.h), so the GUI and the APVTS byte-bridge can never
// drift apart. Every one of
// the 104 patch/part parameters gets a control (rotary Slider or ComboBox)
// plus an APVTS attachment.
//
// Colours come from the HellcatTheme via a single HellcatLookAndFeel set on the
// editor and inherited by the whole component tree — no per-control palette.
// Phase 2a of docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ui/KeyboardView.h"
#include "ui/NoteName.h"       // midiNoteName (keyboard-settings tooltip)
#include "ui/SynthWorkspace.h" // complete type (getSynthWorkspaceForTest)
#include "ui/ModDestMap.h"
#include "ui/ModMatrixView.h"
#include "ui/WheelsComponent.h"
#include "ui/HellcatLookAndFeel.h"

class HellcatAudioProcessor;

// Re-apply every Label font in the component tree via the active L&F's
// appFont() (the system default sans-serif), preserving each label's
// height/style. juce::Label caches its font, so it must be re-pushed when the
// UI is (re)built.
void refreshFontsIn (juce::Component* root, const HellcatLookAndFeel& lnf);
#include "ui/SettingsScrollTracker.h"   // SettingsPanel sizing tracker (extracted; also brings in SettingsPanel.h)
#include "ui/ThemeManager.h"
#include "ui/IconButton.h"
#include "ui/PresetBrowser.h"
#include "ui/ParamControl.h"   // ParamControl (descriptor-driven control cell)
#include "ui/ParamPage.h"      // ParamPage (one generated page of cells)

class PatchPage;
class SynthWorkspace;
class FxWorkspace;
class FxMatrixView;
class FxRoutingBar;
class FxSlotCard;
class EnvelopeDisplay;
class FilterResponseDisplay;

// Live-modulation feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the editor-
// owned poll that reads ONE engine telemetry frame per tick and caches it for
// every consumer (mod-bar strips, envelope stage marker, live filter curve).
// Forward-declared so the header stays light; the unique_ptr member below
// needs only an incomplete type at declaration (defined in the .cpp).
namespace hellcat { class LiveFeedbackHub; }

//==============================================================================
class HellcatEditor : public juce::AudioProcessorEditor,
                     public ParamControlPopupHost,   // hideHostedTooltip (ParamControl context menus)
                     public juce::DragAndDropContainer,
                     private juce::FileDragAndDropTarget,
                     private juce::Timer,
                     private juce::ChangeListener,
                     private juce::KeyListener   // standalone musical typing, no-focus path (below)
{
public:
    explicit HellcatEditor (HellcatAudioProcessor&);
    ~HellcatEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Parses the embedded hellcat_logo.svg (a true vector: outlined <path>/<g>
    // art, no raster) into a juce::Drawable once (idempotent — no-op once the
    // drawable exists). The drawable carries its OWN brand colours and is drawn
    // as-is (NOT theme-tinted). Called lazily from paint().
    void loadLogoIcon();

    // Zoom keyboard shortcuts: Cmd/Ctrl + +/=/-/0 (Phase 4b). Returns true only
    // for handled keys, so typing in combos / text boxes is never swallowed.
    // Also forwards unhandled plain keys to the KeyboardView while the strip
    // is showing, so musical typing survives focus-holding control tweaks
    // (2026-08-21 user report; see the musical-typing note in keyPressed).
    bool keyPressed (const juce::KeyPress& key) override;

    // Release half of the same musical-typing path: bubbles here from any
    // focused control; forwards to the strip so held computer-key notes end
    // when their keys come up.
    bool keyStateChanged (bool isKeyDown) override;

    // ---- Standalone musical typing without the on-screen strip ----
    // JUCE hands a key to the FOCUSED component, then walks the parent chain.
    // When no component holds the focus (the plain state of a fresh standalone
    // window), the walk starts at the top-level window and never reaches this
    // editor. So the editor registers itself as a KeyListener on the current
    // top-level (parentHierarchyChanged keeps the registration in step with
    // reparenting). The KeyboardView gates every note itself (playback is
    // standalone-only, bare keys only), so hosts keep their keys.
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;
    bool keyStateChanged (bool isKeyDown, juce::Component* origin) override;
    void parentHierarchyChanged() override;

    // Test seam for the top-level path above: the peer dispatch itself needs
    // real OS key events, which a headless harness cannot send. Runs the SAME
    // listener logic once, with a null origin.
    bool forwardTopLevelKeyForTest (const juce::KeyPress& key)
    { return keyPressed (key, nullptr); }

    // User zoom, clamped to [0.75, 2.0] (also reachable via Cmd/Ctrl + +/=/-/0).
    // Applies juce::Desktop::setGlobalScaleFactor(), which is PROCESS-WIDE in
    // JUCE: every JUCE window / plugin instance in the host shares one zoom,
    // and the last editor to set it wins (a documented limitation of
    // multi-instance use). ~HellcatEditor resets it to 1.0 so a non-default zoom
    // does not leak after close. Per-editor, transform-based zoom (no global
    // side-effect) is a documented future enhancement, deferred to avoid
    // destabilizing the reflow layout. Default 1.0.
    void   setZoom (double zoom);
    double getZoom() const noexcept { return zoom_; }

    // The editor-wide TooltipWindow. ParamControl context menus hide it the
    // instant they open (the ParamControlPopupHost seam; the 30 Hz timer in
    // timerCallback also hides it while any modal popup stays open).
    void hideHostedTooltip() override
    {
        if (auto* tw = tooltipWindow_.get())
            tw->hideTip();
    }

    // Host context menu provider (AUv3 / VST3 hosts), for the SAME seam: a
    // ParamControl context menu merges the host's parameter menu below its own
    // Reset/Randomize entries (see ParamControl::showContextMenu).
    juce::AudioProcessorEditorHostContext* popupHostContext() const override
    {
        return getHostContext();
    }

    // Enumerate EVERY generated ParamPage as a raw pointer — the 3 top-row
    // direct pages (OSC/Mixer/Filter), the generator pages (ENV/LFO/SEQ/ARP/
    // Modifiers), and the Global page — parented or not. Headless coverage /
    // screenshot tools use this to inspect each page's ParamControls (a
    // ParamPage owns its controls whether parented or not) without depending on
    // the live reparent/visibility state the CentralModBar drives. Exposed for
    // test/tool access only.
    std::vector<ParamPage*> allGeneratedPages() const;

    // Switch the top-level page selector between the SYNTH workspace (false) and
    // the FX workspace (true). Public so the offscreen screen-shot tool (and
    // tests) can drive the mode toggle without simulating header-button clicks.
    void setFxMode (bool fx);

    // Relabel the top-bar Part selector with the current part names/aliases
    // (Hellcat extension). Called on name edits + from the poll timer.
    void refreshPartComboNames();

    // Select which of the three peer top-level pages is shown (0=Synth, 1=FX,
    // 2=Patch) — exactly what the header page buttons do. Public for test/tool
    // access only (same rationale as setFxMode: headless layout + screenshot
    // tools must drive the page switch without simulating clicks).
    void setCurrentTopPage (int pageIndex);

    // F-ios-lc-3 (bug hunt 2026-08-19): live HellcatEditor instances in THIS
    // process (an AUv3 extension process hosts several). Test hook for the
    // reference-counted process-global teardown side-effects (screensaver /
    // tap-assign clear) — the transitions 0->1 / N->0 are what gate them.
    static int liveEditorCountForTest() noexcept;

    // ---- Thermal-hint label surfacing (F-ios-perf-2, 2026-08-19 follow-up) ----
    // Decision for a thermal-hint transition (ThermalAction ints: 0=None,
    // 1=Hint, 2=StrongHint). PURE so the lifecycle test pins the full 3x3
    // matrix: an ESCALATION arms the transient status exactly once; a
    // de-escalation returns Clear (the caller lets the frame-budget expiry
    // handle it — the seam has no explicit clear); same-level repeats are
    // NoOp (the user was already told / nothing changed). The 30 Hz timer
    // applies this ONLY on iOS (JUCE_IOS-gated read of
    // HellcatAudioProcessor::getThermalHint()).
    enum class ThermalStatusAction { NoOp = 0, ShowHint = 1, ShowStrong = 2, Clear = 3 };
    static ThermalStatusAction thermalStatusForTransition (int oldHint, int newHint) noexcept;

    // Test-only (lifecycle test [4]): the Synth workspace (generator-page
    // host) so headless tests can drive setActiveGenerator — the same seam a
    // mod-bar pill click drives.
    SynthWorkspace* getSynthWorkspaceForTest() { return synthWorkspace_.get(); }

    // Test-only (theme regression pins): switch the theme through the REAL
    // path (ThemeManager selection -> change broadcast -> the editor's
    // changeListenerCallback -> applyAllColoursFromTheme) — exactly what the
    // SettingsPanel theme combo drives — so tests can pin behaviour across a
    // theme switch. Returns false for an unknown theme name.
    bool selectThemeForTest (const juce::String& name)
    { return themeManager_.selectByName (name); }

    // Test-only: the SYNCHRONOUS variant of selectThemeForTest — delivers the
    // change broadcast inline (selectByName only POSTS the async message, which
    // a headless single-shot renderer never pumps). Headless probes (the
    // screenshot tool) use this so every capture renders the requested theme.
    bool switchThemeSynchronousForTest (const juce::String& name)
    {
        if (! themeManager_.selectByName (name))
            return false;
        themeManager_.sendSynchronousChangeMessage();
        return true;
    }

    // Test-only (settings drawer regression): opens the settings SidePanel
    // exactly as the gear button does (showOrHide + the toggle-state sync the
    // button's onClick does), so a probe can drive the REAL drawer path
    // (slide animation, tracker, viewport sizing) without private access.
    void openSettingsForTest()
    {
        if (settingsPanelHost_ != nullptr)
        {
            if (settingsScrollTracker_ != nullptr)
                settingsScrollTracker_->preSizeForOpen();   // content in the slide snapshot
            settingsPanelHost_->showOrHide (true);
            settingsButton_.setToggleState (true, juce::dontSendNotification);
        }
    }

    // Test-only (settings drawer regression): the drawer's content tree so a
    // probe can walk SidePanel -> Viewport -> SettingsPanel without private
    // access (ownership stays with the editor).
    juce::Component* settingsContentForTest() { return settingsScroll_.get(); }

    // ---- Chrome separator rules (test hooks; 2026-08-20 full-width wave) ----
    // The status rule's bounds (pinned: FULL editor width) and the keyboard
    // overlay's top rule (visible iff the keyboard strip is shown; bounds
    // span the content width at the strip's top edge).
    juce::Rectangle<int> statusRuleBoundsForTest() const
    { return statusRule_ != nullptr ? statusRule_->getBounds() : juce::Rectangle<int>(); }
    bool keyboardRuleVisibleForTest() const
    { return keyboardRule_ != nullptr && keyboardRule_->isVisible(); }
    juce::Rectangle<int> keyboardRuleBoundsForTest() const
    { return keyboardRule_ != nullptr ? keyboardRule_->getBounds() : juce::Rectangle<int>(); }

    // TEST-ONLY ([20] top-bar chrome pins): the header brand-block geometry
    // (wordmark + version subtitle), for the version/patch-separation layout
    // assertion in editor_test.
    const juce::Rectangle<int>& getLogoAreaForTest() const noexcept { return logoArea_; }

    // One iteration of the poll timer's VISIBLE-Patch-page mirror check: when
    // the Patch page is on screen and the engine's display version moved
    // (an out-of-band write — host automation of part_polyphony / part_raga,
    // MIDI NRPN, host undo — see SynthEngine::getDisplayVersion), re-read the
    // engine into the rows. Public for test/automation only: headless tests
    // drive the exact timer code path without waiting for the 30 Hz tick.
    void pollPatchPageMirror();

    // juce::FileDragAndDropTarget — accept dropped Ambika .PRO/.MUL/.yml
    // files. DECLARED PUBLIC (the base is inherited privately): the drop entry
    // filesDropped -> applyPatchFile is the REAL user load path (drag-drop onto
    // the editor), and headless tests drive exactly that seam instead of
    // re-implementing the load routing (the private-inheritance conversion
    // HellcatEditor* -> FileDragAndDropTarget* is inaccessible outside).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    // Active voices of the CURRENT part (pool voices filtered by their part
    // tag + the SF-1 atomic activity snapshot) — the numerator of the bottom
    // status-strip count. Part-relative since the 96-voice pool: the
    // denominator is the current part's voiceCount_, so a global count
    // produced mixed fractions like "23/16".
    int currentPartActiveVoiceCount() const;

    // Unified 3-way top-level page selector (Synth/FX/Patch). Each header
    // page button calls showTopPage(idx): exclusive page visibility + button
    // states; reparents the shared generator only on a Synth<->FX change.
    void showTopPage (int pageIndex);      // 0=Synth 1=FX 2=Patch
    void reparentGeneratorTo (bool toFx);  // move the shared generator between workspaces

    // juce::DragAndDropContainer — detect the start/end of an internal
    // mod-source drag (payload "parvatiModSrc:<enum>") to toggle the drag-drop
    // affordance: valid destination knobs light up as drop zones and every
    // other control dims. dragOperationEnded fires on BOTH drop and cancel, so
    // the state always clears. (HellcatEditor IS a DragAndDropContainer, so it
    // overrides these two protected virtuals directly — this JUCE version has
    // no separate DragAndDropContainer::Listener / addListener API.)
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& details) override;
    void dragOperationEnded   (const juce::DragAndDropTarget::SourceDetails&) override;

    // juce::Timer — periodic status upkeep (bottom-strip voice count, part-name
    // relabel, CPU readout). The Patch page is NOT timer-refreshed: it re-reads
    // the engine on every successful load path (applyPatchFile) and every time
    // it is revealed (showTopPage(2)), which covers host state restores too
    // (a restore rewrites the engine; the next reveal re-reads it).
    void timerCallback() override;

    // timerCallback stage helpers: one duty each, called in order. The rate
    // decision runs last and reads this tick's results, so no stage returns
    // early. Cross-stage products travel as parameters: the popup state, the
    // drained transient status and the active-voice count.
    void tickSettingsScrollbar();        // re-assert the settings drawer scrollbar
    bool tickTooltipPopupGuard();        // popup state; hides a frozen tip
    void tickTelemetryReasserts();       // hub + bar + live display poll re-assert
    void tickMouseActivity();            // mouse-move tracking for the rate signal
    void tickThermalHint();              // iOS only: surface thermal transitions
    void tickHostTempoHint();            // once per host-to-manual tempo transition
    juce::String drainTransientStatus(); // the single per-tick status drain
    int tickStatusStrip (const juce::String& transientStatus, bool popupOpen);
    void tickKeyboardLatching();         // mirror sounding notes onto the keys
    void tickAdaptiveRate (int activeVoices, bool popupOpen,
                           const juce::String& transientStatus);

    // juce::ChangeListener — re-apply the L&F theme + repaint when the
    // ThemeManager selection moves.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void openLoadDialog();
    void openSaveDialog();
    void openSaveHellcatDialog();

    // Shared tail for the three save pickers (.PRO / .yml / .MUL):
    // default-name fallback, USER/ dir creation, default file, save-mode
    // FileChooser flags, extension forcing and the picker teardown. @p saver
    // runs synchronously for the chosen file; it owns the format-specific
    // write, the failure alert and any follow-up dialog (the .MUL fallback).
    void launchSavePicker (const char* titleKey, const char* ext,
                           const std::function<void (const juce::File&)>& saver);

    // ---- Keyboard-shortcut seams (keyPressed dispatches to these) ----
    // Small, headless-testable handlers: each returns true when the shortcut
    // was consumed. The FileChooser launch inside the Load/Save handlers is
    // DESKTOP-GATED (a headless console has no window server for a native
    // picker — the showFileOpFailure guard idiom), so a headless call is
    // consumed without opening anything (the tests assert the seam fired via
    // the return value). Step: PresetBrowser::selectNext/selectPrev. Part:
    // the same partCombo_ setSelectedId seam the part context menu uses.
    bool handleLoadPresetShortcut();
    bool handleSavePresetShortcut();      // hellcat-format save (full fidelity) — see .cpp
    bool handleStepPresetShortcut (int direction);   // +1 next / -1 prev
    bool handlePartSelectShortcut (int part0Based);  // 0..5
    // Save the whole multitimbral setup as an Ambika .MUL. When the setup uses
    // voice slots beyond the hardware (mul_export::needsFallback), the export
    // fallback dialog (MulExportDialog) picks a voice->card mapping strategy.
    void openSaveMultiDialog();
    // Post-save chrome refresh + iOS Documents mirroring for a saved .MUL.
    void afterMultiSaved (const juce::File& f);
    // The current Part's MIDI channel for editor-injected MIDI (virtual
    // keyboard, wheels, channel pressure). Omni (0) folds to channel 1: the
    // editor injects on a concrete channel, and channel 1 always reaches the
    // Part.
    int currentPartMidiChannel();
    void applyPatchFile (const juce::File&);

    // Re-apply every editor-chrome string through TRANS() (buttons, captions,
    // tab names, page headings) and refresh the settings panel + patch page,
    // so a live language switch updates immediately. Called once after the UI
    // is built and again on every language change.
    void applyChromeTranslations();

    // Apply a user zoom step (clamps + applies the global scale + persists it +
    // mirrors it into the Settings slider). Shared by the keyboard shortcuts and
    // the on-screen +/-/0 buttons so both use one code path.
    void applyZoom (double zoom);

    // ---- Live mod-feedback refresh-rate application ----
    // Push @p hz (already clamped by the processor pref) to the ONE poll pump
    // (liveHub_) and BOTH workspace mod bars, and record it in
    // lastAppliedRefreshHz_. The ctor calls it once; timerCallback re-checks
    // the persisted pref every tick so a Settings-panel change lands within
    // one tick with no dedicated plumbing (the panel's own callback ALSO
    // routes here for an immediate effect).
    void applyLiveFeedbackRefreshRate (int hz);


    // The Patch page is owned here and shown as a full-page view over the
    // content area. It hosts the editor-owned Section::Global ParamPage
    // (patch-wide knobs) with this page's 6-part allocation table (and the
    // arrangement summary) merged into its Global panel.
    std::unique_ptr<PatchPage> patchPage_;
    // Generated ParamPages — EDITOR-OWNED. Every page is created here so every
    // APVTS attachment and the checked byte-bridge survive the layout
    // unchanged: the 3 top-row direct pages (OSC/Mixer/Filter), the generator
    // pages (ENV/LFO/SEQ/ARP/Modifiers), and the Global ParamPage. At most one
    // generator page is reparented into SynthWorkspace's active-editor host at a
    // time (default ENV 1); the rest stay unparented until their CentralModBar
    // pill is clicked. The Global page is a direct-child overlay toggled by the
    // header "Global" button.
    // Declaration order is deliberate for safe teardown: pageSelector_ (hosting
    // synthWorkspace_ as non-owned content) destroys first, then synthWorkspace_
    // (its host + bar merely detach the non-owned pages), then generatedPages_
    // deletes them.
    std::vector<std::unique_ptr<ParamPage>> generatedPages_;
    // The redesigned MOD MATRIX panel (Wave 1). EDITOR-OWNED. Hosted NON-owned as
    // a DIRECT child of SynthWorkspace (setModMatrixView), exactly like the
    // reparented ParamPages, so the view must outlive the workspace that hosts
    // it. Declared BEFORE synthWorkspace_ on purpose: members destroy in REVERSE
    // declaration order, so synthWorkspace_ tears down FIRST and merely DETACHES
    // the non-owned view, then modMatrixView_ deletes it — no use-after-free,
    // no double-free.
    std::unique_ptr<ModMatrixView> modMatrixView_;
    // SYNTH content: the 3-row workspace. TOP = OSC|MIX|FILTER direct ParamPages;
    // MIDDLE = the full-width CentralModBar (the pill hub); BOTTOM = the
    // active-editor host (one generator ParamPage at a time, chosen by the
    // bar's pills) on the left and the ModMatrixView on the right. Owns only its
    // bar + host; pages + view stay editor-owned.
    std::unique_ptr<SynthWorkspace> synthWorkspace_;
    // FX content: a clone of SynthWorkspace for the FX tab (TOP = 3 FX-slot
    // ParamPages, MIDDLE = its own CentralModBar, BOTTOM-LEFT = the SHARED
    // active-generator host, BOTTOM-RIGHT = the editor-owned FxMatrixView). Owns
    // only its bar + host; slot/matrix pages + the shared generator pages stay
    // editor-owned. Declared fxMatrixView_ BEFORE fxWorkspace_ (reverse-destruction
    // discipline): fxWorkspace_ tears down FIRST and merely DETACHES the
    // non-owned FxMatrixView + the shared generator pages, then fxMatrixView_
    // deletes itself — no use-after-free / double-free (mirrors the
    // modMatrixView_/synthWorkspace_ comment above).
    std::unique_ptr<FxMatrixView> fxMatrixView_;

    // FX-slot cards (FX1/FX2/FX3) + the full-width FX routing header bar —
    // editor-owned, hosted NON-owned by fxWorkspace_ (reparented, never
    // regenerated). Declared BEFORE fxWorkspace_ (reverse-destruction
    // discipline, like fxMatrixView_): fxWorkspace_ tears down FIRST and merely
    // DETACHES these non-owned views, then they are destroyed here. Each card
    // owns its 6 ParamControls (param1..5 + drywet) + the power/bypass toggle +
    // the type combo; the bar owns the topology combo +
    // the drag-reorderable chain.
    std::unique_ptr<FxRoutingBar> fxRoutingBar_;
    std::unique_ptr<FxSlotCard>   fxSlotCards_[3] {};

    std::unique_ptr<FxWorkspace>  fxWorkspace_;

    // ---- Live modulation feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The single poll pump: one bounded seqlock read of the engine's telemetry
    // frame per tick at the user's refresh rate (ui_refresh_hz, default 30),
    // cached here for every consumer — the two CentralModBars' pill strips
    // and the envelope/filter display overlays read the CACHE, so the engine's
    // lock is taken once per tick no matter how many components animate.
    // A PURE OBSERVER: no child components, no workspace/bar ownership, no
    // APVTS state — only a fetcher bound to engine.readUiTelemetry. Declared
    // AFTER fxWorkspace_/synthWorkspace_ (reverse-destruction discipline): it
    // destroys FIRST, before the bars whose provider lambdas capture it (the
    // lambdas null-check liveHub_ at call time; timer callbacks run on this
    // same message thread, so nothing can interleave during teardown anyway).
    std::unique_ptr<hellcat::LiveFeedbackHub> liveHub_;

    // Two-tab page selector (bar hidden via depth 0). Index 0 = synthWorkspace_,
    // index 1 = fxWorkspace_; the header [Synth]/[FX] buttons swap the current
    // tab (setFxMode). PATCH is a header-button overlay (patchPage_), not a
    // tab. Non-owned tab content (editor-owned via generatedPages_).
    juce::TabbedComponent pageSelector_ { juce::TabbedButtonBar::TabsAtTop };

    HellcatAudioProcessor& processorRef_;

    // Theme system (Phase 2a). Direct members: ~HellcatEditor's body removes the
    // ChangeListener and resets the L&F pointer before these members (and the
    // base Component) are destroyed, so the broadcaster and the L&F stay valid
    // for the whole teardown.
    ThemeManager themeManager_;
    HellcatLookAndFeel lnf_;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow_;
    double zoom_ = 1.0;

    // Top patch bar. The patch selector is a cascading PresetBrowser (replaces
    // the flat patchCombo_); undo/redo are Path-drawn IconButtons (no font glyph).
    std::unique_ptr<PresetBrowser> presetBrowser_;
    juce::TextButton loadButton_  { "Load" };
    juce::TextButton saveButton_  { "Save" };
    IconButton       undoButton_  { IconButton::Icon::Undo };   // top-bar Undo (Cmd/Ctrl+Z)
    IconButton       redoButton_  { IconButton::Icon::Redo };   // top-bar Redo (Cmd/Ctrl+Shift+Z / Y)
    // The "..." overflow opens the W9 folded-actions popup (Part/pages/
    // toggles on compact panes; the zoom actions moved to the Settings
    // panel 2026-08-20, so it no longer hosts zoom items).
    juce::TextButton zoomOverflowButton_ { "..." };
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Top bar: Part selector (bound to the `part_select` APVTS param).
    juce::ComboBox partCombo_;
    // Display-string cache for refreshPartComboNames: the 30 Hz poll used to
    // call ComboBox::changeItemText 6x every tick unconditionally (it is NOT a
    // no-op internally). Only the items whose label actually changed are
    // rewritten now; a language switch changes the placeholder text, so it
    // still updates through the same compare (W7, lane-A finding 6).
    std::array<juce::String, 6> partComboLabelCache_;
    // Synth<->FX mode toggle (a view-mode selector, like the Patch overlay —
    // NOT an APVTS param). Inserted between partCombo_ and the Patch button in
    // the header cluster: Part [Part 1] [Synth] [FX] [Patch].
    juce::TextButton synthModeButton_ { "Synth" };
    juce::TextButton fxModeButton_    { "FX" };
    bool             fxModeActive_    = false;   // which workspace (Synth/FX) hosts the generator
    int              currentTopPage_  = 0;       // active top-level page: 0=Synth 1=FX 2=Patch
    uint32_t         lastPatchPageDisplayVersion_ = 0;   // engine display version the Patch page rows were last read at (see pollPatchPageMirror)
    juce::TextButton globalButton_ { "Patch" }; // header button -> Patch page overlay (hosts the Section::Global ParamPage; not a patch param)
    juce::TextButton kbdToggleButton_ { "KBD" };  // header toggle: show/hide the bottom virtual keyboard
    juce::TextButton modBarToggleButton_ { "MOD" };  // header toggle: show/hide the central mod-pill bar seam
    juce::TextButton modAssignButton_ { "MAP" };  // header toggle: tap-to-assign modulation mode
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> partComboAttachment_;

    // Top header: brand icon + white "Hellcat" wordmark (painted, left).
    juce::Rectangle<int> logoArea_;   // set in resized(); paint() draws the icon + "Hellcat" text here

    // Chrome bands (set in resized()): the separator rules' geometry source
    // (see ChromeRule — the rules are components, NOT strokes here, because
    // children overdraw the editor's own paint).
    juce::Rectangle<int> headerBand_, statusBand_;
    // headerRule_/statusRule_ + the keyboard-overlay top rule (all the shared
    // hellcat::ChromeRule — see ui/ChromeRule.h). Declared as unique_ptr<Component>
    // to keep the header light (the type lives in ui/ChromeRule.h).
    std::unique_ptr<juce::Component> headerRule_, statusRule_;
    std::unique_ptr<juce::Component> keyboardRule_;   // above the on-screen keyboard strip (visible iff it is)

    // Brand icon: the embedded hellcat_logo.svg (true vector art) parsed once
    // into a juce::Drawable. It carries its OWN brand colours and is drawn as-is
    // (NOT theme-tinted); only the adjacent "Hellcat" text re-colours with the
    // theme `text` token.
    std::unique_ptr<juce::Drawable> logoDrawable_;

    // iOS HIG: the header grows to 44pt with a full-height (44pt) icon strip so
    // every header icon meets the 44x44 touch minimum; desktop stays 40/34.
    // Exposed public (access-only; no symbol/codegen change) so the HIG sizing-
    // contract test can static_assert these values per platform.
public:
    static constexpr int kBarHeight   = 44;   // full-height icon strip (44pt targets)
    static constexpr int kHeaderH     = 52;   // header height (grown 44 -> 52: 44pt HIG strip + wordmark air + the border/shadow stack; the shadow casts BELOW the border into the content clearance)
    static constexpr int kDesktopTopPad = 5;   // non-iOS: air between the window's top edge and the header
    static constexpr int kChromeRuleGap = 5;   // gap between a chrome band and its separator rule (rule is 1px)
    static constexpr int kChromeShadowH = 5;   // depth-falloff height beside a chrome rule (see ChromeRule)
private:
    // Bottom keyboard overlay strip. TALL two-octave keyboard (KeyboardView
    // shows exactly C3..C5 with keys stretched to the strip width): 246 == the
    // workspace bottom-row cap kBottomRowMaxH = 8+22+4+4+4*(48+4) in
    // SynthWorkspace.cpp / FxWorkspace.cpp, so [KBD]-on covers the ENTIRE
    // bottom row (generator editor + mod/FX matrix). Keep in sync with that
    // constant (no shared include: PluginEditor.h must not pull the matrix
    // headers).
    static constexpr int kKeyboardH   = 246;
    static constexpr int kVoiceStripH = 22;   // compact status strip at the very bottom

    // ---- Phase 4a: visualization + settings integration ----
    // The settings drawer's vertical scroll host (2026-08-21): owns + hosts
    // the SettingsPanel so every row stays reachable in short panes (see the
    // construction site). Declared BEFORE the SidePanel so it is destroyed
    // AFTER it (reverse order) — the panel must never outlive its host chain.
    std::unique_ptr<juce::Viewport> settingsScroll_;
    // Sizes the panel inside the viewport (see the class above); declared
    // AFTER the viewport (destroyed first — it holds refs to both).
    std::unique_ptr<SettingsScrollTracker> settingsScrollTracker_;
    // Settings side panel (hosts the scroll viewport; the viewport owns the
    // SettingsPanel itself).
    std::unique_ptr<juce::SidePanel> settingsPanelHost_;
    SettingsPanel* settingsPanel_ { nullptr };
    IconButton       settingsButton_ { IconButton::Icon::Gear };   // gear icon, top-right

    // Virtual keyboard (bottom strip) + status bar (count + tooltip). Voice
    // activity is read from the engine directly by the status-strip count;
    // no per-voice cells meter exists (the former Patch-page voice-pool view
    // was removed — the per-part Voices rows carry the allocation picture).
    std::unique_ptr<KeyboardView>    keyboardView_;
    std::unique_ptr<WheelsComponent> wheels_;   // pitch + mod wheels (left of keyboard)
    ParamPage*  globalPage_ { nullptr };        // Global page overlay (toggled by globalButton_; hosted by the Patch page)
    // (FX-slot cards FX1/FX2/FX3 are owned by fxSlotCards_ above; the FX routing
    // bar is owned by fxRoutingBar_ above — both hosted NON-owned by fxWorkspace_.)

    // Live graph previews (EnvelopeDisplay / OscPreviewDisplay /
    // FilterResponseDisplay) + the theme category token they read for their trace
    // (cyan Env / magenta LFO / amber Audio). Each entry holds a re-tint function
    // (calling the component's setCategoryColour) + a pointer-to-member theme
    // token, so a theme switch can re-resolve the NEW theme's token value and
    // re-push it (a stored Colour snapshot would otherwise freeze on the old
    // theme).
    using ThemeColourField = juce::Colour HellcatTheme::*;
    using GraphTintFn = std::function<void (const juce::Colour&)>;
    std::vector<std::pair<GraphTintFn, ThemeColourField>> graphCategoryBindings_;
    void reapplyGraphCategoryColours();

    // ---- Live mod-feedback: raw pointers to the display components whose
    // polls carry the live overlays (the 3 ADSR EnvelopeDisplays + the
    // FilterResponseDisplay; LFO-mode displays have no live overlay). Same
    // lifetime discipline as graphCategoryBindings_' raw targets: the pages
    // own the components, the editor owns the pages, and both vectors are
    // cleared implicitly at editor teardown. timerCallback re-asserts each
    // poll (reassertPollTimer) every ~30 Hz tick so a starved timer (JUCE's
    // window peer sequencing can starve the components' own visibility hooks
    // — the shipped-dead-overlay bug, [25] e2e) starts within one tick. */
    std::vector<EnvelopeDisplay*>     liveEnvDisplays_;   // env + LFO waveform displays
    std::vector<class OscPreviewDisplay*> liveOscDisplays_;   // osc waveform displays (same re-assert)
    FilterResponseDisplay*            liveFilterDisplay_ = nullptr;

    // Re-apply every theme-derived colour across the whole editor tree
    // (sendLookAndFeelChange + per-page/workspace/patch applyThemeColors +
    // category arc/mod tints + ENV/LFO graph traces + status labels +
    // keyboard/voice-meter refresh). Shared by the theme-CHANGE path
    // (changeListenerCallback) AND called once at the end of the ctor so knobs,
    // graphs and mod tints are coloured from the FIRST paint in every context
    // (standalone, headless screen tool, tests) — changeListenerCallback is only
    // invoked when selectByName actually moves the selection, so without this
    // explicit call the category colours could stay on the L&F default.
    void applyAllColoursFromTheme();

    // Top-bar chrome affordance ([20]): every header TextButton (page/mode/
    // view toggles, Load/Save, the "..." overflow) and the preset-indicator
    // button gets an unselected-state treatment that reads as CLICKABLE — a
    // low-alpha accentSecondary wash fill + the bright textPrimary text tier —
    // instead of the flat backgroundPanel fill. Selected (toggle-on) keeps the
    // L&F's solid accent fill + dark text, so the on state stays clearly
    // stronger. Resolved from the ACTIVE theme; called from
    // applyAllColoursFromTheme() so it tracks every theme switch.
    void applyHeaderButtonChrome();

    // ---- Synth<->FX mode toggle ----
    // Swap the page-selector tab (index 0 = SYNTH workspace, 1 = FX workspace)
    // and reparent the SHARED active generator page into the now-visible
    // workspace (single active selection — the generator pages are editor-owned
    // and shared, NOT duplicated). The outgoing workspace releases its
    // (non-owned) reference to the active page so the incoming workspace's
    // addAndMakeVisible re-parents it cleanly (a JUCE Component has one parent).
    int  activeGeneratorModSrc_ { 0 };   // current active generator (MOD_SRC_*); default ENV 1

    juce::Label statusCountLabel_;              // bottom-right "n/denom" active-voice count (just left of the CPU readout)
    juce::Label statusLoadLabel_;               // realtime audio-load % ("CPU N%", rightmost in the strip; current block only)
    juce::Label statusTooltipLabel_;            // bottom hover-tooltip bar (fills the strip left of the indicators)

    // Keyboard latching state: notes now lit on the virtual keyboard so
    // we only fire latchNoteOn/Off on actual transitions (avoids stuck lamps).
    juce::Array<int> latchedNotes_;
    int lastLatchPart_ { -1 };   // last part seen; clear latches when it changes

    // ---- Top-level key listener (standalone musical typing, no-focus path) ----
    // The top-level component this editor currently listens to (a SafePointer:
    // the standalone window can die first). refreshTopLevelKeyListener moves
    // the registration on every reparent; the destructor removes it.
    juce::Component::SafePointer<juce::Component> topLevelKeyListenerHost_;
    void refreshTopLevelKeyListener();

    // Timer piece: release musical-typing notes when the window lost key focus
    // (no key-up event follows a deactivation, and the no-focus path has no
    // focusLost callback to do it).
    void tickMusicalTypingFocusGuard();

    // ---- Status-strip audio-load readout anti-flicker + idle-poll state ----
    // The per-block load probe jitters 0<->1% from render-timing noise; without
    // the hold gate below the "CPU N%" text (and with it the whole editor
    // repaint region) churned ~20x/sec at idle. lastLoadPct_ seeds an
    // impossible value so the very first tick always publishes.
    int lastLoadPct_ { -999 };          // last displayed current-load percentage
    juce::Time lastLoadTextUpdate_;     // last time the load text was refreshed
    juce::Colour lastLoadColour_ {};    // last applied load-label colour ({} => unset)

    // ---- Adaptive editor-timer rate (30 Hz active / 4 Hz idle) ----
    // Idle = no sounding voices, no transient status draining, no modal popup,
    // no latched keyboard lamps, and the mouse parked for >3 s: at that point
    // nothing the timer refreshes can change, so the poll drops to 4 Hz. Any
    // activity flips back to 30 Hz on the next tick.
    int timerHz_ { 30 };               // current editor-timer rate
    juce::Point<int> lastMousePos_ { -9999, -9999 };   // detects mouse-moved-since-last-tick
    juce::Time lastMouseActivity_;      // last time the cached mouse position changed

    // ---- Live-feedback refresh-rate application (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The persisted processor pref (ui_refresh_hz) is the single source of
    // truth; this editor-side shadow detects a CHANGE across the ~30 Hz status
    // tick and re-applies it to the hub + both mod bars within one tick, so
    // the Settings combo takes effect with no dedicated plumbing. -1 seeds an
    // impossible value so the first tick always applies (matching the ctor's
    // initial application).
    int lastAppliedRefreshHz_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HellcatEditor)
};
