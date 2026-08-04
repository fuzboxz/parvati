// Concurrency / multithreaded fuzz test for Parvati.
//
// Models the REAL plugin threading via tests/mt_harness.h: a background AUDIO
// thread loops processBlock with the transport playing and a HELD NOTE each
// block (so the arpeggiator / note-sequencer actually generate notes), while the
// MESSAGE thread concurrently performs the FULL host surface:
//
//   * EVERY patch/part/arp/seq/option parameter (the whole ~181-param table from
//     getPatchParamDescriptors()) swept to min / max / random -- routed through
//     the APVTS listener (the exact host-knob path) so each hits its faithful
//     engine method, including the full 14-slot mod matrix, 4 modifiers, all 64
//     sequencer bytes, and every oscillator shape.
//   * engine modes: voice capacity (Hardware/Extended), polyphony (Mono..Chain
//     on every Part), filter oversampling (1/2/4), parameter smoothing, VCA curve.
//   * multitimbrality: per-Part MIDI channel, key zone, voice-card allocation.
//   * patch / multi / template loads (.PRO / .MUL / .parvati).
//   * host-state get/set cycling (DAW autosave + scene restore).
//   * optional concurrent MIDI injection (the thread-safe UI click-play path).
//
// Run under the sanitizers to surface bugs (races/bugs are timing-dependent ->
// run repeatedly; see tools/run_sanitizers.sh):
//   ThreadSanitizer (data races):   -DPARVATI_ENABLE_TSAN=ON
//   AddressSanitizer + UBSan (mem): -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "mt_harness.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

double peakAbs (const juce::AudioBuffer<float>& buf)
{
    double p = 0.0;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const auto* d = buf.getReadPointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            p = std::max (p, std::fabs (static_cast<double> (d[i])));
    }
    return p;
}

// Resolve preset files that exist in this checkout (tests degrade gracefully if a
// bank is absent). PARVATI_SOURCE_DIR is defined for this target in CMake.
const juce::File srcDir()        { return juce::File (PARVATI_SOURCE_DIR); }
// NOTE: findChildFiles returns Array<File> by value; .getFirst() on the
// temporary would dangle. Capture the array, return a copy.
juce::File findFactoryMulti()
{
    const auto d = srcDir().getChildFile ("presets/FACTORY_MULTI");
    if (! d.exists()) return {};
    auto f = d.findChildFiles (juce::File::findFiles, false, "*.MUL");
    return f.isEmpty() ? juce::File() : f.getFirst();
}
juce::File findFactoryPatch()
{
    const auto d = srcDir().getChildFile ("presets/FACTORY");
    if (! d.exists()) return {};
    auto f = d.findChildFiles (juce::File::findFiles, true, "*.PRO");
    return f.isEmpty() ? juce::File() : f.getFirst();
}
juce::File findParvatiTemplate()
{
    const auto d = ParvatiAudioProcessor::getTemplatesDir();
    if (! d.exists()) return {};
    auto f = d.findChildFiles (juce::File::findFiles, false, "*.parvati");
    return f.isEmpty() ? juce::File() : f.getFirst();
}

// A pool of preset files from the source tree: factory .PRO patches (a handful
// per bank A/B/F/S for speed), the factory .MUL multis, and the .parvati multi
// templates. Cached after the first scan. Drives rapid preset switching under
// the audio thread -- the "clicking through the browser" load that exercises
// every load path (.PRO / .MUL / .parvati-multi) and the deferred voice reset /
// part re-seed / allocation rebuild each one triggers.
const juce::Array<juce::File>& collectPresetFiles()
{
    static const juce::Array<juce::File> files = []()
    {
        juce::Array<juce::File> f;
        const juce::File root = srcDir().getChildFile ("presets");
        if (! root.exists()) return f;
        for (const auto& bank : { "FACTORY/A", "FACTORY/B", "FACTORY/F", "FACTORY/S" })
        {
            const auto b = root.getChildFile (bank);
            if (! b.exists()) continue;
            const auto ps = b.findChildFiles (juce::File::findFiles, false, "*.PRO");
            for (int i = 0; i < ps.size() && i < 8; ++i)   // cap per bank for speed
                f.add (ps.getReference (i));
        }
        f.addArray (root.getChildFile ("FACTORY_MULTI").findChildFiles (juce::File::findFiles, false, "*.MUL"));
        f.addArray (root.getChildFile ("TEMPLATES").findChildFiles (juce::File::findFiles, false, "*.parvati"));
        return f;
    }();
    return files;
}

// Deterministic FULL coverage: for every Part (1..6), switch to it and set EVERY
// parameter to its min, then max, then a random in-range value. Guarantees the
// entire surface (osc/mix/filter/3x env+lfo/voice lfo/14 mods/4 modifiers/part/
// seq/arp/options) is exercised against the audio thread. Routed through the
// APVTS listener so each param hits its faithful engine method.
void fullParameterSweep (ParvatiAudioProcessor& proc, juce::Random& rng)
{
    const auto& descs = getPatchParamDescriptors();
    for (int part = 1; part <= SynthEngine::getNumParts(); ++part)
    {
        parvati_test::setParamRaw (proc, "part_select", static_cast<float> (part));
        std::this_thread::sleep_for (std::chrono::microseconds (40));
        for (const auto& d : descs)
        {
            if (d.paramID == "part_select")
                continue;
            parvati_test::setParamRaw (proc, d.paramID.c_str(), static_cast<float> (d.minValue));
            parvati_test::setParamRaw (proc, d.paramID.c_str(), static_cast<float> (d.maxValue));
            parvati_test::setParamRaw (proc, d.paramID.c_str(), parvati_test::randomRawValue (d, rng));
        }
    }
}

// Deterministically drive EVERY oscillator render path with extreme parameters
// (param/range/detune maxed) so shape-dependent OOB / UB (e.g. a wave-table
// index exceeding the resource table) surfaces RELIABLY rather than by timing
// luck. Runs single-threaded: the bug is in the render path itself, independent
// of threading. `outFinite` reports whether any shape produced NaN/Inf.
void dspExtremeShapeRender (ParvatiAudioProcessor& proc, bool& outFinite)
{
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer noteOn;
    noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
    outFinite = true;
    for (int shape = 0; shape < 38; ++shape)   // WAVEFORM_LAST == 38
    {
        parvati_test::setParamRaw (proc, "osc1_shape", (float) shape);
        parvati_test::setParamRaw (proc, "osc1_param",  127.0f);
        parvati_test::setParamRaw (proc, "osc1_range",   24.0f);
        parvati_test::setParamRaw (proc, "osc1_detune",  64.0f);
        parvati_test::setParamRaw (proc, "osc2_shape", (float) shape);
        parvati_test::setParamRaw (proc, "osc2_param",  127.0f);
        buf.clear();
        proc.processBlock (buf, noteOn);   // executes this shape's Render() path
        for (int ch = 0; ch < buf.getNumChannels() && outFinite; ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                if (! std::isfinite (buf.getSample (ch, i))) { outFinite = false; break; }
    }
}

// Rapidly switch presets on the MESSAGE thread while the audio thread renders.
// Cycles through every load path (.PRO / .MUL / .parvati-multi) and interleaves a
// .parvati single-patch save->load round-trip (the 4th path) plus part-select
// switches. Each load defers a voice reset / part re-seed / allocation rebuild
// to the audio thread -- the race-prone path this is designed to hammer.
void presetSwitchStress (ParvatiAudioProcessor& proc, const juce::Array<juce::File>& files,
                         juce::Random& rng, int switches)
{
    const juce::File roundTrip = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("parvati_mt_roundtrip.parvati");
    for (int i = 0; i < switches; ++i)
    {
        if (files.isEmpty() || rng.nextInt (5) == 0)
        {
            // .parvati single-patch save -> load round-trip (loadParvatiPatchFile path).
            (void) proc.saveParvatiPatchFile (roundTrip);
            (void) proc.loadParvatiPatchFile (roundTrip);
        }
        else
        {
            const juce::File& f = files.getReference (rng.nextInt (files.size()));
            const auto ext = f.getFileExtension().toLowerCase();
            if (ext == ".pro")            (void) proc.loadProgramFile (f);
            else if (ext == ".mul")       (void) proc.loadMultiFile (f);
            else if (ext == ".parvati")   (void) proc.loadParvatiMultiFile (f);
        }
        if (rng.nextInt (4) == 0)   // editor flips which Part it shows mid-switch
            parvati_test::setParamRaw (proc, "part_select",
                (float) (1 + rng.nextInt (SynthEngine::getNumParts())));
        std::this_thread::sleep_for (std::chrono::microseconds (50 + rng.nextInt (250)));
    }
    roundTrip.deleteFile();
}

// The full message-thread (host) surface, exercised RANDOMLY and concurrently
// with the audio thread. `savedState` (if non-null) enables host-state restore.
// `modeMask` selects which op classes run (bit0..bit9) so a corruptor can be
// isolated (pass hex on argv[1]). The random parameter edit (bit0) draws from the
// FULL descriptor table, not a fixed subset.
void chaosSurface (ParvatiAudioProcessor& proc, juce::Random& rng, int iters,
                   const juce::MemoryBlock* savedState, unsigned modeMask = 0xFFFFu)
{
    const auto& descs = getPatchParamDescriptors();
    const int nDescs = static_cast<int> (descs.size());
    const juce::File multi = findFactoryMulti();
    const juce::File patch = findFactoryPatch();
    const juce::File tpl   = findParvatiTemplate();
    auto& engine = proc.getEngine();

    for (int i = 0; i < iters; ++i)
    {
        unsigned op;
        do { op = static_cast<unsigned> (rng.nextInt (10)); } while (! (modeMask & (1u << op)));
        switch (op)
        {
            case 0: { const auto& d = descs[(size_t) rng.nextInt (nDescs)];           // random param (full table)
                      parvati_test::setParamRaw (proc, d.paramID.c_str(), parvati_test::randomRawValue (d, rng)); } break;
            case 1: parvati_test::setParamRaw (proc, "arp_mode",       (float) rng.nextInt (3));      // Off/Arp/Seq
                    parvati_test::setParamRaw (proc, "arp_direction",  (float) rng.nextInt (6));
                    parvati_test::setParamRaw (proc, "arp_octave",     (float) (1 + rng.nextInt (4)));
                    parvati_test::setParamRaw (proc, "arp_resolution", (float) rng.nextInt (15));
                    parvati_test::setParamRaw (proc, "seq_length_1",   (float) (1 + rng.nextInt (32)));
                    parvati_test::setParamRaw (proc, "seqnote_step0",  (float) rng.nextInt (256)); break;
            case 2: parvati_test::setParamRaw (proc, "part_polyphony", (float) rng.nextInt (5)); break; // Mono..Chain
            case 3: parvati_test::setParamRaw (proc, "vca_curve",    (float) rng.nextInt (2));        // global options
                    parvati_test::setParamRaw (proc, "filter_card",  (float) rng.nextInt (3));
                    parvati_test::setParamRaw (proc, "filter_drive", (float) rng.nextInt (8)); break;
            case 4: { const int sel = 1 + rng.nextInt (SynthEngine::getNumParts());
                      parvati_test::setParamRaw (proc, "part_select", (float) sel); } break;
            case 5: { juce::MemoryBlock b; proc.getStateInformation (b); } break;                    // host autosave
            case 6: if (savedState != nullptr) proc.setStateInformation (savedState->getData(), (int) savedState->getSize()); break;  // host restore
            case 7: switch (rng.nextInt (3)) {                                                        // engine modes
                        case 0: proc.setUiVoiceMode (rng.nextInt (2)); break;                        // Hardware/Extended
                        case 1: proc.setOversamplingFactor (rng.nextInt (2) ? 2 : 4); break;         // OS 2/4 (1 left for default)
                        case 2: proc.setParameterSmoothing (rng.nextBool()); break;
                    } break;
            case 8: { const int p = rng.nextInt (SynthEngine::getNumParts());                        // multitimbral routing
                      engine.setPartMidiChannel (p, rng.nextInt (17));                               // 0=Omni, 1..16
                      const int lo = rng.nextInt (128), hi = lo + rng.nextInt (128 - lo);
                      engine.setPartKeyZone (p, lo, hi);
                      engine.setPartVoiceAllocation (p, static_cast<uint8_t> (rng.nextInt (64))); } break;
            case 9: switch (rng.nextInt (3)) {                                                        // file loads
                        case 0: if (multi.existsAsFile()) (void) proc.loadMultiFile (multi); break;
                        case 1: if (patch.existsAsFile()) (void) proc.loadProgramFile (patch); break;
                        case 2: if (tpl.existsAsFile())   (void) proc.loadParvatiMultiFile (tpl); break;
                    } break;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (20 + rng.nextInt (80)));
    }
}
}  // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    // Optional bisect: PARVATI_MT_MASK (hex, via argv[1]) selects which chaos op
    // classes run concurrently with the audio thread, so the corruptor can be
    // isolated. Default = all ops.
    const unsigned mask = (argc > 1) ? (unsigned) std::strtoul (argv[1], nullptr, 0) : 0xFFFFu;
    std::printf ("=== Parvati Concurrency / Multithreaded Fuzz (mask=0x%x) ===\n", mask);

    // -------------------------------------------------------------------------
    // [1] FULL deterministic parameter sweep vs the audio thread: every Part,
    //     every parameter to min/max/random, while a note renders. The broadest
    //     single coverage pass (osc shapes incl. wavetable/wavequence, full mod
    //     matrix, modifiers, all seq bytes).
    // -------------------------------------------------------------------------
    std::printf ("\n[1] full parameter sweep (all parts, all params) vs audio\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
        proc.syncAllParamsToEngine();
        juce::Random rng { 0xC0FFEE };
        const auto out = parvati_test::runConcurrent (proc,
            [&] { fullParameterSweep (proc, rng); },
            1 << 30, /*heldNote*/ 60);
        char m[160];
        std::snprintf (m, sizeof (m), "audio thread did not throw (%ld blocks)", out.blocksRendered);
        check (! out.audioThrew, m);
        check (out.allFinite, "output stayed finite (no NaN/Inf)");
        check (out.noSubnormals, "no denormal floats (audio-thread stall risk)");
    }

    // ---------------------------------------------------------------------
    // [1b] DSP extreme-shape render: every oscillator shape at maxed params,
    //      single-threaded. Reliably surfaces shape-dependent OOB/UB (the
    //      wavequence / wavetable table-index paths) under the sanitizers.
    // ---------------------------------------------------------------------
    std::printf ("\n[1b] DSP extreme-shape render (every osc shape, maxed params)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        bool finite = true;
        dspExtremeShapeRender (proc, finite);
        check (finite, "every osc shape rendered finite output (OOB/UB shows under sanitizers)");
    }

    // -------------------------------------------------------------------------
    // [2] Randomized chaos: the full host surface (random params from the whole
    //     table + arp/seq + polyphony + options + part switch + host state +
    //     engine modes + multitimbral routing + loads) on the default patch,
    //     WITH concurrent MIDI injection through the thread-safe UI path.
    // -------------------------------------------------------------------------
    std::printf ("\n[2] chaos: full surface + engine modes + MIDI injection\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        juce::Random rng { 0xBA5EBA11 };
        const auto out = parvati_test::runConcurrent (proc,
            [&] { chaosSurface (proc, rng, 2000, &saved, mask); },
            1 << 30, /*heldNote*/ 60, 256, /*fireMidi*/ true);
        check (! out.audioThrew, "chaos: audio thread did not throw");
        check (out.allFinite, "chaos: output stayed finite");
        check (out.noSubnormals, "chaos: no denormal floats");
        check (out.blocksRendered > 0, "chaos: audio thread progressed under contention");
    }

    // -------------------------------------------------------------------------
    // [3] Polyphony x voice-mode stress: cycle every polyphony mode on every
    //     Part and flip Hardware/Extended while notes play -- exercises the
    //     voice-allocation rebuild path under contention (the deferred
    //     allocationDirty_ release/acquire).
    // -------------------------------------------------------------------------
    std::printf ("\n[3] polyphony x voice-mode stress vs audio\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        juce::Random rng { 0xFEEDFACE };
        const auto out = parvati_test::runConcurrent (proc,
            [&]()
            {
                for (int round = 0; round < 12; ++round)
                {
                    proc.setUiVoiceMode (round & 1);   // Extended (16) <-> Hardware (6)
                    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
                    {
                        parvati_test::setParamRaw (proc, "part_select", (float) (p + 1));
                        parvati_test::setParamRaw (proc, "part_polyphony", (float) rng.nextInt (5));
                        std::this_thread::sleep_for (std::chrono::microseconds (80));
                    }
                }
            },
            1 << 30, /*heldNote*/ 48);
        check (! out.audioThrew, "poly/voicemode: audio thread did not throw");
        check (out.allFinite, "poly/voicemode: output stayed finite");
    }

    // -------------------------------------------------------------------------
    // [4] TekDrums factory multi (note-sequencer) + host-state cycling -- the
    //     live crash repro. Note 36 drives the note-sequencer while the message
    //     thread restores state / loads / edits.
    // -------------------------------------------------------------------------
    std::printf ("\n[4] TekDrums multi: note-sequencer vs host state + loads\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        const juce::File tekDrums = findFactoryMulti();
        check (tekDrums.existsAsFile() ? proc.loadMultiFile (tekDrums) : true, "loaded a factory multi");
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        juce::Random rng { 0xDECAFBAD };
        const auto out = parvati_test::runConcurrent (proc,
            [&] { chaosSurface (proc, rng, 1500, &saved, 0xFFFFu); },
            1 << 30, /*heldNote*/ 36);
        check (! out.audioThrew, "TekDrums: audio thread did not throw");
        check (out.allFinite, "TekDrums: output stayed finite");
        check (out.peak > 0.0, "TekDrums: note-sequencer produced audio");
    }

    // ---------------------------------------------------------------------
    // [6] Preset switching under load: rapidly cycle through the factory preset
    //     pool (.PRO / .MUL / .parvati-multi) plus a .parvati single-patch
    //     save->load round-trip, while a note renders. Hammers every load path's
    //     deferred voice reset / part re-seed / allocation rebuild under audio
    //     contention (the classic preset-click race surface).
    // ---------------------------------------------------------------------
    std::printf ("\n[6] preset switching vs audio (all load paths)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        const auto& pool = collectPresetFiles();
        std::printf ("     preset pool: %d files\n", pool.size());
        juce::Random rng { 0xFEEDC0DE };
        const auto out = parvati_test::runConcurrent (proc,
            [&] { presetSwitchStress (proc, pool, rng, 600); },
            1 << 30, /*heldNote*/ 60);
        check (! out.audioThrew, "preset switch: audio thread did not throw");
        check (out.allFinite, "preset switch: output stayed finite");
        check (out.noSubnormals, "preset switch: no denormal floats");
    }

    // -------------------------------------------------------------------------
    // [5] Post-run: the engine still responds to a note (single-threaded check).
    // -------------------------------------------------------------------------
    std::printf ("\n[5] engine still responds after the concurrent run\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
        proc.processBlock (buf, midi);
        double p = peakAbs (buf);
        for (int b = 0; b < 6; ++b)
        {
            juce::AudioBuffer<float> rb (2, 256);
            rb.clear();
            proc.processBlock (rb, midi);
            p = std::max (p, peakAbs (rb));
        }
        check (p > 0.001, "engine responds to a note (peak > 0.001)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "CONCURRENCY TEST: FAILURES" : "CONCURRENCY TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
