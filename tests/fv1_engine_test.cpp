// Standalone verification of the FV-1 emulation framework (Fv1Engine.h /
// Fv1FxProcessor.h). JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_engine_test.cpp -o /tmp/fv1_engine_test && /tmp/fv1_engine_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector> only).

#include <cmath>
#include <cstdio>

#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}
bool approx (float a, float b, float tol) { return std::fabs (a - b) <= tol; }
} // namespace

// A trivial effect that delays by 1 sample and halves, to exercise the base.
class PassthroughFx : public fv1::Fv1FxProcessor
{
public:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override
    {
        lout = fv1::f24_mulk (lin, 4096); // *0.5 (14-bit: q14(0.5)=4096)
        rout = fv1::f24_mulk (rin, 4096);
    }
    void setParams (const float[5]) override {}
    FxType type() const override { return FxType::None; }
};

int main()
{
    using namespace parvati::fv1;

    // ---- Fixed-point round-trip ----
    {
        for (float v : { -0.99f, -0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 0.999f })
        {
            const int32_t q = f24_fromFloat (v);
            const float r = f24_toFloat (q);
            check (approx (r, v, 1.5e-7f), "f24 round-trip");
        }
        check (f24_fromFloat (2.0f) == kMaxQ23, "f24 saturates high");
        check (f24_fromFloat (-2.0f) == kMinQ23, "f24 saturates low");
        check (f24_sat (kMaxQ23 + 100) == kMaxQ23, "f24_sat high");
        check (f24_sat (kMinQ23 - 100) == kMinQ23, "f24_sat low");
    }

    // ---- 14-bit coefficient quantization ----
    {
        check (q14 (1.0f) == 8191, "q14 clamps high");
        check (q14 (-1.0f) == -8192, "q14 clamps low");
        check (q14 (0.0f) == 0, "q14 zero");
        // f24_mulk by 0.5 (14-bit) ~= halve.
        const int32_t h = f24_mulk (kOneQ23 >> 1, q14 (0.5f));
        check (approx (f24_toFloat (h), 0.25f, 2e-4f), "f24_mulk 0.5");
    }

    // ---- Grit (bit-truncation) ----
    {
        const int32_t x = f24_fromFloat (0.123456f);
        const int32_t g = f24_quantBits (x, 8); // 8-bit
        // 8-bit keeps top 8 bits (incl sign); low 16 zeroed.
        check ((g & 0xFFFF) == 0, "grit zeros low 16 bits at 8-bit");
        check (f24_quantBits (x, 24) == x, "grit 24-bit is identity");
    }

    // ---- LUTs ----
    {
        check (approx (lutSine32 (0.0f), 0.0f, 1e-3f), "sine LUT at 0");
        check (approx (lutSine32 (0.25f), 1.0f, 1e-3f), "sine LUT peak");
        check (approx (lutSine32 (0.5f), 0.0f, 1e-3f), "sine LUT at 0.5");
        check (approx (lutSine32 (0.75f), -1.0f, 1e-3f), "sine LUT trough");
        check (approx (lutTri32 (0.0f), -1.0f, 1e-3f), "tri LUT at 0");
        check (approx (lutTri32 (0.5f), 1.0f, 1e-3f), "tri LUT peak");
        check (approx (lutTri32 (0.25f), 0.0f, 1e-3f), "tri LUT zero crossing");
    }

    // ---- Delay line ----
    {
        DelayLine<8> d;
        d.clear();
        d.write (f24_fromFloat (0.1f));
        d.write (f24_fromFloat (0.2f));
        d.write (f24_fromFloat (0.3f));
        check (approx (f24_toFloat (d.read (1)), 0.3f, 1e-6f), "delay read(1)");
        check (approx (f24_toFloat (d.read (2)), 0.2f, 1e-6f), "delay read(2)");
        check (approx (f24_toFloat (d.readFrac (1.5f)), 0.25f, 1e-3f), "delay readFrac(1.5)");
    }

    // ---- One-pole LP sanity (DC passes ~unity after settling) ----
    {
        OnePoleLpFx lp;
        lp.setCutoff (2000.0f);
        int32_t y = 0;
        for (int i = 0; i < 4000; ++i)
            y = lp.process (f24_fromFloat (0.5f));
        check (approx (f24_toFloat (y), 0.5f, 1e-2f), "1-pole LP passes DC at 0.5");
    }

    // ---- Allpass unity-gain (1st-order AP is unity magnitude) ----
    {
        Allpass1Fx ap;
        ap.setCoef (0.5f);
        ap.process (f24_fromFloat (0.0f));
        int32_t y = ap.process (f24_fromFloat (0.5f));
        // AP output is bounded; just check finite-ish (within [-1,1]).
        check (y >= kMinQ23 && y <= kMaxQ23, "allpass output in range");
    }

    // ---- RateBridge round-trip at 48 kHz ----
    {
        constexpr int n = 512;
        RateBridge rb;
        rb.prepare (48000.0, n);
        float L[n], R[n], Lo[n], Ro[n];
        // 1 kHz sine at 48k -> must survive the 32.768k round-trip (well under 15k BW).
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * std::sin (6.28318530718f * 1000.0f * static_cast<float> (i) / 48000.0f);
            L[i] = v;
            R[i] = v;
        }
        const int m = rb.hostToInternal (L, R, n);
        check (m > 0 && m < n * 2, "rate bridge produces internal samples");
        // Echo internal back out (effect = identity here).
        rb.internalToHost (Lo, Ro, n);
        bool finite = true, nonzero = false;
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (Lo[i]) || ! std::isfinite (Ro[i])) finite = false;
            if (std::fabs (Lo[i]) > 1e-3f) nonzero = true;
        }
        check (finite, "rate bridge round-trip output finite");
        check (nonzero, "rate bridge round-trip passes 1 kHz tone");
    }

    // ---- Fv1FxProcessor base: halving passthrough ----
    {
        PassthroughFx fx;
        fx.prepare (48000.0, 256);
        float L[256], R[256];
        for (int i = 0; i < 256; ++i) { L[i] = 0.4f; R[i] = 0.4f; }
        fx.process (L, R, 256);
        // After BW-limit + halve, mid-block steady state ~ 0.2 (allow tolerance).
        float sum = 0.0f;
        for (int i = 128; i < 256; ++i) sum += L[i];
        const float mean = sum / 128.0f;
        check (approx (mean, 0.2f, 0.03f), "Fv1FxProcessor halves DC (0.4 -> ~0.2)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 ENGINE TEST: FAILURES" : "FV1 ENGINE TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
