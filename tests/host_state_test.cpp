// Host plugin-state persistence regression test for Parvati.
//
// Verifies getStateInformation / setStateInformation round-trip preserves the
// FULL 6-Part multitimbral state (patch bytes, arp/seq config, MIDI routing,
// voice allocation, current part) -- not just the current Part. Before the fix
// the host state carried only the current Part's APVTS values, so Parts 1..5
// (patch / arp / seq / routing) reverted to init on every DAW project reload.
//
// Built by default. Run with: ./build/parvati_host_state_test

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/patch.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void renderOnce (ParvatiAudioProcessor& p)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear ();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

// Per-part byte stride of a v7 engine-state blob with EMPTY part names:
// core (patch112 + part84 + routing4) + FX block (4 + 78) + slots/name tail
// (2) + tuning block (4 + 25). Shared by the hand-crafted v1/v2 derivations.
constexpr size_t kV7PartStride = 112 + 84 + 4 + 4 + 78 + 2 + 29;   // 313

// Set an APVTS param by raw value via the host notification path.
void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (value)));
}

// Select part @p partIndex (0-based) via the 1-based part_select param.
void selectPart (ParvatiAudioProcessor& proc, int partIndex)
{
    setParam (proc, "part_select", partIndex + 1);
}

// Paint a distinctive FX state onto the CURRENT part (routes through
// applyFxParameter into the current Part's fxState).
void paintFx (ParvatiAudioProcessor& proc)
{
    setParam (proc, "fx1_type",    3);   // Reverb
    setParam (proc, "fx1_enabled", 1);
    setParam (proc, "fx1_drywet",  77);
    setParam (proc, "fx1_param2",  100);
    setParam (proc, "fx3_type",    4);   // Chorus
    setParam (proc, "fx3_enabled", 1);
    setParam (proc, "fx3_drywet",  44);
    setParam (proc, "fx_topo",     1);   // Parallel
    setParam (proc, "fx_order",    2);   // {1,0,2}
    setParam (proc, "fxmod3_source", 6);
    setParam (proc, "fxmod3_dest",   9);
    setParam (proc, "fxmod3_amount", -50);
}

int countFxMismatches (const PartFxState& a, const PartFxState& b)
{
    int m = 0;
    for (int s = 0; s < kNumFxSlots; ++s)
    {
        if (a.slotType   [(size_t) s].load() != b.slotType   [(size_t) s].load()) ++m;
        if (a.slotEnabled[(size_t) s].load() != b.slotEnabled[(size_t) s].load()) ++m;
        if (a.slotDryWet [(size_t) s].load() != b.slotDryWet [(size_t) s].load()) ++m;
        for (int k = 0; k < kNumFxSlotParams; ++k)
            if (a.slotParam[(size_t) s][(size_t) k].load() != b.slotParam[(size_t) s][(size_t) k].load()) ++m;
    }
    if (a.topology.load() != b.topology.load()) ++m;
    if (a.orderIdx.load()  != b.orderIdx.load())  ++m;
    // Master section (v3).
    if (a.mix.load()       != b.mix.load())       ++m;
    if (a.eqLow.load()     != b.eqLow.load())     ++m;
    if (a.eqMid.load()     != b.eqMid.load())     ++m;
    if (a.eqHigh.load()    != b.eqHigh.load())    ++m;
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
    {
        if (a.modSource[(size_t) i].load() != b.modSource[(size_t) i].load()) ++m;
        if (a.modDest  [(size_t) i].load() != b.modDest  [(size_t) i].load()) ++m;
        if (a.modAmount[(size_t) i].load() != b.modAmount[(size_t) i].load()) ++m;
    }
    return m;
}

bool allFxAtDefaults (const PartFxState& fx)
{
    return countFxMismatches (fx, PartFxState{}) == 0;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    // [1] Full 6-Part state survives getStateInformation / setStateInformation.
    // ---------------------------------------------------------------------
    std::printf ("[1] Full 6-Part state survives a host-state round-trip\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);

        // Customize Parts 1 and 2 distinctly (directly on the engine -- they are
        // NOT the current part, so without full-persistence they would be lost).
        a.getEngine().getPart (1).patchBytes[0] = 7;            // osc1_shape
        a.getEngine().setPartChannel (1, 5);
        a.getEngine().setPartKeyrange (1, 36, 72);
        a.getEngine().setPartVoiceAllocation (1, 0x06);         // vc1 + vc2
        a.getEngine().getPart (1).pendingConfig_.arpOctave = 4;
        a.getEngine().getPart (1).pendingConfig_.seqData[0] = 77;
        a.getEngine().getPart (1).configDirty_.store (true);

        a.getEngine().getPart (2).patchBytes[0] = 11;
        a.getEngine().setPartChannel (2, 9);
        a.getEngine().setPartKeyrange (2, 48, 84);
        a.getEngine().setPartVoiceAllocation (2, 0x18);         // vc3 + vc4
        a.getEngine().getPart (2).pendingConfig_.arpResolution = 3;
        a.getEngine().getPart (2).configDirty_.store (true);

        // Make Part 2 the current part (saved + restored).
        a.getEngine().setCurrentPart (2);
        renderOnce (a);

        // Capture host state.
        juce::MemoryBlock blob;
        a.getStateInformation (blob);
        check (blob.getSize() > 0, "getStateInformation produced a non-empty block");

        // Restore into a fresh processor + service the deferred rebuild.
        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        renderOnce (b);

        // Part 1 survived verbatim.
        check (b.getEngine().getPart (1).patchBytes[0] == 7, "Part 1 osc1_shape preserved");
        check (b.getEngine().getPartChannel (1) == 5, "Part 1 midi channel preserved");
        check (b.getEngine().getPartKeyrangeLow (1) == 36 && b.getEngine().getPartKeyrangeHigh (1) == 72,
               "Part 1 keyzone preserved");
        check (b.getEngine().getPartVoiceSlots (1) == 2, "Part 1 voice slots preserved");
        check (b.getEngine().getPartVoiceAllocation (1) == 0x10, "Part 1 derived mask = contiguous 1-card share (vc4)");
        check (b.getEngine().getPart (1).pendingConfig_.arpOctave == 4, "Part 1 arp octave preserved");
        check (b.getEngine().getPart (1).pendingConfig_.seqData[0] == 77, "Part 1 seq step preserved");

        // Part 2 survived + is the restored current part.
        check (b.getEngine().getPart (2).patchBytes[0] == 11, "Part 2 osc1_shape preserved");
        check (b.getEngine().getPartChannel (2) == 9, "Part 2 midi channel preserved");
        check (b.getEngine().getPartVoiceSlots (2) == 2, "Part 2 voice slots preserved");
        check (b.getEngine().getPartVoiceAllocation (2) == 0x20, "Part 2 derived mask = contiguous 1-card share (vc5)");
        check (b.getEngine().getPart (2).pendingConfig_.arpResolution == 3, "Part 2 arp resolution preserved");
        check (b.getEngine().getCurrentPart() == 2, "current part preserved");
    }

    // ---------------------------------------------------------------------
    // [2] Backward compat: a legacy state with no engine_state falls back
    //     gracefully (no crash; Parts seed init; current Part from APVTS).
    // ---------------------------------------------------------------------
    std::printf ("\n[2] Legacy state (no engine_state) restores the saved part + all params\n");
    {
        // Save a real state from Part 3 with painted params, then strip the
        // engine_state attribute to mimic a pre-persistence saved project.
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        selectPart (a, 3);
        // Paint a spread of parameter classes on the (current) Part 3: part
        // bytes, a patch byte, an arp param, a sequencer byte, FX params.
        setParam (a, "part_tuning",    -33);
        setParam (a, "osc1_shape",      3);
        setParam (a, "arp_octave",      4);
        setParam (a, "seq1_step0",      99);
        setParam (a, "fx1_type",        3);
        setParam (a, "fx1_enabled",     1);
        renderOnce (a);

        juce::MemoryBlock full;
        a.getStateInformation (full);

        auto xml = juce::AudioProcessor::getXmlFromBinary (full.getData(), (int) full.getSize());
        check (xml != nullptr && xml->hasAttribute ("engine_state"), "fresh state carries engine_state");
        // Snapshot the APVTS-side values BEFORE stripping (the restored set).
        std::map<std::string, float> savedValues;
        for (const auto& d : getPatchParamDescriptors())
            savedValues[d.paramID] = a.getApvts().getRawParameterValue (d.paramID)->load();
        xml->removeAttribute ("engine_state");

        juce::MemoryBlock legacy;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacy);

        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        bool threw = false;
        try { b.setStateInformation (legacy.getData(), (int) legacy.getSize()); }
        catch (...) { threw = true; }
        renderOnce (b);
        check (! threw, "legacy state restores without throwing");

        // FULL parameter compare: every descriptor's restored raw value equals
        // the saved one (the APVTS survived the replaceState round-trip).
        int mismatches = 0;
        for (const auto& d : getPatchParamDescriptors())
            if (std::fabs (b.getApvts().getRawParameterValue (d.paramID)->load()
                           - savedValues[d.paramID]) > 0.001f)
                ++mismatches;
        char m[96];
        std::snprintf (m, sizeof (m), "all %zu APVTS params restored (mismatches=%d)",
                       savedValues.size(), mismatches);
        check (mismatches == 0, m);

        // The saved part is tracked: parameter + engine + edits all on Part 3.
        check (juce::roundToInt (b.getApvts().getRawParameterValue ("part_select")->load()) == 4,
               "legacy restore: part_select == 4 (Part 3)");
        check (b.getEngine().getCurrentPart() == 3,
               "legacy restore: engine current part == 3");
        // The painted values must have LANDED on Part 3's storage (synced
        // through the restored currentPart_, not a stale pre-restore part):
        // tuning is a signed byte; arp/seq live in pendingConfig_.
        check ((int) (int8_t) b.getEngine().getPart (3).partBytes[2] == -33,
               "legacy restore: part_tuning landed on Part 3");
        check (b.getEngine().getPart (3).patchBytes[0] == 3,
               "legacy restore: osc1_shape landed on Part 3");
        check (b.getEngine().getPart (3).pendingConfig_.arpOctave == 4,
               "legacy restore: arp_octave landed on Part 3");
        check (b.getEngine().getPart (3).pendingConfig_.seqData[0] == 99,
               "legacy restore: seq1_step0 landed on Part 3");
        check (b.getEngine().getPart (3).fxState.slotType[0].load() == 3
                   && b.getEngine().getPart (3).fxState.slotEnabled[0].load() == 1,
               "legacy restore: FX type/enabled landed on Part 3");
        // Other parts keep the seeded init (legacy states carry one part only).
        check (b.getEngine().getPart (0).patchBytes[0] == ambika::dsp::WAVEFORM_SAW,
               "legacy restore: Part 0 seeds init patch");
        // A post-restore byte edit routes to Part 3 (currentPart_ tracking).
        setParam (b, "part_octave", 1);
        check (b.getEngine().getPart (3).partBytes[1] == 1
                   && b.getEngine().getPart (0).partBytes[1] != 1,
               "legacy restore: post-restore edits route to Part 3");
    }

    // ---------------------------------------------------------------------
    // [3] Binary host-state v2 round-trips per-part FX (Parts 1 + 4 painted).
    // ---------------------------------------------------------------------
    std::printf ("\n[3] Per-part FX survives a host-state round-trip (binary v3)\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);

        selectPart (a, 1);  paintFx (a);
        selectPart (a, 4);  paintFx (a);
        selectPart (a, 1);   // current part = 1 (saved + restored)
        renderOnce (a);
        check (! allFxAtDefaults (a.getEngine().getPart (1).fxState),
               "source Part 1 has non-default FX (sanity)");
        check (! allFxAtDefaults (a.getEngine().getPart (4).fxState),
               "source Part 4 has non-default FX (sanity)");

        juce::MemoryBlock blob;
        a.getStateInformation (blob);
        check (blob.getSize() > 0, "getStateInformation produced a non-empty block");

        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        renderOnce (b);

        // Painted parts round-trip field-for-field.
        check (countFxMismatches (a.getEngine().getPart (1).fxState,
                                  b.getEngine().getPart (1).fxState) == 0,
               "Part 1 fxState round-trips (binary v3)");
        check (countFxMismatches (a.getEngine().getPart (4).fxState,
                                  b.getEngine().getPart (4).fxState) == 0,
               "Part 4 fxState round-trips (binary v3)");
        // Unpainted parts stay at defaults on both sides.
        check (allFxAtDefaults (b.getEngine().getPart (0).fxState),
               "unpainted Part 0 fxState stays at defaults");
        check (allFxAtDefaults (b.getEngine().getPart (5).fxState),
               "unpainted Part 5 fxState stays at defaults");
        check (b.getEngine().getCurrentPart() == 1, "current part (1) preserved");
    }

    // ---------------------------------------------------------------------
    // [4] Backward compat: a v1 engine-state blob (no FX block) is accepted
    //     and loads with FX at defaults, while the 6-Part core still round-
    //     trips. The v1 blob is hand-crafted from a v2 capture: keep the v1
    //     core (drop every Part's FX block) and rewrite the version byte 2->1.
    //     restoreState is exercised directly (the full setStateInformation path
    //     is covered for v2 in [3] above; the v1 path differs only in omitting
    //     the FX read).
    // ---------------------------------------------------------------------
    std::printf ("\n[4] v1 engine-state blob loads with FX at defaults\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);

        // Distinct CORE on Part 1 + non-default FX on Part 1, so we can prove the
        // v1 blob carries the core but NOT the FX. NOTE: paintFx drives the
        // part-select + APVTS sync (which seeds Part 1 from the APVTS), so the
        // manual core edits MUST come AFTER it (else syncAllParamsToEngine would
        // clobber patchBytes[0] back to the APVTS default).
        selectPart (a, 1);
        paintFx (a);                         // Part 1 gets non-default FX
        a.getEngine().getPart (1).patchBytes[0] = 7;
        a.getEngine().setPartChannel (1, 5);
        renderOnce (a);
        check (! allFxAtDefaults (a.getEngine().getPart (1).fxState),
               "source Part 1 has non-default FX (sanity)");

        // Capture the v7 host state and derive a v1 engine blob from its
        // engine_state. A v7 blob interleaves an 82-byte FX block per Part
        // (4-byte length prefix + 78 FX bytes) after the routing bytes, plus a
        // 2-byte slots/name tail and a 29-byte tuning block (4-byte length
        // prefix + {mode; offsets[12]}), so a naive truncation is NOT a valid
        // v1 blob -- we must extract each Part's core (patch112 + part84 +
        // routing4 = 200 bytes) and skip everything after it.
        constexpr size_t kV1Core = 6 + 6 * (112 + 84 + 4);          // 1206
        juce::MemoryBlock v1Engine;
        {
            juce::MemoryBlock v5Host;
            a.getStateInformation (v5Host);
            auto xml = juce::AudioProcessor::getXmlFromBinary (v5Host.getData(), (int) v5Host.getSize());
            juce::MemoryBlock v5Engine;
            v5Engine.fromBase64Encoding (xml->getStringAttribute ("engine_state"));
            check (v5Engine.getSize() >= 6 + 6 * kV7PartStride && ((const uint8_t*) v5Engine.getData())[4] == 7,
                   "captured engine_state is a v7 blob large enough to derive v1");
            v1Engine.ensureSize (kV1Core);
            const auto* v5 = (const uint8_t*) v5Engine.getData();
            auto* v1 = (uint8_t*) v1Engine.getData();
            std::memcpy (v1, v5, 6);                 // magic + version + currentpart
            v1[4] = 1;                               // rewrite version 5 -> 1
            for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            {
                const size_t v5off = 6 + (size_t) p * kV7PartStride;   // Part's core in v7
                const size_t v1off = 6 + (size_t) p * 200;             // Part's core in v1
                std::memcpy (v1 + v1off, v5 + v5off, 200);             // patch + part + routing (no FX)
            }
        }

        // Restore the v1 blob directly into a fresh engine. A v1 blob has NO FX
        // block, so fxState must stay at defaults while the core round-trips.
        ParvatiAudioProcessor c;
        c.prepareToPlay (48000.0, 256);
        check (c.getEngine().restoreState (v1Engine.getData(), v1Engine.getSize()),
               "restoreState accepts the hand-crafted v1 blob");
        renderOnce (c);

        // Core round-trips (proves the version-1 reader still works).
        check (c.getEngine().getPart (1).patchBytes[0] == 7, "v1 blob: Part 1 patch bytes round-trip");
        check (c.getEngine().getPartChannel (1) == 5, "v1 blob: Part 1 midi channel round-trips");
        // FX stays at defaults (v1 has no FX block).
        bool allDefault = true;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
            if (! allFxAtDefaults (c.getEngine().getPart (i).fxState))
                allDefault = false;
        check (allDefault, "v1 blob: every Part's fxState stays at defaults (FX absent in v1)");
    }

    // ---------------------------------------------------------------------
    // [5] Backward compat: a v2 engine-state blob (pre-master-section: FX block
    //     present but WITHOUT the 4 master fields) loads with the master section
    //     at its audio-preserving defaults, while the v2-era FX round-trips.
    // ---------------------------------------------------------------------
    std::printf ("\n[5] v2 engine-state blob loads with master section at defaults\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        selectPart (a, 1);
        paintFx (a);                       // Part 1 non-default v2-era FX
        // Paint the master section non-default too, so a v2 load (which drops
        // these 4 fields) is detectable as exactly-4 mismatches vs defaults.
        setParam (a, "fx_mix",        90);
        setParam (a, "fx_eq_low",     40);
        setParam (a, "fx_eq_mid",     80);
        setParam (a, "fx_eq_high",   100);
        renderOnce (a);
        check (! allFxAtDefaults (a.getEngine().getPart (1).fxState),
               "source Part 1 has non-default FX incl. master (sanity)");

        // Derive a v2 blob from the v5 capture. The v5 FX block has 5 params/slot
        // (param5 interleaved BEFORE topo/order), while v2 has 4 params/slot and
        // NO master section, so a naive memcpy of the first 71 FX bytes would
        // mis-map fields. Reassemble field-by-field instead: core + fxlen prefix
        // (rewritten 78 -> 71), then each FX field at its v2 offset. Version 5 -> 2.
        constexpr size_t kV2PartStride = 112 + 84 + 4 + 4 + 71;   // 275
        // (the SOURCE stride is the v7 layout, kV7PartStride above)
        // FX-field offsets WITHIN the per-Part FX block (after the 4-byte fxlen).
        // v5: type0 enabled3 drywet6 param9(15) topo24 order25 modSrc26 modDst42 modAmt58 master74.
        // v2: type0 enabled3 drywet6 param9(12) topo21 order22 modSrc23 modDst39 modAmt55.
        juce::MemoryBlock v2Engine;
        {
            juce::MemoryBlock v5Host;
            a.getStateInformation (v5Host);
            auto xml = juce::AudioProcessor::getXmlFromBinary (v5Host.getData(), (int) v5Host.getSize());
            juce::MemoryBlock v5Engine;
            v5Engine.fromBase64Encoding (xml->getStringAttribute ("engine_state"));
            check (v5Engine.getSize() >= 6 + 6 * kV7PartStride && ((const uint8_t*) v5Engine.getData())[4] == 7,
                   "captured engine_state is a v7 blob large enough to derive v2");
            v2Engine.ensureSize (6 + 6 * kV2PartStride);
            const auto* v5 = (const uint8_t*) v5Engine.getData();
            auto* v2 = (uint8_t*) v2Engine.getData();
            std::memcpy (v2, v5, 6);                       // magic + version + currentpart
            v2[4] = 2;                                     // rewrite version 5 -> 2
            for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            {
                const size_t v5off = 6 + (size_t) p * kV7PartStride;
                const size_t v2off = 6 + (size_t) p * kV2PartStride;
                std::memcpy (v2 + v2off, v5 + v5off, 200 + 4);   // core + 4-byte fxlen prefix
                v2[v2off + 200] = 71;                            // rewrite fxlen 78 -> 71 (LE)
                v2[v2off + 201] = 0;
                v2[v2off + 202] = 0;
                v2[v2off + 203] = 0;
                const size_t v5fx = v5off + 204;                 // v5 FX block start
                const size_t v2fx = v2off + 204;                 // v2 FX block start
                std::memcpy (v2 + v2fx + 0,  v5 + v5fx + 0,  9);  // type + enabled + drywet (9 bytes)
                for (int s = 0; s < 3; ++s)                       // params: 4/slot, dropping each slot's param5
                    std::memcpy (v2 + v2fx + 9 + (size_t) s * 4,
                                 v5 + v5fx + 9 + (size_t) s * 5, 4);
                v2[v2fx + 21] = v5[v5fx + 24];                   // topo
                v2[v2fx + 22] = v5[v5fx + 25];                   // order
                std::memcpy (v2 + v2fx + 23, v5 + v5fx + 26, 16); // modSource
                std::memcpy (v2 + v2fx + 39, v5 + v5fx + 42, 16); // modDest
                std::memcpy (v2 + v2fx + 55, v5 + v5fx + 58, 16); // modAmount
                // (no master section in v2)
            }
        }

        ParvatiAudioProcessor c;
        c.prepareToPlay (48000.0, 256);
        check (c.getEngine().restoreState (v2Engine.getData(), v2Engine.getSize()),
               "restoreState accepts the hand-crafted v2 blob");
        renderOnce (c);

        // The v2-era FX round-trips exactly; ONLY the 4 master fields differ
        // (they are absent in v2, so c has them at defaults while a had them painted).
        check (countFxMismatches (a.getEngine().getPart (1).fxState,
                                  c.getEngine().getPart (1).fxState) == 4,
               "v2 blob: only the 4 master fields differ from source (absent in v2)");
        // Every Part's master section sits at the audio-preserving defaults.
        const PartFxState def {};
        bool masterAtDefault = true;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            const auto& fx = c.getEngine().getPart (i).fxState;
            if (fx.mix.load()       != def.mix.load()
                || fx.eqLow.load()     != def.eqLow.load()
                || fx.eqMid.load()     != def.eqMid.load()
                || fx.eqHigh.load()    != def.eqHigh.load())
                masterAtDefault = false;
        }
        check (masterAtDefault, "v2 blob: every Part's master section loads at defaults");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "HOST-STATE TEST: FAILURES" : "HOST-STATE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
