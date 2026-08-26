// mono_retrigger_continuity_test — fast note changes in MONO must not punch
// the voice to silence, even with a slow nominal amp attack.
//
// The 2026-08-22 fix this pins: a mono retrigger while the previous note's
// RELEASE TAIL still sounds used to route through juce::Synthesiser::
// startVoice, whose pre-emptive stopNote(0,false) -> Voice::Kill ZEROED the
// envelope — each fast note change dropped the tail to ~0 and re-attacked
// from silence (audible chop on fast mono lines). The firmware re-Triggers
// the SAME voicecard: Envelope::Trigger(ATTACK) seeds its start a_ from the
// CURRENT value, so the attack rises from the release level, continuously.
//
// PRIMARY ASSERTION (envelope state, robust to waveform interference): after
// the retrigger's first rendered block, env1's 8-bit value must still be at
// least the pre-retrigger tail value (continuing attack). A kill drops it to
// ~env_expo[inc] (≈12% after one block) — far below the tail level.
//
// Secondary (audio-level): the output never collapses for long (post-100 ms
// peak >= 25% of pre-peak) and a boundary single-sample step stays bounded
// (triangle osc: edge-free, natural step ~4*A*f/sr ≈ 0.04 — a FIFO-clear /
// gain-snap click is ~10x that).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "test_utils.h"
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
constexpr double kSr  = 44100.0;
constexpr int    kBuf = 256;

void render (HellcatAudioProcessor& p, juce::MidiBuffer& midi, std::vector<float>& cap)
{
    juce::AudioBuffer<float> b (2, kBuf);
    b.clear();
    p.processBlock (b, midi);
    cap.reserve (cap.size() + (size_t) kBuf);
    for (int i = 0; i < kBuf; ++i)
        cap.push_back (b.getSample (0, i));
}
} // namespace

TEST(mono_retrigger_continuity_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor proc;
    proc.prepareToPlay (kSr, kBuf);
    proc.syncAllParamsToEngine();

    // MONO (the part's whole voice set retriggers as a unison — the factory
    // default allocation; the continuity invariant is per-voice and identical
    // for every voice in the set). TRIANGLE osc1: edge-free, so the boundary
    // step bound is meaningful (a saw's edge is itself a full-amplitude
    // single-sample step). Osc2 SILENT: the firmware re-Triggers osc_2 (phase
    // Reset) on every non-legato retrigger — a real Ambika blip, not ours to
    // fix — so it is muted to observe only the plumbing. Slow attack/release
    // (the reported config class: 495 ms-class attack).
    setChoice (proc, "part_polyphony", 0);   // MONO
    setChoice (proc, "osc1_shape", 3);       // triangle
    setChoice (proc, "osc2_shape", 0);       // none
    setInt (proc, "env1_attack", 60);        // ~630 ms nominal
    setInt (proc, "env1_release", 80);       // ~1.8 s tail
    setInt (proc, "env1_sustain", 110);
    setInt (proc, "filter1_cutoff", 127);    // filter wide open (not under test)
    proc.syncAllParamsToEngine();

    // Flush the deferred allocation/mode change.
    {
        juce::AudioBuffer<float> b (2, kBuf);
        b.clear();
        juce::MidiBuffer m;
        proc.processBlock (b, m);
    }

    SynthEngine& engine = proc.getEngine();
    AmbikaVoice* av = nullptr;
    for (int vi : engine.getPart (0).voiceIndices)
        if (auto* v = engine.getAmbikaVoice (vi)) { av = v; break; }
    CHECK(av != nullptr, "part 0 has a voice to observe");

    // Note ON C4; hold ~1.2 s (clearly audible), then OFF; hold ~80 ms so the
    // release tail is mid-decay and clearly sounding.
    std::vector<float> cap;
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
        render (proc, m, cap);
    }
    for (int blk = 0; blk < (int) (1.2 * kSr / kBuf); ++blk)
    { juce::MidiBuffer m; render (proc, m, cap); }
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOff (1, 60, (uint8_t) 0), 0);
        render (proc, m, cap);
    }
    for (int blk = 0; blk < (int) (0.08 * kSr / kBuf); ++blk)
    { juce::MidiBuffer m; render (proc, m, cap); }

    const uint8_t v0 = av->envelopeValueByte (0);   // amp env during the tail
    const size_t retrigAt = cap.size();

    // RETRIGGER: new note E4 while the C4 tail still sounds (the bug case).
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 64, (uint8_t) 110), 0);
        render (proc, m, cap);   // one block: several internal renders
    }
    const uint8_t v1 = av->envelopeValueByte (0);

    // ...and let the new note develop (~0.7 s).
    for (int blk = 0; blk < (int) (0.7 * kSr / kBuf); ++blk)
    { juce::MidiBuffer m; render (proc, m, cap); }

    // ---- Analysis -------------------------------------------------------
    auto peakOf = [&cap] (size_t from, size_t to)
    {
        float pk = 0.0f;
        for (size_t i = from; i < to && i < cap.size(); ++i)
            pk = std::max (pk, std::fabs (cap[i]));
        return pk;
    };
    const float prePeak = peakOf (retrigAt - (size_t) (0.04 * kSr), retrigAt);
    const float postPk  = peakOf (retrigAt, retrigAt + (size_t) (0.10 * kSr));
    float maxStep = 0.0f;
    for (size_t i = retrigAt + 1; i < retrigAt + (size_t) (0.10 * kSr) && i < cap.size(); ++i)
        maxStep = std::max (maxStep, std::fabs (cap[i] - cap[i - 1]));

    std::printf ("env tail=%d post-retrigger=%d | pre-peak %.4f post-100ms %.4f | max step %.4f\n",
                 (int) v0, (int) v1, prePeak, postPk, maxStep);

    CHECK(v0 > 20, "release tail is mid-decay when the retrigger lands (setup sanity)");
    CHECK(v1 >= v0 - 4,
          "envelope CONTINUES from the tail value at the retrigger (no Kill-to-zero)");
    CHECK(postPk >= 0.25f * std::max (prePeak, 0.05f),
          "audio continuity: no collapse after the retrigger");
    CHECK(maxStep <= 0.20f * std::max (prePeak, 0.05f) + 0.05f,
          "no plumbing click at the boundary (triangle osc; osc2's firmware phase-reset blip is muted)");
    return true;
}
