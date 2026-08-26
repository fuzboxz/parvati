// Reusable multi-thread test harness for Hellcat.
//
// A real plugin renders on a realtime AUDIO thread while the host / GUI operate
// on a MESSAGE thread (parameter automation, patch loads, part switches, host
// state save/restore). Single-threaded tests cannot surface the data races and
// memory-safety bugs that live in that split. This harness spins a background
// AUDIO thread that loops processBlock -- with the transport playing and a held
// note each block so the arpeggiator / note-sequencer actually generate notes --
// while a caller lambda mutates state on the calling (message) thread.
//
// An OPTIONAL third thread fires MIDI (note/CC/bend/pressure) through the
// processor's thread-safe MidiMessageCollector (the UI click-play surface),
// stressing that concurrent path too. The engine's internal state is still
// mutated by a SINGLE message thread (its seqlocks assume a sole writer, an
// invariant the processor enforces by deferring audio-thread-origin
// arp/seq/part_select parameter writes back to the message thread), so this
// stays within the real plugin's threading contract.
//
// Run under the sanitizers to surface bugs (tools/run_sanitizers.sh builds
// the tests-only trees on demand):
//   ThreadSanitizer  -> message<->audio data races:
//     ./build_san_tsan/hellcat_unified_tests concurrency_test
//   AddressSanitizer + UBSan -> OOB reads/writes, use-after-free, UB:
//     ./build_san_asan/hellcat_unified_tests concurrency_test
//   Races/bugs are timing-dependent -> run a few times.

#ifndef HELLCAT_MT_HARNESS_H_
#define HELLCAT_MT_HARNESS_H_

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "PluginProcessor.h"

namespace hellcat_test
{

struct Outcome
{
    bool   audioThrew     = false;   // the audio thread threw / crashed
    bool   allFinite      = true;    // no NaN/Inf in any rendered block
    bool   noSubnormals   = true;    // no denormal float (audio-thread stall risk)
    double peak           = 0.0;     // loudest sample seen (audio was produced)
    long   blocksRendered = 0;       // how many blocks the audio thread got through
};

// A valid random raw (denormalized) APVTS value for any descriptor: the choice
// index for a Choice param, or a uniform int in [minValue, maxValue] otherwise.
// Used to exercise the FULL ~181-parameter surface uniformly.
inline float randomRawValue (const PatchParamDescriptor& d, juce::Random& rng)
{
    if (d.choices != nullptr && d.choices->size() > 0)
        return static_cast<float> (rng.nextInt (d.choices->size()));
    const int lo = d.minValue;
    const int hi = d.maxValue;
    if (hi <= lo)
        return static_cast<float> (lo);
    return static_cast<float> (lo + rng.nextInt (hi - lo + 1));
}

// Run `messageOp` on the CALLING (message) thread while a background AUDIO thread
// renders up to `audioBlocks` blocks (it stops once `messageOp` returns). Each
// audio block carries a held note (`heldNote` 0..127; <=0 => silent) so the
// arpeggiator / note-sequencer note-generation path is exercised. processBlock's
// standalone default has the transport playing, so arp/seq run. If `fireMidi` is
// set, a third thread injects random MIDI through the processor's
// MidiMessageCollector (the thread-safe UI->audio path).
inline Outcome runConcurrent (HellcatAudioProcessor& proc,
                              const std::function<void()>& messageOp,
                              int audioBlocks,
                              int heldNote = 60,
                              int blockSize = 256,
                              bool fireMidi = false)
{
    Outcome out;
    std::atomic<bool>   running { true };
    std::atomic<bool>   threw    { false };
    std::atomic<bool>   finite   { true };
    std::atomic<bool>   subnormal{ true };
    std::atomic<double> peak     { 0.0 };
    std::atomic<long>   rendered { 0 };

    const juce::MidiMessage noteOn = (heldNote > 0)
        ? juce::MidiMessage::noteOn (1, juce::jlimit (0, 127, heldNote), (uint8_t) 110)
        : juce::MidiMessage();

    std::thread audio ([&]()
    {
        juce::AudioBuffer<float> buf (2, blockSize);
        try
        {
            for (int b = 0; b < audioBlocks && running.load (std::memory_order_relaxed); ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (heldNote > 0)
                    midi.addEvent (noteOn, 0);
                proc.processBlock (buf, midi);
                double p = 0.0;
                for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                {
                    const auto* d = buf.getReadPointer (ch);
                    for (int i = 0; i < buf.getNumSamples(); ++i)
                    {
                        const float  vf = d[i];
                        const double v  = static_cast<double> (vf);
                        if (! std::isfinite (v))
                            finite.store (false, std::memory_order_relaxed);
                        if (std::fabs (vf) > 0.0f && std::fabs (vf) < std::numeric_limits<float>::min())
                            subnormal.store (false, std::memory_order_relaxed);
                        p = std::max (p, std::fabs (v));
                    }
                }
                peak.store (p, std::memory_order_relaxed);
                rendered.store (b + 1, std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            threw.store (true, std::memory_order_relaxed);
        }
    });

    // Optional concurrent MIDI injector (the thread-safe UI click-play surface).
    std::thread midiInjector;
    juce::Random midiRng { 0xFEE1D };
    if (fireMidi)
    {
        midiInjector = std::thread ([&]()
        {
            while (running.load (std::memory_order_relaxed))
            {
                switch (midiRng.nextInt (4))
                {
                    case 0: proc.addMidiEvent (juce::MidiMessage::noteOn   (1 + midiRng.nextInt (6), 36 + midiRng.nextInt (48), (uint8_t) 90)); break;
                    case 1: proc.addMidiEvent (juce::MidiMessage::noteOff  (1 + midiRng.nextInt (6), 36 + midiRng.nextInt (48), (uint8_t) 64)); break;
                    case 2: proc.addMidiEvent (juce::MidiMessage::pitchWheel (1 + midiRng.nextInt (6), midiRng.nextInt (16384)));                 break;
                    default:proc.addMidiEvent (juce::MidiMessage::controllerEvent (1 + midiRng.nextInt (6), 1 + midiRng.nextInt (32), midiRng.nextInt (128))); break;
                }
                std::this_thread::sleep_for (std::chrono::microseconds (30 + midiRng.nextInt (120)));
            }
        });
    }

    messageOp();   // runs on this (message) thread, concurrently with the audio thread

    running.store (false, std::memory_order_relaxed);
    audio.join();
    if (fireMidi)
        midiInjector.join();

    out.audioThrew     = threw.load();
    out.allFinite      = finite.load();
    out.noSubnormals   = subnormal.load();
    out.peak           = peak.load();
    out.blocksRendered = rendered.load();
    return out;
}

// Set an APVTS parameter by its raw (denormalized) value. Fires parameterChanged
// synchronously on the CALLING thread -- the same path a host knob / automation
// uses on the message thread.
inline void setParamRaw (HellcatAudioProcessor& proc, const char* id, float value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

}  // namespace hellcat_test

#endif  // HELLCAT_MT_HARNESS_H_
