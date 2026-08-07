// Host plugin-state persistence regression test for Parvati.
//
// Verifies getStateInformation / setStateInformation round-trip preserves the
// FULL 6-Part multitimbral state (patch bytes, arp/seq config, MIDI routing,
// voice allocation, current part) -- not just the current Part. Before the fix
// the host state carried only the current Part's APVTS values, so Parts 1..5
// (patch / arp / seq / routing) reverted to init on every DAW project reload.
//
// Built by default. Run with: ./build/parvati_host_state_test

#include <cstdio>
#include <cstring>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

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
        check (b.getEngine().getPartVoiceAllocation (1) == 0x06, "Part 1 voice allocation preserved");
        check (b.getEngine().getPart (1).pendingConfig_.arpOctave == 4, "Part 1 arp octave preserved");
        check (b.getEngine().getPart (1).pendingConfig_.seqData[0] == 77, "Part 1 seq step preserved");

        // Part 2 survived + is the restored current part.
        check (b.getEngine().getPart (2).patchBytes[0] == 11, "Part 2 osc1_shape preserved");
        check (b.getEngine().getPartChannel (2) == 9, "Part 2 midi channel preserved");
        check (b.getEngine().getPartVoiceAllocation (2) == 0x18, "Part 2 voice allocation preserved");
        check (b.getEngine().getPart (2).pendingConfig_.arpResolution == 3, "Part 2 arp resolution preserved");
        check (b.getEngine().getCurrentPart() == 2, "current part preserved");
    }

    // ---------------------------------------------------------------------
    // [2] Backward compat: a legacy state with no engine_state falls back
    //     gracefully (no crash; Parts seed init; current Part from APVTS).
    // ---------------------------------------------------------------------
    std::printf ("\n[2] Legacy state (no engine_state) falls back gracefully\n");
    {
        // Capture a real state, then strip the engine_state attribute to mimic
        // a pre-persistence saved project.
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        juce::MemoryBlock full;
        a.getStateInformation (full);

        auto xml = juce::AudioProcessor::getXmlFromBinary (full.getData(), (int) full.getSize());
        check (xml != nullptr && xml->hasAttribute ("engine_state"), "fresh state carries engine_state");
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
        // Fallback path: Part 0 keeps the seeded audible init patch (no blob apply).
        check (b.getEngine().getPart (0).patchBytes[0] == ambika::dsp::WAVEFORM_SAW,
               "Part 0 seeds init patch after legacy restore");
    }

    // ---------------------------------------------------------------------
    // [3] Binary host-state v2 round-trips per-part FX (Parts 1 + 4 painted).
    // ---------------------------------------------------------------------
    std::printf ("\n[3] Per-part FX survives a host-state round-trip (binary v2)\n");
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
               "Part 1 fxState round-trips (binary v2)");
        check (countFxMismatches (a.getEngine().getPart (4).fxState,
                                  b.getEngine().getPart (4).fxState) == 0,
               "Part 4 fxState round-trips (binary v2)");
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

        // Capture the v2 host state and derive a v1 engine blob from its
        // engine_state. A v2 blob interleaves a 75-byte FX block per Part
        // (4-byte length prefix + 71 FX bytes) AFTER the routing bytes, so a
        // naive truncation is NOT a valid v1 blob -- we must extract each Part's
        // core (patch112 + part84 + routing4 = 200 bytes) and skip the FX block.
        constexpr size_t kV1Core = 6 + 6 * (112 + 84 + 4);          // 1206
        constexpr size_t kV2PartStride = 112 + 84 + 4 + 4 + 71;    // 275 (core + fxlen + fx)
        juce::MemoryBlock v1Engine;
        {
            juce::MemoryBlock v2Host;
            a.getStateInformation (v2Host);
            auto xml = juce::AudioProcessor::getXmlFromBinary (v2Host.getData(), (int) v2Host.getSize());
            juce::MemoryBlock v2Engine;
            v2Engine.fromBase64Encoding (xml->getStringAttribute ("engine_state"));
            check (v2Engine.getSize() >= 6 + 6 * kV2PartStride && ((const uint8_t*) v2Engine.getData())[4] == 2,
                   "captured engine_state is a v2 blob large enough to derive v1");
            v1Engine.ensureSize (kV1Core);
            const auto* v2 = (const uint8_t*) v2Engine.getData();
            auto* v1 = (uint8_t*) v1Engine.getData();
            std::memcpy (v1, v2, 6);                 // magic + version + currentpart
            v1[4] = 1;                               // rewrite version 2 -> 1
            for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            {
                const size_t v2off = 6 + (size_t) p * kV2PartStride;   // Part's core in v2
                const size_t v1off = 6 + (size_t) p * 200;             // Part's core in v1
                std::memcpy (v1 + v1off, v2 + v2off, 200);             // patch + part + routing (no FX)
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

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "HOST-STATE TEST: FAILURES" : "HOST-STATE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
