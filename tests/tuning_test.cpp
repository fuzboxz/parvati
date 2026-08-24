// Per-part microtonal tuning verification (firmware raga presets):
//   1. vendored table contents (verbatim firmware values, aliases, ranges)
//   2. the AmbikaVoice::startNote hook mapping (preset/composition)
//   3. sentinel (AcceptNote) gates for muted note classes
//   4. MT -> AT staging (frameDirty_ preset path)
//   5. engine-state blob round-trip (v8) + v7/v6 legacy fallback
//      (the custom-tuning subsystem was removed 2026-08-19; v7 blobs that
//      carry a custom mode 33 must load with the custom DROPPED and the
//      raga byte intact)
//   6. .parvati preset round-trip (params: part_raga) + legacy tuning_mode
//      key acceptance (33 -> 12-EDO, 1..32 -> raga byte)
//   7. standing-bend pickup on newly triggered voices
//   8. .MUL round-trip of the raga byte (PartData byte 4)
//
// Run: ./build_unified/parvati_unified_tests tuning_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>   // ScopedJuceInitialiser_GUI (message thread for the processor's timers)

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)
#include "SynthEngine.h"
#include "TuningTables.h"

using parvati::preset::applyParvatiMulti;
using parvati::preset::serializeParvatiMulti;

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Host-prepare a freshly constructed processor (processBlock requires
// prepareToPlay to size the engine scratch buffers first).
void prepareProc (ParvatiAudioProcessor& proc)
{
    proc.prepareToPlay (48000.0, 256);
}

// Trigger @p note on @p channel for one block, then find the voice it landed
// on (via the SF-1 displayed-note mirror). Returns nullptr if no active voice.
AmbikaVoice* playNote (ParvatiAudioProcessor& p, int note, int channel = 1, uint8_t vel = 100)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (channel, note, vel), 0);
    p.processBlock (buf, midi);
    auto& eng = p.getEngine();
    for (int i = 0; i < SynthEngine::getNumParts() * 16; ++i)
        if (auto* av = eng.getAmbikaVoice (i))
            if (av->isDisplayedActive() && av->getDisplayedNote() == note)
                return av;
    return nullptr;
}

void allNotesOff (ParvatiAudioProcessor& p, int note = -1, int channel = 1)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    if (note >= 0)
    {
        midi.addEvent (juce::MidiMessage::noteOff (channel, note, (uint8_t) 0), 0);
    }
    else
    {
        for (int n = 0; n < 128; ++n)
            midi.addEvent (juce::MidiMessage::noteOff (channel, n, (uint8_t) 0), 0);
    }
    p.processBlock (buf, midi);
    renderBlocks (p, 12);   // let releases finish so voices free
}

// ---------------------------------------------------------------------------
// 1. Vendored tables.
// ---------------------------------------------------------------------------
void testTables()
{
    std::printf ("[vendored tables]\n");

    const int16_t* just = parvati::tuningPresetTable (1);
    const int16_t wantJust[12] = { 0, 15, 5, 20, -17, -2, -12, 2, 17, -20, -5, -15 };
    check (just != nullptr && std::memcmp (just, wantJust, sizeof (wantJust)) == 0,
           "preset 1 (just) matches the firmware bytes verbatim");

    check (parvati::tuningPresetTable (0) == nullptr, "id 0 returns nullptr (no preset)");
    bool allResolve = true, namesOk = true, rangeOk = true;
    for (int id = 1; id <= parvati::kNumTuningPresets; ++id)
    {
        const int16_t* t = parvati::tuningPresetTable (id);
        if (t == nullptr) { allResolve = false; continue; }
        if (parvati::tuningPresetName (id) == nullptr || parvati::tuningPresetName (id)[0] == '\0')
            namesOk = false;
        for (int c = 0; c < 12; ++c)
            if (! (t[c] == parvati::kTuningSilence || (t[c] >= -127 && t[c] <= 127)))
                rangeOk = false;
    }
    check (allResolve, "all 32 preset ids resolve to tables");
    check (namesOk, "all 32 preset names are non-empty");
    check (rangeOk, "every table entry is within [-127,127] or the sentinel");

    check (parvati::tuningPresetTable (16) == parvati::tuningPresetTable (13),
           "id 16 (bageshree) aliases the kafi array");
    check (parvati::tuningPresetTable (32) == parvati::tuningPresetTable (12),
           "id 32 (rasia) aliases the yaman array");

    int silences = 0;
    for (int c = 0; c < 12; ++c)
        if (parvati::tuningPresetTable (30)[c] == parvati::kTuningSilence)
            ++silences;
    check (silences == 7, "preset 30 (kaushik todi) mutes 7 classes (firmware data)");

    const int16_t* edo = parvati::tuningEdoTable();
    bool edoZero = true;
    for (int c = 0; c < 12; ++c)
        if (edo[c] != 0) edoZero = false;
    check (edoZero, "12-EDO table is all zeros");
}

// ---------------------------------------------------------------------------
// 2. The startNote hook (preset / composition).
// ---------------------------------------------------------------------------
void testHook()
{
    std::printf ("[startNote hook mapping]\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);

    // Baseline: 12-EDO -> plain baseNote*128.
    {
        AmbikaVoice* av = playNote (proc, 60);
        check (av != nullptr && av->getLastNote14() == 60 * 128, "12-EDO: note14 == baseNote*128");
        allNotesOff (proc);
    }

    // Preset 1 (just): note14 == baseNote*128 + table[note % 12].
    setParam (proc, "part_raga", 1);
    renderBlocks (proc, 2);   // frameDirty_ -> AT push -> voices see the table
    {
        const int16_t* t = parvati::tuningPresetTable (1);
        bool okc = true;
        for (int n : { 60, 64, 67, 71 })   // C E G B: classes 0, 4, 7, 11
        {
            AmbikaVoice* av = playNote (proc, n);
            const int want = n * 128 + t[n % 12];
            if (av == nullptr || av->getLastNote14() != want)
                okc = false;
            allNotesOff (proc, n);
        }
        check (okc, "raga preset: note14 == baseNote*128 + table[note % 12]");
    }

    // Composition: octave -1 shifts the base note, tuning adds, then the table.
    setParam (proc, "part_octave", -1);
    setParam (proc, "part_tuning", 10);
    renderBlocks (proc, 2);
    {
        AmbikaVoice* av = playNote (proc, 62);   // class 2, table[2] = 5
        const int want = (62 - 12) * 128 + 5 + 10;
        check (av != nullptr && av->getLastNote14() == want,
               "octave/tuning compose with the table (offsets add after octave*12)");
        allNotesOff (proc, 62);
    }
    setParam (proc, "part_octave", 0);
    setParam (proc, "part_tuning", 0);

    // Spread composition via UNISON_2X (firmware: 2nd voice of the pair +spread).
    setParam (proc, "part_polyphony", 2 /*UNISON_2X*/);
    setParam (proc, "part_spread", 40);
    renderBlocks (proc, 2);
    {
        AmbikaVoice* av = playNote (proc, 60);
        int n0 = 0, n40 = 0;
        auto& e = proc.getEngine();
        for (int i = 0; i < SynthEngine::getNumParts() * 16; ++i)
            if (auto* v = e.getAmbikaVoice (i))
                if (v->isDisplayedActive() && v->getDisplayedNote() == 60)
                {
                    if (v->getLastNote14() == 60 * 128) ++n0;
                    else if (v->getLastNote14() == 60 * 128 + 40) ++n40;
                }
        check (av != nullptr && n0 >= 1 && n40 >= 1, "spread drift adds after the table (unison pair {0,+40})");
        allNotesOff (proc, 60);
    }
    setParam (proc, "part_spread", 0);
    setParam (proc, "part_polyphony", 1 /*POLY*/);
    setParam (proc, "part_raga", 0);
    renderBlocks (proc, 2);
}

// ---------------------------------------------------------------------------
// 3. Sentinel gates (firmware AcceptNote).
// ---------------------------------------------------------------------------
void testSentinel()
{
    std::printf ("[sentinel gates]\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);
    setParam (proc, "part_raga", 30);   // kaushik todi: 7 muted classes
    renderBlocks (proc, 2);

    bool refused = true, accepted = true;
    for (int n = 60; n <= 72; ++n)
    {
        AmbikaVoice* av = playNote (proc, n);
        const bool muted = parvati::tuningPresetTable (30)[n % 12] == parvati::kTuningSilence;
        if (muted && av != nullptr) refused = false;        // muted must NOT play
        if (! muted && av == nullptr) accepted = false;     // in-scale must play
        allNotesOff (proc, n);
    }
    check (refused, "muted classes produce no active voice (AcceptNote refusal)");
    check (accepted, "in-scale classes still play under the raga");

    setParam (proc, "part_raga", 0);
    renderBlocks (proc, 2);
}

// ---------------------------------------------------------------------------
// 4. MT -> AT staging.
// ---------------------------------------------------------------------------
void testStaging()
{
    std::printf ("[staging]\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);

    // Byte-4 preset change rides frameDirty_ (applyPartByte path): the offsets
    // must be live for the note in the block AFTER the edit block.
    proc.getApvts().getParameter ("part_raga")->setValueNotifyingHost (
        proc.getApvts().getParameter ("part_raga")->convertTo0to1 (7.0f));
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer empty2;
        proc.processBlock (buf, empty2);   // services frameDirty_
    }
    {
        AmbikaVoice* av = playNote (proc, 60);
        const int16_t* t = parvati::tuningPresetTable (7);
        check (av != nullptr && av->getLastNote14() == 60 * 128 + t[0],
               "byte-4 preset change applied via frameDirty_ push");
        allNotesOff (proc, 60);
    }
    setParam (proc, "part_raga", 0);
    renderBlocks (proc, 2);
}

// ---------------------------------------------------------------------------
// 5. Engine-state blob round-trip (v8) + v7/v6 legacy fallback.
// ---------------------------------------------------------------------------
void testStateBlob()
{
    std::printf ("[engine state blob]\n");

    ParvatiAudioProcessor a;
    prepareProc (a);
    renderBlocks (a, 2);
    auto& ea = a.getEngine();

    ea.getPart (0).partBytes[4] = 5;   // preset on part 0
    ea.getPart (2).partBytes[4] = 12;  // yaman on part 2
    renderBlocks (a, 2);

    juce::MemoryBlock blob;
    ea.captureState (blob);
    check (blob.getSize() > 0, "captureState produces a blob");
    check (static_cast<const uint8_t*> (blob.getData())[4] == 8, "blob carries version 8");

    {   // Fresh engine restore: presets preserved from partBytes[4].
        ParvatiAudioProcessor b;
        prepareProc (b);
        renderBlocks (b, 2);
        auto& eb = b.getEngine();
        check (eb.restoreState (blob.getData(), blob.getSize()), "v8 blob restores");
        bool okc = eb.resolvedTuningMode (0) == 5 && eb.resolvedTuningMode (2) == 12
                && eb.resolvedTuningMode (1) == 0;
        int16_t t2[12] = {}, tWant[12] = {};
        eb.resolveTuningOffsets (2, t2);
        std::memcpy (tWant, parvati::tuningPresetTable (12), sizeof (tWant));
        okc = okc && std::memcmp (t2, tWant, sizeof (tWant)) == 0;
        check (okc, "restored presets match (5 on part 0, 12 on part 2)");

        // The restored state is LIVE: a new note on part 0 uses the preset table.
        renderBlocks (b, 2);
        AmbikaVoice* av = playNote (b, 60);
        const int16_t* t = parvati::tuningPresetTable (5);
        check (av != nullptr && av->getLastNote14() == 60 * 128 + t[0],
               "restored preset is applied to new notes after the AT push");
        allNotesOff (b, 60);
    }

    {   // Hand-crafted v7 view: a REAL v7 blob ended each Part's tail with a
        // length-prefixed tuning block (4-byte LE length + 25 bytes
        // {u8 mode; i16 LE offsets[12]}). Rebuild that layout from the v8
        // capture: append a tuning block per Part, one of them carrying the
        // former CUSTOM mode 33 + offsets. Restore must load it cleanly and
        // DROP the custom (12-EDO; its raga byte was 0 by the custom-active
        // invariant), keeping every real preset intact.
        const uint8_t* src = static_cast<const uint8_t*> (blob.getData());
        const size_t total = blob.getSize();
        juce::MemoryBlock v7;
        v7.append (src, 6);   // magic + version + current part
        v7[4] = 7;            // version 8 -> 7
        size_t o = 6;
        for (int p = 0; p < 6; ++p)
        {
            if (o + 112 + 84 + 4 > total) break;
            v7.append (src + o, 112 + 84 + 4);          // patch + part + 4 routing bytes
            o += 112 + 84 + 4;
            const uint32_t fxLen = (uint32_t) src[o] | ((uint32_t) src[o + 1] << 8)
                                 | ((uint32_t) src[o + 2] << 16) | ((uint32_t) src[o + 3] << 24);
            v7.append (src + o, 4 + (size_t) fxLen);    // FX block (len + data)
            o += 4 + fxLen;
            const size_t nameLen = src[o + 1];
            v7.append (src + o, 1 + 1 + nameLen);       // slots + namelen + name
            o += 2 + nameLen;
            // Synthesize the v7 tuning block: mode 33 + offsets on part 1 (the
            // "custom was active" case), mirrored preset ids elsewhere.
            uint8_t tune[25] = {};
            tune[0] = (p == 1) ? 33 : static_cast<uint8_t> ((int) ea.getPart (p).partBytes[4]);
            if (p == 1)
            {
                // offsets bytes 1..24: class 1 -> +21 (1/128-st LE)
                tune[1] = 21; tune[2] = 0;
            }
            const uint32_t tuneLen = 25;
            v7.append (&tuneLen, 4);
            v7.append (tune, 25);
        }
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        auto& ec = c.getEngine();
        check (ec.restoreState (v7.getData(), v7.getSize()), "v7-view blob (with tuning tails) restores");
        bool okc = ec.resolvedTuningMode (0) == 5 && ec.resolvedTuningMode (2) == 12
                && ec.resolvedTuningMode (1) == 0 && ec.resolvedTuningMode (3) == 0;
        check (okc, "v7 blob: presets intact, custom mode 33 dropped to 12-EDO (backcompat)");
        int16_t t1[12] = {};
        ec.resolveTuningOffsets (1, t1);
        bool zero = true;
        for (int i = 0; i < 12; ++i) if (t1[i] != 0) zero = false;
        check (zero, "v7 blob: the dropped custom's offsets resolve as 12-EDO zeros");
    }

    {   // Hand-crafted v6 view: a REAL v6 blob ends after each Part's name
        // block, so strip the v7 tuning tails' absence is already the v8 shape —
        // merely flip the version byte (v6 has no tuning blocks, like v8).
        juce::MemoryBlock v6 (blob);
        static_cast<uint8_t*> (v6.getData())[4] = 6;
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        auto& ec = c.getEngine();
        check (ec.restoreState (v6.getData(), v6.getSize()), "v6-view blob restores");
        bool okc = ec.resolvedTuningMode (0) == 5 && ec.resolvedTuningMode (2) == 12;
        check (okc, "v6 blob restores presets from byte 4");
    }

    {   // v9 is rejected (strict version gate -> legacy APVTS fallback upstream).
        juce::MemoryBlock v9 (blob);
        static_cast<uint8_t*> (v9.getData())[4] = 9;
        ParvatiAudioProcessor d;
        prepareProc (d);
        auto& ed = d.getEngine();
        check (! ed.restoreState (v9.getData(), v9.getSize()), "future version strictly rejected");
    }
}

// ---------------------------------------------------------------------------
// 6. .parvati preset round-trip + legacy tuning_mode acceptance.
// ---------------------------------------------------------------------------
void testParvatiMulti()
{
    std::printf (".parvati tuning fields\n");

    ParvatiAudioProcessor a;
    prepareProc (a);
    renderBlocks (a, 2);
    auto& ea = a.getEngine();

    ea.getPart (1).partBytes[4] = 12;   // yaman
    renderBlocks (a, 2);

    const juce::String yaml = serializeParvatiMulti (a);
    check (! yaml.contains ("tuning_mode"), "serializer emits NO tuning_mode keys (raga rides params: part_raga)");
    check (! yaml.contains ("tuning_offsets"), "serializer emits NO tuning_offsets keys");

    {
        ParvatiAudioProcessor b;
        prepareProc (b);
        renderBlocks (b, 2);
        check (applyParvatiMulti (b, yaml), "applyParvatiMulti parses the round-trip");
        auto& eb = b.getEngine();
        check (eb.resolvedTuningMode (1) == 12, "round-tripped preset matches (12 on part 1)");
    }

    {   // Legacy file: carry tuning_mode keys from the pre-removal writer.
        // mode 33 + offsets (the custom case) must load as 12-EDO; mode 5 must
        // load as raga 5; the parse must NEVER fail.
        juce::String legacy = yaml;
        // Inject a legacy tuning_mode/tuning_offsets pair into part 2's entry
        // (after its params block close is fiddly; simpler: rewrite a minimal
        // two-key legacy part entry appended as a NEW part entry is invalid —
        // instead mutate the EXISTING part-2 entry text).
        // The serializer writes one "  - name: ..." block per part; find the
        // second part's "params:" section and inject before it.
        // (Deterministic injection: the multi format is line-based.)
        juce::String out;
        int partIdx = -1;
        for (const auto& l : juce::StringArray::fromLines (legacy))
        {
            if (l.trim().startsWith ("- name:") || l.trim().startsWith ("- channel:"))
                ++partIdx;
            if (partIdx == 2 && l.trim().startsWith ("params:"))
            {
                out << "    tuning_mode: " << ((partIdx == 2) ? juce::String (33) : juce::String()) << "\n";
                out << "    tuning_offsets: 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0\n";
            }
            out << l << "\n";
        }
        legacy = out;
        // Also give part 3's entry a legacy preset mode (rewrite after the fact
        // via a second pass is overkill — part 2's 33 is the critical case; a
        // preset-mode acceptance is covered by the params part_raga path).
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        check (applyParvatiMulti (c, legacy), "legacy multi WITH tuning_mode:33 + offsets still parses");
        auto& ec = c.getEngine();
        check (ec.resolvedTuningMode (2) == 0,
               "legacy custom mode 33 loads as 12-EDO (custom subsystem removed)");
        int16_t t2[12] = {};
        ec.resolveTuningOffsets (2, t2);
        bool zero = true;
        for (int i = 0; i < 12; ++i) if (t2[i] != 0) zero = false;
        check (zero, "legacy custom offsets are ignored (12-EDO zeros)");
    }

    {   // Legacy preset mode: tuning_mode: 5 must map to raga byte 5.
        juce::String out;
        int partIdx = -1;
        for (const auto& l : juce::StringArray::fromLines (yaml))
        {
            if (l.trim().startsWith ("- name:") || l.trim().startsWith ("- channel:"))
                ++partIdx;
            if (partIdx == 3 && l.trim().startsWith ("params:"))
                out << "    tuning_mode: 5\n";
            out << l << "\n";
        }
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        check (applyParvatiMulti (c, out), "legacy multi WITH tuning_mode:5 still parses");
        check (c.getEngine().resolvedTuningMode (3) == 5,
               "legacy preset mode 5 maps to the raga byte");
    }
}

// ---------------------------------------------------------------------------
// 7. APVTS part_raga writes + part-switch survival (byte 4 is the whole
//    tuning state now — the custom-flag interplay was removed with the
//    custom-tuning subsystem).
// ---------------------------------------------------------------------------
void testApvtsRaga()
{
    std::printf ("part_raga via APVTS\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);
    auto& eng = proc.getEngine();

    proc.getApvts().getParameterAsValue ("part_raga") = 7.0f;
    check (eng.resolvedTuningMode (0) == 7, "part_raga=7 selects preset 7");
    proc.getApvts().getParameterAsValue ("part_raga") = 0.0f;
    check (eng.resolvedTuningMode (0) == 0, "part_raga=0 returns to 12-EDO");

    // Part-switch invariant: a part's stored raga byte survives the bulk sync
    // re-push a part switch performs (byte 4 is plain PartData).
    eng.getPart (2).partBytes[4] = 9;
    renderBlocks (proc, 2);
    setParam (proc, "part_select", 3);   // 1-based: switch to part 2 and back
    renderBlocks (proc, 2);
    check (eng.resolvedTuningMode (2) == 9,
           "raga byte on part 2 survives a part switch (bulk sync re-push)");
    setParam (proc, "part_select", 1);
}

// ---------------------------------------------------------------------------
// 8. Standing-bend pickup.
// ---------------------------------------------------------------------------
void testStandingBend()
{
    std::printf ("[standing bend pickup]\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);

    // Pre-wheel voice: no bend.
    AmbikaVoice* before = playNote (proc, 60);
    check (before != nullptr && std::fabs (before->getMpePitchBendSemitones()) < 1e-6f,
           "voice triggered before any wheel starts unbent");

    // Wheel to +8191 of range on ch1 => +2 st (fixed range). No further wheel
    // events: the NEXT voice must still pick the bend up from the latch.
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0);
        proc.processBlock (buf, midi);
    }
    AmbikaVoice* after = playNote (proc, 67);
    check (after != nullptr && std::fabs (after->getMpePitchBendSemitones() - 2.0f) < 1e-3f,
           "voice triggered after the wheel inherits the standing bend (+2 st)");
    check (before != nullptr && std::fabs (before->getMpePitchBendSemitones() - 2.0f) < 1e-3f,
           "the active pre-wheel voice was also bent by the wheel event itself");

    // Per-channel isolation: a ch2 note stays unbent (wheel was ch1). Give
    // Part 1 one voicecard (the default single-part layout leaves Parts 1..5
    // with none) and let the AT rebuild settle before playing it.
    proc.getEngine().setPartVoiceAllocation (1, 0b10);   // claims card 1 (exclusive)
    renderBlocks (proc, 2);
    AmbikaVoice* ch2 = playNote (proc, 72, 2);
    check (ch2 != nullptr && std::fabs (ch2->getMpePitchBendSemitones()) < 1e-3f,
           "standing bend is per-channel (ch2 unaffected by the ch1 wheel)");

    allNotesOff (proc);
    // Reset the wheel so later cases are clean.
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
        proc.processBlock (buf, midi);
    }
}

// ---------------------------------------------------------------------------
// 9. .MUL round-trip of the raga byte.
// ---------------------------------------------------------------------------
void testMulRagaRoundTrip()
{
    std::printf ("[.MUL raga byte round-trip]\n");

    ParvatiAudioProcessor a;
    prepareProc (a);
    renderBlocks (a, 2);
    // Set the raga through the PARAM path: saveMultiFile gathers the CURRENT
    // part's bytes from the APVTS (capturing uncommitted edits), not engine
    // storage — the honest round-trip is param -> engine -> .MUL.
    setParam (a, "part_raga", 7);
    renderBlocks (a, 2);

    const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("parvati_tuning_test")
                              .getChildFile ("raga.MUL");
    tmp.getParentDirectory().createDirectory();
    check (a.saveMultiFile (tmp), "saves .MUL with raga byte 7 on part 0");

    ParvatiAudioProcessor b;
    prepareProc (b);
    renderBlocks (b, 2);
    check (b.loadMultiFile (tmp), "loads the .MUL back");
    check (b.getEngine().getPart (0).partBytes[4] == 7, "raga byte 7 survives the .MUL round-trip");
    check (b.getEngine().resolvedTuningMode (0) == 7, "loaded raga resolves to preset 7");

    bool applied = false;
    {
        AmbikaVoice* av = playNote (b, 60);
        const int16_t* t = parvati::tuningPresetTable (7);
        applied = av != nullptr && av->getLastNote14() == 60 * 128 + t[0];
        allNotesOff (b, 60);
    }
    check (applied, "loaded raga is applied to new notes after the AT push");
    tmp.deleteFile();
}
}  // namespace

TEST(tuning_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor runs Timers (deferred-param drain)
    std::printf ("parvati_tuning_test\n");
    testTables();
    testHook();
    testSentinel();
    testStaging();
    testStateBlob();
    testParvatiMulti();
    testApvtsRaga();
    testStandingBend();
    testMulRagaRoundTrip();

    if (g_failures == 0)
    {
        std::printf ("ALL PASS\n");
        return true;
    }
    std::printf ("%d FAILURE(S)\n", g_failures);
    return false;
}
