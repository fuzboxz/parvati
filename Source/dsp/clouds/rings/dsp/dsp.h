// Copyright 2015 Emilie Gillet. See rings/dsp/resonator.h for full MIT notice.
//
// Minimal stub of rings/dsp/dsp.h — only the constants the resonator references.
// Vendored alongside the Rings modal resonator for Hellcat's per-part FX system.
// kSampleRate is only used as the default frequency denominator in Init(); the
// FX overrides frequency (normalised) before the first Process at the host rate.

#ifndef RINGS_DSP_DSP_H_
#define RINGS_DSP_DSP_H_

namespace rings {

static const float kSampleRate = 48000.0f;

}  // namespace rings

#endif  // RINGS_DSP_DSP_H_
