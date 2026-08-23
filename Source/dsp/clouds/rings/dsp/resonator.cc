// Copyright 2015 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Resonator.

#include "rings/dsp/resonator.h"

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/cosine_oscillator.h"
#include "stmlib/dsp/parameter_interpolator.h"

#include "rings/resources.h"

namespace rings {

using namespace std;
using namespace stmlib;

void Resonator::Init() {
  for (int32_t i = 0; i < kMaxModes; ++i) {
    f_[i].Init();
  }

  set_frequency(220.0f / kSampleRate);
  previous_frequency_ = frequency_;
  set_structure(0.25f);
  set_brightness(0.5f);
  set_damping(0.3f);
  set_position(0.999f);
  previous_position_ = 0.0f;
  set_resolution(kMaxModes);
}

int32_t Resonator::ComputeFilters() {
  float stiffness = Interpolate(lut_stiffness, structure_, 256.0f);
  float harmonic = frequency_;
  float stretch_factor = 1.0f; 
  float q = 500.0f * Interpolate(
      lut_4_decades,
      damping_,
      256.0f);
  float brightness_attenuation = 1.0f - structure_;
  // Reduces the range of brightness when structure is very low, to prevent
  // clipping.
  brightness_attenuation *= brightness_attenuation;
  brightness_attenuation *= brightness_attenuation;
  brightness_attenuation *= brightness_attenuation;
  float brightness = brightness_ * (1.0f - 0.2f * brightness_attenuation);
  float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
  float q_loss_damping_rate = structure_ * (2.0f - structure_) * 0.1f;
  int32_t num_modes = 0;
  for (int32_t i = 0; i < min(kMaxModes, resolution_); ++i) {
    float partial_frequency = harmonic * stretch_factor;
    if (partial_frequency >= 0.49f) {
      partial_frequency = 0.49f;
    } else {
      num_modes = i + 1;
    }
    f_[i].set_f_q<FREQUENCY_FAST>(
        partial_frequency,
        1.0f + partial_frequency * q);
    stretch_factor += stiffness;
    if (stiffness < 0.0f) {
      // Make sure that the partials do not fold back into negative frequencies.
      stiffness *= 0.93f;
    } else {
      // This helps adding a few extra partials in the highest frequencies.
      stiffness *= 0.98f;
    }
    // This prevents the highest partials from decaying too fast.
    q_loss += q_loss_damping_rate * (1.0f - q_loss);
    harmonic += frequency_;
    q *= q_loss;
  }
  
  return num_modes;
}

void Resonator::Process(const float* in, float* out, float* aux, size_t size) {
  // Per-sample frequency interpolation: ramp the normalized frequency across
  // the block so the 64 SVF coefficients glide instead of stepping (prevents
  // coefficient-jump transients / pops from raw 980 Hz param modulation).
  //
  // COEFFICIENT RECOMPUTE THROTTLE (2026-08-23 CPU fix): ComputeFilters()
  // used to run PER SAMPLE — ~64 set_f_q + LUT interps + the stretch/Q
  // recursion every sample, which micro-benchmarks showed is ~58% of the
  // Resonator's total cost (0.641 -> 0.269 ms per 512-block when hoisted;
  // upstream Rings recomputes once per block). The recompute now runs only
  // when the interpolated frequency has actually MOVED since the last
  // recompute (>= 1e-4 normalized — a couple of cents): during a sweep it
  // still updates per sample (the pop-free glide is fully preserved), but a
  // static frequency — the common case — recomputes once and then reuses the
  // coefficients, paying only the 64-mode SVF filter bank per sample (the
  // actual resonance work). The amplitude/position interpolation and the
  // constant num_modes processing below are unchanged.
  ParameterInterpolator position(&previous_position_, position_, size);
  ParameterInterpolator frequency(&previous_frequency_, frequency_, size);
  float cfFrequency_ = -1.0f;  // normalized frequency the current coefficients were computed for (-1 = none)
  while (size--) {
    frequency_ = frequency.Next();
    if (std::fabs(frequency_ - cfFrequency_) >= 1e-4f) {
      ComputeFilters();  // coefficients track the interpolated frequency (throttled)
      cfFrequency_ = frequency_;
    }
    // Always process ALL modes (not just sub-Nyquist ones): Nyquist-clamped
    // modes (partial_frequency >= 0.49) contribute ~0 at audio frequencies, but
    // EXCLUDING them when pitch sweeps would cause a discontinuity (mode
    // contribution abruptly drops to 0). Processing a constant count eliminates
    // the num_modes transition pop.
    int32_t num_modes = std::min(kMaxModes, resolution_);
    
    CosineOscillator amplitudes;
    amplitudes.Init<COSINE_OSCILLATOR_APPROXIMATE>(position.Next());
    
    float input = *in++ * 0.125f;
    float odd = 0.0f;
    float even = 0.0f;
    amplitudes.Start();
    for (int32_t i = 0; i < num_modes;) {
      odd += amplitudes.Next() * f_[i++].Process<FILTER_MODE_BAND_PASS>(input);
      even += amplitudes.Next() * f_[i++].Process<FILTER_MODE_BAND_PASS>(input);
    }
    *out++ = odd;
    *aux++ = even;
  }
}

}  // namespace rings
