// UI live-modulation telemetry (engine side) regression test — the durable
// suite for SynthEngine's telemetry block (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
//
// The engine maintains, on the audio thread, ONE seqlock-guarded frame with:
//   * the tracked part's effective mod-source values + a decimated recent
//     history ring (one append per 12 internal ticks ~= 81.7 Hz -> a 256-sample
//     window spans ~3.13 s; doubled from 128/1.57 s on 2026-08-22 user
//     feedback — the strips scrolled visibly faster than the previews),
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
#include "unified_test_runner.h"
#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)

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

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

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
TEST(ui_telemetry_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr int kLen  = parvati::ModTelemetrySnapshot::kHistoryLen;
    constexpr int kLfo1 = 3;   // MOD_SRC_LFO_1 (enum order in dsp/patch.h)

    // =========================================================================
    std::printf ("[1] History always scrolls: zero buffer at start, live while held\n");
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
        renderBlocks (proc, 2, nullptr, kBlock);                          // part service lands

        // ---- ALWAYS-ON CONTRACT (2026-08-21): before any note the history
        // POPULATES WITH ZEROS (the strips start at a zero buffer and always
        // scroll; a per-voice modulator that is not running = zero). The
        // constants keep their literal values (a constant's state is its
        // value) — the frame is not misindexed.
        parvati::ModTelemetrySnapshot snap;
        renderBlocks (proc, blocksForMs (300.0), nullptr, kBlock);        // idle: zero rows append
        readSnap (proc, snap, "[1] frame valid while idle (pre-note)");
        check (snap.historyCount > 0, "[1] history populates BEFORE any note (zero buffer start)");
        {
            bool allZero = true;
            for (int i = 0; i < snap.historyCount && allZero; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0
                 || snap.history[(size_t) 0 * kLen + (size_t) i] != 0)   // ENV 1
                    allZero = false;
            check (allZero, "[1] idle rows are ZERO (LFO/ENV not running = 0)");
        }
        const int kConst256 = 24;   // MOD_SRC_CONSTANT_256
        check (snap.sources[kConst256] == 255, "[1] constant source reads 255 while idle");
        check (! snap.voiceActive, "[1] voiceActive truthful (false) while idle");

        const auto on = noteOnMsg();
        renderBlocks (proc, blocksForMs (500.0), &on, kBlock);   // ~0.5 s held
        readSnap (proc, snap, "[1] frame valid while held");
        check (snap.voiceActive, "[1] voiceActive true while held");
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[1] history growing while held (count=%d)", snap.historyCount);
            check (snap.historyCount > 20, m);
        }

        renderBlocks (proc, blocksForMs (3600.0), nullptr, kBlock);      // well past the ~3.13 s window
        readSnap (proc, snap, "[1] frame valid at full window");
        check (snap.historyCount == kLen, "[1] history saturates at kHistoryLen");

        // The NEWEST tail must move between reads ~0.21 s apart (the LFO swept).
        parvati::ModTelemetrySnapshot a, b;
        eng.readUiTelemetry (a);
        renderBlocks (proc, 20, nullptr, kBlock);
        eng.readUiTelemetry (b);
        bool tailMoved = false;
        for (int i = b.historyCount - 16; i < b.historyCount; ++i)
            if (a.history[(size_t) kLfo1 * kLen + (size_t) i]
             != b.history[(size_t) kLfo1 * kLen + (size_t) i]) { tailMoved = true; break; }
        check (tailMoved, "[1] LFO 1 history tail moves between reads");

        // Sources carry the CURRENT value; constants stay pinned (255 for
        // MOD_SRC_CONSTANT_256 — sanity that the frame is not misindexed).
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
        std::printf ("[2] Falls to zero after release and keeps scrolling\n");
        const auto off = noteOffMsg();
        renderBlocks (proc, 5, &off, kBlock);
        renderBlocks (proc, blocksForMs (3800.0), nullptr, kBlock);      // > tail (~0.63 s) + the full-scale idle drag-out (<= 256 appends ~ 3.13 s)
        readSnap (proc, snap, "[2] frame valid after the release tail");
        check (! snap.voiceActive, "[2] voiceActive false after the tail");
        check (snap.historyCount == kLen, "[2] history kept (not cleared)");
        // The NEWEST window must have FALLEN TO ZERO and STAY there while
        // idle (the actual state: the per-voice LFO is not running).
        {
            bool zeros = true;
            for (int i = snap.historyCount - 16; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0) { zeros = false; break; }
            check (zeros, "[2] LFO fell to zero after release (not frozen mid-air)");
        }
        renderBlocks (proc, blocksForMs (350.0), nullptr, kBlock);       // still idle: must KEEP appending
        readSnap (proc, snap, "[2] frame valid while idle");
        check (snap.historyCount == kLen, "[2] history keeps appending while idle (saturated)");
        {
            bool zeros2 = true;
            for (int i = snap.historyCount - 16; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0) { zeros2 = false; break; }
            check (zeros2, "[2] LFO STAYS zero while idle (actual state)");
        }

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
        renderBlocks (proc, 5, nullptr, kBlock);                          // AT services the wipe (no note)
        readSnap (proc, snap, "[3] valid again once the clear is serviced");
        // ALWAYS-ON: the wipe clears, then the idle zero rows immediately
        // begin repopulating (a handful of fresh appends in 5 blocks is the
        // contract, not a leak) — all zero, never stale values.
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[3] history wiped then re-fills with zeros (count=%d < 16)", snap.historyCount);
            check (snap.historyCount < 16, m);
            bool zeros = true;
            for (int i = 0; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0) { zeros = false; break; }
            check (zeros, "[3] post-reset rows are zeros (stale window gone)");
        }
        check (! snap.voiceActive, "[3] voiceActive false after the reset");

        renderBlocks (proc, 3, &on, kBlock);
        renderBlocks (proc, blocksForMs (400.0), nullptr, kBlock);
        readSnap (proc, snap, "[3] valid after a fresh note");
        check (snap.historyCount > 0, "[3] history repopulates on the next note");

        // ---- [4] part tracking ----
        std::printf ("[4] Part tracking\n");
        eng.setUiTelemetryPart (1);                      // Part 1: no voices (default single-part)
        {
            parvati::ModTelemetrySnapshot s2;
            check (! eng.readUiTelemetry (s2), "[4] invalid until serviced after a part switch");
        }
        renderBlocks (proc, 2, nullptr, kBlock);
        readSnap (proc, snap, "[4] valid once the new part is serviced");
        check (snap.part == 1, "[4] frame follows the tracked part");
        // ALWAYS-ON: the new part's window restarts from (near) zero — a
        // couple of fresh zero appends may already have landed.
        check (snap.historyCount < 16, "[4] history (re)starts fresh for the new part");
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2, nullptr, kBlock);
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
        renderBlocks (proc, 2, nullptr, kBlock);                          // part service

        parvati::ModTelemetrySnapshot snap;
        const auto on = noteOnMsg();
        const auto off = noteOffMsg();

        renderBlocks (proc, 3, &on, kBlock);                     // ~32 ms in: ATTACK
        readSnap (proc, snap, "[5] valid early in the attack");
        check (snap.voiceActive, "[5] voiceActive during the attack");
        check (snap.envStage[2] == 0, "[5] Env 3 in ATTACK early");
        const float progEarly = snap.envProgress[2];

        renderBlocks (proc, blocksForMs (950.0), nullptr, kBlock);        // ~1.0 s < 1.81 s: still ATTACK
        readSnap (proc, snap, "[5] valid mid-attack");
        check (snap.envStage[2] == 0, "[5] Env 3 still ATTACK mid-segment");
        check (snap.envProgress[2] > progEarly, "[5] attack progress grows");

        // DECAY pin (review follow-up: the walk previously skipped stage 1):
        // past the 1.81 s attack, ~0.15 s into the 0.41 s DECAY — well clear
        // of both neighbouring stages.
        renderBlocks (proc, blocksForMs (950.0), nullptr, kBlock);        // ~1.96 s total: inside DECAY
        readSnap (proc, snap, "[5] valid inside the decay");
        check (snap.envStage[2] == 1, "[5] Env 3 passes through DECAY (stage 1)");
        check (snap.envProgress[2] >= 0.0f && snap.envProgress[2] <= 1.0f,
               "[5] decay progress within 0..1");

        renderBlocks (proc, blocksForMs (1700.0), nullptr, kBlock);       // ~3.66 s total: DECAY done
        readSnap (proc, snap, "[5] valid at the plateau");
        check (snap.envStage[2] == 2, "[5] Env 3 reaches SUSTAIN");
        check (snap.envProgress[2] == 1.0f, "[5] SUSTAIN progress pinned at 1.0");
        check (snap.envLevel[2] > 0.7f && snap.envLevel[2] < 0.85f,
               "[5] Env 3 level rests at the sustain target (~200/255)");

        renderBlocks (proc, 10, &off, kBlock);                   // ~0.11 s: RELEASE (0.41 s)
        readSnap (proc, snap, "[5] valid during release");
        check (snap.envStage[2] == 3, "[5] Env 3 enters RELEASE after note-off");

        renderBlocks (proc, blocksForMs (1100.0), nullptr, kBlock);       // > every envelope's release
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
        renderBlocks (proc, 2, nullptr, kBlock);

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
        renderBlocks (proc, 3, &on, kBlock);
        renderBlocks (proc, blocksForMs (1000.0), nullptr, kBlock);       // env 2 settled at sustain (amount 0)

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
            renderBlocks (proc, 2, nullptr, kBlock);
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
        renderBlocks (proc2, 2, nullptr, kBlock);
        renderBlocks (proc2, 3, &on, kBlock);
        renderBlocks (proc2, blocksForMs (1000.0), nullptr, kBlock);      // env 2 rests at its sustain

        parvati::ModTelemetrySnapshot snap2;
        readSnap (proc2, snap2, "[6] valid (modulated patch) while held");
        check (snap2.effCutoff > expected + 40,
               "[6] effCutoff departs from the baseline under env-2 -> cutoff");
    }

    // =========================================================================
    std::printf ("[7] Slow-envelope history is smooth (representative-voice stability)\n");
    {
        // A SINGLE held note with a ~6 s attack (attack byte maxed): the ENV 1
        // history must be a clean ramp — consecutive samples differ by ~1 byte.
        // The regression this pins: the telemetry follows the REPRESENTATIVE
        // voice (most-recently-triggered); if that pick churns between voices
        // mid-hold (or any indexing bug interleaves ring entries), the ENV row
        // jumps around and the pill strip reads as noise ("way too jumpy to be
        // true with slow envelopes" — 2026-08-21).
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        setParam (proc, "env1_attack", 127);   // byte 127 ~= slowest attack
        proc.syncAllParamsToEngine();
        proc.getEngine().setUiTelemetryPart (0);   // wire tracking (the editor's job in app life)
        const auto on = noteOnMsg();
        renderBlocks (proc, blocksForMs (2500.0), &on, kBlock);   // ~2.5 s INTO the attack

        parvati::ModTelemetrySnapshot snap;
        if (readSnap (proc, snap, "[7] valid while the slow attack runs"))
        {
            const uint8_t* env = snap.history + (size_t) 0 * kLen;   // MOD_SRC_ENV_1 == 0
            const int n = snap.historyCount;
            int maxJump = 0, bigJumps = 0, prev = -1;
            bool monotonic = true;
            for (int i = 0; i < n; ++i)
            {
                const int v = env[(size_t) i];
                if (prev >= 0)
                {
                    const int d = std::abs (v - prev);
                    if (d > maxJump) maxJump = d;
                    if (d > 4) ++bigJumps;
                    if (v < prev) monotonic = false;
                }
                prev = v;
            }
            {
                char m[128];
                std::snprintf (m, sizeof (m),
                               "[7] ENV 1 ramp is smooth (maxJump=%d bytes, want <= 4)", maxJump);
                check (maxJump <= 4, m);
                std::snprintf (m, sizeof (m),
                               "[7] ENV 1 ramp is monotonic non-decreasing (n=%d, first=%d last=%d)",
                               n, n > 0 ? (int) env[0] : -1, n > 0 ? (int) env[n - 1] : -1);
                check (n > 0 && monotonic, m);
                check (bigJumps == 0, "[7] no per-sample jumps > 4 bytes during the attack");
            }

            // And the CURRENT value must sit mid-ramp (0 < v < 255): the
            // attack is still running at 2.5 s of a ~6 s sweep.
            const int now = snap.sources[0];
            {
                char m[96];
                std::snprintf (m, sizeof (m),
                               "[7] ENV 1 mid-attack current value (=%d, want 20..235)", now);
                check (now > 20 && now < 235, m);
            }
            // STICKY-VOICE pin (the "jumpy slow envelope" regression): a
            // RE-STRIKE while the first note still sounds must NOT interleave
            // the two envelopes — the ENV row keeps telling the first note's
            // story (monotonic attack) instead of snapping to the new voice.
            // Note 62 re-triggers a FRESH voice while note 60 is held; the
            // trace must stay monotonic across the re-strike.
            const auto on2 = juce::MidiMessage::noteOn (1, 62, 0.9f);
            renderBlocks (proc, blocksForMs (1500.0), &on2, kBlock);
            {
                parvati::ModTelemetrySnapshot s3;
                if (readSnap (proc, s3, "[7] valid after the re-strike"))
                {
                    const uint8_t* e2 = s3.history + 0 * kLen;
                    bool mono2 = true; int prev2 = -1, maxJump2 = 0;
                    for (int i = 0; i < s3.historyCount; ++i)
                    {
                        const int v = e2[(size_t) i];
                        if (prev2 >= 0)
                        {
                            if (v < prev2) mono2 = false;
                            maxJump2 = std::max (maxJump2, std::abs (v - prev2));
                        }
                        prev2 = v;
                    }
                    check (mono2 && maxJump2 <= 4,
                           "[7] re-strike does not interleave envelopes (sticky telemetry voice)");
                }
            }
        }
    }

    // =========================================================================
    std::printf ("[8] ALWAYS-ON contract: scroll forever, actual state per source class\n");
    {
        // The user-requested semantics end-to-end: (a) zero buffer at start,
        // (b) LFO animates while held, (c) LFO falls to zero after release and
        // STAYS zero while idle, (d) a controller (mod wheel, CC1) moved while
        // IDLE shows on the WHEEL strip while the LFO strip is zero.
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        setParam (proc, "env1_lfo_rate", 60);          // free-running LFO 1
        proc.syncAllParamsToEngine();
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2, nullptr, kBlock);                          // service the tracked part

        parvati::ModTelemetrySnapshot snap;
        constexpr int kWheel = 17;                       // MOD_SRC_WHEEL

        // (a) zero buffer at start: history grows while idle, all values 0.
        renderBlocks (proc, blocksForMs (250.0), nullptr, kBlock);
        readSnap (proc, snap, "[8] valid while idle (pre-note)");
        check (snap.historyCount > 0, "[8a] history populates from a zero buffer (idle)");
        {
            bool zeros = true;
            for (int i = 0; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0
                 || snap.history[(size_t) kWheel * kLen + (size_t) i] != 0)
                    { zeros = false; break; }
            check (zeros, "[8a] all rows zero before any note/wheel");
        }

        // (b) held note: the LFO row moves (nonzero span within the window).
        const auto on = noteOnMsg();
        renderBlocks (proc, blocksForMs (600.0), &on, kBlock);
        readSnap (proc, snap, "[8] valid while held");
        {
            int lo = 255, hi = 0;
            for (int i = snap.historyCount - 24; i < snap.historyCount; ++i)
            {
                const int v = snap.history[(size_t) kLfo1 * kLen + (size_t) i];
                lo = std::min (lo, v); hi = std::max (hi, v);
            }
            char m[96];
            std::snprintf (m, sizeof (m), "[8b] LFO animates while held (24-sample span %d..%d)", lo, hi);
            check (hi - lo > 16, m);
        }

        // (c) release: LFO decays out and STAYS zero while idle.
        const auto off = noteOffMsg();
        renderBlocks (proc, 5, &off, kBlock);
        renderBlocks (proc, blocksForMs (1500.0), nullptr, kBlock);      // past the tail
        readSnap (proc, snap, "[8] valid after release");
        {
            bool zeros = true;
            for (int i = snap.historyCount - 24; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0) { zeros = false; break; }
            check (zeros, "[8c] LFO fell to zero and stays zero while idle");
        }

        // (d) mod wheel moved WHILE IDLE: the WHEEL strip shows it, others 0.
        const auto wheel = juce::MidiMessage::controllerEvent (1, 1, 100);   // CC1 -> MOD_SRC_WHEEL
        renderBlocks (proc, 5, &wheel, kBlock);
        renderBlocks (proc, blocksForMs (300.0), nullptr, kBlock);       // idle: appends carry the wheel
        readSnap (proc, snap, "[8] valid after idle wheel move");
        {
            bool wheelUp = false, lfoStillZero = true;
            for (int i = snap.historyCount - 24; i < snap.historyCount; ++i)
            {
                if (snap.history[(size_t) kWheel * kLen + (size_t) i] > 64) wheelUp = true;
                if (snap.history[(size_t) kLfo1 * kLen + (size_t) i] != 0) lfoStillZero = false;
            }
            check (wheelUp, "[8d] WHEEL strip shows the held controller value while idle");
            check (lfoStillZero, "[8d] LFO strip stays zero (per-voice generator idle)");
            char m[96];
            std::snprintf (m, sizeof (m), "[8d] sources[WHEEL]=%d (live truth)", (int) snap.sources[kWheel]);
            check (snap.sources[kWheel] == 200, m);     // CC 100 << 1 = 200
        }
    }

    std::printf ("[9] PITCH_BEND idle truth: rests at 128, tracks the wheel, survives a wipe\n");
    {
        // Pre-existing bug (fixed 2026-08-23): the idle row read
        // lastModSources_[PITCH_BEND] — only written from a SOUNDING voice's
        // ring — so at startup / after a telemetry wipe (before any note) the
        // Pitch Bend pill strip scrolled from the FLOOR (0) although the wheel
        // rests at 128 = mid = 50%. The row now derives from the standing-bend
        // LATCH (lastWheel_), like WHEEL/WHEEL_2/EXPRESSION derive from the
        // part's voice table. Pins: rest == 128 with NO note ever played, a
        // wheel move while IDLE tracks immediately (both directions), and a
        // telemetry wipe (the patch-load seam) does NOT resurrect the zero.
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2, nullptr, kBlock);                          // service the tracked part

        constexpr int kPitchBend = 16;                   // MOD_SRC_PITCH_BEND
        parvati::ModTelemetrySnapshot snap;

        // (a) rest, no note ever: the strip AND the current source read MID.
        renderBlocks (proc, blocksForMs (250.0), nullptr, kBlock);
        if (readSnap (proc, snap, "[9] valid while idle (never a note)"))
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[9a] sources[PITCH_BEND]=%d at rest (want 128)",
                          (int) snap.sources[kPitchBend]);
            check (snap.sources[kPitchBend] == 128, m);
            bool allMid = snap.historyCount > 0;
            for (int i = 0; i < snap.historyCount; ++i)
                if (snap.history[(size_t) kPitchBend * kLen + (size_t) i] != 128) { allMid = false; break; }
            check (allMid, "[9a] idle history rows all 128 (strip starts at mid, not the floor)");
        }

        // (b) wheel moved while IDLE tracks both directions (no note needed).
        const auto bendUp = juce::MidiMessage::pitchWheel (1, 16383);   // full +
        renderBlocks (proc, 5, &bendUp, kBlock);
        renderBlocks (proc, blocksForMs (150.0), nullptr, kBlock);
        readSnap (proc, snap, "[9] valid after idle bend up");
        check (snap.sources[kPitchBend] == 255,
               "[9b] full-up wheel reads 255 while idle");
        const auto bendDown = juce::MidiMessage::pitchWheel (1, 0);     // full -
        renderBlocks (proc, 5, &bendDown, kBlock);
        renderBlocks (proc, blocksForMs (150.0), nullptr, kBlock);
        readSnap (proc, snap, "[9] valid after idle bend down");
        check (snap.sources[kPitchBend] == 1,
               "[9b] full-down wheel reads 1 while idle");

        // (c) telemetry WIPE (patch-load seam): the row re-derives from the
        // latch — the pre-fix code resurrected the zero buffer here.
        eng.resetUiTelemetry();
        renderBlocks (proc, blocksForMs (150.0), nullptr, kBlock);       // appends after the wipe
        if (readSnap (proc, snap, "[9] valid after the wipe"))
        {
            char m[96];
            std::snprintf (m, sizeof (m), "[9c] sources[PITCH_BEND]=%d after wipe (latch held)",
                          (int) snap.sources[kPitchBend]);
            check (snap.sources[kPitchBend] == 1, m);
        }

        // (d) re-centering while idle returns the strip to mid.
        const auto bendCentre = juce::MidiMessage::pitchWheel (1, 8192);
        renderBlocks (proc, 5, &bendCentre, kBlock);
        renderBlocks (proc, blocksForMs (150.0), nullptr, kBlock);
        readSnap (proc, snap, "[9] valid after re-centre");
        check (snap.sources[kPitchBend] == 128,
               "[9d] re-centred wheel reads 128 while idle");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "UI TELEMETRY TEST: FAILURES" : "UI TELEMETRY TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
