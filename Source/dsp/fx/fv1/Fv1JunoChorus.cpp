// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1JunoChorus implementation — the Dual-BBD Chorus. This file holds the
// documented source configuration table and the FV-1 realization of it.

#include "dsp/fx/fv1/Fv1JunoChorus.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

static_assert (2 * DelayLine<2048>::capacity <= kMaxMemorySamples,
               "Fv1JunoChorus within the FV-1 RAM budget");

// ---------------------------------------------------------------------------
// DOCUMENTED SOURCE CONFIGURATION (Roland Juno-60/106 chorus section).
//
// Values come from the service-manual-derived community consensus cited
// across synth-service literature (web search was not available to the
// implementer; ranges are consensus, not measured facts). Sources agree on
// the structure; they disagree slightly on the clock and depth constants.
// The model uses range midpoints and does not claim more precision.
//
//   Item                Documented range         Model value
//   BBD lines           2 x MN3007, 1024 stages  two 2048-sample rings
//   Line delay          ~20-26 ms (clock ~20 kHz) 25.6 ms center
//                       delay = 1024/(2*clock)   (1024 / 2*20 kHz)
//   LFO                 ONE shared LFO           one phase accumulator
//   Line 2 phase        opposite (180 deg)       +0.5 in the LUT domain
//   Mode I rate         ~0.5-0.6 Hz              0.56 Hz
//   Mode II rate        ~1.1-1.2 Hz              1.13 Hz
//   Mode I depth        shallow                  2.5 ms sweep
//   Mode II depth       deeper                   4.0 ms sweep
//   Mode II wet filter  brighter than I          LP 13 kHz vs 9 kHz
//   Leak / DC           clock residue            HP 30 Hz per line
//   Feedback            none (open lines)        none
//   Output stage        dry + line L + line R    internal Mix law (below)
//
// DEVIATIONS, stated plainly:
//   * The BBD stages quantize charge packets. This model keeps the family
//     24-bit path; no charge quantization is modeled (the source chorus is
//     a clean ensemble effect, not a dirt effect).
//   * The internal Mix param is a family-contract deviation: every other
//     family member emits wet only and the chain blends. The source chorus
//     sums dry and wet at a fixed internal ratio, so the stock mix must
//     live inside the effect. At Mix 1.0 the output is wet-dominant with a
//     0.35 dry bleed, like the hardware summing node.
// ---------------------------------------------------------------------------

// Mode constants live in fxlaw (FxTypes.h) — one definition serves the
// DSP, the slot-card readout and the tail estimate. The source table below
// documents WHERE the values come from.

// Mix law. m = Mix param in [0,1]. The weights hold the coherent peak sum
// at unity (no rail clip at Mix 1.0 with a loud input):
//   dry   = 1 - 0.65*m
//   line  = 0.325*m each
// At the stock point m = 0.7: dry 0.545, each line 0.2275. The wet sum sits
// below the dry level, matching the documented fixed-wet output stage.
constexpr float kDrySpan = 0.65f;      // dry attenuation across the law
constexpr float kLineWMax = 0.325f;    // per-line weight at Mix 1.0

void Fv1JunoChorus::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is unused.

    // Mode: below 0.5 = Chorus I, else Chorus II. Both settings share one
    // dual-line chorus; the mode sets rate, depth and wet filter.
    const bool modeII = p0 >= 0.5f;

    // Rate: the mode rate times the trim, through the shared fxlaw laws.
    const float rateHz = fxlaw::junoModeRateHz<float> (modeII) * fxlaw::junoRateTrim (p1);
    inc_ = rateHz / static_cast<float> (kInternalRate);

    // Depth: the trim (0..2x stock) times the mode depth.
    depthSamp_  = fxlaw::junoDepthTrim (p2) * fxlaw::junoModeDepthSeconds<float> (modeII)
                  * static_cast<float> (kInternalRate);
    centerSamp_ = fxlaw::junoCenterSeconds<float>() * static_cast<float> (kInternalRate);

    // Depth guard: keep the deepest read at 1 sample or more (family rule).
    if (depthSamp_ > centerSamp_ - 1.0f)
        depthSamp_ = centerSamp_ - 1.0f;

    // Mix weights, 14-bit quantized (the FV-1 coefficient register).
    const float m = p3;
    dry14_  = q14 (1.0f - kDrySpan * m);
    line14_ = q14 (kLineWMax * m);

    // Post-BBD treatment: 2-pole low-pass (mode-dependent brightness) and
    // the clock-leak high-pass. Coefficients live at setParams rate only.
    constexpr float kLpHzI  = 9000.0f;    // mode I: darker anti-clock filter
    constexpr float kLpHzII = 13000.0f;   // mode II: brighter (documented)
    constexpr float kLeakHz = 30.0f;      // clock residue / DC leak
    const float lpHz = modeII ? kLpHzII : kLpHzI;
    lpLa_.setCutoff (lpHz);  lpLb_.setCutoff (lpHz);
    lpRa_.setCutoff (lpHz);  lpRb_.setCutoff (lpHz);
    hpL_.setCutoff (kLeakHz);
    hpR_.setCutoff (kLeakHz);
}

void Fv1JunoChorus::resetInternal()
{
    lineL_.clear();
    lineR_.clear();
    phase_ = 0.0f;
    lpLa_.clear(); lpLb_.clear(); lpRa_.clear(); lpRb_.clear();
    hpL_.clear(); hpR_.clear();
}

void Fv1JunoChorus::processSampleFx (int32_t lin, int32_t /*rin*/,
                                    int32_t& lout, int32_t& rout)
{
    // ONE shared LFO; line 2 reads at INVERTED phase (the source signature).
    const float dL = centerSamp_ + depthSamp_ * lutSine32 (phase_);
    const float dR = centerSamp_ + depthSamp_ * lutSine32 (phase_ + 0.5f);

    const int32_t readL = lineL_.readFrac (dL);
    const int32_t readR = lineR_.readFrac (dR);

    // Open lines: the input feeds both BBDs; no feedback path exists.
    lineL_.write (lin);
    lineR_.write (lin);

    // Post-BBD treatment per line: 2-pole low-pass, then the leak high-pass.
    const int32_t wetL = hpL_.process (lpLb_.process (lpLa_.process (readL)));
    const int32_t wetR = hpR_.process (lpRb_.process (lpRa_.process (readR)));

    // Output stage: dry + the two treated lines (L / R dominant).
    lout = f24_addSat (f24_mulk (lin, dry14_), f24_mulk (wetL, line14_));
    rout = f24_addSat (f24_mulk (lin, dry14_), f24_mulk (wetR, line14_));

    phase_ += inc_;
    phase_ -= std::floor (phase_);
}

} // namespace parvati::fv1
