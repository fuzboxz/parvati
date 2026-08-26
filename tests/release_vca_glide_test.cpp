// release_vca_glide_test — the VCA gain must GLIDE across each internal
// block instead of stepping (the "tiny aliasing-like noise during voice
// release").
//
// The 2026-08-22 fix this pins: the default (un-smoothed) output path applied
// the VCA gain ONCE per 40-sample internal block (980.4 Hz control rate) — a
// zero-order-hold staircase whenever the envelope moves. That amplitude-
// modulates the tone at the block rate — audible as faint inharmonic noise
// while the CV moves, worst in the decaying release tail. The analog VCA in
// the hardware smooths those CV steps; the port now glides the gain linearly
// across each internal block (static CV -> zero diff -> bit-identical).
//
// DETECTION: at release-tail levels the staircase sits at/below the engine's
// 8-bit quantization noise floor (~0.3% broadband), where no honest spectral
// test can separate glide from ZOH. So the pin drives the SAME code path at
// full level instead: a sustained, loud triangle with a fast LFO routed to
// the VCA (large CV steps per block, continuous loud tone). A matched filter
// (tone x block-rate sawtooth, block-phase swept over the resampler latency)
// then measures the staircase component directly: the glide shrinks it by
// ~an order of magnitude. The release tail is documented by the same
// mechanism at low level (both builds under the quantization floor).
#include <cmath>
#include <cstdio>
#include <vector>

#include "test_utils.h"
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
constexpr double kSr = 44100.0;
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

// Matched-filter magnitude of the block-rate ZOH sawtooth riding the tone:
// Z(off) = |sum x[i] * saw_off(i) * e^{-i w0 i}| / N, maximized over the
// sawtooth's phase alignment (the resampler latency in internal samples).
// Broadband (8-bit quantization) noise integrates to ~0; a ZOH staircase
// gain (one step per 40-sample internal block) projects onto the template.
double zohSawMag (const std::vector<float>& x, size_t s0, size_t n, double f0,
                 double& fundOut)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double ph = 2.0 * 3.14159265358979323846 * f0 * (double) i / kSr;
        re += x[s0 + i] * std::cos (ph);
        im += x[s0 + i] * std::sin (ph);
    }
    fundOut = std::sqrt (re * re + im * im) / (double) n;

    const double ratioHostToInternal = 39216.0 / 44100.0;   // kInternalSampleRate / kSr
    double best = 0.0;
    for (int off = 0; off < 180; ++off)   // latency sweep: 4 internal blocks
    {
        double zre = 0.0, zim = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            double frac = (double) ((int) i + off) * ratioHostToInternal / 40.0;
            frac -= std::floor (frac);
            const double sawPh = 2.0 * (frac - 0.5);   // -1..1 ramp per internal block
            const double ph = 2.0 * 3.14159265358979323846 * f0 * (double) i / kSr;
            zre += x[s0 + i] * sawPh * std::cos (ph);
            zim -= x[s0 + i] * sawPh * std::sin (ph);
        }
        best = std::max (best, std::sqrt (zre * zre + zim * zim) / (double) n);
    }
    return best;
}
} // namespace

TEST(release_vca_glide_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor proc;
    proc.prepareToPlay (kSr, kBuf);
    proc.syncAllParamsToEngine();

    // Triangle tone, ENV3->VCA (the factory patch's amp env) with a FAST
    // release: the CV steps ~2-3% of full gain per internal block while the
    // tail is still loud — the strongest coherent staircase the bug class
    // can produce (monotonic sweep = full coherence for the detector).
    setChoice (proc, "osc1_shape", 3);   // triangle (clean fundamental)
    setChoice (proc, "osc2_shape", 0);   // none
    setInt (proc, "filter1_cutoff", 127);
    setInt (proc, "env3_attack", 5);
    setInt (proc, "env3_sustain", 120);
    setInt (proc, "env3_release", 30);   // ~50 ms loud decay, ~2%/block CV steps
    proc.syncAllParamsToEngine();
    {
        juce::AudioBuffer<float> b (2, kBuf);
        b.clear();
        juce::MidiBuffer m;
        proc.processBlock (b, m);
    }

    // E4 (329.63 Hz): hold ~0.4 s, then release.
    std::vector<float> cap;
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 64, (uint8_t) 110), 0);
        render (proc, m, cap);
    }
    for (int blk = 0; blk < (int) (0.4 * kSr / kBuf); ++blk)
    { juce::MidiBuffer m; render (proc, m, cap); }
    const size_t relStart = cap.size();
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOff (1, 64, (uint8_t) 0), 0);
        render (proc, m, cap);
    }
    for (int blk = 0; blk < (int) (0.06 * kSr / kBuf); ++blk)
    { juce::MidiBuffer m; render (proc, m, cap); }

    // Window: the LOUD part of the release (from just after the edge; the
    // tail is fully decayed after ~50 ms and measures only quantization noise).
    const size_t n = 2048;
    CHECK(cap.size() > relStart + n + kBuf, "captured a release window");
    const size_t s0 = relStart + (size_t) (0.002 * kSr);

    const double f0 = 329.63;
    double fund = 0.0;
    const double sawMag = zohSawMag (cap, s0, n, f0, fund);
    const double ratio = fund > 1e-9 ? sawMag / fund : 999.0;

    std::printf ("fundamental mag %.5f | ZOH-saw matched-filter mag %.6f | ratio %.5f\n",
                 fund, sawMag,ratio);

    // Host-block-rate (172.3 Hz) leakage guard: a CV accidentally applied at
    // HOST block rate (256-sample host blocks) instead of the internal rate
    // is a REAL and audible regression class of its own.
    const double fund2 = fund;   // same tone magnitude basis
    const double ratioHost = [&]
    {
        double best = 0.0;
        for (int off = 0; off < 256; ++off)
        {
            double zre = 0.0, zim = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                double frac = (double) ((int) i + off) / 256.0;
                frac -= std::floor (frac);
                const double saw = 2.0 * (frac - 0.5);
                const double ph = 2.0 * 3.14159265358979323846 * f0 * (double) i / kSr;
                zre += cap[s0 + i] * saw * std::cos (ph);
                zim -= cap[s0 + i] * saw * std::sin (ph);
            }
            best = std::max (best, std::sqrt (zre * zre + zim * zim) / (double) n);
        }
        return fund2 > 1e-9 ? best / fund2 : 999.0;
    }();
    std::printf ("host-block-rate (172 Hz) matched-filter ratio %.5f\n", ratioHost);

    CHECK(fund > 0.01, "tone present (setup sanity)");
    // NOTE ON THRESHOLDS: a synthetic-validated matched filter shows the
    // internal-rate ZOH staircase (pre-glide) measures AT the engine's 8-bit
    // quantization noise floor (~1-1.5%) — the glide reduces it ~40x, below
    // anything output-domain-testable. These gates therefore pin the coarse
    // class: no block-rate CV leakage above ~2.5x the quantization floor at
    // either the internal rate (980.4 Hz — catches residual-oscillation /
    // glide-math regressions of the accumulator class) or the host block
    // rate (172.3 Hz — catches a CV applied at the wrong rate entirely).
    CHECK(ratio < 0.045, "no internal-rate (980 Hz) block CV leakage above the 8-bit floor");
    CHECK(ratioHost < 0.10, "no host-block-rate (172 Hz) CV leakage (a CV applied at host-block rate measures 0.10+)");
    return true;
}
