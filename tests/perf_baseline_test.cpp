// perf_baseline_test — normalized offline render-time harness for the
// unified suite.
//
// Purpose: measure the render cost of fixed DSP workloads and gate future
// changes against a machine-local baseline. The scenarios cover the render
// paths that the 2026-08 refactor campaign touched: the idle pool walk, a
// single voice, a busy multitimbral render, the oversampled filter path,
// and both FX rate-bridge families at a 48 kHz host rate.
//
// Methodology: every scenario builds a fresh processor, prepares it, sets
// its scenario state, then renders a fixed 2.0 s stream in blocks of 256.
// Setup stays OUTSIDE the timed window. One warmup pass runs first, then
// nine timed repeats. The report prints the median and the median absolute
// deviation (MAD) of the per-render wall time (std::chrono::steady_clock).
// The SCORE divides the MINIMUM render time by the MINIMUM of a normalizer
// workload (a fixed arithmetic loop) measured with the same method in the
// same run. The minimum of N repeats is the least-interfered estimate of
// the true cost, so the score stays stable when the host slows between
// runs (thermal states, background load). The score also removes machine
// speed: a baseline harvested on one host gates a run on that host.
//
// Modes:
//   * Default suite run (no baseline file): each scenario renders ONCE as
//     a smoke pass. Only functional sanity gates (finite audio, idle is
//     silent, active renders carry energy). Timing never fails the test.
//     This keeps the suite green on any host.
//   * Baseline present (tests/perf_baseline.local.json): the full method
//     runs and the test compares scores. A score above baseline * 1.20
//     fails the test; above baseline * 1.10 prints a warning. A config
//     mismatch (Debug baseline on a Release binary, or the reverse)
//     prints a warning and skips the compare, because Debug and Release
//     codegen do not share score magnitudes.
//   * Harvest (PARVATI_PERF_HARVEST=1): the full method runs, the test
//     prints a paste-ready JSON block and writes the baseline file.
//
// The baseline file is gitignored on purpose. It holds numbers for one
// machine and one build config. Harvest a fresh file per host:
//   PARVATI_PERF_HARVEST=1 ./build_unified/parvati_unified_tests perf_baseline_test
//
// Telemetry note: the engine writes the UI telemetry frame on every
// renderPartFx call. No runtime switch exists for it. The scenarios
// therefore always measure the telemetry write. A "telemetry off" pass
// needs a source change, so it stays out of this harness.
//
// Unified runner. Run with:
//   ./build_unified/parvati_unified_tests perf_baseline_test

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_utils.h"   // setInt/setChoice host-style writes

#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"

namespace
{

//==============================================================================
// Scenario table. One row = one measured render. fxType uses the FxType
// enum value (FxTypes.h) so this file needs no DSP include.
struct PerfScenario
{
    const char* label;
    double sampleRate;
    int notesPerChannel;   // 0 = silent idle render
    int channels;          // MIDI channels that receive notes (1..6)
    bool allParts16;       // give every part 16 voice slots (busy pool)
    bool osFactor4;        // oversample the voice filter path 4x
    int fxType;            // -1 = FX off; else FxType for slots 1..fxSlots
    int fxSlots;           // number of series FX slots in use
    bool expectEnergy;     // functional sanity: render must carry sound
};

const std::vector<PerfScenario>& perfScenarios()
{
    static const std::vector<PerfScenario> s = {
        { "idle-44100",                44100.0, 0,  1, false, false, -1, 0, false },
        { "voice1-saw-44100",          44100.0, 1,  1, false, false, -1, 0, true  },
        { "multipart-6x16-44100",      44100.0, 16, 6, true,  false, -1, 0, true  },
        { "voice-os4-8v-44100",        44100.0, 8,  1, false, true,  -1, 0, true  },
        { "fx-diffuser-1x-48000",      48000.0, 4,  1, false, false, 1,  1, true  },
        { "fx-diffuser-3x-48000",      48000.0, 4,  1, false, false, 1,  3, true  },
        { "fx-echo-3x-48000",          48000.0, 4,  1, false, false, 22, 3, true  },
    };
    return s;
}

constexpr int kBlock = 256;         // matches the golden render fixtures
constexpr double kRenderSeconds = 2.0;

//==============================================================================
// One scenario render, timed. Setup (construction, prepare, parameter
// writes, the oversampling swap) runs before the clock starts, so the
// timed window holds processBlock calls only.
struct RenderOutcome
{
    double micros = 0.0;
    double peak   = 0.0;
    bool allFinite = true;
};

RenderOutcome renderOnce (const PerfScenario& sc)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sc.sampleRate, kBlock);

    if (sc.allParts16)
        for (int p = 0; p < 6; ++p)
            proc.getEngine().setPartVoiceSlots (p, 16);

    if (sc.osFactor4)
        proc.setOversamplingFactor (4);

    if (sc.fxType >= 0)
    {
        for (int slot = 1; slot <= sc.fxSlots; ++slot)
        {
            const std::string prefix = "fx" + std::to_string (slot);
            setChoice (proc, (prefix + "_type").c_str(), sc.fxType);
            setInt (proc, (prefix + "_enabled").c_str(), 1);
            setInt (proc, (prefix + "_drywet").c_str(), 127);
            for (int k = 1; k <= 5; ++k)
                setInt (proc, (prefix + "_param" + std::to_string (k)).c_str(), 64);
        }
        setInt (proc, "fx_topo", 0);   // Series: A -> B -> C
    }

    if (sc.notesPerChannel > 0)
        setInt (proc, "osc1_shape", 1);   // saw

    const int total = (int) std::llround (kRenderSeconds * sc.sampleRate);

    juce::MidiBuffer midiNotes;
    for (int ch = 1; ch <= sc.channels; ++ch)
        for (int n = 0; n < sc.notesPerChannel; ++n)
            midiNotes.addEvent (juce::MidiMessage::noteOn (
                ch, (uint8_t) juce::jlimit (0, 127, 36 + ch + 5 * n),
                (uint8_t) 110), 0);
    juce::MidiBuffer midiEmpty;

    juce::AudioBuffer<float> buf (2, kBlock);

    RenderOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    bool sentNotes = false;
    for (int written = 0; written < total; )
    {
        buf.clear();
        proc.processBlock (buf, sentNotes ? midiEmpty : midiNotes);
        sentNotes = true;
        const int n = std::min (kBlock, total - written);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const float v = buf.getSample (ch, i);
                if (! std::isfinite (v)) { out.allFinite = false; }
                out.peak = std::max (out.peak, (double) std::fabs (v));
            }
        written += n;
    }
    const auto t1 = std::chrono::steady_clock::now();
    out.micros = static_cast<double> (std::chrono::duration_cast<std::chrono::microseconds> (t1 - t0).count());
    return out;
}

//==============================================================================
// Normalizer: a fixed arithmetic workload measured like a scenario. The
// score divides a render minimum by this minimum. The loop reads and writes
// a heap array, so the compiler cannot fold it away. The checksum feeds a
// sanity check, not a timing gate.
double normalizerPass (std::vector<double>& work)
{
    double sink = 0.0;
    for (size_t i = 0; i < work.size(); ++i)
    {
        work[i] = std::sqrt (work[i] + 1.0) * 1.0000001;
        sink += work[i];
    }
    return sink;
}

double timedNormalizerUs (std::vector<double>& work)
{
    const auto t0 = std::chrono::steady_clock::now();
    normalizerPass (work);
    const auto t1 = std::chrono::steady_clock::now();
    return (double) std::chrono::duration_cast<std::chrono::microseconds> (t1 - t0).count();
}

//==============================================================================
// Median / MAD helpers.
double minOf (const std::vector<double>& v)
{
    double m = v[0];
    for (double x : v) m = std::min (m, x);
    return m;
}

double medianOf (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

double madOf (std::vector<double> v, double med)
{
    for (double& x : v) x = std::fabs (x - med);
    return medianOf (std::move (v));
}

//==============================================================================
// Baseline file: tests/perf_baseline.local.json (gitignored).
juce::File baselineFile()
{
    return juce::File (PARVATI_SOURCE_DIR "/tests/perf_baseline.local.json");
}

juce::String configName()
{
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

} // namespace


//==============================================================================
TEST(perf_baseline_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const char* const harvestEnv = std::getenv ("PARVATI_PERF_HARVEST");
    const bool harvest = harvestEnv != nullptr && *harvestEnv != '\0';
    const juce::File baseline = baselineFile();
    const bool haveBaseline = ! harvest && baseline.existsAsFile();

    // Full method (warmup + nine repeats) only for harvest or gated runs.
    // The default suite run does one smoke pass per scenario.
    const int repeats = (harvest || haveBaseline) ? 9 : 1;
    const int warmup  = (harvest || haveBaseline) ? 1 : 0;

    // --- normalizer, same method as the scenarios ---
    std::vector<double> work (8192 * 96, 0.5);
    for (int i = 0; i < warmup; ++i) normalizerPass (work);
    std::vector<double> normSamples;
    normSamples.reserve ((size_t) repeats);
    for (int i = 0; i < repeats; ++i)
        normSamples.push_back (timedNormalizerUs (work));
    const double normMedianUs = medianOf (normSamples);
    const double normMadUs = madOf (normSamples, normMedianUs);
    const double normMinUs = minOf (normSamples);
    std::printf ("normalizer: min %.0f us, median %.0f us, MAD %.0f us, %d repeat(s)\n",
                 normMinUs, normMedianUs, normMadUs, repeats);
    CHECK (normMinUs > 0.0, "normalizer produced a positive measurement");

    struct Row
    {
        const char* label = "";
        double minUs = 0.0;
        double medianUs = 0.0;
        double madUs = 0.0;
        double score = 0.0;
        bool allFinite = true;
        double peak = 0.0;
    };

    std::vector<Row> rows;
    for (const PerfScenario& sc : perfScenarios())
    {
        // Warmup pass (its result is not measured).
        for (int i = 0; i < warmup; ++i) renderOnce (sc);

        std::vector<double> samples;
        samples.reserve ((size_t) repeats);
        RenderOutcome last;
        for (int i = 0; i < repeats; ++i)
        {
            last = renderOnce (sc);
            samples.push_back (last.micros);
        }
        Row row;
        row.label = sc.label;
        row.minUs = minOf (samples);
        row.medianUs = medianOf (samples);
        row.madUs = madOf (std::move (samples), row.medianUs);
        row.score = row.minUs / normMinUs;
        row.allFinite = last.allFinite;
        row.peak = last.peak;
        rows.push_back (row);

        std::printf ("%-26s min %9.0f us  median %9.0f us  MAD %7.0f us  score %8.4f  peak %.5f\n",
                     row.label, row.minUs, row.medianUs, row.madUs, row.score, row.peak);

        // Functional sanity gates (never timing): every sample finite; the
        // idle render stays silent; an active render carries energy.
        CHECK (row.allFinite, (std::string (sc.label) + ": all samples finite").c_str());
        if (! sc.expectEnergy)
            CHECK (row.peak < 0.01, (std::string (sc.label) + ": idle render stays silent").c_str());
        else
            CHECK (row.peak > 1e-4, (std::string (sc.label) + ": active render carries energy").c_str());
    }

    // --- harvest: print + write the machine-local baseline ---
    if (harvest)
    {
        // juce::var (DynamicObject*) TAKES OWNERSHIP: every object must be
        // heap-allocated and released into its var (a stack object would be
        // deleted by the var).
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("config", configName());
        obj->setProperty ("normalizer_min_us", normMinUs);
        auto scen = std::make_unique<juce::DynamicObject>();
        for (const Row& r : rows)
        {
            auto entry = std::make_unique<juce::DynamicObject>();
            entry->setProperty ("min_us", r.minUs);
            entry->setProperty ("median_us", r.medianUs);
            entry->setProperty ("mad_us", r.madUs);
            entry->setProperty ("score", r.score);
            scen->setProperty (juce::Identifier (r.label), juce::var (entry.release()));
        }
        obj->setProperty ("scenarios", juce::var (scen.release()));
        const juce::String text = juce::JSON::toString (juce::var (obj.release()), true);

        baseline.replaceWithText (text);
        std::printf ("\nharvest: wrote %s\n", baseline.getFullPathName().toRawUTF8());
        std::printf ("%s\n", text.toRawUTF8());
        return true;
    }

    // --- gated compare against the local baseline ---
    if (haveBaseline)
    {
        juce::var parsed = juce::JSON::parse (baseline.loadFileAsString());
        juce::DynamicObject* root = parsed.getDynamicObject();
        if (root == nullptr)
        {
            CHECK (false, "baseline file parses as a JSON object");
            return false;
        }

        const juce::String baseConfig = root->getProperty ("config").toString();
        if (baseConfig != configName())
        {
            std::printf ("baseline config mismatch: file=%s binary=%s — compare skipped\n",
                         baseConfig.toRawUTF8(), configName().toRawUTF8());
        }
        else if (auto* scen = root->getProperty ("scenarios").getDynamicObject())
        {
            for (const Row& r : rows)
            {
                const juce::var entry = scen->getProperty (juce::Identifier (r.label));
                if (! entry.isObject())
                {
                    std::printf ("warn: no baseline entry for %s\n", r.label);
                    continue;
                }
                const double baseScore = (double) entry.getDynamicObject()->getProperty ("score");
                if (baseScore <= 0.0)
                {
                    std::printf ("warn: baseline score for %s is not positive\n", r.label);
                    continue;
                }
                const double ratio = r.score / baseScore;
                if (ratio > 1.20)
                    CHECK (false, (std::string (r.label) + ": score "
                                   + std::to_string (ratio) + "x of baseline (> 1.20)").c_str());
                else if (ratio > 1.10)
                    std::printf ("warn: %s at %.3fx of baseline (> 1.10)\n", r.label, ratio);
                else
                    std::printf ("gate: %s at %.3fx of baseline\n", r.label, ratio);
            }
            // Baseline rows with no live counterpart also deserve a note.
            const int rowCount = (int) rows.size();
            int matched = 0;
            for (const Row& r : rows)
                if (scen->hasProperty (juce::Identifier (r.label))) ++matched;
            if (matched < rowCount)
                std::printf ("warn: %d of %d baseline rows unmatched\n", rowCount - matched, rowCount);
        }
        else
        {
            CHECK (false, "baseline file holds a scenarios object");
        }
    }
    else
    {
        std::printf ("\nno baseline file: smoke pass only (timing does not gate). "
                     "Harvest one with PARVATI_PERF_HARVEST=1.\n");
    }

    return true;
}
