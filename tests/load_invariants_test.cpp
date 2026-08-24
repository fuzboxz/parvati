// Deterministic tooling T3 — EDGE-CORPUS LOAD INVARIANTS.
//
// Property under test (the "unclamped-loaded-bytes" class this exists to pin):
//   ANY file load that SUCCEEDS must leave the engine INSIDE its invariant
//   ranges on every mirrored surface — the staged arp/sequencer config
//   (pendingConfig + the live objects after the audio thread services it),
//   the PartData bytes the descriptors define (spread / polyphony / raga),
//   the per-part routing (channel 0..16, keyzone 0..127 — inverted zones
//   allowed: the firmware wrap-around-zone semantics, W8), the
//   voice-slot count (0..16), the resolved tuning mode (0..33), the installed
//   FX slot types (0..FxType::Count-1) and the sanitized part name (<=16
//   chars) — AND must render 32 blocks inside a 10 s watchdog with finite
//   audio.
//
//   Historical instances of the class this corpus targets:
//     - a .MUL with PartData[7]=5 staged arpMode=5 — isActive() (notes
//       swallowed into the held-key stack) but neither Arp nor Sequencer:
//       a SILENT part (fixed by the wave-5 clamps in
//       stageArpSeqFromPartBytes);
//     - PartData[9]=0 (arpOctave 0) hung the audio thread in
//       Arpeggiator::stepArpeggio's Random loop (fixed by the same clamps);
//     - channel byte 17/255 wrapped uint8 and silently deadened a part
//       (fixed by the loader-side jlimits).
//
//   FINDING AT AUTHORING TIME (the tool's first catch, kept as a regression):
//   the .parvati MULTI YAML path (ParvatiPreset.cpp applyParvatiMulti) staged
//   arp_mode / arp_direction / arp_pattern / arp_resolution / seq_length_*
//   RAW (jlimit 0..255) — only arp_octave was clamped — so a hand-edited
//     params: { arp_mode: 5 }  in a .parvati multi reproduced the silent-part
//   bug through a different loader. The corpus cases below re-prove the
//   descriptor-range clamps added at that staging site: if they ever regress,
//   this test fails with the exact category + value.
//
// Corpus: hand-written .parvati multi YAML documents (the human-editable
// format — the only format whose bytes a user edits directly), one edge per
// document, on part 0 of an otherwise default 6-part multi. Out-of-range
// values are EXPECTED TO CLAMP (the format's contract: a present key is the
// whole truth, hand-edited values clamp to the engine's accepted ranges);
// valid boundary values are EXPECTED TO ROUND-TRIP EXACTLY. Any load that
// returns FALSE for a structurally valid document is reported as a finding
// (a "should load" note) — every corpus document here should load.
//
// Canary self-check: a hand-broken engine (raw out-of-range values written
// through the message-thread seams the loaders use) MUST trip >= 5 invariant
// categories — proving the checker itself can fail before the corpus runs.
//
// Determinism: no RNG, no wall clock beyond the watchdog budget; every
// document is a fixed string.
//
// Run: ./build_unified/parvati_unified_tests load_invariants_test

#include <atomic>
#include "unified_test_runner.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <utility>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const juce::String& msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg.toRawUTF8());
    std::fflush (stdout);
    if (! cond) ++g_failures;
}

//==============================================================================
// One corpus part: absent keys (-1 / empty) are OMITTED from the document so
// each case exercises the loader's own key handling; raw text param values
// keep the emission byte-exact (including deliberately out-of-range ints).
struct PartSpec
{
    int channel = -999;                       // -999 = omit key
    int keyLo = -999, keyHi = -999;           // -999 = omit both keys
    int voiceSlots = -999;                    // -999 = omit key
    juce::String name;                        // empty = omit key
    int tuningMode = -999;                    // -999 = omit key
    juce::String tuningOffsets;               // emitted after "tuning_offsets: "
    std::vector<std::pair<juce::String, juce::String>> params;   // insertion order
};

PartSpec defaultPart()
{
    PartSpec p;
    p.channel   = 1;
    p.keyLo     = 0;
    p.keyHi     = 127;
    p.voiceSlots = 4;
    return p;
}

juce::String emitMultiYaml (const std::array<PartSpec, 6>& parts, const char* docName)
{
    juce::String y;
    y << "format: parvati-multi\nversion: 1\nparvati_version: 0.1.0\n";
    y << "name: \"" << docName << "\"\nauthor: \"\"\nparts:\n";
    for (const auto& p : parts)
    {
        y << "  - channel: "        << p.channel   << "\n";
        y << "    keyzone_low: "    << p.keyLo     << "\n";
        y << "    keyzone_high: "   << p.keyHi     << "\n";
        y << "    voice_slots: "    << p.voiceSlots << "\n";
        if (p.name.isNotEmpty())
            y << "    name: \"" << p.name << "\"\n";
        if (p.tuningMode != -999)
        {
            y << "    tuning_mode: " << p.tuningMode << "\n";
            if (p.tuningOffsets.isNotEmpty())
                y << "    tuning_offsets: " << p.tuningOffsets << "\n";
        }
        if (! p.params.empty())
        {
            y << "    params:\n";
            for (const auto& kv : p.params)
                y << "      " << kv.first << ": " << kv.second << "\n";
        }
    }
    return y;
}

//==============================================================================
// Render watchdog (same contract as the T1 loader fuzzer): 32 processBlocks on
// a worker thread inside a 10 s budget; non-finite output is flagged. A load
// whose staged bytes wedge the render loop is caught as a timeout and
// hard-exits (a spinning audio thread cannot be reclaimed).
struct WatchResult { bool completed; bool finite; };

WatchResult renderWatchdog (ParvatiAudioProcessor& proc)
{
    std::atomic<bool> finite { true };
    auto fut = std::async (std::launch::async, [&proc, &finite]() {
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer empty;
        for (int b = 0; b < 32; ++b)
        {
            buf.clear();
            proc.processBlock (buf, empty);
            if (! finite.load())
                break;
            for (int ch = 0; ch < 2 && finite.load(); ++ch)
                for (int i = 0; i < 256; ++i)
                    if (! std::isfinite (buf.getSample (ch, i))) { finite.store (false); break; }
        }
    });
    if (fut.wait_for (std::chrono::seconds (10)) != std::future_status::ready)
    {
        std::printf ("      [HANG] processBlock did not finish 32 blocks within 10 s\n");
        std::fflush (stdout);
        std::_Exit (97);   // cannot join a wedged audio thread
    }
    fut.get();
    return { true, finite.load() };
}

//==============================================================================
// The invariant checker. Returns the number of VIOLATIONS (does NOT touch
// g_failures — the canary uses the same checker to prove it can fail; the
// corpus wraps it in check()). Every line prints the offending category +
// value so a regression names its byte immediately.
int countInvariantViolations (ParvatiAudioProcessor& proc, const char* label)
{
    auto& e = proc.getEngine();
    int v = 0;
    auto flag = [&v, label] (const char* cat, int part, int got, int lo, int hi)
    {
        std::printf ("      [violation] %s / %s part %d: %d (want %d..%d)\n",
                     label, cat, part, got, lo, hi);
        ++v;
    };

    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
    {
        // ---- routing ----
        const int ch = (int) e.getPartChannel (p);
        if (ch < 0 || ch > 16) flag ("channel", p, ch, 0, 16);
        const int lo = (int) e.getPartKeyrangeLow (p);
        const int hi = (int) e.getPartKeyrangeHigh (p);
        if (lo < 0 || lo > 127) flag ("keyzone-low", p, lo, 0, 127);
        if (hi < 0 || hi > 127) flag ("keyzone-high", p, hi, 0, 127);
        // NOTE: lo > hi is a LEGAL wrap-around zone (firmware multi.h
        // accept_note; W8 item 1) — only out-of-range ends are violations.

        // ---- voice slots ----
        const int slots = e.getPartVoiceSlots (p);
        if (slots < 0 || slots > 16) flag ("voice-slots", p, slots, 0, 16);

        // ---- tuning ----
        // 0..32 only: the custom-table mode 33 was removed with the
        // custom-tuning subsystem (2026-08-19); a legacy file carrying it
        // loads as 12-EDO (0).
        const int tm = e.resolvedTuningMode (p);
        if (tm < 0 || tm > 32) flag ("tuning-mode", p, tm, 0, 32);

        // ---- sanitized name ----
        if (e.getPartName (p).length() > 16)
            flag ("name-length", p, e.getPartName (p).length(), 0, 16);

        // ---- staged arp/seq config (the message-thread authority) ----
        // Ranges are the DESCRIPTOR ranges (ParameterLayout.cpp): arp_mode 3
        // choices, arp_direction 6, arp_octave 1..4, arp_pattern 22,
        // arp_resolution 15, seq_length_* 1..16.
        const auto cfg = e.getPart (p).readPendingConfig();
        if (cfg.arpMode       < 0 || cfg.arpMode       > 2)  flag ("arp-mode",       p, cfg.arpMode,       0, 2);
        if (cfg.arpDirection  < 0 || cfg.arpDirection  > 5)  flag ("arp-direction",  p, cfg.arpDirection,  0, 5);
        if (cfg.arpOctave     < 1 || cfg.arpOctave     > 4)  flag ("arp-octave",     p, cfg.arpOctave,     1, 4);
        if (cfg.arpPattern    < 0 || cfg.arpPattern    > 21) flag ("arp-pattern",    p, cfg.arpPattern,    0, 21);
        if (cfg.arpResolution < 0 || cfg.arpResolution > 14) flag ("arp-resolution", p, cfg.arpResolution, 0, 14);
        for (int s = 0; s < 3; ++s)
            if (cfg.seqLength[(size_t) s] < 1 || cfg.seqLength[(size_t) s] > 16)
                flag ("seq-length", p, cfg.seqLength[(size_t) s], 1, 16);

        // ---- PartData semantic bytes (descriptor-defined) ----
        const auto& pb = e.getPart (p).partBytes;
        if (pb[3]  > 40)                                     flag ("spread-byte",    p, pb[3],  0, 40);
        if (pb[15] > 4)                                      flag ("polyphony-byte", p, pb[15], 0, 4);
        if (pb[4]  > 32)                                     flag ("raga-byte",      p, pb[4],  0, 32);

        // ---- installed FX slot types ----
        for (int slot = 0; slot < 3; ++slot)
        {
            const int ft = (int) e.fxChainSlotTypeForTest (p, slot);
            if (ft < 0 || ft >= (int) FxType::Count)
                flag ("fx-slot-type", p, ft, 0, (int) FxType::Count - 1);
        }
    }
    return v;
}

// The live arp/seq objects after the audio thread has serviced the staged
// config must equal the staged snapshot exactly (servicePendingConfig applies
// pendingConfig verbatim — a drift means the staged bytes never landed, the
// "loaded config silently ignored" sub-case).
int countLiveConfigDrift (ParvatiAudioProcessor& proc)
{
    auto& e = proc.getEngine();
    int v = 0;
    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
    {
        const auto& part = e.getPart (p);
        const auto cfg = part.readPendingConfig();
        if (part.arp.getMode()       != cfg.arpMode)          ++v;
        if (part.arp.getDirection()  != cfg.arpDirection)     ++v;
        if (part.arp.getOctave()     != cfg.arpOctave)        ++v;
        if (part.arp.getPattern()    != cfg.arpPattern)       ++v;
        if (part.seq.getSequenceLength (0) != cfg.seqLength[0]) ++v;
    }
    return v;
}

//==============================================================================
// One corpus case: build the document, load through the REAL entry point,
// assert success + invariants + watchdog + live-config equality.
struct CaseResult { bool loaded; int violations; bool finite; int drift; };

CaseResult runCase (const juce::String& yaml, const char* label)
{
    const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("parvati_load_invariants_case.parvati");
    f.replaceWithText (yaml);

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    CaseResult r { false, 0, true, 0 };
    r.loaded = proc.loadParvatiMultiFile (f);
    if (! r.loaded)
        return r;   // reported by the caller as a "should load" finding
    r.violations = countInvariantViolations (proc, label);
    const auto w = renderWatchdog (proc);          // also services the staged config
    r.finite = w.finite;
    r.drift = countLiveConfigDrift (proc);
    return r;
}
}  // namespace

//==============================================================================
TEST(load_invariants_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== Parvati load invariants (edge corpus) ===\n");
    auto withPart0 = [] (const PartSpec& p0)
    {
        std::array<PartSpec, 6> parts;
        for (int i = 0; i < 6; ++i) parts[(size_t) i] = defaultPart();
        parts[0] = p0;
        return parts;
    };
    auto param = [] (PartSpec p, const char* k, const char* v)
    {
        p.params.push_back ({ k, v });
        return p;
    };

    // label, yaml, expectLoaded (every corpus document is structurally valid —
    // a false here is itself a finding), exact post-check (optional)
    struct Case
    {
        const char* label;
        juce::String yaml;
    };
    const std::vector<Case> cases = {
        // ---- arp mode: valid boundaries round-trip, out-of-range clamps ----
        { "arp_mode:0 (valid Off)",       emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "0")),  "CaseArpMode0") },
        { "arp_mode:2 (valid Sequencer)", emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "2")),  "CaseArpMode2") },
        { "arp_mode:3 (clamps to 2)",     emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "3")),  "CaseArpMode3") },
        { "arp_mode:5 (clamps to 2)",     emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "5")),  "CaseArpMode5") },
        { "arp_mode:255 (clamps to 2)",   emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "255")),"CaseArpMode255") },
        // ---- arp octave: the AT-hang byte (0) must never stage ----
        { "arp_octave:0 (clamps to 1)",   emitMultiYaml (withPart0 (param (defaultPart(), "arp_octave", "0")),   "CaseArpOct0") },
        { "arp_octave:5 (clamps to 4)",   emitMultiYaml (withPart0 (param (defaultPart(), "arp_octave", "5")),   "CaseArpOct5") },
        { "arp_octave:255 (clamps to 4)", emitMultiYaml (withPart0 (param (defaultPart(), "arp_octave", "255")), "CaseArpOct255") },
        // ---- the remaining arp fields ----
        { "arp_direction:6 (clamps to 5)", emitMultiYaml (withPart0 (param (defaultPart(), "arp_direction", "6")),   "CaseArpDir6") },
        { "arp_pattern:200 (clamps to 21)",emitMultiYaml (withPart0 (param (defaultPart(), "arp_pattern", "200")),   "CaseArpPat200") },
        { "arp_resolution:250 (clamps to 14)", emitMultiYaml (withPart0 (param (defaultPart(), "arp_resolution", "250")), "CaseArpRes250") },
        // ---- sequencer lengths (descriptor 1..16) ----
        { "seq_length_1:0 (clamps to 1)",  emitMultiYaml (withPart0 (param (defaultPart(), "seq_length_1", "0")),  "CaseSeqLen0") },
        { "seq_length_2:17 (clamps to 16)",emitMultiYaml (withPart0 (param (defaultPart(), "seq_length_2", "17")), "CaseSeqLen17") },
        // ---- channel: 0=Omni / 16 valid; 17 / 255 clamp to 16 (17 wraps to a
        //      dead part on hardware — the uint8-wrap class) ----
        { "channel:0 (Omni valid)",       emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.channel = 0;   return p; }()), "CaseCh0") },
        { "channel:16 (valid)",           emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.channel = 16;  return p; }()), "CaseCh16") },
        { "channel:17 (clamps to 16)",    emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.channel = 17;  return p; }()), "CaseCh17") },
        { "channel:255 (clamps to 16)",   emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.channel = 255; return p; }()), "CaseCh255") },
        // ---- keyzones: clamp to 0..127; inverted zones now stay inverted
        // (wrap-around zone contract, W8 item 1) ----
        { "keyzone_low:200 (clamps to 127)", emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.keyLo = 200; return p; }()), "CaseLo200") },
        { "keyzone_high:300 (clamps to 127)",emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.keyHi = 300; return p; }()), "CaseHi300") },
        { "keyzone 90/40 inverted (wrap zone PRESERVED)", emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.keyLo = 90; p.keyHi = 40; return p; }()), "CaseZoneInv") },
        // ---- voice slots: 0 disables, 16 maxes, 17/99 clamp ----
        { "voice_slots:0 (disables part)", emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.voiceSlots = 0;  return p; }()), "CaseSlots0") },
        { "voice_slots:1 (valid)",         emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.voiceSlots = 1;  return p; }()), "CaseSlots1") },
        { "voice_slots:16 (valid)",        emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.voiceSlots = 16; return p; }()), "CaseSlots16") },
        { "voice_slots:17 (clamps to 16)", emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.voiceSlots = 17; return p; }()), "CaseSlots17") },
        { "voice_slots:99 (clamps to 16)", emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.voiceSlots = 99; return p; }()), "CaseSlots99") },
        // ---- PartData byte params (descriptor-clamped via the byte bridge) ----
        { "part_spread:255 (clamps to 40)",     emitMultiYaml (withPart0 (param (defaultPart(), "part_spread", "255")),     "CaseSpread255") },
        { "part_polyphony:6 (clamps to 4)",     emitMultiYaml (withPart0 (param (defaultPart(), "part_polyphony", "6")),   "CasePoly6") },
        { "part_raga:33 (clamps to 32)",        emitMultiYaml (withPart0 (param (defaultPart(), "part_raga", "33")),       "CaseRaga33") },
        // ---- legacy tuning keys (custom subsystem removed 2026-08-19) ----
        { "tuning_mode:33 + offsets (legacy custom -> 12-EDO)",
          emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.tuningMode = 33; p.tuningOffsets = "10, -20, 30, -40, 50, -60, 70, -80, 90, -100, 110, -120"; return p; }()), "CaseTuneCustom") },
        { "tuning_mode:34 (out of range -> 12-EDO)",
          emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.tuningMode = 34; return p; }()), "CaseTune34") },
        // ---- names: the sanitizer + the raw-nested-quote document ----
        { "name 40 chars (sanitizes to <=16)",
          emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.name = "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDD"; return p; }()), "CaseName40") },
        { "name with raw nested quotes",
          emitMultiYaml (withPart0 ([] { auto p = defaultPart(); p.name = "He said \"Hi\" there"; return p; }()), "CaseNameQuotes") },
    };

    int idx = 0;
    for (const auto& c : cases)
    {
        ++idx;
        std::printf ("[%2d] %s\n", idx, c.label);
        const auto r = runCase (c.yaml, c.label);
        // Every corpus document is structurally valid: the load must SUCCEED
        // (a false is a rejected-should-load finding, not a pass).
        check (r.loaded, juce::String (c.label) + ": loads");
        if (r.loaded)
        {
            check (r.violations == 0,
                   juce::String (c.label) + ": post-load invariants hold (0 violations)");
            check (r.finite,     juce::String (c.label) + ": 32 blocks render finite (10 s watchdog)");
            check (r.drift == 0, juce::String (c.label) + ": live arp/seq == staged config after service");
        }
    }

    // ---- targeted post-conditions (the exact clamped/round-tripped values,
    //      so a regression names the byte) ----
    {
        // arp_mode:5 must clamp to 2 — the authoring-time finding. The corpus
        // table already proves the RANGE; this pins the exact clamped value.
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_load_invariants_case.parvati");
        f.replaceWithText (emitMultiYaml (withPart0 (param (defaultPart(), "arp_mode", "5")), "PostArpMode5"));
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        check (proc.loadParvatiMultiFile (f), "post: arp_mode:5 document loads");
        const int staged = (int) proc.getEngine().getPart (0).readPendingConfig().arpMode;
        check (staged == 2, juce::String ("arp_mode:5 stages as 2 (Sequencer) [got ")
                                + juce::String (staged) + "] — the authoring-time finding stays fixed");
    }

    // ---- CANARY: the checker must detect a hand-broken engine ----
    std::printf ("\n[canary] hand-broken engine must trip the checker (expected FAIL lines)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        auto& e = proc.getEngine();
        // Break through the SAME message-thread seams the loaders use:
        // raw staged arp bytes (the silent-part byte + the AT-hang octave +
        // a zero seq length) and an out-of-range channel (the setters store
        // uint8 verbatim — the loaders clamp BEFORE them, exactly the seam a
        // loader regression would miss). An inverted keyzone is NO LONGER a
        // violation (wrap-around zones are legal, W8 item 1) — the fifth
        // break is an out-of-range zone END instead.
        e.getPart (0).writePendingConfig ([] (auto& c)
        {
            c.arpMode = 5;
            c.arpOctave = 0;
            c.seqLength[0] = 0;
        });
        e.setPartChannel (0, 200);
        e.setPartKeyrange (0, 130, 40);   // low > 127 (wrap zones stay legal)
        const int v = countInvariantViolations (proc, "CANARY");
        check (v >= 5, juce::String ("canary: hand-broken state trips >= 5 categories [got ")
                           + juce::String (v) + "]");
    }

    std::printf ("\nLOAD-INVARIANTS TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
