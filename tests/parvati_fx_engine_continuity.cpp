// Full-engine continuity regression: renders a sustained note through the ACTUAL
// ParvatiAudioProcessor::processBlock (-> renderPartFx -> FxChain -> each Clouds
// FX's HostRateBridge) with each Clouds FX enabled, no modulation, and asserts
// the main-bus output is click-free.
//
// WHY this test exists (and why the isolated fx->process tests missed the bug):
// the audible Clouds-FX crackle was a 1-sample sub-chunk produced ONLY by
// renderPartFx's drift-corrected ~980 Hz sub-chunking. That 1-sample block hit
// HostRateBridge's m==0 branch, which ZEROED the output (a full-amplitude
// dropout). Isolated fx->process / chain.process tests never generate that
// 1-sample chunk, so they passed while the plugin clicked. This test drives the
// real path, so a regression of THIS bug (or any future renderPartFx/chain
// orchestration glitch) fails here.
//
// Detector: curvature-immune -- |delta[i]| that is both > 8x the 93rd-percentile
// |delta| of a trailing 64-sample window AND > 0.004 absolute. The threshold
// (0.06) sits in the gap between the modes' intrinsic grain (PitchShifter/WSOLA
// ~0.017-0.043, their crossfade/splice character) and the m==0 dropout bug
// (>=0.082). FAILS if the HostRateBridge m==0 hold fix is reverted.
//
// Build: linked as parvati_fx_engine_continuity_test (see CMakeLists).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}
void setChoice (ParvatiAudioProcessor& proc, const char* id, int index)
{
    if (auto* param = proc.getApvts().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (index)));
}

const char* fxName (int ti)
{
    switch (ti) { case 1: return "Diffuser"; case 2: return "PitchShifter"; case 3: return "Reverb";
        case 4: return "LoopingDelay"; case 5: return "WSOLA"; case 6: return "Spectral"; default: return "?"; }
}

// Returns the worst flagged impulse (max |delta| among curvature-immune outliers)
// over the steady-state region of the captured main-bus L. 0.0 if none flagged.
double worstImpulse (const std::vector<float>& out, int warmup, int total)
{
    std::vector<float> d (static_cast<size_t> (total), 0.0f);
    for (int i = warmup + 1; i < total; ++i)
        d[(size_t) i] = std::fabs (out[(size_t) i] - out[(size_t) i - 1]);
    double worst = 0.0;
    for (int i = warmup + 65; i < total - 1; ++i)
    {
        float window[64];
        for (int k = 0; k < 64; ++k) window[k] = d[(size_t) (i - 64 + k)];
        std::sort (window, window + 64);
        const float base = window[60];   // ~93rd percentile of the trailing window
        if (d[(size_t) i] > 8.0f * base && d[(size_t) i] > 0.004f)
            worst = std::max (worst, static_cast<double> (d[(size_t) i]));
    }
    return worst;
}

// Render a sustained note through the full processor with FX slot 1 = @p fxType.
// Returns the captured main-bus L.
std::vector<float> render (int fxType, double sr, int bufferSize, double durSec)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, bufferSize);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);   // SAW (audible source on Part 0)
    setChoice (proc, "fx1_type", fxType);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);                       // full wet
    for (int k = 1; k <= 4; ++k)
        setInt (proc, ("fx1_param" + std::to_string (k)).c_str(), 64);   // 0.5 (no modulation)

    // flush one block so the FX state + note-on settle
    { juce::AudioBuffer<float> f (2, bufferSize); f.clear(); juce::MidiBuffer e; proc.processBlock (f, e); }

    const int total = static_cast<int> (durSec * sr);
    std::vector<float> cap (static_cast<size_t> (total), 0.0f);
    bool noteOn = false;
    int written = 0;
    while (written < total)
    {
        juce::AudioBuffer<float> buf (2, bufferSize); buf.clear();
        juce::MidiBuffer midi;
        if (! noteOn) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0); noteOn = true; }
        proc.processBlock (buf, midi);
        const int n = std::min (bufferSize, total - written);
        const float* L = buf.getReadPointer (0);
        for (int i = 0; i < n; ++i) cap[(size_t) (written + i)] = L[i];
        written += n;
    }
    return cap;
}

// The continuity bound. See file header: sits above the modes' intrinsic grain
// (max ~0.043) and below the m==0 dropout bug (>=0.082).
constexpr double kClickBound = 0.06;

void runConfig (double sr, int bufferSize)
{
    const double durSec = 4.0;
    const int total = static_cast<int> (durSec * sr);
    const int warmup = static_cast<int> (0.5 * sr);   // skip note-attack + buffer-fill transients
    std::printf ("-- @%.1fk, host buffer %d --\n", sr / 1000.0, bufferSize);
    for (int t : { 1, 2, 3, 4, 5, 6 })
    {
        const auto cap = render (t, sr, bufferSize, durSec);
        const double worst = worstImpulse (cap, warmup, total);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%-12s @%.0fk buf=%d: worst impulse=%.4f (must be < %.3f)",
                       fxName (t), sr / 1000.0, bufferSize, worst, kClickBound);
        check (worst < kClickBound, msg);
    }
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== Full-engine Clouds-FX continuity regression ===\n");
    std::printf ("Renders a sustained note through processBlock with each Clouds FX (no mod);\n");
    std::printf ("asserts the main bus is click-free (worst impulse < %.3f).\n\n", kClickBound);

    // 48k at two host buffer sizes (the drift-cadence / 1-sample-sub-chunk rate
    // depends on the buffer size) + 44.1k (the other common rate).
    runConfig (48000.0, 128);
    runConfig (48000.0, 256);
    runConfig (44100.0, 256);

    std::printf ("\n%s\n", g_failures == 0
        ? "Full-engine Clouds-FX continuity PASSED."
        : "Full-engine Clouds-FX continuity FAILED.");
    return g_failures == 0 ? 0 : 1;
}
