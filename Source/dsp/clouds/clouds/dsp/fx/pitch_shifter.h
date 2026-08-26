// Copyright 2014 Emilie Gillet.
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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Pitch shifter.

#ifndef CLOUDS_DSP_FX_PITCH_SHIFTER_H_
#define CLOUDS_DSP_FX_PITCH_SHIFTER_H_

#include "stmlib/stmlib.h"

#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/fx_engine.h"

namespace clouds {

class PitchShifter {
 public:
  PitchShifter() { }
  ~PitchShifter() { }
  
  void Init(uint16_t* buffer) {
    engine_.Init(buffer);
    phase_ = 0;
    size_ = 2047.0f;
    target_size_ = 2047.0f;
    spread_ = 0.0f;
    offR_ = 0.0f;
  }
  
  void Clear() {
    engine_.Clear();
  }

  inline void Process(FloatFrame* input_output, size_t size) {
    // Per-sample ONE_POLE rate-limiting on size_ (the original Clouds
    // ONE_POLE(0.05) per-block, spread per-sample at 0.05/size). Unlike linear
    // interpolation, the one-pole approaches the target asymptotically (~20 ms
    // time constant), which is required because size_ controls the delay READ
    // POSITION: a fast linear ramp sweeps through the buffer (Doppler zap);
    // the slow one-pole gives a musical glide with no boundary spikes.
    const float size_coeff = 0.05f / static_cast<float>(size);
    while (size--) {
      Process(input_output, size_coeff);
      ++input_output;
    }
  }
  
  void Process(FloatFrame* input_output, float size_coeff) {
    typedef E::Reserve<2047, E::Reserve<2047> > Memory;
    E::DelayLine<Memory, 0> left;
    E::DelayLine<Memory, 1> right;
    E::Context c;
    engine_.Start(&c);
    
    size_ += size_coeff * (target_size_ - size_);
    
    phase_ += (1.0f - ratio_) / size_;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    if (phase_ <= 0.0f) {
      phase_ += 1.0f;
    }
    float tri = 2.0f * (phase_ >= 0.5f ? 1.0f - phase_ : phase_);
    float phase = phase_ * size_;
    float half = phase + size_ * 0.5f;
    if (half >= size_) {
      half -= size_;
    }
    
    c.Read(input_output->l, 1.0f);
    c.Write(left, 0.0f);
    c.Interpolate(left, phase, tri);
    c.Interpolate(left, half, 1.0f - tri);
    c.Write(input_output->l, 0.0f);

    // Stereo SPREAD (Hellcat add) — wrap/gain-zero invariant.
    //
    // A dual-tap pitch shifter reads two windows a half-window apart and
    // crossfades them with a triangle envelope (tri / 1-tri). It is
    // click-free ONLY when each read position wraps at a sample where ITS
    // OWN envelope gain is zero: the `phase` tap must wrap when tri == 0
    // (phase_ == 0 or 1) and the `half` tap when 1-tri == 0 (phase_ == 0.5).
    // The original Clouds code satisfies this by construction (both taps
    // derive from the same phase_).
    //
    // The original Hellcat stereo-spread implementation offset the R taps
    // by rOff = spread_ * size_ but kept the SHARED tri as the crossfade
    // envelope. The R taps then wrap at phase_ = 1 - rOff/size_ and
    // phase_ = 0.5 - rOff/size_ — points where tri is ~2*rOff/size_, NOT
    // zero — so the R read position teleported while its gain was non-zero:
    // a periodic discontinuity proportional to Spread (the reported
    // one-sided crackle; spread=0 was exact mono, hence invisible to tests).
    //
    // Fix: the R channel gets its OWN crossfade phase, phaseR_ = wrap(phase_ +
    // offR_), where offR_ is the rate-limited normalized spread offset
    // (glides like size_ instead of jumping on a spread change). Its own
    // triangle triR is zero exactly at phaseR_'s own wrap points, so the R
    // taps wrap on THEIR gain-zero crossings — same invariant as L. L and R
    // stay phase-locked to the same ratio/size glide (they differ only by
    // the constant offR_), and offR_ == 0 collapses to the original mono
    // path bit-for-bit (triR == tri, phaseR/halfR == phase/half).
    offR_ += size_coeff * (spread_ - offR_);
    float phaseR_ = phase_ + offR_;
    if (phaseR_ >= 1.0f) {
      phaseR_ -= 1.0f;
    }
    if (phaseR_ <= 0.0f) {
      phaseR_ += 1.0f;
    }
    float triR = 2.0f * (phaseR_ >= 0.5f ? 1.0f - phaseR_ : phaseR_);
    float phaseR = phaseR_ * size_;
    float halfR = phaseR + size_ * 0.5f;
    if (halfR >= size_) {
      halfR -= size_;
    }

    c.Read(input_output->r, 1.0f);
    c.Write(right, 0.0f);
    c.Interpolate(right, phaseR, triR);
    c.Interpolate(right, halfR, 1.0f - triR);
    c.Write(input_output->r, 0.0f);
  }
  
  inline void set_ratio(float ratio) {
    ratio_ = ratio;
  }
  
  inline void set_size(float size) {
    target_size_ = 128.0f + (2047.0f - 128.0f) * size * size * size;
  }

  // Stereo spread: 0 = both channels share the read window (mono); 1 = the
  // right channel's read window is offset by up to one full window length.
  // The offset itself is RATE-LIMITED per sample (offR_, like size_) so a
  // spread knob move glides instead of jumping the R read position.
  // (Hellcat add.)
  inline void set_spread(float spread) {
    spread_ = spread;
  }
  
 private:
  typedef FxEngine<4096, FORMAT_16_BIT> E;
  E engine_;
  float phase_;
  float ratio_;
  float size_;
  float target_size_;
  float spread_;   // target normalized R-window offset (0..1) — Hellcat add
  float offR_;     // rate-limited current R-window offset (0..1) — Hellcat add
  
  DISALLOW_COPY_AND_ASSIGN(PitchShifter);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_FX_MINI_CHORUS_H_