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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Sample rate converter.

#ifndef WARPS_DSP_SAMPLE_RATE_CONVERTER_H_
#define WARPS_DSP_SAMPLE_RATE_CONVERTER_H_

#include "stmlib/stmlib.h"

#include <algorithm>

namespace warps {

enum SampleRateConversionDirection {
  SRC_UP,
  SRC_DOWN
};

template <SampleRateConversionDirection direction, int32_t ratio, int32_t length>
struct SRC_FIR { };

}

#include "warps/dsp/sample_rate_conversion_filters.h"

namespace warps {

template<int32_t N>
struct FilterState {
 public:
  enum {
    n = N
  };
  inline void Push(float value) {
    tail.Push(head);
    head = value;
  }

  template<int32_t i> inline float Read() const {
    return i == 0 ? head : tail.template Read<i - 1>();
  }

  inline void Load(const float* x_state) {
    head = x_state[0];
    tail.Load(x_state + 1);
  }

  inline void Save(float* x_state) {
    x_state[0] = head;
    tail.Save(x_state + 1);
  }

 private:  
  float head;
  FilterState<N-1> tail;
};

template<>
class FilterState<1> {
 public:
  enum {
    n = 1
  };
  inline void Push(float value) {
    head = value;
  }

  template<int32_t i> inline float Read() const {
    return head;
  }

  inline void Load(const float* x_state) {
    head = x_state[0];
  }

  inline void Save(float* x_state) {
    x_state[0] = head;
  }
 private:
  float head;
};

template<int32_t N, int32_t x_stride, int32_t h_stride, int32_t mirror = 0, int32_t i = 0, int32_t h_offset = 0>
struct Accumulator {
  enum {
    h_index = mirror != 0 && h_offset + i * h_stride >= mirror / 2 ?
        mirror - 1 - i * h_stride - h_offset : h_offset + i * h_stride
  };
  
  template<typename IR>
  inline float operator()(const float* x, const IR& h) const {
    Accumulator<N - 1, x_stride, h_stride, mirror, i + 1, h_offset> a;
    return x[i * x_stride] * h.template Read<h_index>() + a(x, h);
  }
  
  template<int32_t NN, typename IR>
  inline float operator()(const FilterState<NN>& x, const IR& h) const {
    Accumulator<N - 1, x_stride, h_stride, mirror, i + 1, h_offset> a;
    return x.template Read<i * x_stride>() * h.template Read<h_index>() + a(x, h);
  }
};

template<int32_t x_stride, int32_t h_stride, int32_t mirror, int32_t i, int32_t h_offset>
struct Accumulator<0, x_stride, h_stride, mirror, i, h_offset> {
  template<typename IR>
  inline float operator()(const float* x, const IR& h) const {
    return 0.0f;
  }

  template<int32_t NN, typename IR>
  inline float operator()(const FilterState<NN>& x, const IR& h) const {
    return 0.0f;
  }
};

template<int32_t K, int32_t mirror = 0, int32_t remaining = K>
struct PolyphaseStage {
  template<typename T, typename IR>
  inline void operator()(float* &y, const T& x, const IR& h) const {
    Accumulator<T::n, 1, K, mirror, 0, K - remaining> a;
    *y++ = a(x, h);
    PolyphaseStage<K, mirror, remaining - 1> p;
    p(y, x, h);
  }
};

template<int32_t K, int32_t mirror>
struct PolyphaseStage<K, mirror, 0> {
  template<typename T, typename IR>
  inline void operator()(float* &y, const T& x, const IR& h) const { }
};

template<
    SampleRateConversionDirection direction,
    int32_t ratio,
    int32_t filter_size>
class SampleRateConverter { };

template<int32_t ratio, int32_t filter_size>
class SampleRateConverter<SRC_UP, ratio, filter_size> {
 private:
  enum {
    N = filter_size / ratio,
    K = ratio
  };
 
 public:
  SampleRateConverter() { }
  ~SampleRateConverter() { }

  inline void Init() {
    std::fill(&x_[0], &x_[N], 0);
  };

  inline int32_t delay() const { return filter_size / ratio / 2; }

  inline void Process(const float* in, float* out, size_t input_size) {
    SRC_FIR<SRC_UP, ratio, filter_size> ir;
    FilterState<N> x;
    x.Load(x_);
    while (input_size--) {
      x.Push(*in++);
      PolyphaseStage<K, filter_size> polyphase_stage;
      polyphase_stage(out, x, ir);
    }
    x.Save(x_);
  }
  
 private:
  float x_[N];

  DISALLOW_COPY_AND_ASSIGN(SampleRateConverter);
};

template<int32_t ratio, int32_t filter_size>
class SampleRateConverter<SRC_DOWN, ratio, filter_size> {
 private:
  enum {
    N = filter_size,
    K = ratio
  };
 
 public:
  SampleRateConverter() { }
  ~SampleRateConverter() { }

  inline void Init() {
    std::fill(&x_[0], &x_[2 * N], 0);
    x_ptr_ = &x_[N - 1];
  };

  inline int32_t delay() const { return filter_size / 2; }

  inline void Process(const float* in, float* out, size_t input_size) {
    // When downsampling, the number of input samples must be a multiple
    // of the downsampling ratio.
    if ((input_size % ratio) != 0) {
      return;
    }

    SRC_FIR<SRC_DOWN, ratio, filter_size> ir;

    // Upstream Warps keeps two Process paths: a linear-in-buffer "fast path"
    // for input_size >= 8 * filter_size and this circular-buffer path for
    // smaller sizes. The fast path never touches x_ptr_ (it is positioned only
    // by Init() and by this circular path) and its trailing history copy fills
    // just the LOWER half of x_, leaving the mirror half (x_[N..2N)) stale —
    // so the first circular call after a fast call reads an FIR window that
    // mixes fresh, stale-mirror and stale-history samples: a large-amplitude
    // discontinuity. That is harmless upstream, where one caller feeds whole
    // fixed-size buffers and never switches paths mid-stream, but Hellcat
    // drives this from the FX chain with VARIABLE sub-chunk sizes that cross
    // the 8 * filter_size threshold inside a single host block (full sub-
    // chunks take the fast path, the trailing remainder drops back to the
    // circular path), producing a periodic discontinuity at (nearly) every
    // block boundary in exactly the two SRC_DOWN users (Wavefolder,
    // RingModulator). The circular path alone is correct for ANY size that is
    // a multiple of ratio and is its own invariant: every push writes both a
    // slot and its mirror and wraps x_ptr_ within [x_, x_ + N), so the window
    // [x_ptr_ + 1, x_ptr_ + N] always holds the most recent N inputs in read
    // order. Its cost is the same N-tap accumulation per output sample as the
    // fast path (the extra two stores and wrap check per INPUT sample are
    // noise next to the 48 MACs per OUTPUT sample at these block sizes), so
    // the fast path is removed rather than made transition-safe.
    while (input_size) {
      for (int32_t i = 0; i < ratio; ++i) {
        x_ptr_[0] = x_ptr_[N] = *in++;
        --x_ptr_;
        if (x_ptr_ < x_) {
          x_ptr_ += N;
        }
      }
      input_size -= ratio;

      Accumulator<N, 1, 1, filter_size> accumulator;
      *out++ = accumulator(&x_ptr_[1], ir);
    }
  }
 
 private:
  float x_[2 * N];
  float* x_ptr_;

  DISALLOW_COPY_AND_ASSIGN(SampleRateConverter);
};

}  // namespace warps

#endif  // WARPS_DSP_SAMPLE_RATE_CONVERTER_H_
