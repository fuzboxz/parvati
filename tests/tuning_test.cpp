// Per-part microtonal tuning verification (firmware raga presets + custom
// tables + Scala import glue):
//   1. vendored table contents (verbatim firmware values, aliases, ranges)
//   2. the AmbikaVoice::startNote hook mapping (preset/custom/composition)
//   3. sentinel (AcceptNote) gates for muted note classes
//   4. MT -> AT staging (frameDirty_ preset path + tuningDirty_ custom path)
//   5. engine-state v7 round-trip + v6 legacy fallback
//   6. .parvati multi tuning fields round-trip + old-format load
//   7. standing-bend pickup on newly triggered voices
//   8. .MUL round-trip of the raga byte (PartData byte 4)
//
// Built by default. Run with: ./build/parvati_tuning_test

#include <cmath>
#include <cstdio>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>   // ScopedJuceInitialiser_GUI (message thread for the processor's timers)

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
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

void renderBlocks (ParvatiAudioProcessor& p, int blocks = 3)
{
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer empty;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        p.processBlock (buf, empty);
    }
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

void setParam (ParvatiAudioProcessor& p, const char* id, int value)
{
    if (auto* param = p.getApvts().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (value)));
}

// (A countActiveVoices helper is intentionally omitted: every case asserts
// on the specific voice the note landed on via playNote.)

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
// 2. The startNote hook (preset / custom / composition).
// ---------------------------------------------------------------------------
void testHook()
{
    std::printf ("[startNote hook mapping]\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);
    auto& eng = proc.getEngine();

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

    // Custom table: setPartTuningCustom + the tuningDirty_ AT service.
    int16_t custom[12] = {};
    for (int c = 0; c < 12; ++c) custom[c] = static_cast<int16_t> ((c + 1) * 10 - 30);   // -30..80
    setParam (proc, "part_raga", 0);   // 12-EDO base; custom still inactive
    eng.setPartTuningCustom (0, custom);
    renderBlocks (proc, 2);   // tuningDirty_ service
    {
        AmbikaVoice* av = playNote (proc, 65);   // class 5 -> custom[5] = (5+1)*10-30 = 30
        check (av != nullptr && av->getLastNote14() == 65 * 128 + 30, "custom table applied at the hook");
        allNotesOff (proc, 65);
    }
    // D4 resolution rule: a preset selection overrides the custom flag.
    setParam (proc, "part_raga", 2);   // pythagorean
    renderBlocks (proc, 2);
    {
        AmbikaVoice* av = playNote (proc, 65);   // table[5] = -2
        check (av != nullptr && av->getLastNote14() == 65 * 128 - 2, "preset overrides the custom flag (D4)");
        allNotesOff (proc, 65);
    }
    check (eng.resolvedTuningMode (0) == 2, "resolvedTuningMode reports the preset");
    setParam (proc, "part_raga", 0);
    renderBlocks (proc, 2);
    // D4-inverse doctrine (Wave-1 BUG-4): a LIVE part_raga write of 0 is an
    // explicit 12-EDO selection — it now CLEARS the (dormant) custom flag
    // instead of letting it resurface. The old resurface behaviour made the
    // hosted part_raga combo / host automation / NRPN disagree with the Patch
    // page's Tune combo (whose explicit 12-EDO pick clears by design, D4):
    // the combo read "12-EDO" while the part kept playing the microtonal
    // table. The way back to a custom table is the TuningEditor / re-import.
    check (eng.resolvedTuningMode (0) == 0,
           "live part_raga=0 clears the dormant custom (D4 inverse — explicit 12-EDO)");
    eng.clearPartTuningCustom (0);
    renderBlocks (proc, 2);
    check (eng.resolvedTuningMode (0) == 0, "clearPartTuningCustom returns to 12-EDO");

    // A custom table may carry the mute sentinel itself (Scala import writes
    // one per kbm-unmapped class): it must survive setPartTuningCustom's ±127
    // clamp verbatim and refuse that note class like a raga preset would.
    {
        int16_t muted[12] = {};
        muted[1] = parvati::kTuningSilence;   // C# class muted
        muted[4] = 20;
        eng.setPartTuningCustom (0, muted);
        renderBlocks (proc, 2);
        int16_t back[12] = {};
        eng.resolveTuningOffsets (0, back);
        check (back[1] == parvati::kTuningSilence && back[4] == 20,
               "custom-table sentinel survives storage (not clamped to +127)");
        check (! eng.isNoteAcceptedByPartTuning (0, 61), "custom-table muted class refuses notes");
        check (eng.isNoteAcceptedByPartTuning (0, 64), "custom-table unmuted class accepts notes");
        eng.clearPartTuningCustom (0);
    }
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
    auto& eng = proc.getEngine();

    // Custom-offset-only edit: NO render between the edit and the note — the
    // note lands one block AFTER the tuningDirty_ service, so the offsets must
    // already be live (staging proven by the block-after-block sequencing).
    int16_t custom[12] = {};
    custom[3] = -42;   // only class 3
    eng.setPartTuningCustom (0, custom);
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 63, (uint8_t) 100), 0);
        proc.processBlock (buf, midi);   // first block: services tuningDirty_, THEN renders the note
        AmbikaVoice* av = playNote (proc, 75);   // class 3 again (fresh voice)
        check (av != nullptr && av->getLastNote14() == 75 * 128 - 42,
               "tuningDirty_ serviced before the next block's notes");
        allNotesOff (proc, 75);
        allNotesOff (proc, 63);
    }

    // Byte-4 preset change rides frameDirty_ (applyPartByte path).
    eng.clearPartTuningCustom (0);
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
// 5. Engine-state v7 round-trip + v6 fallback.
// ---------------------------------------------------------------------------
void testStateV7()
{
    std::printf ("[engine state v7]\n");

    ParvatiAudioProcessor a;
    prepareProc (a);
    renderBlocks (a, 2);
    auto& ea = a.getEngine();

    int16_t custom2[12] = {};
    custom2[1] = 21;  custom2[6] = -6;
    int16_t custom5[12] = {};
    custom5[11] = 99; custom5[0] = -99;
    ea.setPartTuningCustom (2, custom2);
    ea.setPartTuningCustom (5, custom5);
    ea.getPart (0).partBytes[4] = 5;   // preset on part 0
    renderBlocks (a, 2);

    juce::MemoryBlock blob;
    ea.captureState (blob);
    check (blob.getSize() > 0, "captureState produces a blob");

    {   // Fresh engine restore: modes + tables preserved.
        ParvatiAudioProcessor b;
        prepareProc (b);
        renderBlocks (b, 2);
        auto& eb = b.getEngine();
        check (eb.restoreState (blob.getData(), blob.getSize()), "v7 blob restores");
        bool okc = eb.resolvedTuningMode (0) == 5 && eb.resolvedTuningMode (2) == 33
                && eb.resolvedTuningMode (5) == 33 && eb.resolvedTuningMode (1) == 0;
        int16_t t2[12] = {}, t5[12] = {};
        eb.resolveTuningOffsets (2, t2);
        eb.resolveTuningOffsets (5, t5);
        okc = okc && std::memcmp (t2, custom2, sizeof (custom2)) == 0
                  && std::memcmp (t5, custom5, sizeof (custom5)) == 0;
        check (okc, "restored modes + custom tables match (preset 5, customs 33 on parts 2/5)");

        // The restored state is LIVE: a new note on part 0 uses the preset table.
        renderBlocks (b, 2);
        AmbikaVoice* av = playNote (b, 60);
        const int16_t* t = parvati::tuningPresetTable (5);
        check (av != nullptr && av->getLastNote14() == 60 * 128 + t[0],
               "restored preset is applied to new notes after the AT push");
        allNotesOff (b, 60);
    }

    {   // Hand-crafted v6 view: a REAL v6 blob ends after each Part's name
        // block, so strip the v7 tuning tails (29 bytes per part) in addition
        // to rewriting the version byte — a mere version flip would feed the
        // v7 tails to the v6 reader as the next Part's bytes and rightly fail.
        const uint8_t* src = static_cast<const uint8_t*> (blob.getData());
        const size_t total = blob.getSize();
        juce::MemoryBlock v6;
        v6.append (src, 6);   // magic + version + current part
        v6[4] = 6;            // version 7 -> 6
        size_t o = 6;
        for (int p = 0; p < 6; ++p)
        {
            if (o + 112 + 84 + 4 > total) break;
            v6.append (src + o, 112 + 84 + 4);          // patch + part + 4 routing bytes
            o += 112 + 84 + 4;
            const uint32_t fxLen = (uint32_t) src[o] | ((uint32_t) src[o + 1] << 8)
                                 | ((uint32_t) src[o + 2] << 16) | ((uint32_t) src[o + 3] << 24);
            v6.append (src + o, 4 + (size_t) fxLen);    // FX block (len + data)
            o += 4 + fxLen;
            const size_t nameLen = src[o + 1];
            v6.append (src + o, 1 + 1 + nameLen);       // slots + namelen + name
            o += 2 + nameLen;
            o += 4 + 25;   // DROP the v7 tuning block
        }
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        auto& ec = c.getEngine();
        check (ec.restoreState (v6.getData(), v6.getSize()), "v6-view blob restores");
        bool okc = ec.resolvedTuningMode (2) == 0 && ec.resolvedTuningMode (5) == 0
                && ec.resolvedTuningMode (0) == 5;   // preset rides partBytes[4]
        check (okc, "v6 blob restores presets from byte 4, customs cleared (12-EDO)");
    }

    {   // v8 is rejected (strict version gate -> legacy APVTS fallback upstream).
        juce::MemoryBlock v8 (blob);
        static_cast<uint8_t*> (v8.getData())[4] = 8;
        ParvatiAudioProcessor d;
        prepareProc (d);
        auto& ed = d.getEngine();
        check (! ed.restoreState (v8.getData(), v8.getSize()), "future version strictly rejected");
    }
}

// ---------------------------------------------------------------------------
// 6. .parvati multi round-trip + old-format load.
// ---------------------------------------------------------------------------
void testParvatiMulti()
{
    std::printf (".parvati tuning fields\n");

    ParvatiAudioProcessor a;
    prepareProc (a);
    renderBlocks (a, 2);
    auto& ea = a.getEngine();

    // Sentinel rider: a muted class in the custom table must survive the
    // .parvati write/read trip (the loader pre-clamps offsets to ±127; the
    // clamp has to exempt 32767 or the class comes back as a +127 detune).
    int16_t custom4[12] = {};
    custom4[9] = -77; custom4[2] = 33;
    custom4[6] = parvati::kTuningSilence;
    ea.setPartTuningCustom (4, custom4);
    ea.getPart (1).partBytes[4] = 12;   // yaman
    renderBlocks (a, 2);

    const juce::String yaml = serializeParvatiMulti (a);
    check (yaml.contains ("tuning_mode: 33"), "custom mode serialized as 33");
    check (yaml.contains ("tuning_offsets: "), "custom offsets serialized");
    check (yaml.contains ("tuning_mode: 12"), "preset mode serialized by id");

    {
        ParvatiAudioProcessor b;
        prepareProc (b);
        renderBlocks (b, 2);
        check (applyParvatiMulti (b, yaml), "applyParvatiMulti parses the tuning fields");
        auto& eb = b.getEngine();
        bool okc = eb.resolvedTuningMode (1) == 12 && eb.resolvedTuningMode (4) == 33;
        int16_t t4[12] = {};
        eb.resolveTuningOffsets (4, t4);
        okc = okc && std::memcmp (t4, custom4, sizeof (custom4)) == 0;
        check (okc, "round-tripped modes + custom table match");
    }

    {   // Old format: strip the tuning_* lines entirely -> customs clear,
        // presets survive via params: part_raga.
        juce::String old;
        for (const auto& l : juce::StringArray::fromLines (yaml))
            if (! l.trim().startsWith ("tuning_mode:") && ! l.trim().startsWith ("tuning_offsets:"))
                old << l << "\n";
        ParvatiAudioProcessor c;
        prepareProc (c);
        renderBlocks (c, 2);
        check (applyParvatiMulti (c, old), "old-format multi (no tuning keys) still parses");
        auto& ec = c.getEngine();
        bool okc = ec.resolvedTuningMode (4) == 0 && ec.resolvedTuningMode (1) == 12;
        check (okc, "old format loads as 12-EDO for customs, preset via params (forward-compat)");
    }
}

// ---------------------------------------------------------------------------
// 7. APVTS part_raga=0 clears an armed custom table (the live param path —
// the same D4 rule the Patch page's Tune combo and the file loaders apply).
// ---------------------------------------------------------------------------
void testApvtsRagaClear()
{
    std::printf ("part_raga=0 via APVTS clears custom\n");

    ParvatiAudioProcessor proc;
    prepareProc (proc);
    renderBlocks (proc, 2);
    auto& eng = proc.getEngine();

    // Arm a custom table on the current part (0).
    int16_t custom[12] = {};
    custom[5] = 24;
    eng.setPartTuningCustom (0, custom);
    check (eng.resolvedTuningMode (0) == 33, "precondition: custom armed (mode 33)");

    // A live part_raga write of 0 (the hosted param-grid combo, host automation
    // or NRPN 116) is an explicit 12-EDO selection — it must clear the flag, not
    // just write byte 4 = 0 while the engine keeps playing the custom table.
    // (JUCE's adapter early-returns on a same-value write, so drive the real
    // transition a host/user produces: land a preset first, then 0.)
    proc.getApvts().getParameterAsValue ("part_raga") = 7.0f;
    check (eng.resolvedTuningMode (0) == 7, "precondition: preset 7 selected");
    proc.getApvts().getParameterAsValue ("part_raga") = 0.0f;
    check (eng.resolvedTuningMode (0) == 0,
           "part_raga=0 param write clears the custom flag (D4 inverse)");

    // A non-zero write selects the preset and stays a preset afterwards.
    proc.getApvts().getParameterAsValue ("part_raga") = 7.0f;
    check (eng.resolvedTuningMode (0) == 7, "part_raga=7 selects preset 7");

    // Part-switch invariant (the regression guard for the clear's placement): a
    // part with an ARMED custom table (byte 4 == 0 under D4) must KEEP it when
    // the engine re-pushes its own stored bytes on a part switch (the bulk sync
    // path deliberately does NOT clear).
    eng.setPartTuningCustom (2, custom);
    renderBlocks (proc, 2);
    setParam (proc, "part_select", 3);   // 1-based: switch to part 2 and back
    renderBlocks (proc, 2);
    check (eng.resolvedTuningMode (2) == 33,
           "custom on part 2 survives a part switch (bulk sync does not clear)");
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
// 8. .MUL round-trip of the raga byte.
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

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor runs Timers (deferred-param drain)
    std::printf ("parvati_tuning_test\n");
    testTables();
    testHook();
    testSentinel();
    testStaging();
    testStateV7();
    testParvatiMulti();
    testApvtsRagaClear();
    testStandingBend();
    testMulRagaRoundTrip();

    if (g_failures == 0)
    {
        std::printf ("ALL PASS\n");
        return 0;
    }
    std::printf ("%d FAILURE(S)\n", g_failures);
    return 1;
}
