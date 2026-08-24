// FX BIT-EXACTNESS GOLDEN PINS (full-render audio bytes).
//
// Purpose: any future refactor that changes ONE SAMPLE of a full FX render
// turns this test red. Scope: the two FX families that reach the host
// through a rate bridge. (1) The Clouds family runs internally at 32000 Hz
// through HostRateBridge (Diffuser pins it). (2) The FV-1 family runs at
// 32768 Hz (LutDistortion and Echo pin it).
//
// Four scenarios cover bridged and native rates:
//   A  Diffuser    @ 48000  Hz  — Clouds path, host rate differs from 32000.
//   B  Diffuser    @ 32000  Hz  — Clouds path, ratio 1.0, resampler idle.
//   C  LutDistortion @ 44100 Hz — FV-1 path (the historical drift case of
//                                 the aborted rate-bridge unification).
//   D  Echo        @ 32768  Hz  — FV-1 path at its native rate.
//
// Each scenario renders a fixed chord (four notes, fixed velocity) through
// a fresh processor, block size 256, for three seconds. The digest covers
// both channels, interleaved bytes excluded (L then R, float bits). Anchor
// samples give a quick size-of-error read when a pin breaks.
//
// Determinism first: every scenario renders twice and the two runs must be
// byte-identical BEFORE the golden compare. A canary check proves the
// digest changes when one sample shifts by one ulp. An energy guard stops
// a silent render from ever being pinned.
//
// The goldens are pinned to the Debug build of build_unified on arm64
// macOS (CLAUDE.md build policy). A Release or sanitizer build may change
// code generation and therefore float results; Release pinning stays open.
// To update a digest after an APPROVED change: run this test, copy the
// printed digest and anchors from the failure line, and paste them here.
//
// Unified runner. Run with:
//   ./build_unified/parvati_unified_tests fx_render_golden_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_utils.h"   // setInt/setChoice host-style writes
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"

namespace
{

//==============================================================================
// Scenario table. Digests pin the bytes of build_unified Debug.
struct Scenario
{
    const char* label;
    double sampleRate;
    int fxType;      // FxType enum value as stored by fx1_type
    juce::String goldenDigest;
};

const std::vector<Scenario>& scenarios()
{
    // Goldens generated from the build_unified Debug binary (arm64 macOS,
    // 2026-08-23). Update them only after an approved DSP change.
    static const std::vector<Scenario> s = {
        { "clouds-bridge-48000",   48000.0, 1,  "f6d961053c9444543df30dae735d847f" },
        { "clouds-identity-32000", 32000.0, 1,  "b5c44857f68bc34d7f29b4017cdcdb62" },
        { "fv1-bridge-44100",      44100.0, 17, "f28284dba5acf9e9f4474219a8a2b3b9" },
        { "fv1-native-32768",      32768.0, 22, "c4b1b3801d2e725c2cc5c5261e0d9d35" },
    };
    return s;
}

//==============================================================================
// Fixed deterministic render: fresh processor, one chord at block 0.
struct Capture
{
    std::vector<float> l, r;
};

Capture renderScenario (double sr, int fxType)
{
    const int total = (int) std::llround (3.0 * sr);

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, 256);

    setChoice (proc, "fx1_type", fxType);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);
    setInt (proc, "osc1_shape", 1);
    for (int k = 1; k <= 5; ++k)
        setInt (proc, ("fx1_param" + std::to_string (k)).c_str(), 64);

    Capture cap;
    cap.l.assign ((size_t) total, 0.0f);
    cap.r.assign ((size_t) total, 0.0f);

    bool noteOn = false;
    for (int written = 0; written < total; )
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        if (! noteOn)
        {
            for (int c = 0; c < 4; ++c)
                midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c),
                                                          (uint8_t) 110), 0);
            noteOn = true;
        }
        proc.processBlock (buf, midi);

        const int n = std::min (256, total - written);
        for (int i = 0; i < n; ++i)
        {
            cap.l[(size_t) (written + i)] = buf.getSample (0, i);
            cap.r[(size_t) (written + i)] = buf.getSample (1, i);
        }
        written += n;
    }
    return cap;
}

//==============================================================================
// Digest over both channels (L bytes then R bytes). Two independent FNV-1a
// 64-bit passes (different offset bases) give 128 bits of accident
// detection without a crypto dependency: juce_cryptography is not in the
// module set, and a test must not widen it. The canary check below proves
// the pair reacts to a one-ulp sample change.
uint64_t fnv1a64 (const void* data, size_t bytes, uint64_t basis)
{
    const auto* p = static_cast<const uint8_t*> (data);
    uint64_t h = basis;
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= (uint64_t) p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

juce::String digestOf (const Capture& cap)
{
    const size_t bytesL = cap.l.size() * sizeof (float);
    const size_t bytesR = cap.r.size() * sizeof (float);
    const uint64_t a = fnv1a64 (cap.l.data(), bytesL, 0xcbf29ce484222325ULL);
    const uint64_t b = fnv1a64 (cap.r.data(), bytesR, 0x9e3779b97f4a7c15ULL);
    char hex[33];
    std::snprintf (hex, sizeof (hex), "%016llx%016llx",
                   (unsigned long long) a, (unsigned long long) b);
    return juce::String (hex);
}

// Byte comparison with the first differing sample index per channel. The
// test pins BITS, not values: a NaN or a denormal must survive a refactor
// unchanged. IEEE-754 float32 has no padding bits, so memcmp is exact.
struct CmpResult
{
    bool equal = true;
    long firstDiffL = -1;
    long firstDiffR = -1;
};

CmpResult compareCaptures (const Capture& a, const Capture& b)
{
    CmpResult r;
    const size_t n = a.l.size();
    if (b.l.size() != n || b.r.size() != a.r.size())
    {
        r.equal = false;
        return r;
    }
    for (size_t i = 0; i < n; ++i)
        if (std::memcmp (&a.l[i], &b.l[i], sizeof (float)) != 0)
        {
            r.equal = false;
            r.firstDiffL = (long) i;
            break;
        }
    for (size_t i = 0; i < a.r.size(); ++i)
        if (std::memcmp (&a.r[i], &b.r[i], sizeof (float)) != 0)
        {
            r.equal = false;
            r.firstDiffR = (long) i;
            break;
        }
    return r;
}

// RMS of a channel window (energy guard input).
double channelRms (const std::vector<float>& d, size_t from, size_t to)
{
    double s = 0.0;
    const size_t n = std::min (to, d.size());
    for (size_t i = from; i < n; ++i)
        s += (double) d[i] * (double) d[i];
    return n > from ? std::sqrt (s / (double) (n - from)) : 0.0;
}

} // namespace


//==============================================================================
TEST(fx_render_golden_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    // The canary proves the digest reacts to a one-ulp change. A digest that
    // ignores a sample change pins nothing.
    {
        Capture probe;
        probe.l.assign (4096, 0.125f);
        probe.r.assign (4096, -0.25f);
        const juce::String d0 = digestOf (probe);
        uint32_t bits = 0;
        std::memcpy (&bits, &probe.l[1000], sizeof (bits));
        bits ^= 1u;   // one ulp
        std::memcpy (&probe.l[1000], &bits, sizeof (bits));
        const juce::String d1 = digestOf (probe);
        CHECK (d0 != d1, "canary: one-ulp sample change alters the digest");
    }

    for (const Scenario& sc : scenarios())
    {
        std::printf ("  scenario %s (sr=%.0f, fx=%d)\n",
                     sc.label, sc.sampleRate, sc.fxType);

        // Determinism first: two fresh renders must agree byte for byte.
        const Capture run1 = renderScenario (sc.sampleRate, sc.fxType);
        const Capture run2 = renderScenario (sc.sampleRate, sc.fxType);
        const CmpResult det = compareCaptures (run1, run2);
        {
            char m[160];
            if (det.equal)
                std::snprintf (m, sizeof (m), "%s: two fresh renders agree byte for byte",
                               sc.label);
            else
                std::snprintf (m, sizeof (m),
                               "%s: run-to-run determinism broken (diffL=%ld diffR=%ld)",
                               sc.label, det.firstDiffL, det.firstDiffR);
            CHECK (det.equal, m);
        }

        // Energy guard: never pin a silent render.
        {
            const size_t n = run1.l.size();
            const double rmsL = channelRms (run1.l, n / 4, n);
            const double rmsR = channelRms (run1.r, n / 4, n);
            char m[128];
            std::snprintf (m, sizeof (m), "%s: render carries energy (rmsL=%.5f rmsR=%.5f)",
                           sc.label, rmsL, rmsR);
            CHECK (rmsL > 1.0e-4 && rmsR > 1.0e-4, m);
        }

        // Golden compare. On mismatch the line prints the computed digest
        // and anchors, so an approved change updates the table in one paste.
        const juce::String digest = digestOf (run1);
        {
            const size_t n = run1.l.size();
            char m[512];
            std::snprintf (m, sizeof (m),
                           "%s: digest %s matches golden %s | anchors l[n/4]=%.6f l[n/2]=%.6f "
                           "l[3n/4]=%.6f r[n/2]=%.6f (%zu samples/ch)",
                           sc.label, digest.toRawUTF8(), sc.goldenDigest.toRawUTF8(),
                           (double) run1.l[n / 4], (double) run1.l[n / 2],
                           (double) run1.l[3 * n / 4], (double) run1.r[n / 2], n);
            CHECK (digest == sc.goldenDigest, m);
        }
    }
    return true;
}
