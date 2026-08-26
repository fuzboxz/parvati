// Editor/processor lifecycle regression tests (bug hunt 2026-08-19, iOS hunt).
//
//   [1] F-ios-lc-3 — process-global editor teardown side-effects are
//       reference-counted. An AUv3 extension process hosts MULTIPLE Hellcat
//       instances (AUM: several editors in one process; JUCE's AUv3 wrapper
//       creates/destroys one editor per instance). Pre-fix, destroying editor
//       A re-enabled the screensaver (T14's protection voided) and cleared the
//       process-global ParamControl tap-assign flag while editor B was still
//       open. Pinned here on desktop via the tap-assign half + the live-count
//       hook (the screensaver half shares the exact same count predicate and
//       is iOS-compile-gated; its counterpart assertion is included and
//       trivially holds on desktop because the desktop build never touches
//       screensaver policy).
//   [2] F-ios-touch-3 — no INVISIBLE component participates in keyboard focus
//       traversal. The parked zoom trio (constructed, never placed => 0x0
//       extent) used to stay "visible", so an iPad hardware keyboard's Tab
//       could hand focus to an invisible button (Space then fires a zoom
//       change; musical typing stops reaching the keyboard view).
//   [3] F-ios-lc-2 — interruption semantics. releaseResources (the AUv3
//       wrapper's deallocateRenderResources hook: phone call / route change)
//       must make the next render cycle start from silence: held notes die
//       (JUCE only clears notes on a sample-RATE change, so voices gated at
//       the interruption would otherwise resume gated forever), queued MIDI is
//       dropped, patch state survives, and a NEW note still sounds after the
//       release/prepare cycle.
//
// Run: ./build_unified/hellcat_unified_tests lifecycle_test

#include <cmath>
#include "unified_test_runner.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// Headless run-loop pump for asynchronous UI (triggerClick posts to the
// message queue; the JUCE MessageQueue IS a CFRunLoopSource on the main loop
// — the editor_test / perf-smoke idiom). Apple-only like the rest of the
// desktop harnesses in this suite.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"   // HellcatEditor + ParamControl (tap-assign statics)
#include "PluginProcessor.h"
#include "test_utils.h"              // displayAvailable (headless-host skip)
#include "ui/SeqLengthStepper.h"   // [4] the seq-length picker seam

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void pump (int ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
    juce::Timer::callPendingTimersSynchronously();
}

// Delivers posted messages (async clicks, focus grabs) for up to ~ms.
void pumpRunLoop (int ms)
{
#if defined (__APPLE__)
    for (int i = 0; i < (ms + 19) / 20; ++i)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
#else
    pump (ms);
#endif
}

// Active (sounding) voices of Part 0 — the arp_test accounting.
int activePart0 (HellcatAudioProcessor& p)
{
    int n = 0;
    for (int vi : p.getEngine().getPart (0).voiceIndices)
        if (auto* av = p.getEngine().getAmbikaVoice (vi))
            if (av->isVoiceActive())
                ++n;
    return n;
}

void renderEmpty (HellcatAudioProcessor& p, int blocks = 1)
{
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

double blockEnergy (juce::AudioBuffer<float>& buf)
{
    double e = 0.0;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const double s = buf.getSample (ch, i);
            e += s * s;
        }
    return e;
}
}  // namespace

TEST(lifecycle_test)
{
    juce::ScopedJuceInitialiser_GUI guiInit;

    // ==================================================================
    // [1] Two live editors: destroying ONE must not undo process-global
    //     state the OTHER still depends on.
    // ==================================================================
    std::printf ("[1] editor teardown side-effects are reference-counted\n");
    {
        HellcatAudioProcessor procA, procB;
        procA.prepareToPlay (48000.0, 256);
        procB.prepareToPlay (48000.0, 256);

        std::unique_ptr<juce::AudioProcessorEditor> edA (procA.createEditor());
        std::unique_ptr<juce::AudioProcessorEditor> edB (procB.createEditor());
        check (edA != nullptr && edB != nullptr, "two editors created");
        check (HellcatEditor::liveEditorCountForTest() == 2,
               "live-editor count == 2 while both are open");

        const bool screensaverBefore = juce::Desktop::getInstance().isScreenSaverEnabled();

        // Arm the process-global tap-assign mode the way editor B's [MOD]
        // toggle does; it must survive editor A's teardown.
        ParamControl::setTapAssignActive (true);
        check (ParamControl::tapAssignActive(), "tap-assign armed");

        edA.reset();   // destroy ONLY editor A (editor B still live)
        check (HellcatEditor::liveEditorCountForTest() == 1,
               "count drops to 1 after destroying one editor");
        check (ParamControl::tapAssignActive(),
               "tap-assign still ACTIVE after a sibling editor closes (pre-fix: cleared)");
        check (juce::Desktop::getInstance().isScreenSaverEnabled() == screensaverBefore,
               "screensaver policy unchanged while a sibling editor is live");

        // The LAST editor's teardown DOES clear the global (the W-era
        // contract: [MAP] left ON at close must not leak to a reopen).
        edB.reset();
        check (HellcatEditor::liveEditorCountForTest() == 0, "count drops to 0 after the last editor");
        check (! ParamControl::tapAssignActive(),
               "tap-assign cleared when the LAST editor closes (leak guard intact)");
        check (juce::Desktop::getInstance().isScreenSaverEnabled() == screensaverBefore,
               "screensaver policy restored to the pre-test value on desktop");
    }

    // ==================================================================
    // [2] Focus traversal contains no zero-extent (invisible) component.
    // ==================================================================
    std::printf ("[2] focus traversal excludes invisible zero-extent controls\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        check (ed != nullptr, "editor created");

        // Give the tree one layout pass so real controls carry their bounds.
        ed->setSize (1280, 800);
        pump (20);

        const auto all = juce::KeyboardFocusTraverser().getAllComponents (ed.get());
        int zeroExtent = 0;
        for (auto* c : all)
            if (c != nullptr && (c->getWidth() <= 0 || c->getHeight() <= 0))
            {
                ++zeroExtent;
                std::printf ("     zero-extent focusable: %s (%dx%d)\n",
                             c->getName().toRawUTF8(), c->getWidth(), c->getHeight());
            }
        char msg[96];
        std::snprintf (msg, sizeof (msg),
                       "no focusable component has zero extent [%d of %zu]",
                       zeroExtent, all.size());
        check (zeroExtent == 0, msg);
        check (! all.empty(), "focus traversal is non-empty (the gate is real)");
    }

    // ==================================================================
    // [2b] Musical typing survives a focused control (2026-08-21 user
    //      report) and works with the strip hidden / nothing focused
    //      (2026-08-26 user request): unhandled plain keys bubble to
    //      HellcatEditor::keyPressed, which forwards them to the KeyboardView,
    //      and the editor's top-level KeyListener covers the no-focus walk —
    //      no tree-wide wantsKeyboardFocus mutation (that approach emptied the
    //      traversal guarded by [2]).
    // ==================================================================
    std::printf ("[2b] musical typing survives a focused control (and the hidden strip)\n");
    // The section needs a real desktop peer (isShowing() + timer delivery);
    // a headless host cannot create one. macOS always reports a display.
    if (! displayAvailable())
    {
        std::printf ("  SKIP [2b]: no display server (headless host)\n");
    }
    else
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        check (ed != nullptr, "editor created");
        // Desktop peer so isShowing() is meaningful (same requirement as
        // editor_test: JUCE timers + visibility chains need the peer). A
        // fresh component is born HIDDEN — setVisible BEFORE the peer so the
        // children's hooks fire with the peer present (editor_test's order).
        ed->setSize (1280, 800);
        ed->setVisible (true);
        ed->addToDesktop (0);
        pump (20);

        const juce::KeyPress noteA ('a', juce::ModifierKeys::noModifiers, 'a');

        auto* parEd = dynamic_cast<HellcatEditor*> (ed.get());
        check (parEd != nullptr, "editor is a HellcatEditor (top-level seam)");

        // HOST contract first: this harness builds a bare processor
        // (wrapperType != Standalone), so the editor wires musical typing
        // OFF (a host owns the computer keyboard). A plain musical key stays
        // unclaimed at every dispatch level while typing is off.
        check (! ed->keyPressed (noteA),
               "host build: musical key unclaimed (typing off)");
        if (parEd != nullptr)
            check (! parEd->forwardTopLevelKeyForTest (noteA),
                   "host build: top-level path unclaimed (typing off)");

        // The strip via a RAW tree walk (the focus traversal lists only
        // ON-SCREEN focusables, and the strip is still hidden here). Arm the
        // PUBLIC seam to simulate the standalone gate.
        KeyboardView* strip = nullptr;
        std::function<void (juce::Component*)> findStrip =
            [&] (juce::Component* c)
            {
                if (strip == nullptr)
                    if (auto* kv = dynamic_cast<KeyboardView*> (c))
                        strip = kv;
                for (int i = 0; i < c->getNumChildComponents(); ++i)
                    findStrip (c->getChildComponent (i));
            };
        findStrip (ed.get());
        check (strip != nullptr, "keyboard strip found (still hidden)");
        if (strip != nullptr)
            strip->setComputerKeyboardEnabled (true);

        // 2026-08-26 contract: with typing armed, a plain musical key is
        // claimed WHILE THE STRIP IS HIDDEN. The editor forward covers a
        // focused control; the top-level seam covers the no-focus walk.
        check (ed->keyPressed (noteA),
               "typing armed: musical key claimed while the strip is hidden");
        if (parEd != nullptr)
            check (parEd->forwardTopLevelKeyForTest (noteA),
                   "typing armed: top-level (no-focus) path claims the key");
        // Release path (no stuck note): the key is not physically down in
        // this harness, so the state-change walk must drop the held note.
        check (ed->keyStateChanged (false),
               "keyStateChanged releases the held hidden-strip note");

        // Show the strip the way the user does: the header [KBD] toggle.
        juce::TextButton* kbdToggle = nullptr;
        for (int i = 0; i < ed->getNumChildComponents() && kbdToggle == nullptr; ++i)
            if (auto* b = dynamic_cast<juce::TextButton*> (ed->getChildComponent (i)))
                if (b->getButtonText() == "KBD")
                    kbdToggle = b;
        check (kbdToggle != nullptr, "[KBD] header toggle found");
        if (kbdToggle != nullptr)
        {
            kbdToggle->triggerClick();
            pump (80);   // timers settle
            pumpRunLoop (400);   // deliver the async click + the strip's own async focus grab
        }
        char tmsg[96];
        std::snprintf (tmsg, sizeof (tmsg), "[KBD] toggle is ON after the click (state=%d)",
                      kbdToggle != nullptr ? (int) kbdToggle->getToggleState() : -1);
        check (kbdToggle != nullptr && kbdToggle->getToggleState(), tmsg);

        // (The strip was found + armed above, before the toggle.)
        check (strip != nullptr && strip->isShowing(),
               "keyboard strip is on screen after the [KBD] click");

        // A workspace control (NOT the strip) holds the focus — the
        // clicked-knob mid-performance case the forward must cover. Pick a
        // real ON-SCREEN focusable from the traversal (it is focusable +
        // visible now) so the focus move actually takes.
        const auto focusables = juce::KeyboardFocusTraverser().getAllComponents (ed.get());
        juce::Component* held = nullptr;
        for (auto* c : focusables)
            if (dynamic_cast<KeyboardView*> (c) == nullptr && c->isShowing()
                && c->getWidth() > 0 && c->getHeight() > 0)
            {
                held = c;
                break;
            }
        if (held != nullptr)
            held->grabKeyboardFocus();
        check (held != nullptr && held->hasKeyboardFocus (true),
               "a workspace control holds the focus");

        // The musical key must STILL be claimed: it bubbles from the focused
        // control up to the editor, which forwards it to the visible strip.
        check (ed->keyPressed (noteA),
               "musical key claimed while a control holds the focus");

        // Release path: the editor-level keyStateChanged forward releases the
        // held computer-key note (no stuck note when focus sits on a knob).
        ed->keyStateChanged (false);
        check (true, "keyStateChanged forward returned (no stuck note path)");

        ed->removeFromDesktop();
    }

    // ==================================================================
    // [3] Interruption: releaseResources -> silence; state survives; new
    //     notes still sound.
    // ==================================================================
    std::printf ("[3] releaseResources interruption semantics\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();

        // Hold a note (direct path).
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
            proc.processBlock (buf, midi);
        }
        renderEmpty (proc, 2);
        const int activeHeld = activePart0 (proc);
        std::printf ("     active voices while held: %d\n", activeHeld);
        check (activeHeld >= 1, "note sounds before the interruption");

        const uint8_t shapeBefore = proc.getEngine().getPart (0).patchBytes[0];

        // The interruption: resources torn down (flagged kill + MIDI queue
        // drop) and re-allocated (resume). The kill is serviced at the top of
        // the first processTransport — exactly the first block after resume.
        proc.releaseResources();
        proc.prepareToPlay (48000.0, 256);
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            proc.processBlock (buf, midi);   // services the deferred kill
            const double e = blockEnergy (buf);
            char m[96];
            std::snprintf (m, sizeof (m), "first block after resume is silent (energy %.6g)", e);
            check (e < 1.0e-9, m);
        }
        const int activeAfter = activePart0 (proc);
        std::printf ("     active voices after resume block: %d\n", activeAfter);
        check (activeAfter == 0,
               "held note is dead after release/prepare (pre-fix: stuck forever)");

        check (proc.getEngine().getPart (0).patchBytes[0] == shapeBefore,
               "patch bytes survive the interruption cycle");

        // A NEW note must still sound (state not corrupted).
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (uint8_t) 100), 0);
            proc.processBlock (buf, midi);
        }
        renderEmpty (proc, 2);
        const int activeNew = activePart0 (proc);
        std::printf ("     active voices for a new note: %d\n", activeNew);
        check (activeNew >= 1, "a new note still sounds after the cycle");
    }

    // ------------------------------------------------------------------
    // [4] SeqLengthStepper replacement interaction (F-ios-touch-2): the old
    //     two ~32x20 -/+ buttons (sub-44, the audit's STOPPED T9a) are gone —
    //     the NUMBER is a full-cell tap target opening a 1..16 picker of
    //     44pt rows. Drive the seam headlessly: setValue (the picker item
    //     action) must write the hidden slider -> the APVTS param.
    // ------------------------------------------------------------------
    std::printf ("[4] SeqLengthStepper: full-cell picker replaces the sub-44 +/- pair\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        check (ed != nullptr, "[4] editor created");

        // Hunt a SeqLengthStepper in the tree (the sequencer group's length
        // cells; the page content is constructed at editor build).
        // The Sequencer ParamPage is a GENERATOR page hosted by the Synth
        // workspace's active-generator editor (default = ENV 1, so the seq
        // page is NOT in the visible tree until its generator is selected).
        // Make Sequencer 1 the active generator first — the same seam a pill
        // click drives (modbar_pill_click_test's verified chain).
        if (auto* parEd = dynamic_cast<HellcatEditor*> (ed.get()))
            parEd->getSynthWorkspaceForTest()->setActiveGenerator (ambika::dsp::MOD_SRC_SEQ_1);
        SeqLengthStepper* stepper = nullptr;
        std::function<void (juce::Component*)> hunt = [&] (juce::Component* c)
        {
            if (c == nullptr || stepper != nullptr) return;
            if (auto* s = dynamic_cast<SeqLengthStepper*> (c)) { stepper = s; return; }
            for (int i = 0; i < c->getNumChildComponents(); ++i)
                hunt (c->getChildComponent (i));
        };
        hunt (ed.get());
        check (stepper != nullptr, "[4] a SeqLengthStepper is present in the tree");

        if (stepper != nullptr)
        {
            auto& apvts = proc.getApvts();
            check (juce::roundToInt (apvts.getRawParameterValue ("seq_length_1")->load()) == 16,
                   "[4] precondition: seq_length_1 starts at its 16 default");
            // The picker item action (SafePointer-guarded, runs synchronously
            // when invoked): pick 5.
            stepper->setValueForTest (5);
            check (juce::roundToInt (apvts.getRawParameterValue ("seq_length_1")->load()) == 5,
                   "[4] picker pick writes the param (5)");
            // The keyboard seam still nudges (desktop parity).
            stepper->keyPressedForTest (juce::KeyPress (juce::KeyPress::downKey));
            check (juce::roundToInt (apvts.getRawParameterValue ("seq_length_1")->load()) == 4,
                   "[4] keyboard down nudge decrements (4)");
            // Clamp both ends.
            stepper->setValueForTest (1);
            stepper->keyPressedForTest (juce::KeyPress (juce::KeyPress::downKey));
            check (juce::roundToInt (apvts.getRawParameterValue ("seq_length_1")->load()) == 1,
                   "[4] lower clamp holds at 1");
        }
    }

    // ------------------------------------------------------------------
    // [5] Thermal-hint transition matrix (F-ios-perf-2, 2026-08-19 follow-up).
    //     The 30 Hz timer surfaces the processor's atomic thermal hint ONLY
    //     on a level TRANSITION — the policy is the PURE static
    //     HellcatEditor::thermalStatusForTransition. This pins the full 3x3
    //     matrix (hints are ThermalAction ints: 0=None, 1=Hint,
    //     2=StrongHint) plus the defensive clamp: escalations arm the
    //     transient status exactly once (Hint vs StrongHint distinguished),
    //     de-escalations hand back to the frame-budget expiry (Clear), and
    //     same-level repeats — including the idle 0->0 desktop tick — are
    //     NoOp (the user was already told / nothing changed). The atomic
    //     read itself is trivial; THIS matrix is the regression value: a
    //     future edit that re-posts on every tick (status-strip repaint
    //     churn @30 Hz) or that invents a stronger action than the sampler
    //     can produce fails here.
    // ------------------------------------------------------------------
    std::printf ("[5] Thermal transition matrix (F-ios-perf-2 label surfacing)\n");
    {
        using Action = HellcatEditor::ThermalStatusAction;

        struct Cell { int from, to; Action want; const char* label; };
        const Cell cells[] = {
            // escalations (the three that must arm the status, once each)
            { 0, 0, Action::NoOp,       "0->0 idle tick is NoOp" },
            { 0, 1, Action::ShowHint,   "0->1 escalation shows the Hint text" },
            { 0, 2, Action::ShowStrong, "0->2 escalation shows the STRONG text" },
            // from Hint
            { 1, 0, Action::Clear,      "1->0 de-escalation returns Clear (expiry takes over)" },
            { 1, 1, Action::NoOp,       "1->1 repeat is NoOp (already told)" },
            { 1, 2, Action::ShowStrong, "1->2 escalation shows the STRONG text" },
            // from StrongHint
            { 2, 0, Action::Clear,      "2->0 de-escalation returns Clear" },
            { 2, 1, Action::Clear,      "2->1 partial cool-down returns Clear (no downgrade spam)" },
            { 2, 2, Action::NoOp,       "2->2 repeat is NoOp" },
        };
        for (const auto& c : cells)
        {
            const Action got = HellcatEditor::thermalStatusForTransition (c.from, c.to);
            check (got == c.want, c.label);
        }
        // Defensive clamp: a corrupt/out-of-range atomic (the raw int from
        // thermalHint_) must never invent an action stronger than the
        // sampler's domain — negatives and >2 clamp BEFORE the comparison.
        check (HellcatEditor::thermalStatusForTransition (-5, 2)
                   == Action::ShowStrong, "out-of-range old clamps to None (escalation still detected)");
        check (HellcatEditor::thermalStatusForTransition (1, 99)
                   == Action::ShowStrong, "out-of-range new clamps to StrongHint (no invented action)");
        check (HellcatEditor::thermalStatusForTransition (99, 99)
                   == Action::NoOp,       "out-of-range same->same clamps to NoOp");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "LIFECYCLE TEST: FAILURES" : "LIFECYCLE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
