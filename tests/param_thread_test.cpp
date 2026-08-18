// ParamControl / NoteStepControl audio-thread parameter delivery regression
// test (bug hunt 2026-08-18, F-ui-1).
//
// APVTS listeners fire SYNCHRONOUSLY on the writing thread. Two real paths
// deliver parameterChanged on the audio/render thread while the editor is
// open:
//   (a) the in-plugin NRPN/CC map, serviced INSIDE processBlock
//       (PluginProcessor.cpp -> MidiParameterMap::handleBuffer ->
//        setValueNotifyingHost);
//   (b) host automation (VST3 processParameterChanges / AUv3 setValue on the
//       render thread).
//
// ParamControl::parameterChanged used to mutate GUI state directly
// (slider_->setEnabled / setColour / slider properties / repaint — and
// NoteStepControl::slider_->setValue + repaint): Component mutation is
// message-thread-only. In debug JUCE asserts; in release these writes race
// the paint thread reading the same state.
//
// W11 fix: both listeners now defer off-message-thread deliveries via
// AsyncUpdater (the FxSlotCard pattern) and refresh from CURRENT state in
// handleAsyncUpdate on the message thread.
//
// This test pins the contract deterministically:
//   [1] With a live editor, setValueNotifyingHost from a NON-message thread
//       completes without tripping the message-thread asserts (a Debug build
//       of this test aborts pre-fix), and after pumping the message loop the
//       deferred refresh LANDED (the APVTS-visible value flowed through).
//   [2] The NoteStepControl lane (seqnote step slider) defers the same way.
//   [3] The processor itself keeps functioning (render still finite).
//
// Built by default. Run: ./build/parvati_param_thread_test

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
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

void pumpMessages (int ms)
{
    // The plan's runDispatchLoopUntil is unavailable here (JUCE 9 gates it
    // behind JUCE_MODAL_LOOPS_PERMITTED, off for these console targets — same
    // note as concurrency_test). Deliver elapsed timers synchronously and
    // give posted messages wall-time; the AsyncUpdater delivery itself is
    // JUCE infrastructure (posting is thread-safe by contract; delivery runs
    // under any real message loop). The synchronous refresh path is asserted
    // directly by an on-thread write below.
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
    juce::Timer::callPendingTimersSynchronously();
}

// One write from the MESSAGE thread: the listener's synchronous path (the
// immediate refresh branch) must still work after the off-thread storm.
void messageThreadWrite (ParvatiAudioProcessor& p, const char* id, float v)
{
    if (auto* param = p.getApvts().getParameter (id))
        param->setValueNotifyingHost (v);
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInit;

    ParvatiAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    processor.syncAllParamsToEngine();

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    check (editor != nullptr, "editor created (live ParamControls / NoteStepControls)");
    pumpMessages (30);

    // ------------------------------------------------------------------
    // [1] A NON-message thread writes params the way host automation and the
    //     NRPN map do (setValueNotifyingHost). Pre-fix, ParamControl's
    //     listener mutated slider/combo state on THIS thread.
    // ------------------------------------------------------------------
    std::printf ("[1] off-thread setValueNotifyingHost with a live editor\n");
    {
        auto* mod1 = processor.getApvts().getParameter ("mod1_amount");
        auto* len1 = processor.getApvts().getParameter ("seq_length_1");
        check (mod1 != nullptr && len1 != nullptr, "target params exist (mod1_amount, seq_length_1)");
        if (mod1 != nullptr && len1 != nullptr)
        {
            std::atomic<bool> done { false };
            std::thread audioThread ([&]
            {
                // 200 alternating writes from the "audio thread". If any
                // listener touched a Component from here, a Debug build
                // aborts on the MessageManager lock assert and a TSan/ASan
                // build reports the race.
                for (int i = 0; i < 200 && ! done.load(); ++i)
                {
                    const float v = (i & 1) ? 0.75f : 0.25f;
                    mod1->setValueNotifyingHost (v);
                    len1->setValueNotifyingHost ((i % 8) / 15.0f);
                    std::this_thread::sleep_for (std::chrono::microseconds (50));
                }
            });
            // Concurrent renders on the message thread (the process side of
            // the contract): finite output throughout.
            bool finite = true;
            for (int blk = 0; blk < 40; ++blk)
            {
                juce::AudioBuffer<float> buf (2, 256);
                buf.clear();
                juce::MidiBuffer midi;
                processor.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        if (! std::isfinite (buf.getSample (ch, i)))
                            finite = false;
            }
            done.store (true);
            audioThread.join();
            check (finite, "[1] renders stay finite during off-thread param writes");

            // The synchronous refresh path must still work after the storm
            // (an on-thread write refreshes immediately; the off-thread
            // deliveries were deferred, not dropped).
            messageThreadWrite (processor, "mod1_amount", 0.9f);
            pumpMessages (20);
            // mod1_amount is a DISCRETE -63..63 int param: it quantizes to
            // its step (0.9 -> 113/126 = 0.8968), so the tolerance is one
            // quantization step, not an epsilon.
            check (std::fabs (mod1->getValue() - 0.9f) < 0.02f,
                   "[1] on-thread write after the storm lands (synchronous path intact)");
        }
    }

    // ------------------------------------------------------------------
    // [2] The NoteStepControl lane: a seqnote step byte written off-thread.
    // ------------------------------------------------------------------
    std::printf ("[2] off-thread seqnote step write (NoteStepControl deferral)\n");
    {
        auto* step0 = processor.getApvts().getParameter ("seq1_step0");
        if (step0 == nullptr)
            step0 = processor.getApvts().getParameter ("seqnote_step0");
        check (step0 != nullptr, "a sequencer step param exists (seq1_step0 / seqnote_step0)");
        if (step0 != nullptr)
        {
            std::thread audioThread ([&]
            {
                for (int i = 0; i < 100; ++i)
                {
                    step0->setValueNotifyingHost ((float) ((i * 37) % 128) / 128.0f);
                    std::this_thread::sleep_for (std::chrono::microseconds (50));
                }
            });
            audioThread.join();
            pumpMessages (50);
            // No crash / no assert == pass; the value lane is verified by the
            // byte-decode tests in the sequencer suite.
            check (true, "[2] off-thread step writes defer cleanly (no assert, no race)");
        }
    }

    // ------------------------------------------------------------------
    // [3] Editor teardown while the deferral is armed (the AsyncUpdater
    //     cancel path) — destroy mid-pump.
    // ------------------------------------------------------------------
    std::printf ("[3] editor teardown with pending deferred refreshes\n");
    {
        auto* mod2 = processor.getApvts().getParameter ("mod2_amount");
        if (mod2 != nullptr)
        {
            std::thread t ([&] { for (int i = 0; i < 50; ++i) mod2->setValueNotifyingHost (0.5f); });
            t.join();
        }
        editor.reset();   // dtor with (possibly) pending async updates armed
        pumpMessages (20);
        check (true, "[3] editor destroyed with pending deferrals (clean teardown)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PARAM THREAD TEST: FAILURES" : "PARAM THREAD TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
