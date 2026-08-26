// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
// Ambika analog-filter emulation (juce::dsp). See analog_filter.h for details.

#include "dsp/analog_filter.h"

#include <cmath>
#include <limits>

namespace ambika::dsp {

namespace {
// Builds a mono ProcessSpec for a given sample rate / max block.
juce::dsp::ProcessSpec makeSpec (double sampleRate, int blockSize) noexcept
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (juce::jmax (1, blockSize));
    spec.numChannels      = 1;
    return spec;
}
} // namespace

void AnalogFilter::prepare (double sampleRate, int blockSize)
{
    sampleRate_ = sampleRate;
    blockSize_  = juce::jmax (1, blockSize);

    const auto spec = makeSpec (sampleRate_, blockSize_);

    // DC blocker pole: one-pole highpass at ~10 Hz (see the header). Depends
    // only on the sample rate, so it is computed HERE, not in applyParams()
    // (the parameter-smoothing path calls commit() once per sample).
    otaDcCoeff_ = static_cast<float> (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 10.0 / sampleRate_));
    otaDcState_ = 0.0f;

    // Both 4-pole topologies are lowpass-only in the hardware -> LPF24.
    ladder_.prepare (spec);
    ladder_.setMode (juce::dsp::LadderFilterMode::LPF24);

    // 4-pole "4P": two series lowpass TPT SVFs (cutoff+resonance linked in
    // applyParams()).
    svf4p_[0].prepare (spec);
    svf4p_[1].prepare (spec);
    svf4p_[0].setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    svf4p_[1].setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    svf4p_[0].reset (0.0f);
    svf4p_[1].reset (0.0f);

    svf_.prepare (spec);
    svfNotch_.prepare (spec);
    // svfNotch_ is a fixed highpass; svf_'s type is selected per mode in applyParams().
    svfNotch_.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    prepared_       = true;
    topologyChanged_ = true;
    dirty_          = true;
    commit();
}

void AnalogFilter::setTopology (FilterTopology newTopology)
{
    if (newTopology == topology_)
        return;

    topology_       = newTopology;
    topologyChanged_ = true;
    dirty_          = true;
}

void AnalogFilter::setMode (int newMode)
{
    newMode = juce::jlimit (0, 3, newMode);
    const auto m = static_cast<AnalogFilterMode> (newMode);
    if (m == mode_)
        return;

    mode_  = m;
    dirty_ = true;
}

void AnalogFilter::setCutoffHz (float newCutoffHz)
{
    const float nyq  = 0.49f * static_cast<float> (sampleRate_);
    const float cap  = juce::jmin (kMaxHz, nyq);
    newCutoffHz      = juce::jlimit (kMinHz, cap, newCutoffHz);

    // Exact equal is the change test: an unchanged value keeps the cached
    // coefficients.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
    if (newCutoffHz == cutoffHz_)
        return;
#pragma clang diagnostic pop

    cutoffHz_ = newCutoffHz;
    dirty_    = true;
}

void AnalogFilter::setResonance (float newResonance)
{
    newResonance = juce::jlimit (0.0f, 1.0f, newResonance);
    // Exact equal is the change test.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
    if (newResonance == resonance_)
        return;
#pragma clang diagnostic pop

    resonance_ = newResonance;
    dirty_     = true;
}

void AnalogFilter::commit()
{
    if (! prepared_)
        return;

    if (! dirty_)
        return;

    if (topologyChanged_)
    {
        // Reset the newly-active filter so stale state from the previous
        // topology does not produce a click.
        switch (topology_)
        {
            case FilterTopology::FOUR_POLE_SSM2164:
                svf4p_[0].reset (0.0f);
                svf4p_[1].reset (0.0f);
                break;
            case FilterTopology::TWO_POLE_SVF:
                svf_.reset (0.0f);
                svfNotch_.reset (0.0f);
                break;
            case FilterTopology::FOUR_POLE_OTA:
            case FilterTopology::FOUR_POLE_IR3109:
                otaState_[0] = otaState_[1] = otaState_[2] = otaState_[3] = 0.0f;
                otaDcState_ = 0.0f;
                break;
            case FilterTopology::TWO_POLE_POLIVOKS:
                pvS1_ = 0.0f;
                pvS2_ = 0.0f;
                pvPrevBp_ = 0.0f;
                pvPrevLp_ = 0.0f;
                break;
            case FilterTopology::FOUR_POLE_LADDER:
            default:
                ladder_.reset();
                ladder_.setMode (juce::dsp::LadderFilterMode::LPF24);
                break;
        }
        topologyChanged_ = false;
    }

    applyParams();
    dirty_ = false;
}

void AnalogFilter::applyParams()
{
    const float res      = juce::jlimit (0.0f, 1.0f, resonance_);
    const float safeRes  = juce::jmin (res, kMaxResonance);

    if (topology_ == FilterTopology::FOUR_POLE_LADDER)
    {
        ladder_.setCutoffFrequencyHz (cutoffHz_);
        // Resonance remap: JUCE maps knob r to feedback k = 0.4 + 3.6*r
        // internally. ladderResonanceKnob() inverts that offset, so the
        // ladder tracks the ideal law k = 4*knob above knob 0.1. Below 0.1
        // JUCE cannot go under k = 0.4: a dead zone holds that floor.
        ladder_.setResonance (ladderResonanceKnob (safeRes));
        ladder_.setDrive (drive_);   // tanh saturation drive (default 1.2 == JUCE default)
        // No trim, no resonance compensation: the JUCE path is pinned
        // bit-identical (hellcat_analog_filter_batch_test) and its comp = 0.5
        // term already halves the resonance bass drop.
        loudnessGain_ = 1.0f;
    }
    else if (topology_ == FilterTopology::FOUR_POLE_SSM2164)
    {
        // "4P": two series lowpass TPT SVFs. Per-stage Q = 0.5*(1-res)^(-kSsm4PeakExp):
        //   * res 0 -> q = 0.5 per stage: (s^2 + 2ws + w^2)^2 == (s + w)^4,
        //     the EXACT classic 4-pole cascade (24 dB/oct, -12.04 dB at fc).
        //     Every power law keeps this anchor, because (1-0)^(-E) = 1.
        //     The old raw-Q mapping (Q = knob = 0.05) was a droopy overdamped
        //     shelf: up to 38 dB quieter than the sibling cards at low cutoff.
        //   * kSsm4PeakExp = 0.616 -> q(0.95)^2 = 10.0 (+20 dB): the 2-pole
        //     family peak. The earlier sqrt law gave q^2 = 0.25/(1-res) = 5
        //     (+14 dB), 6..8 dB under the family cluster at high knob.
        const double resEff = juce::jlimit (0.0, 1.0, double (safeRes));
        const double q      = 0.5 * std::pow (juce::jmax (1.0e-6, 1.0 - resEff), -kSsm4PeakExp);
        for (int i = 0; i < 2; ++i)
        {
            svf4p_[i].setCutoffFrequency (cutoffHz_);
            svf4p_[i].setResonance (static_cast<float> (q));
        }
        loudnessGain_ = kSsm2164CardGain;
    }
    else if (topology_ == FilterTopology::FOUR_POLE_OTA || topology_ == FilterTopology::FOUR_POLE_IR3109)
    {
        // OTA-cascade family coefficients (SMR4 / IR3109): SAME structure,
        // per-card constants. All math in double; stored as float.
        // The resonance cap kMaxResonance does NOT apply here: both models are
        // bounded by their tanh stages. The SMR4 lands resonance 1.0 exactly
        // at the self-oscillation onset by design (kfb*G^4 == res). The
        // IR3109 caps kfb at 3.4, BELOW the 4.0 onset: the factory-capped
        // Juno character. The card never self-oscillates at any setting.
        const bool   ir3109  = (topology_ == FilterTopology::FOUR_POLE_IR3109);
        const double nyq     = 0.49 * sampleRate_;
        const double fc      = juce::jlimit (double (kMinHz), juce::jmin (double (kMaxHz), nyq), double (cutoffHz_));
        const double pi      = juce::MathConstants<double>::pi;
        const double gLin    = std::tan (pi * fc / sampleRate_);   // exact TPT pole conductance
        const double G       = gLin / (1.0 + gLin);
        // Stage knee = card knee base x the SOFT drive law (1.2/drive)^(1/3),
        // anchored at drive 1.2: knee(1.2) == kneeBase/1.2 exactly (the
        // documented default-drive calibration is unchanged). The old linear
        // law (base/drive) collapsed the saturated output proportional to
        // 1/drive. At Filter Drive 12 the card sat ~16 dB below every other
        // card. The cube root softens that collapse. The saturation threshold
        // still tracks the knob. The SMR4 uses the LM13700 2Vt (0.052); the
        // IR3109 runs 0.10 (the cleaner Juno headroom).
        const double kneeBase = (ir3109 ? kIr3109TwoVt : double (kTwoVt)) * kOtaCoreScale;
        const double knee     = (kneeBase / 1.2) * std::cbrt (1.2 / juce::jmax (0.1, double (drive_)));
        // Loop-gain normalisation. The self-oscillation onset of four
        // identical bilinear one-pole stages with gain feedback is EXACTLY
        // kfb = 4.0, at every cutoff:
        //   * analog: (1 + s/w0)^4 + k = s^4/w0^4 + 4s^3/w0^3 + 6s^2/w0^2 +
        //     4s/w0 + (1 + k) is jw-axis marginal at k = 4 (Routh condition
        //     a3a2a1 = a3^2 a0 + a1^2 evaluates to 96 = 16(1+k) + 16);
        //   * the TPT stage is the exact bilinear image of the analog one-pole
        //     (pole 1-2G, zero at Nyquist), so the discrete onset equals the
        //     analog one: the char roots of (z-(1-2G))^4 + kfb*G^4*(z+1)^4 = 0
        //     touch |z| = 1 exactly at kfb = 4 (verified to machine precision
        //     against a polynomial-root solve).
        // kfb = res * kfbMax therefore puts the SMR4 onset exactly at res = 1.0
        // for every cutoff: the knob tracks the onset. (The task draft already
        // carried res*4.0; the constant is exact, not an approximation.) The
        // IR3109 caps kfbMax at 3.4: 85 percent of the onset. The seeded ring
        // at res = 1.0 decays with a few-ms time constant — the famous
        // factory cap, this card never screams.
        const double resEff = juce::jlimit (0.0, 1.0, double (resonance_));
        const double kfbMax = ir3109 ? kIr3109KfbMax : 4.0;
        double kfb          = resEff * kfbMax;
        kfb                 = juce::jmin (kfb, kKfbHardMax);   // numeric guard (vestigial: kfb <= 4)
        const double r      = kfb * G * G * G * G;              // linear-solve denominator term

        gLin_        = static_cast<float> (gLin);
        G_           = static_cast<float> (G);
        G2_          = static_cast<float> (G * G);
        G3_          = static_cast<float> (G * G * G);
        G4_          = static_cast<float> (G * G * G * G);
        gk_          = static_cast<float> (gLin * knee);
        knee_        = static_cast<float> (knee);
        invKnee_     = static_cast<float> (1.0 / knee);
        kfb_         = static_cast<float> (kfb);
        // VCA soft-clip pair: kfbVca_ = kfb * vcaKnee with the fixed VCA knee
        // (the resonance path has its own OTA; its knee does not follow the
        // Filter Drive knob, which shapes the four pole stages). SMR4: the
        // VCA clips at its own 2Vt. IR3109: kIr3109VcaKnee*2Vt, a milder
        // feedback path (the thinner Juno high-Q character). The unit-slope
        // form keeps the small-signal loop gain exactly kfb on both cards.
        const double vcaKnee = ((ir3109 ? kIr3109VcaKnee : 1.0) * double (kTwoVt)) * kOtaCoreScale;
        kfbVca_      = static_cast<float> (kfb * vcaKnee);
        vcaKnee_     = static_cast<float> (vcaKnee);
        invVcaKnee_  = static_cast<float> (1.0 / vcaKnee);
        invOnePlusR_ = static_cast<float> (1.0 / (1.0 + r));
        // Loudness trim: static card calibration times a partial resonance
        // compensation. The feedback loop thins the bass by 1/(1+kfb) (DC
        // gain) and the stage saturation deepens with the loop signal, so a
        // hot program sags badly at high resonance. (1+kfb)^exp restores part
        // of that sag: the card tracks the Q-based cards (which keep DC gain
        // 1) within the parity bounds without boosting the resonant peak to
        // the full (1+kfb) gain. The IR3109 takes the stronger exponent: its
        // factory kfb cap leaves its ring suppressed, so it sags more.
        loudnessGain_ = static_cast<float> ((ir3109 ? kIr3109CardGain : kSmr4CardGain)
                                            * std::pow (1.0 + kfb,
                                                        ir3109 ? kIr3109ResCompExp : kSmr4ResCompExp));
    }
    else if (topology_ == FilterTopology::TWO_POLE_POLIVOKS)
    {
        // Polivoks SVF coefficients. All math in double; stored as float.
        // The resonance cap kMaxResonance does NOT apply here: the model is
        // bounded by its rails, and resonance 1.0 lands exactly at the
        // self-oscillation onset by design (R = 0).
        const double nyq    = 0.49 * sampleRate_;
        const double fc     = juce::jlimit (double (kMinHz), juce::jmin (double (kMaxHz), nyq), double (cutoffHz_));
        const double pi     = juce::MathConstants<double>::pi;
        const double gLin   = std::tan (pi * fc / sampleRate_);   // exact TPT integrator conductance
        // Damping map: R = 2*(1 - res). The constant is EXACT, not a fit:
        //   * linearised loop: u = x - R*ybp - ylp; ybp = s1 + g*u; ylp = s2 + g*ybp
        //     gives the analog H_lp = w^2/(s^2 + R*w*s + w^2) and H_bp = w*s/(...),
        //     so analog Q = 1/R: res 0 -> Q 0.5, res -> 1 -> Q -> infinity;
        //   * discrete: the trapezoid stage is g*(z+1)/(z-1); on |z| = 1 it is
        //     -j*g*cot(theta/2). The char poly 1 + R*I + I^2 = 0 becomes
        //     (1 - a^2) - j*R*a = 0 with a = g*cot(theta/2). Roots need a = 1
        //     (oscillation at EXACTLY fc) AND R = 0. So R = 0 (res = 1.0) is
        //     the exact onset at every cutoff, and roots sit strictly inside
        //     the unit circle for every R > 0 (verified to machine precision
        //     against the quadratic (z-1)^2 + R*g*(z+1)(z-1) + g^2*(z+1)^2).
        // The character layer sits on this linear skeleton. Every shaper has
        // slope 1 at the origin, so small-signal tuning is unchanged.
        const double resEff = juce::jlimit (0.0, 1.0, double (resonance_));
        const double R      = 2.0 * (1.0 - resEff);
        // Damping sag: kR = R*(1 - 0.12*res^2). High resonance removes damping
        // beyond the linear map, so the loop breaks up into clipping instead
        // of a clean limit cycle. At res = 1.0 the sag scales R = 0, so the
        // onset stays exact.
        const double kR     = R * (1.0 - 0.12 * resEff * resEff);

        // Character constants. The drive factor maps Filter Drive 1.2 to
        // unity. More drive lowers every clip point and every rate limit.
        const double driveNorm = juce::jmax (0.05 / 1.2, double (drive_) / 1.2);
        // Stage shapers: op-amp output rails. The negative side clips 22
        // percent lower and holds a steeper shoulder: real parts are not
        // symmetric, and the asymmetry makes even harmonics.
        const double kneeP = 0.90 / driveNorm;
        const double kneeN = 0.78 * kneeP;
        pvShapeP_.setup (kneeP, 0.08);
        pvShapeN_.setup (kneeN, 0.12);
        // Return shapers: the diode-limited resonance path. They clip at half
        // the stage knee with a harder shoulder, so the resonance breaks up
        // before the stages rail. This makes the low-cutoff growl.
        pvReturnP_.setup (0.50 * kneeP, 0.05);
        pvReturnN_.setup (0.50 * kneeN, 0.08);
        // Input offset: op-amp bias current into the first integrator.
        const double dc = 0.008 * kneeP;
        // Output rate caps at unity drive, in units per sample. Calibration:
        // a full-rail ring at 1 kHz and 48 kHz demands 2*pi*f/fs = 0.131 per
        // sample, so 0.05 limits mid-cutoff resonance mildly; hot high-
        // frequency content and rail corners engage it hard.
        const double slewP = (0.05 * 48000.0 / sampleRate_) / driveNorm;

        pvGLin_       = static_cast<float> (gLin);
        pvR_          = static_cast<float> (R);
        pvKrg_        = static_cast<float> (kR);
        pvInvDen_     = static_cast<float> (1.0 / (1.0 + gLin * kR + gLin * gLin));
        pvInvDenLin_  = static_cast<float> (1.0 / (1.0 + gLin * R + gLin * gLin));
        pvDc_         = static_cast<float> (dc);
        pvSlewP_      = static_cast<float> (slewP);
        pvKneeP_      = static_cast<float> (kneeP);
        pvKneeN_      = static_cast<float> (kneeN);
        pvInvTwoKneeP_ = static_cast<float> (0.5 / kneeP);
        pvInvTwoKneeN_ = static_cast<float> (0.5 / kneeN);
        // The real card provides LP and BP outputs only. Highpass and Notch
        // clamp to LP (documented in the header). The voice passes the patch
        // mode through; the model picks its tap here.
        pvBandpass_ = (mode_ == AnalogFilterMode::Bandpass);
        loudnessGain_ = kPolivoksCardGain;
    }
    else // TWO_POLE_SVF
    {
        // Q = 1/(2*(1-res)): the SAME law as the Polivoks card (the other
        // 2-pole). Q 0.5 at knob 0 (the classic minimum, -6 dB at fc),
        // onset exactly at 1.0, capped at kMaxResonance (Q 10, no
        // self-oscillation). The old raw-Q mapping (Q = knob) ran Q 0.05 at
        // knob 0: a droopy shelf far below every sibling card, and it never
        // resonated (Q < 1 at the maximum). JUCE computes R2 = 1/Q internally,
        // so Q >= 0.5 keeps R2 finite and the filter stable.
        const double resEff = juce::jlimit (0.0, 1.0, double (safeRes));
        const float svfQ    = static_cast<float> (0.5 / juce::jmax (1.0e-6, 1.0 - resEff));
        svf_.setCutoffFrequency (cutoffHz_);
        svf_.setResonance (svfQ);
        svfNotch_.setCutoffFrequency (cutoffHz_);
        svfNotch_.setResonance (svfQ);
        loudnessGain_ = kSvfCardGain;

        // svfNotch_ stays highpass; pick svf_'s tap by mode.
        switch (mode_)
        {
            case AnalogFilterMode::Bandpass:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
                break;
            case AnalogFilterMode::Highpass:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::highpass);
                break;
            case AnalogFilterMode::Notch:
                // notch = lowpass(svf_) + highpass(svfNotch_)
                svf_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
                break;
            case AnalogFilterMode::Lowpass:
            default:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
                break;
        }
    }
}

float AnalogFilter::processSample (float inputValue)
{
    if (! prepared_)
        return inputValue;

    if (topology_ == FilterTopology::FOUR_POLE_SSM2164)
    {
        // Cascade: stage 0 then stage 1 (both lowpass). 24 dB/oct, linear.
        const float a = svf4p_[0].processSample (0, inputValue);
        return loudnessGain_ * svf4p_[1].processSample (0, a);
    }

    if (topology_ == FilterTopology::FOUR_POLE_OTA || topology_ == FilterTopology::FOUR_POLE_IR3109)
    {
        // OTA family: model output -> loudness trim -> DC blocker. The blocker
        // is the standard one-pole form H(z) = (1 - z^-1)/(1 - (1-k) z^-1):
        // the state accumulates the BLOCKED OUTPUT (accummulating the input
        // instead would put a marginal pole at z = 1 and oscillate).
        const float y = loudnessGain_ * processOTASample (inputValue);
        const float out = y - otaDcState_;
        otaDcState_ += otaDcCoeff_ * out;
        return out;
    }

    if (topology_ == FilterTopology::TWO_POLE_POLIVOKS)
        return loudnessGain_ * processPolivoksSample (inputValue);

    if (topology_ == FilterTopology::TWO_POLE_SVF)
    {
        // NOTE: juce::dsp::StateVariableTPTFilter::processSample(channel, input).
        // Lowpass / Bandpass / Highpass all take the single svf_ call (the
        // filter is constructed in the matching mode); Notch sums the svf_
        // lowpass with the svfNotch_ highpass.
        if (mode_ == AnalogFilterMode::Notch)
            return loudnessGain_ * (svf_.processSample (0, inputValue)
                                  + svfNotch_.processSample (0, inputValue));
        return loudnessGain_ * svf_.processSample (0, inputValue);
    }

    // 4-pole. Direct per-sample call through the LadderTap: JUCE's public
    // process() runs `updateSmoothers(); processSample (v, ch);` per sample,
    // so this reproduces the exact per-sample sequence of the legacy 1-sample
    // AudioBlock + ProcessContextReplacing routing (bit-identical output —
    // pinned by hellcat_analog_filter_batch_test) without the per-sample
    // block/context construction. loudnessGain_ stays 1.0 on this path (x * 1
    // is bit-identical to x), so the pin holds.
    ladder_.updateSmoothers();
    return ladder_.processSample (inputValue, 0);
}

float AnalogFilter::processOTASample (float x) noexcept
{
    // OTA-cascade family (SMR4 / IR3109): four stages, each an OTA
    // (gm*tanh on the error) into an integrating capacitor. The two cards
    // share this sample code; only the coefficients differ (see applyParams).
    // The model solves each stage with one Newton step (the linear warm start
    // makes one step sufficient in both the linear and the saturated zone),
    // and closes the resonance loop with a linearised delay-free-loop solve
    // plus a resonance-VCA tanh.
    //
    // Stage equation (implicit trapezoid):  y = s + gk*tanh((x - y)/knee)
    // Linearised stage (the feedback solve): y = (1 - G)*s + G*x

    // 1) Linearised delay-free-loop solve. sigma carries the (1 - G) state
    //    weights: y4_lin = (G^4*x + sigma) / (1 + kfb*G^4). The draft dropped
    //    the (1 - G) weights; the exact form keeps the resonance prediction
    //    accurate across the cutoff range.
    const float sigma = (1.0f - G_) * (G3_ * otaState_[0] + G2_ * otaState_[1]
                                     +  G_  * otaState_[2] +      otaState_[3]);
    const float y4lin = (G4_ * x + sigma) * invOnePlusR_;

    // 2) Resonance feedback through its own VCA soft clip. The SMR4 routes
    //    the resonance signal through OTA/VCA sections (the spare LM13700
    //    sections on the schematic). The VCA tanh uses the UNIT-SLOPE form
    //    kfb*vcaKnee*tanh(y4/vcaKnee): its small-signal gain is exactly kfb,
    //    so the loop gain stays res (the plain kfb*tanh(y4/knee) form would
    //    scale the loop by 1/knee = ~23 and move the onset to res ~ 0.05).
    //    The IR3109 runs the same form with vcaKnee = 3*2Vt: a milder clip.
    const float u = x - kfbVca_ * std::tanh (y4lin * invVcaKnee_);

    // 3) Four OTA stages. Each: linear warm start, one Newton step on
    //    F(y) = y - s - gk*tanh((x-y)/knee), then the trapezoid state update
    //    s' = 2y - s (F(y) = 0 implies s' = y + gk*tanh = 2y - s).
    float y = u;
    for (int i = 0; i < 4; ++i)
    {
        const float s   = otaState_[i];
        const float y0  = s + G_ * (y - s);                  // linear warm start
        const float t0  = std::tanh ((y - y0) * invKnee_);   // tanh at the warm start
        const float F   = y0 - s - gk_ * t0;
        const float dF  = 1.0f + gLin_ * (1.0f - t0 * t0);   // F'(y) = 1 + (gk/knee)*(1-t^2)
        y               = y0 - F / dF;
        otaState_[i]    = 2.0f * y - s;
    }

    // Denormal flush: a decaying tail drives the capacitor states into the
    // subnormal range, where CPUs stall. Zero them (inaudible: below -290 dB).
    for (int i = 0; i < 4; ++i)
        if (std::fabs (otaState_[i]) < std::numeric_limits<float>::min())
            otaState_[i] = 0.0f;

    return y;
}

void AnalogFilter::PvShaper::setup (double kneeIn, double sIn) noexcept
{
    // Region layout, v >= 0:
    //   [0, knee]            slope 1        (linear zone)
    //   [knee, knee+w]       cubic ease 1 -> s
    //   [knee+w, knee+w+sh]  shoulder, slope s (the hard shoulder)
    //   [.., +railT]         cubic ease s -> 0
    //   beyond               flat rail at phi3 (the supply clamp)
    // w = 25 percent of the knee, the shoulder spans 2*w, the rail ease is
    // 4 percent of the knee. phi1..3 are the shaper values at the region
    // boundaries: the cubic integral of the slope across each ease region.
    knee = static_cast<float> (kneeIn);
    s    = static_cast<float> (sIn);
    w    = static_cast<float> (0.25 * kneeIn);
    sh   = static_cast<float> (0.50 * kneeIn);
    railT = static_cast<float> (0.04 * kneeIn);
    phi1 = static_cast<float> (kneeIn + 0.25 * kneeIn * (1.0 + sIn) * 0.5);
    phi2 = static_cast<float> (kneeIn + 0.25 * kneeIn * (1.0 + sIn) * 0.5 + sIn * 0.50 * kneeIn);
    phi3 = static_cast<float> (kneeIn + 0.25 * kneeIn * (1.0 + sIn) * 0.5 + sIn * 0.50 * kneeIn + sIn * 0.02 * kneeIn);
}

float AnalogFilter::PvShaper::eval (float v) const noexcept
{
    if (v <= knee)
        return v;
    const float v1 = knee + w;
    if (v <= v1)
    {
        // Cubic ease of the slope from 1 to s. Integral of
        // 1 - (1-s)*(3u^2 - 2u^3) over u in [0, 1] scaled by w.
        const float u = (v - knee) / w;
        return knee + w * (u - (1.0f - s) * (u * u * u - 0.5f * u * u * u * u));
    }
    const float v2 = v1 + sh;
    if (v <= v2)
        return phi1 + s * (v - v1);
    const float v3 = v2 + railT;
    if (v <= v3)
    {
        // Cubic ease of the slope from s to 0.
        const float u = (v - v2) / railT;
        return phi2 + railT * s * (u - (u * u * u - 0.5f * u * u * u * u));
    }
    return phi3;   // flat rail
}

float AnalogFilter::PvShaper::slope (float v) const noexcept
{
    if (v <= knee)
        return 1.0f;
    const float v1 = knee + w;
    if (v <= v1)
    {
        const float u = (v - knee) / w;
        const float smooth = u * u * (3.0f - 2.0f * u);
        return 1.0f - (1.0f - s) * smooth;
    }
    const float v2 = v1 + sh;
    if (v <= v2)
        return s;
    const float v3 = v2 + railT;
    if (v <= v3)
    {
        const float u = (v - v2) / railT;
        const float smooth = u * u * (3.0f - 2.0f * u);
        return s * (1.0f - smooth);
    }
    return 0.0f;
}

float AnalogFilter::pvPhi (float v) const noexcept
{
    return (v >= 0.0f) ? pvShapeP_.eval (v) : -pvShapeN_.eval (-v);
}

float AnalogFilter::pvPhiSlope (float v) const noexcept
{
    return (v >= 0.0f) ? pvShapeP_.slope (v) : pvShapeN_.slope (-v);
}

float AnalogFilter::pvReturn (float v) const noexcept
{
    return (v >= 0.0f) ? pvReturnP_.eval (v) : -pvReturnN_.eval (-v);
}

float AnalogFilter::pvReturnSlope (float v) const noexcept
{
    return (v >= 0.0f) ? pvReturnP_.slope (v) : pvReturnN_.slope (-v);
}

float AnalogFilter::processPolivoksSample (float x) noexcept
{
    // Polivoks SVF with the op-amp character layer. The linear skeleton is
    // the shipped model; the equations with the character terms:
    //   u   = x + pvDc_ - pvKrg_*ybp - pvS2_ - pvGLin_*psi(ybp)
    //   ybp = pvS1_ + pvGLin_*phi(u)             (implicit: u holds ybp)
    //   ylp = pvS2_ + pvGLin_*phi(ybp)
    //   outputs: rate-limited ybp and ylp; states track the limited values.
    // phi = the asymmetric op-amp stage shapers; psi = the harder diode-style
    // resonance-return shapers; pvKrg_ = the sagged damping; pvDc_ = the
    // input offset. All shapers hold slope 1 at the origin, so the small-
    // signal pole tuning and the exact R = 0 onset are unchanged.

    if (pvTestLinear_)
    {
        // TEST-ONLY reference: the pure linear skeleton.
        const float ybp = (pvS1_ + pvGLin_ * (x - pvS2_)) * pvInvDenLin_;
        const float ylp = pvS2_ + pvGLin_ * ybp;
        pvS1_ = 2.0f * ybp - pvS1_;
        pvS2_ = 2.0f * ylp - pvS2_;
        return pvBandpass_ ? ybp : ylp;
    }

    // 1) Linear warm start: the exact solve of the linearised loop with the
    //    sagged damping, clamped into the bracket. The bracket bounds the
    //    root because phi is flat at the rails: the root obeys
    //    |ybp - s1| <= g*max(phi3P, phi3N).
    const float lo = pvS1_ - pvGLin_ * juce::jmax (pvShapeP_.phi3, pvShapeN_.phi3);
    const float hi = pvS1_ + pvGLin_ * juce::jmax (pvShapeP_.phi3, pvShapeN_.phi3);
    float ybp = juce::jlimit (lo, hi, (pvS1_ + pvGLin_ * (x + pvDc_ - pvS2_)) * pvInvDen_);
    float a = lo, b = hi;

    // 2) Safeguarded Newton on F(ybp) = ybp - s1 - g*phi(u(ybp)).
    //    F' = 1 + g*phi'(u)*(kR + g*psi'(ybp)) >= 1: F is strictly increasing,
    //    every slope is in [0, 1] and kR >= 0. A step that leaves the bracket
    //    (or is NaN) takes the bisection point instead. The ! (.. && ..)
    //    form keeps NaN inside the fallback.
    const float tol = 1.0e-6f * (1.0f + pvGLin_);
    for (int it = 0; it < kPvMaxIters; ++it)
    {
        const float pr = pvReturn (ybp);
        const float u  = x + pvDc_ - pvKrg_ * ybp - pvS2_ - pvGLin_ * pr;
        const float F  = ybp - pvS1_ - pvGLin_ * pvPhi (u);
        if (std::fabs (F) < tol)
            break;
        if (F > 0.0f)  b = juce::jmin (b, ybp);
        else           a = juce::jmax (a, ybp);
        const float dF = 1.0f + pvGLin_ * pvPhiSlope (u) * (pvKrg_ + pvGLin_ * pvReturnSlope (ybp));
        float yn = ybp - F / dF;
        if (! (yn > a && yn < b))
            yn = 0.5f * (a + b);
        ybp = yn;
    }

    // 3) Stage-2 output with the solved ybp.
    const float pbF = pvPhi (ybp);
    const float uF  = x + pvDc_ - pvKrg_ * ybp - pvS2_ - pvGLin_ * pvReturn (ybp);
    const float ylp = pvS2_ + pvGLin_ * pbF;

    // 4) Rate limits on both node outputs. The cap tightens with the input
    //    overdrive of the driving stage (an input stage pushed past its
    //    linear range recovers slowly). Inside the linear zone the gate
    //    holds at the floor, so small signals never rate limit.
    auto rateLimit = [] (float cur, float prev, float limUp, float limDown)
    {
        const float dy = cur - prev;
        return prev + (dy > limUp ? limUp : (dy < -limDown ? -limDown : dy));
    };
    const float odU = juce::jlimit (pvSlewFloor_, 1.0f, (std::fabs (uF) - pvKneeP_) * pvInvTwoKneeP_);
    const float odB = juce::jlimit (pvSlewFloor_, 1.0f, (std::fabs (ybp) - pvKneeN_) * pvInvTwoKneeN_);
    const float limB = pvSlewP_ / odU;   // ybp node, gated by stage-1 input
    const float limL = pvSlewP_ / odB;   // ylp node, gated by stage-2 input
    const float ybpS = rateLimit (ybp, pvPrevBp_, limB, pvSlewDown_ * limB);
    const float ylpS = rateLimit (ylp, pvPrevLp_, limL, pvSlewDown_ * limL);

    // 5) Trapezoid state updates from the limited nodes, the supply clamp,
    //    and the denormal flush. The clamp models the supplies: no node of a
    //    real card swings past them. It stops the slow low-frequency drift
    //    (motorboating) that the rate limiter's phase lag can start at high
    //    resonance with a hot input. Small signals never reach it.
    pvS1_ = juce::jlimit (-pvNodeClamp_, pvNodeClamp_, 2.0f * ybpS - pvS1_);
    pvS2_ = juce::jlimit (-pvNodeClamp_, pvNodeClamp_, 2.0f * ylpS - pvS2_);
    pvPrevBp_ = ybpS;
    pvPrevLp_ = ylpS;
    if (std::fabs (pvS1_) < std::numeric_limits<float>::min())  pvS1_ = 0.0f;
    if (std::fabs (pvS2_) < std::numeric_limits<float>::min())  pvS2_ = 0.0f;

    return pvBandpass_ ? ybpS : ylpS;
}

void AnalogFilter::processBlock (float* data, int numSamples)
{
    if (! prepared_ || data == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
        data[i] = processSample (data[i]);
}

float AnalogFilter::cutoffByteToHz (uint8_t cutoffByte)
{
    // Exponential sweep across [kMinHz, kMaxHz]. cutoffByte spans the full 8-bit
    // range 0..255 (the firmware's 7-bit value is left-shifted/aligned by the
    // caller if desired); either way this is an exponential Hz mapping.
    const float t = static_cast<float> (cutoffByte) / 255.0f;
    const float hz = kMinHz * std::pow (kMaxHz / kMinHz, t);
    return juce::jlimit (kMinHz, kMaxHz, hz);
}

float AnalogFilter::ladderResonanceKnob (float knob)
{
    // Inverse of JUCE's internal map (feedback k = 0.4 + 3.6*r): return the
    // knob value whose feedback equals the ideal law 4*knob. Clamp to
    // [0, 1]: below knob 0.1 the value goes negative, and JUCE floors the
    // feedback at k = 0.4 there (the documented dead zone).
    const float remapped = (4.0f * knob - 0.4f) / 3.6f;
    return juce::jlimit (0.0f, 1.0f, remapped);
}

} // namespace ambika::dsp
