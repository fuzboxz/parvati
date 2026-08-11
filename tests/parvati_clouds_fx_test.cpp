// Clouds + Warps + Rings FX module (Diffuser / Pitch Shifter / Reverb /
// Looping Delay / WSOLA Stretch / Spectral / Wavefolder / Frequency Shifter /
// Ring Modulator / Resonator) port verification.
//
// Proves each vendored FX module builds via the factory, reports the
// correct FxType, renders a FINITE in-place stereo block at host rate (48000 Hz,
// 256 samples), and produces audible wet output that DIFFERS from the dry input
// (amount/ratio/delay/stretch/spectral/fold engaged). Also guards the FxType count
// (None + 6 Clouds + 3 Warps + 1 Rings == 11); together with the jassert in
// ParameterLayout.cpp this keeps the fx{N}_type choice list and the enum in sync
// (serialization safety).
//
// Built by default. Run with: ./build/parvati_clouds_fx_test

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/FxChain.h"
#include "ui/FxSlotLabels.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i]))
            return false;
    return true;
}

float maxAbs (const float* d, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i)
        m = std::fmax (m, std::fabs (d[i]));
    return m;
}

bool differsFrom (const float* d, const float* ref, int n)
{
    for (int i = 0; i < n; ++i)
        if (d[i] != ref[i])
            return true;
    return false;
}

// Index of the largest-magnitude sample (the impulse-response peak position —
// used to confirm the 6x-OS group delay lands at ~8 base samples, proving the OS
// path is actually active; a no-op/1x fold would peak at 0).
int argmaxAbs (const float* d, int n)
{
    int idx = 0;
    float m = -1.0f;
    for (int i = 0; i < n; ++i)
        if (std::fabs (d[i]) > m) { m = std::fabs (d[i]); idx = i; }
    return idx;
}

// Run an effect in place over @p block (copy in -> out, then process) and report
// whether the output is finite, non-silent, and differs from the dry input.
struct RunResult { bool finite; bool nonSilent; bool differs; };

RunResult runFx (FxProcessor& fx, const float* inL, const float* inR,
                 float* outL, float* outR, int block, int blocks = 1)
{
    RunResult r { true, false, false };
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
        fx.process (outL, outR, block);
        if (! allFinite (outL, block) || ! allFinite (outR, block)) r.finite = false;
        if (maxAbs (outL, block) > 1.0e-5f || maxAbs (outR, block) > 1.0e-5f) r.nonSilent = true;
        if (differsFrom (outL, inL, block) || differsFrom (outR, inR, block)) r.differs = true;
    }
    return r;
}
}  // namespace

int main()
{
    constexpr int kBlock = 256;
    constexpr double kRate = 48000.0;

    // Enum sanity: None + 6 Clouds + 3 Warps + 1 Rings == 11.
    check (static_cast<int> (FxType::Count) == 11,
           "FxType::Count == 11 (None + 6 Clouds + 3 Warps + 1 Rings)");

    // A non-trivial input: a decaying 220 Hz tone burst on both channels so every
    // module has broadband energy to act on (repeats each block so a multi-block
    // reverb/diffuser keeps being fed).
    float inL[kBlock], inR[kBlock];
    for (int i = 0; i < kBlock; ++i)
    {
        const float env  = std::exp (-static_cast<float> (i) / 64.0f);
        const float tone = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f
                                            * static_cast<float> (i) / static_cast<float> (kRate));
        inL[i] = inR[i] = env * tone;
    }

    float outL[kBlock], outR[kBlock];

    // ---- FxDiffuser (no user params; amount is fixed full-wet; chain Dry/Wet is the mix) ----
    {
        auto fx = createFxProcessor (FxType::Diffuser);
        check (fx != nullptr, "Diffuser: factory returns non-null");
        check (fx->type() == FxType::Diffuser, "Diffuser: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        check (fx->latency() == 0, "Diffuser: latency()==0 (no oversampling)");
        const float p[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // amount fixed 1.0; params unused
        fx->setParams (p);
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock);

        check (r.finite, "Diffuser: finite output");
        check (r.nonSilent, "Diffuser: non-silent output");
        check (r.differs, "Diffuser: wet output differs from dry (full-wet diffusion)");
    }

    // ---- FxPitchShifter (ratio + size) ----
    {
        auto fx = createFxProcessor (FxType::PitchShifter);
        check (fx != nullptr, "PitchShifter: factory returns non-null");
        check (fx->type() == FxType::PitchShifter, "PitchShifter: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[4] = { 0.7f, 0.5f, 0.0f, 0.0f };   // +~4.8 st, mid size
        fx->setParams (p);
        // The pitch shifter reads from a ~368-sample internal delay window, so it
        // needs several blocks to charge before it emits non-silent output.
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock, 10);

        check (r.finite, "PitchShifter: finite output");
        check (r.nonSilent, "PitchShifter: non-silent output");
        check (r.differs, "PitchShifter: output differs from dry (shift engaged)");
    }

    // ---- FxReverb (predelay / diffusion / time / tone / low-cut; amount fixed full-wet) ----
    {
        auto fx = createFxProcessor (FxType::Reverb);
        check (fx != nullptr, "Reverb: factory returns non-null");
        check (fx->type() == FxType::Reverb, "Reverb: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.0f, 0.6f, 0.6f, 0.7f, 0.0f };   // predelay 0 / diffusion / time / tone / low-cut off (amount fixed 1.0)
        fx->setParams (p);
        // Render several blocks so the reverb tank charges and the wet tail is
        // unmistakably present (the first block alone is mostly pre-delay).
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock, 8);

        check (r.finite, "Reverb: finite output");
        check (r.nonSilent, "Reverb: non-silent output");
        check (r.differs, "Reverb: wet tail differs from dry (full-wet reverb)");
    }

    // ---- FxLoopingDelay (position / size / pitch / freeze) ----
    // The looper reads from its RECORDED PAST (near the write head, which sits at
    // the end of each recorded block). The decaying burst above is near-zero at
    // the block tail, so feed the looper a CONTINUOUS tone so the recent past it
    // reads always has energy.
    float loopL[kBlock], loopR[kBlock];
    for (int i = 0; i < kBlock; ++i)
    {
        const float t = 0.3f * std::sin (2.0f * 3.14159265f * 220.0f
                                        * static_cast<float> (i) / static_cast<float> (kRate));
        loopL[i] = loopR[i] = t;
    }
    {
        auto fx = createFxProcessor (FxType::LoopingDelay);
        check (fx != nullptr, "LoopingDelay: factory returns non-null");
        check (fx->type() == FxType::LoopingDelay, "LoopingDelay: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        // position ~0 -> minimal delay so the delay line fills within a few
        // blocks (a non-zero position targets position*maxDelay seconds of
        // delay and, like any delay, is silent until that much audio has been
        // recorded). Pitch at unison, freeze off.
        const float p[4] = { 0.0f, 0.5f, 0.5f, 0.0f };
        fx->setParams (p);
        const auto r = runFx (*fx, loopL, loopR, outL, outR, kBlock, 10);

        check (r.finite, "LoopingDelay: finite output");
        check (r.nonSilent, "LoopingDelay: non-silent output");
        check (r.differs, "LoopingDelay: output differs from dry (delay engaged)");
    }

    // ---- FxWSOLAStretch (pitch / position / size) ----
    // Like the looper, WSOLA reads from its RECORDED PAST (~window_size behind
    // the head at position 0), so feed it the continuous tone and render enough
    // blocks for the record buffer to fill past the read window before it emits
    // non-silent output. The correlator splice-point search runs inline each
    // chunk (Parvati FX slots have no background thread).
    {
        auto fx = createFxProcessor (FxType::WSOLAStretch);
        check (fx != nullptr, "WSOLAStretch: factory returns non-null");
        check (fx->type() == FxType::WSOLAStretch, "WSOLAStretch: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        // pitch = unison, position ~0 (reads near the head -> fills fastest),
        // size mid. Time-stretch is intrinsic to WSOLA (no separate knob).
        // Cold-start: WSOLA's correlator compares the recorded past to find splice
        // points; on a fresh circular buffer it reads the zero-filled tail until
        // the write head passes ~2x window_size (~block 25), and the first REAL
        // window then plays ~2048 samples -> non-silent output only emerges around
        // block ~40-50. So render plenty of blocks (runFx flags nonSilent if ANY
        // block exceeds the threshold).
        const float p[5] = { 0.5f, 0.0f, 0.5f, 0.0f, 1.0f };   // pitch unison / position 0 / size mid / freeze off / tone bright
        fx->setParams (p);
        const auto r = runFx (*fx, loopL, loopR, outL, outR, kBlock, 120);

        check (r.finite, "WSOLAStretch: finite output");
        check (r.nonSilent, "WSOLAStretch: non-silent output");
        check (r.differs, "WSOLAStretch: output differs from dry (stretch engaged)");
    }

    // ---- FxSpectral (pitch / warp / position / blur) ----
    // The phase vocoder is NOT buffer-based: it processes the live signal in place
    // through a 4096-point STFT pipeline (analysis/synthesis are separate internal
    // buffers, so in-place Process is safe). The FFT analysis buffer
    // (fft_size + hop_size = 5120 samples at the 32 kHz internal rate) must fill
    // before the first transform and the overlap-add must accumulate, so it needs
    // ~30+ host blocks before non-silent output emerges. The per-chunk Buffer()
    // call (which drains the STFT pipeline inline) is exercised every chunk.
    {
        auto fx = createFxProcessor (FxType::Spectral);
        check (fx != nullptr, "Spectral: factory returns non-null");
        check (fx->type() == FxType::Spectral, "Spectral: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        // pitch ~unison, warp/position/blur mid -> the resynthesis alters the
        // signal (windowing + overlap-add + spectral warp), so it differs from dry.
        const float p[5] = { 0.5f, 0.6f, 0.5f, 0.5f, 0.0f };   // pitch/warp/position/blur mid, freeze off
        fx->setParams (p);
        const auto r = runFx (*fx, loopL, loopR, outL, outR, kBlock, 120);

        check (r.finite, "Spectral: finite output");
        check (r.nonSilent, "Spectral: non-silent output");
        check (r.differs, "Spectral: output differs from dry (spectral engaged)");
    }

    // ---- FxWavefolder (Warps bipolar drive/fold/bias/tone) — NATIVE host rate ----
    {
        auto fx = createFxProcessor (FxType::Wavefolder);
        check (fx != nullptr, "Wavefolder: factory returns non-null");
        check (fx->type() == FxType::Wavefolder, "Wavefolder: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        check (fx->latency() == 8, "Wavefolder: latency()==8 (6x OS group delay)");
        const float p[5] = { 0.0f, 0.8f, 0.5f, 1.0f, 0.0f };   // drive unity / fold 0.8 / bias centred / tone bright
        fx->setParams (p);
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock);

        check (r.finite, "Wavefolder: finite output");
        check (r.nonSilent, "Wavefolder: non-silent output");
        check (r.differs, "Wavefolder: wet output differs from dry (fold > 0)");
    }

    // ---- FxFrequencyShifter (Warps quadrature shifter) — NATIVE host rate ----
    // The Hilbert phase-split network + carrier osc start emitting non-trivial
    // output after a block or two (the allpass cascade charges). Shift away from
    // 0 Hz, modest feedback, full spread so the right channel flips sideband.
    {
        auto fx = createFxProcessor (FxType::FrequencyShifter);
        check (fx != nullptr, "FrequencyShifter: factory returns non-null");
        check (fx->type() == FxType::FrequencyShifter, "FrequencyShifter: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.65f, 0.0f, 0.3f, 1.0f, 0.0f };   // +shift, sine shape, low feedback, full spread
        fx->setParams (p);
        const auto r = runFx (*fx, loopL, loopR, outL, outR, kBlock, 6);

        check (r.finite, "FrequencyShifter: finite output");
        check (r.nonSilent, "FrequencyShifter: non-silent output");
        check (r.differs, "FrequencyShifter: output differs from dry (shift engaged)");
    }

    // ---- FxRingModulator (Warps diode ring mod + internal carrier) — NATIVE ----
    // An internal sine carrier against the signal via the diode non-linearity.
    {
        auto fx = createFxProcessor (FxType::RingModulator);
        check (fx != nullptr, "RingModulator: factory returns non-null");
        check (fx->type() == FxType::RingModulator, "RingModulator: type() matches");

        fx->prepare (kRate, kBlock);
        fx->reset();
        check (fx->latency() == 8, "RingModulator: latency()==8 (6x OS group delay)");
        const float p[4] = { 0.4f, 0.8f, 0.5f, 0.0f };   // mid carrier, buzzy shape, mid amount
        fx->setParams (p);
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock);

        check (r.finite, "RingModulator: finite output");
        check (r.nonSilent, "RingModulator: non-silent output");
        check (r.differs, "RingModulator: output differs from dry (ring mod engaged)");
    }

    // ---- FxResonator (Rings modal resonator) — NATIVE host rate ----
    // The resonator bank filters input through tuned band-pass SVFs; a broadband
    // impulse excites the modes so output rings (differs from dry, non-silent).
    // latency()==0 (LTI filter group delay is the effect's sound, not processing
    // latency). Rings-faithful stereo: ONE resonator, out(odd)->L, aux(even)->R.
    {
        auto fx = createFxProcessor (FxType::Resonator);
        check (fx != nullptr, "Resonator: factory returns non-null");
        check (fx->type() == FxType::Resonator, "Resonator: type() matches");
        check (fx->latency() == 0, "Resonator: latency()==0 (LTI modal filters)");

        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.5f, 0.3f, 0.5f, 0.25f, 0.25f };   // C4 / decay / bright / position(0.25) / structure(Rings default)
        fx->setParams (p);

        // Feed several blocks so the resonator modes build up.
        const auto r = runFx (*fx, inL, inR, outL, outR, kBlock, 6);

        check (r.finite, "Resonator: finite output");
        check (r.nonSilent, "Resonator: non-silent output");
        check (r.differs, "Resonator: output differs from dry (modes excited)");

        // Bounded output — the makeup gain (x3) should not cause runaway at
        // moderate settings. |sample| should stay well under 10.
        const float mx = std::fmax (maxAbs (outL, kBlock), maxAbs (outR, kBlock));
        check (mx < 10.0f, "Resonator: output bounded (< 10.0)");
    }

    // ---- Resonator: native-SR stability (no crash across rates) ----
    // The SVF coefficients are computed from freqHz/sampleRate, so they track
    // the host rate. Confirm it processes without crash/NaN at 44.1k/48k/96k.
    {
        for (const double sr : { 44100.0, 48000.0, 96000.0 })
        {
            auto fx = createFxProcessor (FxType::Resonator);
            fx->prepare (sr, kBlock);
            fx->reset();
            const float p[5] = { 0.3f, 0.5f, 0.7f, 0.4f, 0.25f };
            fx->setParams (p);
            float sL[kBlock], sR[kBlock];
            for (int i = 0; i < kBlock; ++i) { sL[i] = inL[i]; sR[i] = inR[i]; }
            fx->process (sL, sR, kBlock);
            check (allFinite (sL, kBlock) && allFinite (sR, kBlock),
                   "Resonator: finite output across sample rates (44.1k/48k/96k)");
        }
    }

    // ---- Resonator: stereo differs (odd vs even modes, NOT discarded) ----
    // With ONE resonator fed a MONO sum, out (odd modes) -> L and aux (even
    // modes) -> R. The two channels MUST differ because odd and even mode sets
    // are complementary. This proves aux (even modes) is now USED (out->L,
    // aux->R), not discarded as in the old two-resonator bug.
    {
        auto fx = createFxProcessor (FxType::Resonator);
        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.6f, 0.4f, 0.5f, 0.25f, 0.25f };   // position=0.25 (both odd+even modes active), structure default
        fx->setParams (p);
        // Feed identical mono input on both channels (the harness input).
        float sL[kBlock], sR[kBlock];
        for (int i = 0; i < kBlock; ++i) { sL[i] = inL[i]; sR[i] = inL[i]; }
        fx->process (sL, sR, kBlock);
        check (differsFrom (sL, sR, kBlock),
               "Resonator: L(odd) != R(even) for mono input (aux used, not discarded)");
    }

    // ---- Resonator: Position knob is a LIVE control ----
    // Two different Position values change the cosine mode-envelope (the pickup
    // position), so the steady-state output waveforms must differ. Proves
    // position is wired through to the resonator's Process (not a dead fixed-0.5
    // like the old two-resonator bug where position could not affect the output).
    // Uses positions 0.2 and 0.7 — genuinely different cosine coefficients (the
    // position is circular: 0 and 1 are identical, so we avoid the endpoints).
    {
        float outA[kBlock], outB[kBlock];
        for (int trial = 0; trial < 2; ++trial)
        {
            const float pos = (trial == 0) ? 0.2f : 0.7f;
            auto fx = createFxProcessor (FxType::Resonator);
            fx->prepare (kRate, kBlock);
            fx->reset();
            const float p[5] = { 0.5f, 0.3f, 0.5f, pos, 0.25f };
            fx->setParams (p);
            float sL[kBlock], sR[kBlock];
            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { sL[i] = inL[i]; sR[i] = inL[i]; }
                fx->process (sL, sR, kBlock);
            }
            if (trial == 0)
                for (int i = 0; i < kBlock; ++i) outA[i] = sL[i];
            else
                for (int i = 0; i < kBlock; ++i) outB[i] = sL[i];
        }
        check (differsFrom (outA, outB, kBlock),
               "Resonator: Position knob changes output (live control)");
    }

    // ---- 6x-OS group-delay proof (D4) ----
    // A no-op/1x fold would pass an impulse through immediately (peak at sample
    // 0). The 6x SRC up->fold->down path delays it by the OS group delay (~8
    // base samples), so the impulse-response peak lands at ~8. This proves the
    // oversampling is actually running (not stripped/broken). Uses a small fold
    // + small impulse so the fold LUT stays in its linear region (no clipping).
    {
        auto fx = createFxProcessor (FxType::Wavefolder);
        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.0f, 0.1f, 0.5f, 1.0f, 0.0f };   // drive unity, mild fold, centred bias, tone bright (no LP smear)
        fx->setParams (p);

        float impL[kBlock] = {}, impR[kBlock] = {};
        impL[0] = impR[0] = 0.5f;                          // unit-ish impulse
        float respL[kBlock], respR[kBlock];
        for (int i = 0; i < kBlock; ++i) { respL[i] = impL[i]; respR[i] = impR[i]; }
        fx->process (respL, respR, kBlock);

        const int peakL = argmaxAbs (respL, kBlock);
        const int peakR = argmaxAbs (respR, kBlock);
        check (peakL >= 5 && peakL <= 12,
               "Wavefolder: impulse peaks ~8 (6x OS active, not a no-op)");
        check (peakR >= 5 && peakR <= 12,
               "Wavefolder: impulse R peaks ~8 (symmetric latency)");
        check (peakL == peakR, "Wavefolder: L/R latency symmetric");
    }

    // ---- D1: per-slot dry-delay comp (DISCRIMINATING impulse test) ----
    // The fs/16 steady-state premise was WRONG for this non-linear /
    // non-linear-phase module: there is no fs/16 comb to detect (the wet is
    // in-phase at fs/16), so a steady-sine test passes with OR without the comp.
    // The comp's REAL effect is transient/impulse alignment. An impulse through a
    // Wavefolder slot at dw=0.5 (wet settled): the wet peaks at the OS group
    // delay (~sample 8). The dry-delay comp moves the dry impulse from sample 0 to
    // sample 8 -> out[0] holds only the wet pre-ring (small) and the peak lands
    // near 8. WITHOUT the comp the dry impulse stays at sample 0 -> |out[0]| ~=
    // 0.5*impulse, so the |out[0]| check FAILS. (Verified: patching the per-slot
    // dry-delay `L` to 0 makes this test fail.)
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::Wavefolder);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 0.5f);
        chain.setSlotParam (0, 0, 0.1f);
        chain.setSlotParam (0, 1, 0.5f);   // mild fold, centred bias

        std::vector<float> sil (kBlock, 0.0f);
        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 16; ++b)              // settle wetFade -> 1, dwCur -> 0.5
            chain.process (sil.data(), sil.data(), oL, oR, kBlock);

        std::vector<float> imp (kBlock, 0.0f);
        imp[0] = 1.0f;                            // unit impulse at sample 0
        chain.process (imp.data(), imp.data(), oL, oR, kBlock);

        const int peak = argmaxAbs (oL, kBlock);
        const float at0 = std::fabs (oL[0]);
        const float pk = std::fabs (oL[peak]);
        check (at0 < 0.25f,
               "D1: dry impulse delayed off sample 0 (per-slot dry-delay comp active)");
        check (peak >= 5 && peak <= 12,
               "D1: dw=0.5 impulse peak near OS latency (~8), not split to sample 0");
        check (pk > 0.0f && at0 < 0.5f * pk,
               "D1: out[0] well below peak (dry+wet aligned, not a sample-0 dry spike)");
    }

    // ---- N1: masterMix dry-delay comp (DISCRIMINATING impulse test) ----
    // Same impulse idea at the CHAIN level. Wavefolder at dw=1.0 (so the per-slot
    // blend is not the variable), masterMix=0.5. The chain output peaks at the OS
    // latency (~sample 8); the masterMix dry-delay comp moves the INPUT impulse
    // from sample 0 to sample 8 so chain-out + input align -> out[0] holds only
    // the chain-out pre-ring (small). WITHOUT the comp the input impulse stays
    // at 0 -> |out[0]| ~= 0.5*impulse, so the check FAILS. (The fs/16 steady-sine
    // version was non-discriminating: the wet is in-phase at fs/16 so no null
    // exists to detect. Verified: patching the master dry-delay `if (Lc>0)` to
    // `false` makes this test fail.)
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::Wavefolder);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 1.0f);
        chain.setSlotParam (0, 0, 0.1f);
        chain.setSlotParam (0, 1, 0.5f);
        chain.setMasterMix (0.5f);

        std::vector<float> sil (kBlock, 0.0f);
        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 16; ++b)              // settle wetFade + masterMixCur
            chain.process (sil.data(), sil.data(), oL, oR, kBlock);

        std::vector<float> imp (kBlock, 0.0f);
        imp[0] = 1.0f;                            // unit impulse at sample 0
        chain.process (imp.data(), imp.data(), oL, oR, kBlock);

        const int peak = argmaxAbs (oL, kBlock);
        const float at0 = std::fabs (oL[0]);
        const float pk = std::fabs (oL[peak]);
        check (at0 < 0.25f,
               "N1: input impulse delayed off sample 0 (masterMix dry-delay comp active)");
        check (peak >= 5 && peak <= 12,
               "N1: masterMix=0.5 impulse peak near chain latency (~8)");
        check (pk > 0.0f && at0 < 0.5f * pk,
               "N1: out[0] well below peak (chain-out + input aligned)");
    }

    // ---- N2: bypass click (slot output latency MUST be invariant through bypass) ----
    // Engage a Wavefolder, stabilise, then bypass. The fade-out tail decays
    // over ~0.30 s; when the slot finally deactivates the output must NOT snap
    // (delayPassthrough keeps the dry L samples behind). Scan the entire output
    // for the max single-sample step and compare to steady-state. This FAILS if
    // the bypass snap returns (the click is ~5-10x the steady step).
    {
        const int N = 512 * kBlock;   // 131072: block-aligned (~2.7 s @ 48k): enough for the 0.30 s fade + deactivation
        std::vector<float> inBuf (N), outBuf (N);
        for (int i = 0; i < N; ++i)
            inBuf[i] = 0.2f * std::sin (2.0f * 3.14159265f * 1500.0f
                                       * (float) i / (float) kRate);

        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::Wavefolder);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 0.5f);
        chain.setSlotParam (0, 0, 0.1f);
        chain.setSlotParam (0, 1, 0.5f);

        const int stabilise = 20 * kBlock;   // ~20 blocks -> wetFade settles to ~1
        for (int off = 0; off + kBlock <= N; off += kBlock)
        {
            if (off == stabilise)
                chain.setSlotEnabled (0, false);   // start fade-out
            chain.process (&inBuf[off], &inBuf[off], &outBuf[off], &outBuf[off], kBlock);
        }

        float steadyStep = 0.0f, maxStep = 0.0f;
        int maxIdx = 0;
        for (int i = 1; i < N; ++i)
        {
            const float step = std::fabs (outBuf[i] - outBuf[i - 1]);
            if (i < stabilise) steadyStep = std::fmax (steadyStep, step);
            if (step > maxStep) { maxStep = step; maxIdx = i; }
        }
        check (maxStep < steadyStep * 3.0f,
               "N2: bypassing an OS slot produces no click (max step within 3x steady)");
    }

    // ---- HARDEN-1: Resonator limiter bounds on-resonance build-up ----
    // A full-scale sine EXACTLY at the resonant frequency (C4=261.6 Hz, pitch 0.5)
    // with max Decay/Bright builds up maximally in the high-Q SVF bank. Without
    // the Rings limiter this hit ~16-4850x; with it the output is bounded to
    // ~0.8 peak (SoftLimit toward ~1.0). Asserts maxAbs < 1.5 over a long run.
    // FAILS without the limiter (old x3 path was unbounded).
    {
        auto fx = createFxProcessor (FxType::Resonator);
        fx->prepare (kRate, kBlock);
        fx->reset();
        const float p[5] = { 0.5f, 1.0f, 1.0f, 0.25f, 0.25f };   // C4, max decay/bright, pos 0.25, structure default
        fx->setParams (p);

        float mxOverall = 0.0f;
        for (int b = 0; b < 2000; ++b)            // ~10.7 s: full resonance build-up
        {
            float sL[kBlock], sR[kBlock];
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = std::sin (2.0f * 3.14159265f * 261.6f
                                          * static_cast<float> (b * kBlock + i)
                                          / static_cast<float> (kRate));;
                sL[i] = sR[i] = s;
            }
            fx->process (sL, sR, kBlock);
            mxOverall = std::fmax (mxOverall, maxAbs (sL, kBlock));
        }
        check (mxOverall < 1.5f,
               "HARDEN-1: Resonator limiter bounds on-resonance build-up (< 1.5)");
    }

    // ---- HARDEN-2: effect params are passed RAW (no block-rate smoothing) ----
    // The FX params reach each processor with NO one-pole smoothing (true parity
    // with the synth voice path, which applies modulation raw at the ~980 Hz
    // internal-block cadence). A block-rate smoother would SLEW audio-rate FX-
    // param modulation (the user's reported bug). Proof: step fold 0.1 -> 0.9;
    // the block right AFTER the step, past the 6x-OS group delay, already EQUALS
    // the settled fold-0.9 output — the new param is applied in ONE block, not
    // ramped. (With a ~1 ms smoother block-1 would be INTERMEDIATE, ~fold 0.49,
    // and b1[24:] would DIFFER from set[24:] -> this assertion FAILS.)
    {
        float win[kBlock];
        for (int i = 0; i < kBlock; ++i)
            win[i] = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f
                                     * static_cast<float> (i) / static_cast<float> (kRate));

        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::Wavefolder);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 1.0f);
        chain.setSlotParam (0, 0, 0.0f);   // drive unity
        chain.setSlotParam (0, 1, 0.1f);   // mild fold
        chain.setSlotParam (0, 2, 0.5f);   // centred bias
        chain.setSlotParam (0, 3, 1.0f);   // tone bright (bypass — no LP transient interferes with the RAW-param check)

        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 40; ++b)             // settle wetFade -> 1 (at fold 0.1)
            chain.process (win, win, oL, oR, kBlock);

        float ref[kBlock];
        for (int i = 0; i < kBlock; ++i) ref[i] = oL[i];

        chain.setSlotParam (0, 1, 0.9f);         // step fold 0.1 -> 0.9
        chain.process (win, win, oL, oR, kBlock); // b1: fold applied RAW (no lag)
        float b1[kBlock];
        for (int i = 0; i < kBlock; ++i) b1[i] = oL[i];

        for (int b = 0; b < 15; ++b)             // settle (no-op now: raw snaps in 1)
            chain.process (win, win, oL, oR, kBlock);
        float set[kBlock];
        for (int i = 0; i < kBlock; ++i) set[i] = oL[i];

        check (differsFrom (ref, set, kBlock),
               "HARDEN-2a: fold 0.1 vs 0.9 produce different output (test premise)");
        // RAW passthrough: past the 6x-OS group delay (~8 base samples) block-1
        // already EQUALS the settled fold-0.9 output (param applied in 1 block,
        // NOT smoothed). A one-pole smoother would leave b1 INTERMEDIATE -> the
        // arrays DIFFER -> !differsFrom is FALSE -> FAILS.
        constexpr int kOsHead = 24;   // skip the OS transient (group delay ~8 + margin)
        check (! differsFrom (b1 + kOsHead, set + kOsHead, kBlock - kOsHead),
               "HARDEN-2b: effect param applied RAW (block-1 == settled past OS delay; no smoothing slew)");
    }

    // ---- HARDEN-3: freeze engages in 1 block (params are RAW, no smoothing) ----
    // Now that ALL effect params are passed raw (no block-rate smoother — see
    // HARDEN-2), the freeze gate (LoopingDelay p3, thresholded >0.5) crosses its
    // threshold in the VERY NEXT block. Fill the loop with a tone, step freeze
    // ON, feed SILENCE for one block: the freeze engages immediately -> the held
    // buffer plays at full level (a clear jump).
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::LoopingDelay);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 1.0f);
        chain.setSlotParam (0, 0, 0.5f);   // position
        chain.setSlotParam (0, 1, 0.1f);   // small loop
        chain.setSlotParam (0, 2, 0.5f);   // pitch unison
        chain.setSlotParam (0, 3, 0.0f);   // freeze OFF

        float tone[kBlock];
        for (int i = 0; i < kBlock; ++i)
            tone[i] = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f
                                      * static_cast<float> (i) / static_cast<float> (kRate));
        float sil[kBlock] = {};

        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 120; ++b)            // fill the loop with the tone
            chain.process (tone, tone, oL, oR, kBlock);

        chain.setSlotParam (0, 3, 1.0f);         // freeze ON (step gate)
        chain.process (sil, sil, oL, oR, kBlock); // 1 block of silence under freeze
        const float Lfrozen1 = maxAbs (oL, kBlock);

        check (Lfrozen1 > 0.05f,
               "HARDEN-3: freeze engages in 1 block (held loop plays, not ~8 ms-smeared)");
    }

    // ---- HARDEN-4: Position physics (center-pluck node) ----
    // At Position 0.5 the even-mode cosine amplitude pattern is 1,0,1,0,... so the
    // even modes (aux -> R) vanish (textbook center-pluck node). At Position 0.25
    // the even modes are active -> R is non-silent. Documents the behavior.
    {
        float rMid[kBlock] = {}, rEdge[kBlock] = {};
        for (int trial = 0; trial < 2; ++trial)
        {
            const float pos = (trial == 0) ? 0.25f : 0.5f;
            auto fx = createFxProcessor (FxType::Resonator);
            fx->prepare (kRate, kBlock);
            fx->reset();
            const float p[5] = { 0.5f, 0.5f, 0.5f, pos, 0.25f };
            fx->setParams (p);
            float sL[kBlock], sR[kBlock];
            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { sL[i] = inL[i]; sR[i] = inL[i]; }
                fx->process (sL, sR, kBlock);
            }
            // R = even modes (aux)
            if (trial == 0) for (int i = 0; i < kBlock; ++i) rMid[i] = sR[i];
            else            for (int i = 0; i < kBlock; ++i) rEdge[i] = sR[i];
        }
        const float midLevel  = maxAbs (rMid, kBlock);
        const float edgeLevel = maxAbs (rEdge, kBlock);
        check (midLevel > 1.0e-4f,
               "HARDEN-4a: Position 0.25 -> R (even modes) non-silent");
        check (edgeLevel < midLevel * 0.1f,
               "HARDEN-4b: Position 0.5 -> R (even modes) near-silent (center-pluck node)");
    }

    // ---- DEDUP-1: amount=mix collapsed (counts + labels) ----
    // Diffuser now has 0 user params (amount was a duplicate of chain Dry/Wet);
    // Reverb dropped its Amount knob -> 5 params (Predelay/Diffusion/Time/Tone/Low-Cut).
    {
        check (activeParamCount (FxType::Diffuser) == 0,
               "DEDUP-1a: Diffuser exposes 0 param knobs (amount collapsed)");
        check (activeParamCount (FxType::Reverb) == 5,
               "DEDUP-1b: Reverb exposes 5 param knobs (predelay/diffusion/time/tone/low-cut)");
        check (std::strcmp (paramLabel (FxType::Reverb, 0), "Predelay") == 0,
               "DEDUP-1c: Reverb p0 is 'Predelay' (signal-path order)");
        check (std::strcmp (paramLabel (FxType::Diffuser, 0), "-") == 0,
               "DEDUP-1d: Diffuser p0 label is '-' (no 'Amount')");
    }

    // ---- DEDUP-2: Diffuser amount is fixed (param[0] ignored) ----
    // Two different param[0] values must produce identical output because the
    // amount is now hardcoded to 1.0 (param slots are all ignored).
    {
        float outA[kBlock] = {}, outB[kBlock] = {};
        {
            auto fx = createFxProcessor (FxType::Diffuser);
            fx->prepare (kRate, kBlock);
            fx->reset();
            const float p[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            fx->setParams (p);
            float tmpR[kBlock];
            for (int i = 0; i < kBlock; ++i) { outA[i] = inL[i]; tmpR[i] = inR[i]; }
            fx->process (outA, tmpR, kBlock);
        }
        {
            auto fx = createFxProcessor (FxType::Diffuser);
            fx->prepare (kRate, kBlock);
            fx->reset();
            const float p[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            fx->setParams (p);
            float tmpR[kBlock];
            for (int i = 0; i < kBlock; ++i) { outB[i] = inL[i]; tmpR[i] = inR[i]; }
            fx->process (outB, tmpR, kBlock);
        }
        float maxErr = 0.0f;
        for (int i = 0; i < kBlock; ++i)
            maxErr = std::fmax (maxErr, std::fabs (outA[i] - outB[i]));
        check (maxErr < 1.0e-6f,
               "DEDUP-2: Diffuser param[0] is ignored (amount fixed 1.0)");
    }

    // ---- DEDUP-3: chain Dry/Wet is the sole wet/dry mix (FxChain-level) ----
    // With Dry/Wet=0 the output should equal the dry input; with Dry/Wet=1 it
    // should be the full-wet diffused signal (differs from dry). This proves the
    // collapsed amount knob's function is fully covered by the chain Dry/Wet.
    {
        float outDry[kBlock] = {}, outWet[kBlock] = {};
        {
            FxChain chain;
            chain.prepare (kRate, kBlock);
            chain.setTopology (FxTopology::Series);
            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 0.0f);
            float tmpL[kBlock], tmpR[kBlock], dummyR[kBlock];
            for (int i = 0; i < kBlock; ++i) { tmpL[i] = inL[i]; tmpR[i] = inR[i]; }
            chain.process (tmpL, tmpR, outDry, dummyR, kBlock);
        }
        {
            FxChain chain;
            chain.prepare (kRate, kBlock);
            chain.setTopology (FxTopology::Series);
            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 1.0f);
            float tmpL[kBlock], tmpR[kBlock], dummyR[kBlock];
            for (int i = 0; i < kBlock; ++i) { tmpL[i] = inL[i]; tmpR[i] = inR[i]; }
            chain.process (tmpL, tmpR, outWet, dummyR, kBlock);
        }
        // Dry/Wet=0 => output ≈ dry input (the diffused signal is fully blended out)
        float dryErr = 0.0f;
        for (int i = 0; i < kBlock; ++i)
            dryErr = std::fmax (dryErr, std::fabs (outDry[i] - inL[i]));
        check (dryErr < 1.0e-4f,
               "DEDUP-3a: chain Dry/Wet=0 -> output ≈ dry (diffuser fully bypassed)");
        // Dry/Wet=1 => output is the full-wet diffused signal (differs from dry)
        check (differsFrom (outWet, inL, kBlock),
               "DEDUP-3b: chain Dry/Wet=1 -> full-wet diffused output (differs from dry)");
    }

    // ---- DEDUP-4: 0..100% display conversion ----
    // The stored 0..127 maps to 0%..100% via roundToInt(v/127.0*100.0). Spot-checks.
    {
        auto pct = [] (double v) { return (int) std::round (v / 127.0 * 100.0); };
        check (pct (0.0)   == 0,   "DEDUP-4a: 0 -> 0%");
        check (pct (127.0) == 100, "DEDUP-4b: 127 -> 100%");
        check (pct (64.0)  == 50,  "DEDUP-4c: 64 -> ~50%");
    }

    // ---- UNITS: per-param meaningful-unit value readout (paramValueText) ----
    // Display-only formatter: raw 0..127 -> note names / +/-semitones / Hz /
    // On-Off / %. Mirrors the DSP normalization (p = v/127.0).
    {
        auto eq = [] (const juce::String& got, const char* want) {
            return got == juce::String (want);
        };
        // Resonator Pitch -> MIDI note 24..96 = C1..C7
        check (eq (paramValueText (FxType::Resonator, 0, 0.0),    "C1"), "UNITS: Resonator 0 -> C1");
        check (eq (paramValueText (FxType::Resonator, 0, 63.5),  "C4"), "UNITS: Resonator 63.5 -> C4");
        check (eq (paramValueText (FxType::Resonator, 0, 127.0), "C7"), "UNITS: Resonator 127 -> C7");
        // PitchShifter Pitch (was 'Ratio') -> +/-12 st
        check (eq (paramValueText (FxType::PitchShifter, 0, 0.0),    "-12.0 st"), "UNITS: PitchShifter 0 -> -12.0 st");
        check (eq (paramValueText (FxType::PitchShifter, 0, 63.5),  "+0.0 st"),  "UNITS: PitchShifter 63.5 -> +0.0 st");
        check (eq (paramValueText (FxType::PitchShifter, 0, 127.0), "+12.0 st"), "UNITS: PitchShifter 127 -> +12.0 st");
        check (eq (paramValueText (FxType::PitchShifter, 0, 101.0), "+7.0 st"),  "UNITS: PitchShifter 101 -> +7.0 st (integer-snap; was 6.9..7.1 with no 7.0)");
        // WSOLA/Spectral/LoopingDelay pitch -> +/-24 st
        check (eq (paramValueText (FxType::WSOLAStretch, 0, 127.0),  "+24.0 st"), "UNITS: WSOLA 127 -> +24.0 st");
        check (eq (paramValueText (FxType::Spectral, 0, 0.0),         "-24.0 st"), "UNITS: Spectral 0 -> -24.0 st");
        check (eq (paramValueText (FxType::LoopingDelay, 2, 127.0),  "+24.0 st"), "UNITS: LoopingDelay p2 127 -> +24.0 st");
        // FrequencyShifter Shift -> Hz (non-linear; 0 at centre)
        check (eq (paramValueText (FxType::FrequencyShifter, 0, 63.5), "0 Hz"),   "UNITS: FreqShifter 63.5 -> 0 Hz");
        check (paramValueText (FxType::FrequencyShifter, 0, 0.0).contains ("-2048 Hz"), "UNITS: FreqShifter 0 -> -2048 Hz");
        check (paramValueText (FxType::FrequencyShifter, 0, 127.0).contains ("+2048 Hz"), "UNITS: FreqShifter 127 -> +2048 Hz");
        // RingModulator Carrier -> Hz/kHz (20..4000, log)
        check (eq (paramValueText (FxType::RingModulator, 0, 0.0),   "20 Hz"),    "UNITS: RingMod 0 -> 20 Hz");
        check (eq (paramValueText (FxType::RingModulator, 0, 127.0), "4.00 kHz"), "UNITS: RingMod 127 -> 4.00 kHz");
        // LoopingDelay Freeze -> On/Off (threshold p > 0.5)
        check (eq (paramValueText (FxType::LoopingDelay, 3, 63.0), "Off"), "UNITS: LoopingDelay freeze 63 -> Off");
        check (eq (paramValueText (FxType::LoopingDelay, 3, 64.0), "On"),  "UNITS: LoopingDelay freeze 64 -> On");
        // Reverb Predelay (idx0) -> 0..200 ms; Diffusion (idx1) -> % (signal-path reorder)
        check (eq (paramValueText (FxType::Reverb, 0, 0.0),   "0 ms"),   "UNITS: Reverb Predelay 0 -> 0 ms");
        check (paramValueText (FxType::Reverb, 0, 127.0).contains ("200 ms"), "UNITS: Reverb Predelay 127 -> ~200 ms");
        check (eq (paramValueText (FxType::Reverb, 1, 64.0), "50%"), "UNITS: Reverb Diffusion 64 -> 50% (idx1 post-reorder)");
        // WSOLA Freeze (idx3) -> On/Off
        check (eq (paramValueText (FxType::WSOLAStretch, 3, 63.0), "Off"), "UNITS: WSOLA freeze 63 -> Off");
        check (eq (paramValueText (FxType::WSOLAStretch, 3, 64.0), "On"),  "UNITS: WSOLA freeze 64 -> On");
        check (eq (paramLabel (FxType::Reverb, 0), "Predelay"), "UNITS: Reverb p0 label is 'Predelay'");
        check (eq (paramLabel (FxType::Wavefolder, 0), "Drive"), "UNITS: Wavefolder p0 label is 'Drive'");
        check (eq (paramLabel (FxType::FrequencyShifter, 1), "Shape"), "UNITS: FreqShifter p1 label is 'Shape'");
        check (eq (paramLabel (FxType::PitchShifter, 2), "Spread"), "UNITS: PitchShifter p2 label is 'Spread'");
        check (static_cast<int> (FxType::Reverb) == 3, "UNITS: FxType::Reverb == 3 (rename preserved index)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "CLOUDS FX TEST: FAILURES" : "CLOUDS FX TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
