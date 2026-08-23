// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Spring implementation — two dispersive AP-cascade spring loops.

#include "dsp/fx/fv1/Fv1Spring.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// 2*2048 + 12*64 = 4864 words of the 32768 budget.
static_assert (2 * 2048 + 12 * 64 <= kMaxMemorySamples,
               "Fv1Spring within the FV-1 RAM budget");

void Fv1Spring::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    // Decay 0.2..4 s -> PER-SPRING loop feedback
    //   g_s = 10^(-3*D_s/(decay*fs)) * (1 - 0.25*Chirp),
    // the per-pass RT60 law: g_s applies once per loop pass (once every D_s
    // samples), so t60 == Decay exactly at Chirp 0. D_s = delay + the six AP
    // lengths (a Schroeder AP's DC group delay is exactly its length, so D_s
    // is exact for the LF-dominated tail an EDC measures): A = 1146+204 =
    // 1350, B = 1123+210 = 1333 samples. The chirp back-off trades tail
    // length for dispersion (t60 shortens as Chirp rises).
    // The old per-SAMPLE law 10^(-3/(decay*fs)) could never lift the 0.97
    // clamp (it needed decay > 6.93 s > the 4.0 max), so the knob was INERT
    // at every setting and chirp alone set t60 (9.3 s at Chirp 0!);
    // audit/fx_review_20260819/rev_reverbs.md.
    const float decay = 0.2f + p0 * 3.8f;
    for (int s = 0; s < 2; ++s)
    {
        int dLoop = kSpringDelay[s];
        for (int i = 0; i < 6; ++i)
            dLoop += kApLen[s][i];
        float g = std::pow (10.0f,
                            -3.0f * static_cast<float> (dLoop) / (decay * 32768.0f));
        // The AP cascade is unity-gain even at max chirp, but back the raw
        // loop gain off anyway so full Chirp stays comfortably stable.
        g *= (1.0f - 0.25f * p2);
        g = std::clamp (g, 0.0f, 0.97f);
        fb14_[s] = q14 (g);
    }

    const float fc = 500.0f * std::pow (16.0f, p1);       // 500..8000 Hz
    damp_[0].setCutoff (fc);
    damp_[1].setCutoff (fc);
    chirp14_ = q14 (0.35f + 0.6f * p2);                   // AP coefficient
    width14_    = q14 (p3);
    invWidth14_ = q14 (1.0f - p3);
}

void Fv1Spring::resetInternal()
{
    for (int s = 0; s < 2; ++s)
    {
        delay_[s].clear();
        for (auto& ap : aps_[s]) ap.clear();
        damp_[s].clear();
        dck_[s].clear();
    }
}

// One spring loop iteration: returns the pickup (post-AP-cascade) sample.
static inline int32_t springLoop (DelayLine<2048>& delay, int delayLen,
                                  DelayLine<64> (&aps) [6], const int (&apLen) [6],
                                  OnePoleLpFx& damp, LoopDcKiller& dck, int32_t driver,
                                  int16_t fb14, int16_t chirp14)
{
    const int32_t read = delay.readFrac (static_cast<float> (delayLen));
    int32_t y = read;
    // The dispersive cascade: each short Schroeder AP with a HIGH coefficient
    // delays HF more than LF — the chirp/boing. Same table-driven arithmetic
    // as the Room APs but with the chirp coefficient.
    for (int i = 0; i < 6; ++i)
    {
        const int32_t r = aps[i].read (apLen[i]);
        const int32_t o = f24_addSat (-f24_mulk (y, chirp14), r);
        aps[i].write (f24_addSat (y, f24_mulk (o, chirp14)));
        y = o;
    }
    const int32_t d = dck.process (damp.process (y));   // loop DC killer
    delay.write (f24_addSat (driver, f24_mulk (d, fb14)));
    return y;
}

void Fv1Spring::processSampleFx (int32_t lin, int32_t /*rin*/,
                                 int32_t& lout, int32_t& rout)
{
    // Driver: cubic soft clip (the spring's driven transducer — hits its
    // travel limit on hard excitation, which is where the boing comes from).
    auto driver = [] (int32_t x) -> int32_t
    {
        const int32_t v = f24_sat (x);
        const int32_t v2 = f24_mul (v, v);
        const int32_t v3 = f24_mul (v2, v);
        return f24_sat (f24_addSat (v, -f24_mulk (v3, q14 (0.34f))));
    };
    const int32_t drv = driver (lin);

    const int32_t a = springLoop (delay_[0], kSpringDelay[0], aps_[0], kApLen[0],
                                  damp_[0], dck_[0], drv, fb14_[0], chirp14_);
    const int32_t b = springLoop (delay_[1], kSpringDelay[1], aps_[1], kApLen[1],
                                  damp_[1], dck_[1], drv, fb14_[1], chirp14_);

    // Width: L is always spring A. R crossfades from the L signal (TRUE mono
    // at 0) to spring B (fully decorrelated at 1) — mirrors Fv1Room's
    // doc-conformant pattern. w == 0 is special-cased to a bit-exact copy so
    // mono is EXACT (the crossfade path would scale by q14(1) = 8191/8192,
    // and the pre-fix code left two DECORRELATED springs at w=0 — a stereo
    // pair, not the documented "one spring (mono)"; audit rev_reverbs.md).
    const int32_t outL = a;
    int32_t outR;
    if (width14_ == 0)
        outR = outL;
    else
        outR = f24_addSat (f24_mulk (outL, invWidth14_),
                           f24_mulk (b, width14_));
    lout = outL;
    rout = outR;
}

} // namespace parvati::fv1
