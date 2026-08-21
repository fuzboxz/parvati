// UI live-modulation telemetry (engine side) regression test — the durable
// suite for SynthEngine's telemetry block (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
//
// The engine maintains, on the audio thread, ONE seqlock-guarded frame with:
//   * the tracked part's effective mod-source values + a decimated recent
//     history ring (one append per 12 internal ticks ~= 81.7 Hz -> a 128-sample
//     window spans ~1.57 s),
//   * the representative (most-recently-triggered active) voice's envelope
//     stage / progress / level,
//   * the representative voice's EFFECTIVE (modulation-applied) cutoff /
//     resonance / mode,
// and the message thread reads it via readUiTelemetry() with bounded retries.
// resetUiTelemetry() (patch load / part switch / init) invalidates the frame
// through an epoch bump the reader observes immediately; setUiTelemetryPart()
// re-targets the tracked part (the audio thread clears on the change).
//
// Headless: the test drives processBlock directly (the audio thread IS the
// calling thread, so reads between renders are trivially consistent — the
// seqlock is exercised on its steady path, and the stale-epoch / part-switch
// invalidations are observable without concurrency).
//
// Sections:
//   [1] history populates while a note sounds (MOD_SRC_LFO_1 tail moves)
//   [2] frozen after release (voiceActive false, history stops growing)
//   [3] resetUiTelemetry: epoch bump, immediate invalidation, clear, repopulate
//   [4] part tracking: invalid until serviced, then the new part's cleared frame
//   [5] envelope walk: ATTACK -> (progress grows) -> SUSTAIN (pinned), then
//       RELEASE and voiceActive=false after note-off
//   [6] filter: effCutoff matches the LoadSources base mapping exactly and is
//       constant with no modulation; departs from the baseline under a strong
//       env-2 -> cutoff (filter_env) modulation
//
// Built by default. Run with: ./build/parvati_ui_telemetry_test

#include <cmath>
#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"

#include "dsp/fixed_math.h"          // U14ShiftRight6 / S16ClipU14 (base mapping)
#include "ui/ModTelemetryTypes.h"    // parvati::ModTelemetrySnapshot

using ambika::dsp::S16ClipU14;        // the LoadSources base-mapping helpers
using ambika::dsp::U14ShiftRight6;

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Set an Int or Choice parameter to an integer value via the canonical host path
// (setValueNotifyingHost fires APVTS parameterChanged synchronously -> engine).
void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
    {
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (p))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
        else if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (p))
            cp->setValueNotifyingHost (cp->convertTo0to1 (static_cast<float> (value)));
    }
}

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

// Render @p blocks, optionally injecting one MIDI message into block 0.
void renderBlocks (ParvatiAudioProcessor& proc, int blocks, const juce::MidiMessage* inject = nullptr)
{
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0 && inject != nullptr)
            midi.addEvent (*inject, 0);
        buf.clear();
        proc.processBlock (buf, midi);
    }
}

constexpr double kMsPerBlock = 1000.0 * kBlock / kRate;   // 10.67 ms
int blocksForMs (double ms) { return static_cast<int> (ms / kMsPerBlock) + 1; }

// The shared note used everywhere (channel 1 -> Part 0 under the default
// single-part allocation; 12-EDO, no part tuning => 14-bit pitch 60*128).
juce::MidiMessage noteOnMsg()  { return juce::MidiMessage::noteOn  (1, 60, 0.9f); }
juce::MidiMessage noteOffMsg() { return juce::MidiMessage::noteOff (1, 60, 0.0f); }

// One valid read (fails the check on an unexpected invalid frame).
bool readSnap (ParvatiAudioProcessor& proc, parvati::ModTelemetrySnapshot& snap, const char* msg)
{
    const bool ok = proc.getEngine().readUiTelemetry (snap);
    check (ok, msg);
    return ok;
}
}  // namespace

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr int kLen  = parvati::ModTelemetrySnapshot::kHistoryLen;
    constexpr int kLfo1 = 3;   // MOD_SRC_LFO_1 (enum order in dsp/patch.h)

    // =========================================================================
    std::printf ("[1] History populates while a note sounds\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        // Free-running LFO 1, rate byte 60 ~= 1.17 s triangle (lut index 45 ->
        // inc 57 at the 980.4 Hz control cadence): a full window spans more
        // than one cycle, so successive reads visibly move.
        setParam (proc, "env1_lfo_rate", 60);
        proc.syncAllParamsToEngine();
        eng.setUiTelemetryPart (0);
        {
            parvati::ModTelemetrySnapshot s2;
            check (! eng.readUiTelemetry (s2),
                   "[1] invalid until the first render services the tracked part");
        }
        renderBlocks (proc, 2);                          // part service lands

        parvati::ModTelemetrySnapshot snap;
        readSnap (proc, snap, "[1] frame valid once a part is tracked");
        check (snap.part == 0, "[1] frame names the tracked part");
        check (snap.historyCount == 0, "[1] history empty before any note");

        const auto on = noteOnMsg();
        renderBlocks (proc, blocksForMs (500.0), &on);   // ~0.5 s held
        readSnap (proc, snap, "[1] frame valid while held");
        check (snap.voiceActive, "[1] voiceActive true while held");
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[1] history populated (count=%d > 0)", snap.historyCount);
            check (snap.historyCount > 0, m);
        }

        renderBlocks (proc, blocksForMs (2200.0));      // well past the 1.57 s window
        readSnap (proc, snap, "[1] frame valid at full window");
        check (snap.historyCount == kLen, "[1] history saturates at kHistoryLen");

        // The NEWEST tail must move between reads ~0.21 s apart (the LFO swept).
        parvati::ModTelemetrySnapshot a, b;
        eng.readUiTelemetry (a);
        renderBlocks (proc, 20);
        eng.readUiTelemetry (b);
        bool tailMoved = false;
        for (int i = b.historyCount - 16; i < b.historyCount; ++i)
            if (a.history[(size_t) kLfo1 * kLen + (size_t) i]
             != b.history[(size_t) kLfo1 * kLen + (size_t) i]) { tailMoved = true; break; }
        check (tailMoved, "[1] LFO 1 history tail moves between reads");

        // Sources carry the CURRENT value; constants stay pinned (255 for
        // MOD_SRC_CONSTANT_256 — sanity that the frame is not misindexed).
        const int kConst256 = 24;   // MOD_SRC_CONSTANT_256
        check (snap.sources[kConst256] == 255, "[1] constant source reads 255");
        // sources[] refresh BOTH per block and per append, so the current LFO
        // value lies within one decimation window (~12 internal ticks ~= 11 ms
        // of a 1.17 s triangle ~= 5 LSB) of the NEWEST history sample — bounded
        // consistency, not exact equality.
        {
            const int newest = b.historyCount - 1;
            const int diff = std::abs (static_cast<int> (b.sources[kLfo1])
                         - static_cast<int> (b.history[(size_t) kLfo1 * kLen + (size_t) newest]));
            char m[96];
            std::snprintf (m, sizeof (m), "[1] sources consistent with the newest history sample (diff=%d)", diff);
            check (diff <= 8, m);
        }

        // ---- [2] rides on the same held note ----
        std::printf ("[2] Frozen after release\n");
        const auto off = noteOffMsg();
        renderBlocks (proc, 5, &off);
        renderBlocks (proc, blocksForMs (1200.0));      // > the 0.63 s worst-case tail
        readSnap (proc, snap, "[2] frame valid after the release tail");
        check (! snap.voiceActive, "[2] voiceActive false after the tail");
        check (snap.historyCount == kLen, "[2] history kept (not cleared)");
        const int frozen = snap.historyCount;
        renderBlocks (proc, blocksForMs (350.0));       // idle: nothing may append
        readSnap (proc, snap, "[2] frame valid while idle");
        check (snap.historyCount == frozen, "[2] history stops growing while idle");

        // ---- [3] reset semantics ----
        std::printf ("[3] resetUiTelemetry\n");
        const uint32_t e0 = eng.uiTelemetryEpoch();
        eng.resetUiTelemetry();
        check (eng.uiTelemetryEpoch() == e0 + 1, "[3] epoch bumps on reset");
        {
            parvati::ModTelemetrySnapshot s2;
            check (! eng.readUiTelemetry (s2),
                   "[3] read invalid immediately after reset (stale epoch, pre-service)");
        }
        renderBlocks (proc, 5);                          // AT services the wipe (no note)
        readSnap (proc, snap, "[3] valid again once the clear is serviced");
        check (snap.historyCount == 0, "[3] history cleared by the reset");
        check (! snap.voiceActive, "[3] voiceActive false after the reset");

        renderBlocks (proc, 3, &on);
        renderBlocks (proc, blocksForMs (400.0));
        readSnap (proc, snap, "[3] valid after a fresh note");
        check (snap.historyCount > 0, "[3] history repopulates on the next note");

        // ---- [4] part tracking ----
        std::printf ("[4] Part tracking\n");
        eng.setUiTelemetryPart (1);                      // Part 1: no voices (default single-part)
        {
            parvati::ModTelemetrySnapshot s2;
            check (! eng.readUiTelemetry (s2), "[4] invalid until serviced after a part switch");
        }
        renderBlocks (proc, 2);
        readSnap (proc, snap, "[4] valid once the new part is serviced");
        check (snap.part == 1, "[4] frame follows the tracked part");
        check (snap.historyCount == 0, "[4] history cleared for the new part");
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2);
        readSnap (proc, snap, "[4] valid after switching back");
        check (snap.part == 0, "[4] frame back on part 0");
        check (snap.sources[kConst256] == 255, "[4] constant source still 255 (frame intact)");
    }

    // =========================================================================
    std::printf ("[5] Envelope stage walk (Env 3, long attack)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        // attack 80 ~= 1.81 s, decay 50 ~= 0.41 s, sustain 100 (target 200),
        // release 50 ~= 0.41 s — every stage observable with generous margins.
        setParam (proc, "env3_attack", 80);
        setParam (proc, "env3_decay", 50);
        setParam (proc, "env3_sustain", 100);
        setParam (proc, "env3_release", 50);
        proc.syncAllParamsToEngine();
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2);                          // part service

        parvati::ModTelemetrySnapshot snap;
        const auto on = noteOnMsg();
        const auto off = noteOffMsg();

        renderBlocks (proc, 3, &on);                     // ~32 ms in: ATTACK
        readSnap (proc, snap, "[5] valid early in the attack");
        check (snap.voiceActive, "[5] voiceActive during the attack");
        check (snap.envStage[2] == 0, "[5] Env 3 in ATTACK early");
        const float progEarly = snap.envProgress[2];

        renderBlocks (proc, blocksForMs (950.0));        // ~1.0 s < 1.81 s: still ATTACK
        readSnap (proc, snap, "[5] valid mid-attack");
        check (snap.envStage[2] == 0, "[5] Env 3 still ATTACK mid-segment");
        check (snap.envProgress[2] > progEarly, "[5] attack progress grows");

        // DECAY pin (review follow-up: the walk previously skipped stage 1):
        // past the 1.81 s attack, ~0.15 s into the 0.41 s DECAY — well clear
        // of both neighbouring stages.
        renderBlocks (proc, blocksForMs (950.0));        // ~1.96 s total: inside DECAY
        readSnap (proc, snap, "[5] valid inside the decay");
        check (snap.envStage[2] == 1, "[5] Env 3 passes through DECAY (stage 1)");
        check (snap.envProgress[2] >= 0.0f && snap.envProgress[2] <= 1.0f,
               "[5] decay progress within 0..1");

        renderBlocks (proc, blocksForMs (1700.0));       // ~3.66 s total: DECAY done
        readSnap (proc, snap, "[5] valid at the plateau");
        check (snap.envStage[2] == 2, "[5] Env 3 reaches SUSTAIN");
        check (snap.envProgress[2] == 1.0f, "[5] SUSTAIN progress pinned at 1.0");
        check (snap.envLevel[2] > 0.7f && snap.envLevel[2] < 0.85f,
               "[5] Env 3 level rests at the sustain target (~200/255)");

        renderBlocks (proc, 10, &off);                   // ~0.11 s: RELEASE (0.41 s)
        readSnap (proc, snap, "[5] valid during release");
        check (snap.envStage[2] == 3, "[5] Env 3 enters RELEASE after note-off");

        renderBlocks (proc, blocksForMs (1100.0));       // > every envelope's release
        readSnap (proc, snap, "[5] valid after the tail");
        // DEAD is communicated by voiceActive=false (the UI hides its markers
        // on that flag); envStage is a FROZEN tail value while idle — the last
        // active sample rests at/after RELEASE and is deliberately not
        // refreshed (nothing paints from it while !voiceActive). Pin >= RELEASE.
        check (snap.envStage[2] >= 3, "[5] Env 3 frozen at/after RELEASE once idle");
        check (! snap.voiceActive, "[5] voiceActive false once every envelope is DEAD");
    }

    // =========================================================================
    std::printf ("[6] Effective filter values\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        // Kill the init patch's HARDCODED env-2 -> cutoff amount (filter_env
        // 63) so the baseline below is the pure LoadSources base mapping.
        setParam (proc, "filter_env", 0);
        setParam (proc, "filter1_cutoff", 64);
        proc.syncAllParamsToEngine();
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2);

        // Base mapping (dsp/voice.cpp LoadSources -> UpdateDestinations, both
        // hardcoded filter amounts zero):
        //   dst14 = S16ClipU14(cutoff*128 + pitch14 - 8192)
        //   eff   = U14ShiftRight6(dst14)
        // with pitch14 = 60*128 (12-EDO, no part tuning/octave/drift).
        const int cutoffByte = 64;
        const int pitch14 = 60 * 128;
        const int16_t dst14 = S16ClipU14 (static_cast<int16_t> (cutoffByte * 128 + pitch14 - 8192));
        const uint16_t expected = U14ShiftRight6 (static_cast<uint16_t> (dst14));

        const auto on = noteOnMsg();
        renderBlocks (proc, 3, &on);
        renderBlocks (proc, blocksForMs (1000.0));       // env 2 settled at sustain (amount 0)

        parvati::ModTelemetrySnapshot snap;
        readSnap (proc, snap, "[6] valid while held");
        check (snap.voiceActive, "[6] voiceActive while held");
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[6] effCutoff == LoadSources base mapping (== %u)", (unsigned) expected);
            check (snap.effCutoff == expected, m);
        }
        check (snap.effResonance == 0, "[6] effResonance baseline 0 (reso knob 0)");
        check (snap.filterMode == 0, "[6] filterMode LP");

        bool constant = true;
        for (int i = 0; i < 20; ++i)
        {
            renderBlocks (proc, 2);
            parvati::ModTelemetrySnapshot s2;
            if (! eng.readUiTelemetry (s2) || s2.effCutoff != expected)
                constant = false;
        }
        check (constant, "[6] effCutoff constant with no modulation");

        // Strong env-2 -> cutoff (the firmware "filter envelope" path): with
        // filter_env 63 and env2 sustain 100 the 14-bit sum clips, so the live
        // byte departs from the baseline by a wide margin while held.
        ParvatiAudioProcessor proc2;
        proc2.prepareToPlay (kRate, kBlock);
        auto& eng2 = proc2.getEngine();
        setParam (proc2, "filter_env", 63);
        setParam (proc2, "env2_sustain", 100);
        setParam (proc2, "filter1_cutoff", 64);
        proc2.syncAllParamsToEngine();
        eng2.setUiTelemetryPart (0);
        renderBlocks (proc2, 2);
        renderBlocks (proc2, 3, &on);
        renderBlocks (proc2, blocksForMs (1000.0));      // env 2 rests at its sustain

        parvati::ModTelemetrySnapshot snap2;
        readSnap (proc2, snap2, "[6] valid (modulated patch) while held");
        check (snap2.effCutoff > expected + 40,
               "[6] effCutoff departs from the baseline under env-2 -> cutoff");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "UI TELEMETRY TEST: FAILURES" : "UI TELEMETRY TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
