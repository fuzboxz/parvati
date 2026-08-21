// Concurrency / multithreaded fuzz test for Parvati.
//
// Models the REAL plugin threading via tests/mt_harness.h: a background AUDIO
// thread loops processBlock with the transport playing and a HELD NOTE each
// block (so the arpeggiator / note-sequencer actually generate notes), while the
// MESSAGE thread concurrently performs the FULL host surface:
//
//   * EVERY patch/part/arp/seq/option/FX parameter (the whole param table from
//     getPatchParamDescriptors()) swept to min / max / random -- routed through
//     the APVTS listener (the exact host-knob path) so each hits its faithful
//     engine method, including the full 14-slot mod matrix, 4 modifiers, all 64
//     sequencer bytes, every oscillator shape, AND the per-part FX section (3
//     reorderable slots + 16-slot FX mod matrix, ENABLED so the real renderPartFx
//     chain runs on the audio thread, not the dry-copy bypass).
//   * engine modes: polyphony (Mono..Chain on every Part), filter oversampling
//     (1/2/4), parameter smoothing, VCA curve.
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
#include "unified_test_runner.h"
#include <cmath>
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

// Pump the 60 Hz DeferredParamTimer from the message thread. (The plan's
// runDispatchLoopUntil is unavailable here: JUCE 9 gates it behind
// JUCE_MODAL_LOOPS_PERMITTED, which is off for these console targets.
// callPendingTimersSynchronously delivers every timer whose deadline has
// elapsed, which is the same convergence.)
void pumpDeferredTimerMs (int ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
    juce::Timer::callPendingTimersSynchronously();
}

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
        do { op = static_cast<unsigned> (rng.nextInt (11)); } while (! (modeMask & (1u << op)));
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
            case 7: switch (rng.nextInt (2)) {                                                        // engine modes
                        case 0: proc.setOversamplingFactor (rng.nextInt (2) ? 4 : 8); break;         // OS 4/8 (2 is the fresh default, 1 the bit-identical path)
                        case 1: proc.setParameterSmoothing (rng.nextBool()); break;
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
            case 10: { // FX section (Parvati-exclusive): ENABLE a slot so the real renderPartFx
                       // chain runs on the audio thread (not the dry-copy bypass) while we mutate
                       // its params + the FX mod matrix + topology/order concurrently. This is the
                       // MT-write / AT-read race the fxDirty_ flag must guard (run under TSAN).
                      const int slot = 1 + rng.nextInt (kNumFxSlots);               // fx1..3
                      const juce::String pfx = "fx" + juce::String (slot);
                      parvati_test::setParamRaw (proc, (pfx + "_type").toRawUTF8(),    (float) rng.nextInt ((int) FxType::Count));
                      parvati_test::setParamRaw (proc, (pfx + "_enabled").toRawUTF8(), rng.nextBool() ? 1.0f : 0.0f);
                      parvati_test::setParamRaw (proc, (pfx + "_drywet").toRawUTF8(),  (float) rng.nextInt (128));
                      parvati_test::setParamRaw (proc, (pfx + "_param1").toRawUTF8(),  (float) rng.nextInt (128));
                      parvati_test::setParamRaw (proc, (pfx + "_param2").toRawUTF8(),  (float) rng.nextInt (128));
                      parvati_test::setParamRaw (proc, "fx_topo",                       (float) rng.nextInt (3));      // Series / Parallel 1+2->3 / Parallel 1->2+3
                      parvati_test::setParamRaw (proc, "fx_order",                      (float) rng.nextInt (6));      // 6 permutations
                      // an FX-mod routing (16-slot matrix) onto this slot's drywet + a source.
                      const int m = 1 + rng.nextInt (kNumFxMatrixSlots);
                      parvati_test::setParamRaw (proc, ("fxmod" + juce::String (m) + "_source").toRawUTF8(), (float) rng.nextInt (31));
                      parvati_test::setParamRaw (proc, ("fxmod" + juce::String (m) + "_dest").toRawUTF8(),   (float) ((slot - 1) * 5));
                      parvati_test::setParamRaw (proc, ("fxmod" + juce::String (m) + "_amount").toRawUTF8(), (float) (rng.nextInt (127) - 63));
                    } break;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (20 + rng.nextInt (80)));
    }
}
}  // namespace

TEST(concurrency_test)
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    // Optional bisect (was argv[1] in the standalone binary): PARVATI_MT_MASK
    // (hex, env var) selects which chaos op classes run concurrently with the
    // audio thread, so the corruptor can be isolated. Default = all ops.
    const unsigned mask = [] {
        const char* e = std::getenv ("PARVATI_MT_MASK");
        return e ? (unsigned) std::strtoul (e, nullptr, 0) : 0xFFFFu;
    }();
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
    // [3] Polyphony stress: cycle every polyphony mode on every Part while notes
    //     play -- exercises the voice-allocation rebuild path under contention
    //     (the deferred allocationDirty_ release/acquire).
    // -------------------------------------------------------------------------
    std::printf ("\n[3] polyphony stress vs audio\n");
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
                    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
                    {
                        parvati_test::setParamRaw (proc, "part_select", (float) (p + 1));
                        parvati_test::setParamRaw (proc, "part_polyphony", (float) rng.nextInt (5));
                        std::this_thread::sleep_for (std::chrono::microseconds (80));
                    }
                }
            },
            1 << 30, /*heldNote*/ 48);
        check (! out.audioThrew, "polyphony: audio thread did not throw");
        check (out.allFinite, "polyphony: output stayed finite");
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

    // ---------------------------------------------------------------------
    // [7] Deferred audio-thread-origin arp/seq/part_select writes (the
    //     single-writer fix): a NON-message thread drives the REAL host-
    //     automation surface (setValueNotifyingHost, plus the CC102-106
    //     hardware-parity map inside processBlock) while the message thread
    //     performs GUI-style arp edits and pumps the dispatch loop. All
    //     audio-thread-origin writes must be funneled through the deferred
    //     ring to the message thread (no drops), the LAST value must win, and
    //     part_select must converge with currentPart_ tracking.
    // ---------------------------------------------------------------------
    std::printf ("\n[7] deferred arp/seq/part_select: audio-thread writes drain to the MT\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();

        std::atomic<bool> running { true };
        std::atomic<int>  lastArpOctaveCC { -1 };   // raw CC104 value of the last block

        // The "audio thread": renders blocks carrying the CC102-106 arp-map
        // sequence (the real MidiParameterMap path inside processBlock fires
        // parameterChanged ON THIS THREAD, which must defer, never apply) plus
        // direct host-automation-style setValueNotifyingHost calls.
        std::thread audio ([&]()
        {
            juce::AudioBuffer<float> buf (2, 256);
            juce::Random rng { 0x5EED };
            int toggle = 0;
            while (running.load (std::memory_order_relaxed))
            {
                buf.clear();
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 102, 1 + (toggle % 3)), 8);   // arp_mode
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 103, toggle % 6), 16);       // arp_direction
                // CC104 = arp_octave: the CC path scales 7-bit into the 1..4 param
                // range (firmware parameter.Scale: ((range*(v<<1))>>8)+min), so send
                // the full-scale ticks 0/32/64/96 -> octaves 1/2/3/4 and compute
                // the expected value with the same formula below.
                const int octaveCC = 32 * (toggle % 4);
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 104, octaveCC), 24);        // arp_octave
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 105, toggle % 22), 32);     // arp_pattern
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 106, toggle % 15), 40);     // arp_resolution
                proc.processBlock (buf, midi);
                lastArpOctaveCC.store (octaveCC, std::memory_order_relaxed);

                // Host-automation-style writes from this thread (no MIDI):
                // a couple of seq + arp values, deferral branch as well.
                if (auto* p = proc.getApvts().getParameter ("seq1_step0"))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) (toggle % 128)));
                if (auto* p = proc.getApvts().getParameter ("arp_pattern"))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) (toggle % 22)));
                ++toggle;
            }
        });

        // The message thread: GUI-style synchronous arp edits + pump the
        // dispatch loop so the 60 Hz DeferredParamTimer drains the ring.
        for (int i = 0; i < 300; ++i)
        {
            parvati_test::setParamRaw (proc, "arp_resolution", (float) (i % 15));
            pumpDeferredTimerMs (2);
        }
        running.store (false, std::memory_order_relaxed);
        audio.join();
        // Final drain: BOUNDED CONVERGENCE, not a single pump (bug hunt
        // 2026-08-18): a one-shot pumpDeferredTimerMs raced JUCE's timer-
        // thread bookkeeping ~30% of runs (the deadline update vs the
        // synchronous delivery), leaving entries pending. The semantic pinned
        // is "the ring DOES drain" — pump until it has, then assert.
        for (int i = 0; i < 250 && proc.getPendingDeferredCount() > 0; ++i)
            pumpDeferredTimerMs (2);

        check (proc.getDroppedDeferredCount() == 0, "no deferred arp/seq writes dropped (ring never overflowed)");
        check (proc.getPendingDeferredCount() == 0, "deferred ring fully drained");
        {
            // Expected octave from the last CC104, via the firmware Scale the
            // CC path applies (range 4, lo 1): ((4*(v<<1))>>8)+1, clamped.
            const int cc = lastArpOctaveCC.load();
            const int expected = std::max (1, std::min (4, ((4 * (cc << 1)) >> 8) + 1));
            char m[128];
            std::snprintf (m, sizeof (m),
                "last CC104 arp_octave won (engine=%u, last CC=%d, expected=%d)",
                (unsigned) proc.getEngine().getPart (0).pendingConfig_.arpOctave,
                cc, expected);
            check (proc.getEngine().getPart (0).pendingConfig_.arpOctave
                       == (uint8_t) expected, m);
        }

        // part_select from a non-message thread: deferred, then converged. The
        // engine's current part must track the selection AND subsequent byte
        // edits must land on the SELECTED part's storage (currentPart_ tracking).
        {
            std::thread setter ([&]()
            {
                if (auto* p = proc.getApvts().getParameter ("part_select"))
                    p->setValueNotifyingHost (p->convertTo0to1 (3.0f));   // Part 3 (0-based 2)
            });
            setter.join();
            check (proc.getEngine().getCurrentPart() == 0,
                "deferred part_select not applied before the timer fires");
            // Bounded convergence (same fix as the ring-drain check above): a
            // single 25 ms pump raced the timer bookkeeping ~15% of runs.
            for (int i = 0; i < 250 && proc.getEngine().getCurrentPart() != 2; ++i)
                pumpDeferredTimerMs (2);
            check (proc.getEngine().getCurrentPart() == 2,
                "deferred part_select applied: engine current part == 2");
            check (juce::roundToInt (proc.getApvts().getRawParameterValue ("part_select")->load()) == 3,
                "part_select parameter reflects the deferred selection");
            // A byte edit after the deferred switch must land on Part 2 (the
            // onPartSelect-driven currentPart_, not just the parameter value).
            parvati_test::setParamRaw (proc, "part_octave", 2.0f);
            check (proc.getEngine().getPart (2).partBytes[1] == 2,
                "post-switch byte edit routes to the deferred-selected Part 2");
        }
    }

    // ---------------------------------------------------------------------
    // [7] UI-preference state save/restore vs message-thread setters
    //     (iOS hunt 2026-08-19, F-ios-lc-1 — HIGH, crash class).
    //
    // AUv3 hosts call getStateInformation / setStateInformation on
    // NON-message threads (AUM / GarageBand session saves + background
    // autosaves), while the UI writes the same members from the message
    // thread (SettingsPanel theme combo -> setUiTheme, editor language ->
    // setUiLanguage, zoom -> setUiZoom). juce::String is REFCOUNTED — a
    // String copied while another thread reassigns it is a use-after-free
    // class; the scalars (zoom/oversampling) tear silently into the saved
    // state. Pre-fix every access was unsynchronized; post-fix the whole
    // family is guarded by uiPrefsLock_ (get/set accessors + one-lock
    // snapshots in get/setStateInformation).
    //
    // This section is a TSan detector (the release-mode run must also pass
    // and pin the semantics): thread A loops getStateInformation — the host
    // autosave stand-in — while the MAIN thread alternates the setters at
    // full speed. Then semantic pins: setter/getter round-trip + a full
    // get->set state round-trip preserving theme/language/zoom.
    // ---------------------------------------------------------------------
    std::printf ("\n[7] UI-pref state save/restore vs message-thread setters (F-ios-lc-1)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();

        std::atomic<bool> running { true };
        std::atomic<long> savesDone { 0 };
        std::thread hostThread ([&]
        {
            // Host autosave stand-in: serialize full state off-thread.
            juce::MemoryBlock blob;
            while (running.load (std::memory_order_relaxed))
            {
                proc.getStateInformation (blob);
                savesDone.fetch_add (1, std::memory_order_relaxed);
            }
        });

        // Message thread: the SettingsPanel/editor setter surface, alternated.
        // Time-bounded (>= 250 ms AND >= 4000 iterations) so the host-thread
        // saves are guaranteed OVERLAP regardless of machine speed — the
        // assertions below stay deterministic (they never depend on counts).
        const auto stormStart = std::chrono::steady_clock::now();
        for (int i = 0; i < 4000 || std::chrono::duration_cast<std::chrono::milliseconds>
                                      (std::chrono::steady_clock::now() - stormStart).count() < 250; ++i)
        {
            proc.setUiTheme (i % 2 ? "Slate" : "Carbon");
            proc.setUiLanguage (i % 2 ? "fr" : "auto");
            proc.setUiZoom (i % 2 ? 1.25 : 1.0);
            proc.setUiTooltips (i % 2 == 0);
            proc.setUiOversampling (i % 2 ? 2 : 4);
        }
        running.store (false, std::memory_order_relaxed);
        hostThread.join();
        char m[128];
        std::snprintf (m, sizeof (m), "host-thread state saves completed during the setter storm (%ld)",
                       savesDone.load());
        check (savesDone.load() > 0, m);

        // Semantic pins post-storm: setters + locked getters agree.
        proc.setUiTheme ("Slate");
        proc.setUiLanguage ("fr");
        proc.setUiZoom (1.5);
        check (proc.getUiTheme() == "Slate", "setter/getter round-trip: theme");
        check (proc.getUiLanguage() == "fr", "setter/getter round-trip: language");
        check (std::fabs (proc.getUiZoom() - 1.5) < 1e-9, "setter/getter round-trip: zoom");

        // Full host-state round-trip preserves the UI preferences.
        juce::MemoryBlock blob;
        proc.getStateInformation (blob);
        ParvatiAudioProcessor restored;
        restored.prepareToPlay (48000.0, 256);
        restored.setStateInformation (blob.getData(), (int) blob.getSize());
        check (restored.getUiTheme() == "Slate", "state round-trip preserves theme");
        check (restored.getUiLanguage() == "fr", "state round-trip preserves language");
        check (std::fabs (restored.getUiZoom() - 1.5) < 1e-9, "state round-trip preserves zoom");

        // Processor still functional after the storm.
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
        proc.processBlock (buf, midi);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                if (! std::isfinite (buf.getSample (ch, i)))
                    finite = false;
        check (finite, "processor renders finite audio after the setter storm");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "CONCURRENCY TEST: FAILURES" : "CONCURRENCY TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
