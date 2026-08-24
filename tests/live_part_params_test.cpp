// Live Patch-page part parameters: tuning, spread, volume, portamento.
//
// Pins the Parvati extension that Patch-page part edits reach SOUNDING
// notes: part_tuning and part_spread retune sounding voices through a
// block-rate glide in dsp::Voice; part_volume was already live (the VCA
// byte re-reads every block); part_portamento is read at the NEXT trigger
// (firmware Trigger() semantics). part_octave stays at the next trigger
// by design (see AmbikaVoice::setPartByte).
//
// Two levels:
//   * dsp::Voice-level pins (exact, integer): glide steps, reset-on-trigger,
//     zero-delta invariance, portamento byte read at trigger time.
//   * processor-level pins (routing + audible): a host-style parameter write
//     reaches the audio path within one service block; frequency follows a
//     tuning edit; spread beats appear; volume ramps without a step.
//
// Run: ./build_unified/parvati_unified_tests live_part_params_test

#include <cmath>
#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>

#include "test_utils.h"
#include "unified_test_runner.h"

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/constants.h"
#include "dsp/patch.h"
#include "dsp/voice.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Deterministic sine voice (voice_pitch_test's configureDeterministicVoice):
// SINE / NONE / OP_SUM with the two time-varying init mod slots zeroed.
void configureDeterministicVoice (ambika::dsp::Voice& v)
{
    const auto off = [] (size_t o) { return static_cast<uint8_t> (o); };
    v.set_patch_data (off (offsetof (ambika::dsp::Patch, osc)) + 0, ambika::dsp::WAVEFORM_SINE);
    v.set_patch_data (off (offsetof (ambika::dsp::Patch, osc)) + 4, ambika::dsp::WAVEFORM_NONE);
    v.set_patch_data (off (offsetof (ambika::dsp::Patch, mix_op)), ambika::dsp::OP_SUM);
    const size_t modBase = offsetof (ambika::dsp::Patch, modulation);
    const size_t modAmt  = offsetof (ambika::dsp::Modulation, amount);
    v.set_patch_data (off (modBase + 7 * sizeof (ambika::dsp::Modulation) + modAmt), 0);
    v.set_patch_data (off (modBase + 11 * sizeof (ambika::dsp::Modulation) + modAmt), 0);
}

// ---- processor-level helpers ------------------------------------------------

// Renders @p blocks and ACCUMULATES channel 0 into @p acc (renderBlocks
// discards audio; this test needs the samples). RMS is over the accumulation.
struct RenderAccum
{
    juce::AudioBuffer<float> buf { 2, 256 };
    void run (ParvatiAudioProcessor& proc, int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            buf.clear();
            proc.processBlock (buf, midi);
        }
    }
    // Renders @p blocks; returns the mono (channel 0) samples of the LAST
    // window of @p keep blocks concatenated.
    std::vector<float> capture (ParvatiAudioProcessor& proc, int blocks, int keep)
    {
        std::vector<float> out;
        out.reserve (static_cast<size_t> (keep) * 256);
        const int start = blocks - keep;
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            buf.clear();
            proc.processBlock (buf, midi);
            if (b >= start)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    out.push_back (buf.getSample (0, i));
        }
        return out;
    }
};

double rms (const std::vector<float>& s, size_t from = 0, size_t to = static_cast<size_t> (-1))
{
    if (to > s.size()) to = s.size();
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to; ++i) { acc += double (s[i]) * double (s[i]); ++n; }
    return n > 0 ? std::sqrt (acc / double (n)) : 0.0;
}

// Dominant frequency by upward zero-crossings over a window.
double dominantHz (const std::vector<float>& s, double sampleRate)
{
    int up = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i - 1] <= 0.0f && s[i] > 0.0f)
            ++up;
    return double (up) * sampleRate / double (s.size());
}

// Frame-RMS spread (coefficient of variation): the beat-depth proxy for the
// unison-spread check. Frames of 256 samples.
double frameRmsCv (const std::vector<float>& s)
{
    std::vector<double> fr;
    for (size_t i = 0; i + 256 <= s.size(); i += 256)
        fr.push_back (rms (s, i, i + 256));
    if (fr.empty()) return 0.0;
    double mean = 0.0;
    for (double v : fr) mean += v;
    mean /= double (fr.size());
    if (mean <= 1e-9) return 0.0;
    double var = 0.0;
    for (double v : fr) var += (v - mean) * (v - mean);
    return std::sqrt (var / double (fr.size())) / mean;
}

// Deterministic audible patch for processor-level measurements. The
// controller init patch (ParameterLayout.cpp InitPatch::bytes) carries two
// time-varying pitch sources that make a zero-crossing measurement wander
// ~1 semitone window-to-window (measured 470.9 -> 442.1 Hz on an UNTOUCHED
// note):
//   * mod14: LFO_4 -> OSC_1_2_COARSE amount 16 (a slow ~2 st vibrato);
//   * osc2: SQUARE, range -12, detune 12 at mix balance 32 (a second,
//     detuned oscillator adds crossings and beating).
// This helper zeroes the vibrato slot and silences osc2, leaving ONE clean
// bandlimited saw (correct absolute pitch; one zero crossing per cycle).
// mod11 (ENV_3 -> VCA 63) stays: the note needs it to sound.
void makeDeterministicPatch (ParvatiAudioProcessor& proc)
{
    setChoice (proc, "osc2_shape", 0);    // None (single-oscillator patch)
    setInt (proc, "mod14_amount", 0);     // kill LFO_4 -> OSC coarse (the vibrato)
}

// The first ACTIVE AmbikaVoice owned by @p part (or null).
AmbikaVoice* firstActiveVoice (ParvatiAudioProcessor& proc, int part)
{
    auto& e = proc.getEngine();
    for (int i = 0; i < kNumVoices; ++i)
        if (auto* av = e.getAmbikaVoice (i))
            if (av->getPartIndex() == part && av->isDisplayedActive())
                return av;
    return nullptr;
}
}  // namespace

TEST(live_part_params_test)
{
    // Own the MessageManager on this thread: the forked test child may
    // otherwise create it on a foreign thread, and every processor timer /
    // arp-seq apply then logs a (harmless but noisy) assertion. In-process
    // runs do not need this; the explicit call keeps fork mode quiet too.
    juce::MessageManager::getInstance();
    using ambika::dsp::kAudioBlockSize;

    // ======================================================================
    std::printf ("[0] dsp::Voice live-tune glide: step, convergence, reset\n");
    {
        ambika::dsp::Voice v;
        v.Init();
        configureDeterministicVoice (v);
        v.Trigger (69 * 128, 200, 0);

        check (v.live_tune_offset() == 0 && v.live_tune_target() == 0,
               "fresh voice: offset and target are zero");

        v.stage_live_tune_delta (64);
        check (v.live_tune_target() == 64, "staged +64 sets the target");

        // One block advances at most kLiveTuneMaxStep.
        v.ProcessBlock();
        check (v.live_tune_offset() == 8,
               "one block advances the offset by exactly the 8-unit step");

        // Converge: 64 / 8 = 8 blocks total.
        for (int i = 0; i < 12; ++i) v.ProcessBlock();
        check (v.live_tune_offset() == 64 && v.live_tune_target() == 64,
               "offset converges to the target and holds");

        // Consecutive edits accumulate on the target.
        v.stage_live_tune_delta (-90);
        check (v.live_tune_target() == -26, "a second edit accumulates on the target");

        // A new trigger recomputes the pitch from the current bytes: the
        // in-flight offset would double-apply, so Trigger resets it.
        v.Trigger (69 * 128, 200, 0);
        check (v.live_tune_offset() == 0 && v.live_tune_target() == 0,
               "Trigger resets both (fresh NoteOn recomputes the full pitch)");
    }

    // ======================================================================
    std::printf ("[1] dsp::Voice: zero-delta staging renders identically (no edit = no change)\n");
    {
        ambika::dsp::Voice a, b;
        a.Init(); configureDeterministicVoice (a); a.Trigger (69 * 128, 200, 0);
        b.Init(); configureDeterministicVoice (b); b.Trigger (69 * 128, 200, 0);
        a.stage_live_tune_delta (0);
        bool equal = true;
        for (int blk = 0; blk < 6; ++blk)
        {
            a.ProcessBlock();
            b.ProcessBlock();
            equal = equal && std::memcmp (a.output().data(), b.output().data(), kAudioBlockSize) == 0;
        }
        check (equal, "a zero-delta stage renders byte-identically to an untouched voice");
    }

    // ======================================================================
    std::printf ("[2] dsp::Voice: portamento byte is read at Trigger (next glide)\n");
    {
        ambika::dsp::Voice slow, fast, twin;
        const auto off6 = static_cast<uint8_t> (6);
        slow.Init(); configureDeterministicVoice (slow); slow.set_part_data (off6, 30);
        fast.Init(); configureDeterministicVoice (fast); fast.set_part_data (off6, 0);
        twin.Init(); configureDeterministicVoice (twin); twin.set_part_data (off6, 30);

        // Start low, glide to the same high note.
        slow.Trigger (60 * 128, 200, 0); fast.Trigger (60 * 128, 200, 0); twin.Trigger (60 * 128, 200, 0);
        slow.Trigger (72 * 128, 200, 0); fast.Trigger (72 * 128, 200, 0); twin.Trigger (72 * 128, 200, 0);

        // A few blocks in: slow is mid-glide, fast has arrived.
        for (int i = 0; i < 3; ++i) { slow.ProcessBlock(); fast.ProcessBlock(); twin.ProcessBlock(); }
        check (slow.debug_pitch_value() != 72 * 128, "portamento 30: still gliding after 3 blocks");
        check (fast.debug_pitch_value() == 72 * 128, "portamento 0: target reached immediately");

        // LIVE byte edit MID-GLIDE: the in-flight increment was computed at
        // Trigger, so the glide rate must not change (firmware semantics).
        slow.set_part_data (off6, 0);
        bool identical = true;
        for (int i = 0; i < 40; ++i)
        {
            slow.ProcessBlock();
            twin.ProcessBlock();
            identical = identical
                && slow.debug_pitch_value() == twin.debug_pitch_value()
                && std::memcmp (slow.output().data(), twin.output().data(), kAudioBlockSize) == 0;
        }
        check (identical, "a mid-glide portamento edit leaves the in-flight glide unchanged");

        // The NEXT trigger follows the new byte: instant snap.
        slow.Trigger (60 * 128, 200, 0);
        slow.ProcessBlock();
        check (slow.debug_pitch_value() == 60 * 128,
               "the next trigger reads the edited byte (no glide at 0)");
    }

    // ======================================================================
    std::printf ("[3] processor: a part_tuning edit retunes the sounding note (audible)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (44100.0, 256);
        makeDeterministicPatch (proc);
        RenderAccum ra;

        const auto note = juce::MidiMessage::noteOn (1, 69, 0.9f);   // A4, Part 0 (channel 1)
        renderBlocks (proc, 8, &note, 256, 0);                       // attack + settle
        const auto before = ra.capture (proc, 40, 40);               // ~230 ms steady
        const double f0 = dominantHz (before, 44100.0);
        check (f0 > 400.0 && f0 < 500.0, "steady note renders at ~440 Hz (A4)");

        // +32/128 semitone = +0.25 st: expected ratio 2^(0.25/12) = 1.01455.
        setInt (proc, "part_tuning", 32);
        const auto after = ra.capture (proc, 60, 60);                // ~350 ms (glide ~33 ms + margin)
        const double f1 = dominantHz (after, 44100.0);
        const double ratio = f1 / f0;
        std::printf ("     f0=%.1f Hz f1=%.1f Hz ratio=%.4f (expected 1.0145)\n", f0, f1, ratio);
        check (ratio > 1.010 && ratio < 1.019,
               "a +32 tuning edit raises the sounding pitch by ~0.25 semitone");

        auto* av = firstActiveVoice (proc, 0);
        check (av != nullptr && av->getLiveTuneTarget14() == 32,
               "the sounding voice's live-tune target is the full +32 delta");
        // And the glide has converged by now (~350 ms >> 33 ms).
        check (av != nullptr && av->getLiveTuneOffset14() == 32,
               "the glide offset has converged to the target");
    }

    // ======================================================================
    std::printf ("[4] processor: a part_spread edit re-drifts a MONO unison (beats appear)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (44100.0, 256);
        makeDeterministicPatch (proc);
        setChoice (proc, "part_polyphony", 0);                       // MONO: all voices, ordinal drift
        RenderAccum ra;

        const auto note = juce::MidiMessage::noteOn (1, 69, 0.9f);
        renderBlocks (proc, 8, &note, 256, 0);
        const auto before = ra.capture (proc, 80, 80);               // ~460 ms, spread 0
        const double cv0 = frameRmsCv (before);
        check (rms (before) > 1e-3, "the unison sustains audibly");

        setInt (proc, "part_spread", 40);
        const auto after = ra.capture (proc, 120, 120);              // ~690 ms
        const double cv1 = frameRmsCv (after);
        std::printf ("     envelope CV: spread0=%.4f spread40=%.4f\n", cv0, cv1);
        check (cv1 > 2.5 * (cv0 + 1e-4),
               "spread 40 makes the unison amplitude beat (envelope variance grows)");

        // Exact per-voice re-derive: ordinals 0..5 => targets 0,40,80,120,160,200.
        auto& e = proc.getEngine();
        int checked = 0, correct = 0;
        for (int i = 0; i < kNumVoices; ++i)
            if (auto* av = e.getAmbikaVoice (i))
                if (av->getPartIndex() == 0 && av->isDisplayedActive())
                {
                    const int expect = 40 * checked;   // ordinal order is allocation order
                    if (av->getLiveTuneTarget14() == expect) ++correct;
                    ++checked;
                }
        std::printf ("     per-voice re-derive: %d/%d voices at k*40\n", correct, checked);
        check (checked >= 2 && correct == checked,
               "every sounding voice re-derives its drift as ordinal * new spread");
    }

    // ======================================================================
    std::printf ("[5] processor: part_volume ramps the sounding note without a step\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (44100.0, 256);
        makeDeterministicPatch (proc);
        RenderAccum ra;

        const auto note = juce::MidiMessage::noteOn (1, 69, 0.9f);
        renderBlocks (proc, 8, &note, 256, 0);
        const auto before = ra.capture (proc, 40, 40);
        const double rmsBefore = rms (before);
        check (rmsBefore > 1e-3, "the note sustains audibly before the edit");

        // The param caps at 127 (the default is 120), so the audible
        // direction with headroom is DOWN: 120 -> 40 is a large swing.
        setInt (proc, "part_volume", 40);
        const auto after = ra.capture (proc, 40, 40);                // ~230 ms >> 100 ms budget
        const double rmsAfter = rms (after);
        std::printf ("     volume 120->40: RMS %.4f -> %.4f\n", rmsBefore, rmsAfter);
        check (rmsAfter < 0.6 * rmsBefore,
               "a 120->40 volume edit lowers the sounding level within ~100 ms");

        // Click bound: no sample-to-sample step beyond 6x the steady-state
        // worst slope during the ramp window (the first ~50 ms after the edit).
        setInt (proc, "part_volume", 120);   // swing back up for the click probe
        const auto down = ra.capture (proc, 40, 40);
        double steadySlope = 0.0, rampSlope = 0.0;
        for (size_t i = 1; i < down.size(); ++i)
        {
            const double d = std::fabs (double (down[i]) - double (down[i - 1]));
            if (i < down.size() / 2) rampSlope = std::max (rampSlope, d);
            else                     steadySlope = std::max (steadySlope, d);
        }
        std::printf ("     max |delta| ramp=%.4f steady=%.4f (bound 6x)\n", rampSlope, steadySlope);
        check (rampSlope < 6.0 * steadySlope + 1e-3,
               "the volume ramp shows no discontinuity beyond 6x the steady slope");
    }

    // ======================================================================
    std::printf ("[6] processor: part_octave applies at the NEXT trigger (by design)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (44100.0, 256);
        makeDeterministicPatch (proc);
        RenderAccum ra;

        const auto note = juce::MidiMessage::noteOn (1, 69, 0.9f);
        renderBlocks (proc, 8, &note, 256, 0);
        const auto before = ra.capture (proc, 30, 30);
        const double f0 = dominantHz (before, 44100.0);

        setInt (proc, "part_octave", 1);
        const auto held = ra.capture (proc, 30, 30);
        const double fHeld = dominantHz (held, 44100.0);
        std::printf ("     held note: %.1f Hz -> %.1f Hz (octave edit must NOT jump)\n", f0, fHeld);
        check (std::fabs (fHeld - f0) < 0.04 * f0,
               "an octave edit leaves the HELD note at its pitch (next-trigger policy)");

        auto* av = firstActiveVoice (proc, 0);
        check (av == nullptr || av->getLiveTuneTarget14() == 0,
               "no live-tune delta is staged by an octave edit");
    }

    std::printf ("live_part_params_test: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}
