// factory_bank_test — guards the 64 ORIGINAL Hellcat factory presets.
//
// The bank (presets/HFACTORY/*.yml — the CANONICAL, hand-editable preset
// files) is embedded into the plugin binary, so an edit can silently break a
// preset. This test loads every file through the REAL patch
// load path and pins the invariants the browser + installer rely on: the exact
// file count (64), a stable category/polyphony layout (bass/keys/leads mono or
// poly, unison leads on Unison 2x), an audible amp routing in every patch, and
// a byte-exact params round-trip for every file. It reads the REPO tree (run
// from the repo root, like every CWD-relative test here).

#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "HellcatPreset.h"
#include "unified_test_runner.h"

namespace
{
// The params block of a serialized patch (everything from "params:\n").
juce::String paramsBlock (const juce::String& yaml)
{
    const int cut = yaml.indexOf ("\nparams:\n");
    return cut < 0 ? juce::String() : yaml.substring (cut + 1);
}

// ---- Spectrum guard ----
// 4096-point Hann frames over one rendered note on a FRESH processor; seven
// log bands as fractions of the 20 Hz..16 kHz energy. Bounds per category:
//   bass (01-12): sub+bass >= 0.80        — the low end is there
//   keys/leads/unison: sub+bass <= 0.35   — not sitting under the register
//   pads (49-58): mid+himid >= 0.05       — mids present (air via himid)
void fftRadix2 (std::vector<double>& re, std::vector<double>& im)
{
    const int n = (int) re.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[(size_t) i], re[(size_t) j]); std::swap (im[(size_t) i], im[(size_t) j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * juce::MathConstants<double>::pi / (double) len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k)
            {
                const double wr = std::cos (ang * k), wi = std::sin (ang * k);
                const size_t a = (size_t) (i + k), b = (size_t) (i + k + len / 2);
                const double ur = re[a], ui = im[a];
                const double vr = re[b] * wr - im[b] * wi;
                const double vi = re[b] * wi + im[b] * wr;
                re[a] = ur + vr; im[a] = ui + vi;
                re[b] = ur - vr; im[b] = ui - vi;
            }
    }
}

// Renders 1.2 s at @p note on a FRESH processor; fills @p out with the split.
void analyzeBands (const juce::File& patchFile, int note, double out[7])
{
    for (int i = 0; i < 7; ++i) out[i] = 0.0;
    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    if (! proc.loadHellcatPatchFile (patchFile)) return;
    juce::AudioBuffer<float> buf (2, 256);
    std::vector<float> x;
    for (int b = 0; b < 48000 * 6 / 10 / 256; ++b)
    {
        buf.clear();
        juce::MidiBuffer midi;
        if (b == 4) midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) note, 0.79f), 0);
        proc.processBlock (buf, midi);
        const float* d = buf.getReadPointer (0);
        x.insert (x.end(), d, d + 256);
    }
    constexpr int kN = 4096;
    if (x.size() < (size_t) kN) return;
    static const double edges[8] = { 20, 60, 150, 400, 1200, 3500, 8000, 16000 };
    const double binHz = 48000.0 / (double) kN;
    auto binOf = [&] (double hz)
    { return juce::jlimit (1, kN / 2, (int) std::lround (hz / binHz)); };
    double sums[7] = {};
    std::vector<double> re (kN), im (kN), win (kN);
    for (int i = 0; i < kN; ++i)
        win[(size_t) i] = 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * 2.0 * i / (kN - 1)));
    for (size_t st = 0; st + kN <= x.size(); st += kN / 2)
    {
        for (int i = 0; i < kN; ++i)
        {
            re[(size_t) i] = (double) x[st + (size_t) i] * win[(size_t) i];
            im[(size_t) i] = 0.0;
        }
        fftRadix2 (re, im);
        for (int k = 0; k < 7; ++k)
            for (int m = binOf (edges[k]); m < binOf (edges[k + 1]); ++m)
                sums[k] += re[(size_t) m] * re[(size_t) m] + im[(size_t) m] * im[(size_t) m];
    }
    double total = 0.0;
    for (int k = 0; k < 7; ++k) total += sums[k];
    if (total <= 1e-30) return;
    for (int k = 0; k < 7; ++k) out[k] = sums[k] / total;
}
}  // namespace

TEST(factory_bank_test)
{
    const juce::File dir = juce::File::getCurrentWorkingDirectory()
                               .getChildFile ("presets/HFACTORY");
    CHECK(dir.isDirectory(), "presets/HFACTORY exists in the repo tree");

    juce::Array<juce::File> files;
    dir.findChildFiles (files, juce::File::findFiles, false, "*.yml");
    files.sort();
    CHECK(files.size() == 64, "the bank carries exactly 64 presets (got "
                                 + juce::String (files.size()) + ")");

    // The bank layout: 01-12 bass, 13-26 keys, 27-38 flat leads (mono),
    // 39-48 unison leads, 49-58 pads, 59-64 fx.
    auto polyOf = [] (int slot) -> int
    {
        if (slot <= 12)  return 0;   // bass: MONO (08 Poly Pulse Bass re-pins itself)
        if (slot <= 26)  return 1;   // keys: POLY
        if (slot <= 38)  return 0;   // flat leads: MONO
        if (slot <= 48)  return 0;   // unison leads: MONO stack (every voice sounds)
        return 1;                    // pads + fx: POLY
    };

    // Per-patch polyphony (voice count) request, matching the authored
    // `voice_slots:` in each file (see applyHellcatPatch). Bass and flat
    // leads use a single mono voice; keys/pads max the part out at 16; unison
    // leads use an 8-voice mono unison stack; 08 Poly Pulse Bass keeps
    // chord voices; the FX presets carry a light 8-voice stack.
    auto voiceOf = [] (int slot) -> int
    {
        if (slot <= 12)  return (slot == 8) ? 8 : 1;   // bass
        if (slot <= 26)  return 16;                     // keys: full poly
        if (slot <= 38)  return 1;                      // flat leads: mono single voice
        if (slot <= 48)  return 8;                      // unison leads: mono unison stack
        if (slot <= 58)  return 16;                     // pads: full poly
        return 8;                                       // fx: poly
    };

    int checked = 0;
    int fxmodLive = 0, fxmodVel = 0, fxmodLFO = 0;
    for (const auto& f : files)
    {
        const int slot = f.getFileName().substring (0, 2).getIntValue();
        if (slot < 1 || slot > 64)
        {
            CHECK(false, "unexpected non-bank file in HFACTORY: " + f.getFileName());
            continue;
        }

        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);

        CHECK(proc.loadHellcatPatchFile (f),
              f.getFileName() + " loads through the real patch path");

        // Every patch must keep the amp routing (mod11 Env 3 -> VCA) — the
        // one modulation a patch cannot be audible without.
        CHECK((int) proc.getApvts().getRawParameterValue ("mod11_amount")->load() != 0,
              f.getFileName() + " keeps the Env 3 -> VCA amp routing");

        // Category polyphony layout (08 Poly Pulse Bass is the bass exception).
        const int poly = (int) proc.getApvts().getRawParameterValue ("part_polyphony")->load();
        const int want = (slot == 8) ? 1 : polyOf (slot);
        CHECK(poly == want,
              f.getFileName() + " polyphony matches its category (got "
                  + juce::String (poly) + ", want " + juce::String (want) + ")");

        // Polyphony (voice count) request, applied on load to the current
        // part (part 0 in a fresh processor) from the file's `voice_slots:`.
        const int vc = proc.getEngine().getPartVoiceSlots (0);
        CHECK(vc == voiceOf (slot),
              f.getFileName() + " voice count matches its category (got "
                  + juce::String (vc) + ", want " + juce::String (voiceOf (slot)) + ")");

        // FX mod matrix: the bank uses it for evolving/interactive patches.
        // Tally live (amount != 0) routings, velocity-driven and LFO-driven.
        for (int m = 1; m <= 16; ++m)
        {
            const auto pre = "fxmod" + juce::String (m);
            const int src = (int) proc.getApvts().getRawParameterValue (pre + "_source")->load();
            const int amt = (int) proc.getApvts().getRawParameterValue (pre + "_amount")->load();
            if (amt != 0)
            {
                ++fxmodLive;
                if (src == 14) ++fxmodVel;           // Velocity: interactive dirt/FX
                if (src >= 3 && src <= 5) ++fxmodLFO; // LFO 1..3: evolving space/motion
            }
        }

        // Byte-exact params round-trip: serializing the loaded state must
        // reproduce the file's params block exactly.
        juce::String text;
        if (juce::FileInputStream in (f); in.openedOk())
            text = in.readEntireStreamAsString();
        proc.setLoadedProgramName (f.getFileName().substring (3).upToLastOccurrenceOf (".", false, false));
        CHECK(paramsBlock (hellcat::preset::serializeHellcatPatch (proc))
                  == paramsBlock (text),
              f.getFileName() + " params round-trip byte-exactly");

        // FX coverage: every NON-BASS preset (slot >= 13) carries at least one
        // enabled, non-None FX slot; bass (01-12) stays dry (mix headroom for
        // low end). Wet level must be non-zero on every enabled slot. Space
        // coverage: every non-bass preset ALSO carries at least one reverb or
        // delay slot (CVerb/Plate/Room/Spring/Echo/ClockedDelay/LoopingDelay)
        // — subtle decay everywhere outside bass.
        if (slot >= 13)
        {
            int live = 0, space = 0;
            for (int sl = 1; sl <= 3; ++sl)
            {
                const auto pre = "fx" + juce::String (sl);
                const int type = (int) proc.getApvts().getRawParameterValue (pre + "_type")->load();
                const int en   = (int) proc.getApvts().getRawParameterValue (pre + "_enabled")->load();
                const int wet  = (int) proc.getApvts().getRawParameterValue (pre + "_drywet")->load();
                if (type > 0 && en == 1 && wet > 0)
                    ++live;
                switch (type)
                {
                    case 3: case 4: case 11: case 13: case 22: case 23: case 24:
                        if (en == 1 && wet > 0) ++space;
                        break;
                    default: break;
                }
            }
            CHECK(live > 0,
                  f.getFileName() + " carries at least one live FX slot");
            CHECK(space > 0,
                  f.getFileName() + " carries a reverb or delay (space)");
        }
        else
        {
            // Bass (01-12) is monophonic and stays low-end focused: no space
            // (reverb/delay). A dirt patch may carry a single OVERDRIVE (16)
            // or LUT (17) distortion slot ONLY, and keeps the MASTER FX
            // relatively dry (fx_mix <= 96) so the 32 kHz FV-1 dirt does not
            // dominate. A bass with no FX stays legal.
            int space = 0, live = 0, illegal = 0;
            for (int sl = 1; sl <= 3; ++sl)
            {
                const auto pre = "fx" + juce::String (sl);
                const int type = (int) proc.getApvts().getRawParameterValue (pre + "_type")->load();
                const int en   = (int) proc.getApvts().getRawParameterValue (pre + "_enabled")->load();
                const int wet  = (int) proc.getApvts().getRawParameterValue (pre + "_drywet")->load();
                if (type > 0 && en == 1 && wet > 0)
                {
                    ++live;
                    if (type != 16 && type != 17) ++illegal;
                }
                switch (type)
                {
                    case 3: case 4: case 11: case 13: case 22: case 23: case 24:
                        if (en == 1) ++space;
                        break;
                    default: break;
                }
            }
            CHECK(space == 0,
                  f.getFileName() + " bass carries no reverb/delay (space)");
            CHECK(illegal == 0,
                  f.getFileName() + " bass dirt is Overdrive or LUT only");
            CHECK(live <= 1,
                  f.getFileName() + " bass carries at most one dirt FX slot");
            if (live > 0)
            {
                const int mix = (int) proc.getApvts().getRawParameterValue ("fx_mix")->load();
                CHECK(mix <= 96,
                      f.getFileName() + " bass keeps the master FX dry against 32 kHz dirt");
            }
        }

        // Spectrum bounds per category (see analyzeBands): bass keeps its low
        // end, keys/leads/unison stay out of the sub-fundamental mud, pads
        // carry mids. Rendered at the category register (bass C2, rest C4).
        {
            double e[7];
            analyzeBands (f, slot <= 12 ? 36 : 60, e);
            if (slot <= 12)
            {
                CHECK(e[0] + e[1] >= 0.80,
                      f.getFileName() + " keeps the low end (sub+bass "
                          + juce::String ((e[0] + e[1]) * 100.0, 1) + "%)");
            }
            else if (slot <= 48)
            {
                CHECK(e[0] + e[1] <= 0.35,
                      f.getFileName() + " sits at its register (sub+bass "
                          + juce::String ((e[0] + e[1]) * 100.0, 1) + "%)");
            }
            else if (slot <= 58)   // pads only; fx (59-64) are free-form
            {
                CHECK(e[3] + e[4] >= 0.05,
                      f.getFileName() + " carries mids (mid+himid "
                          + juce::String ((e[3] + e[4]) * 100.0, 1) + "%)");
            }
        }
        ++checked;
    }
    CHECK(checked == 64, "every bank file was checked (got " + juce::String (checked) + ")");

    // FX mod matrix coverage: the bank must USE the matrix (not just pose the
    // sliders) — velocity for interactive dirt/FX and LFOs for evolving
    // space/motion across the polyphonic and FX rows.
    CHECK(fxmodLive >= 10,
          "the bank uses the FX mod matrix on >= 10 patches (got "
              + juce::String (fxmodLive) + ")");
    CHECK(fxmodVel >= 3,
          "the FX mod matrix maps velocity on >= 3 patches (got "
              + juce::String (fxmodVel) + ")");
    CHECK(fxmodLFO >= 3,
          "the FX mod matrix maps LFOs on >= 3 patches (got "
              + juce::String (fxmodLFO) + ")");

    // Unique names: the browser labels leaves by FILE name, so a duplicate
    // number prefix would alias in the menu.
    std::set<juce::String> prefixes;
    bool unique = true;
    for (const auto& f : files)
        if (! prefixes.insert (f.getFileName().substring (0, 2)).second)
            unique = false;
    CHECK(unique, "the 64 number prefixes are unique");

    return true;
}
