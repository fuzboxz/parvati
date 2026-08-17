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

void Fv1Spring::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float decay = 0.2f + p0 * 3.8f;
    float g = std::pow (10.0f, -3.0f / (decay * 32768.0f));
    // The AP cascade adds loop gain energy at high chirp; back the raw comb
    // feedback off so the loop stays comfortably stable at full Chirp.
    g *= (1.0f - 0.25f * p2);
    g = std::clamp (g, 0.0f, 0.97f);
    fb14_ = q14 (g);

    const float fc = 500.0f * std::pow (16.0f, p1);       // 500..8000 Hz
    damp_[0].setCutoff (fc);
    damp_[1].setCutoff (fc);
    chirp14_ = q14 (0.35f + 0.6f * p2);                   // AP coefficient
    width14_ = q14 (p3);
}

void Fv1Spring::resetInternal()
{
    for (int s = 0; s < 2; ++s)
    {
        delay_[s].clear();
        for (auto& ap : aps_[s]) ap.clear();
        damp_[s].clear();
    }
}

// One spring loop iteration: returns the pickup (post-AP-cascade) sample.
static inline int32_t springLoop (DelayLine<2048>& delay, int delayLen,
                                  DelayLine<64> (&aps) [6], const int (&apLen) [6],
                                  OnePoleLpFx& damp, int32_t driver,
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
    const int32_t d = damp.process (y);
    delay.write (f24_addSat (driver, f24_mulk (d, fb14)));
    return y;
}

void Fv1Spring::processSampleFx (int32_t lin, int32_t /*rin*/,
                                 int32_t& lout, int32_t& rout)
{
    // Driver: cubic soft clip (the spring's driven transducer — slams into
    // its travel limit on hard hits, which is where the boing comes from).
    auto driver = [] (int32_t x) -> int32_t
    {
        const int32_t v = f24_sat (x);
        const int32_t v2 = f24_mul (v, v);
        const int32_t v3 = f24_mul (v2, v);
        return f24_sat (f24_addSat (v, -f24_mulk (v3, q14 (0.34f))));
    };
    const int32_t drv = driver (lin);

    const int32_t a = springLoop (delay_[0], kSpringDelay[0], aps_[0], kApLen[0],
                                  damp_[0], drv, fb14_, chirp14_);
    const int32_t b = springLoop (delay_[1], kSpringDelay[1], aps_[1], kApLen[1],
                                  damp_[1], drv, fb14_, chirp14_);

    // Width: L = spring A + width*B*0.5, R = B + width*A*0.5 (a mono pair at
    // 0, a cross-fed stereo pair at 1). 4096 == q14(0.5), a compile-time const.
    constexpr int16_t half14 = 4096;
    const int32_t outL = f24_addSat (a, f24_mulk (f24_mulk (b, half14), width14_));
    const int32_t outR = f24_addSat (b, f24_mulk (f24_mulk (a, half14), width14_));
    lout = outL;
    rout = outR;
}

} // namespace parvati::fv1
