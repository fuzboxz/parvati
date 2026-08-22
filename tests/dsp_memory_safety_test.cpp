// dsp_memory_safety_test — regression cover for the memory-safety migration of
// the first-party DSP layer (voice.h / oscillator.h / resources_manager.h /
// FxChain). Every check here pins a guard that converts a former
// out-of-bounds read/write (silent corruption or UB) into a well-defined
// no-op / zero / clamp:
//
//   1. Voice::set_patch_data rejects addresses past the 112-byte Patch struct
//      (the firmware indexed a flat uint8_t* with no bound; a malformed
//      preset could previously walk into Part / tempo / envelopes).
//   2. Voice modulation-source/destination accessors guard the fixed
//      31/19-slot arrays (OOB read -> 0, OOB write -> dropped).
//   3. Voice::mutable_envelope clamps like its const twin envelope().
//   4. ResourcesManager::Lookup(resource, i) rejects resource ids outside
//      the 6-entry lookup_table_table indirection.
//   5. FxChain::setSlotParam/setSlotDryWet reject slot/param indices outside
//      their fixed arrays (regression guard for the existing checks).
//
// The sized-reference render-buffer API (Oscillator/SubOscillator/
// TransientGenerator taking uint8_t (&)[kAudioBlockSize]) is compile-time
// enforcement: every existing render test (osc_sync / sub_oscillator /
// transient_generator / firmware_parity / roundtrip_golden) now doubles as
// its behavioral proof, so no runtime check is needed here.

#include <cstddef>   // offsetof
#include <cstdio>
#include <cstring>

#include "dsp/voice.h"
#include "dsp/fx/FxChain.h"
#include "dsp/resources/resources.h"
#include "dsp/resources/resources_manager.h"

#include "test_utils.h"          // setInt/setChoice/setParam host-style writes
#include "unified_test_runner.h"

using namespace ambika::dsp;

TEST(dsp_memory_safety_test)
{
    // ------------------------------------------------------------------
    // [1] set_patch_data: out-of-struct addresses are dropped, in-range
    //     writes still land. Part lives directly after Patch in the Voice
    //     object, so an unguarded address 112 previously overwrote
    //     part_.volume — observable through vca(), which seeds from
    //     part_.volume << 1 every block (dead envelopes contribute 0).
    // ------------------------------------------------------------------
    printf("[1] Voice::set_patch_data bounds guard\n");
    {
        Voice v;
        v.Init();

        // In-range write still lands (osc[0].shape is byte 0 of Patch).
        v.set_patch_data (0, static_cast<uint8_t> (WAVEFORM_FM));
        CHECK (v.patch().osc[0].shape == WAVEFORM_FM,
               "in-range set_patch_data write lands in patch_.osc[0].shape");

        // Drive the voice to a DEAD, deterministic state: with every envelope
        // DEAD the mod matrix contributes a fixed value every block (the
        // init patch routes ENV_2->VCA; a dead tail is constant), so vca()
        // is stable across blocks and any drift means corruption.
        v.set_part_data (0, 64);
        v.Trigger (60 * 128, 127, 0);
        v.Release();
        int guard = 100000;
        while (! v.envelopesDead() && --guard > 0)
            v.ProcessBlock();
        CHECK (v.envelopesDead(), "released voice reaches DEAD (finite tail)");
        v.ProcessBlock();
        const int vcaBefore = v.vca();
        v.ProcessBlock();
        CHECK (v.vca() == vcaBefore, "dead voice vca() is block-stable");

        // Snapshot in-range fields, then hammer EVERY out-of-struct address
        // (112..255) with 0xFF. Unguarded, this walks Part
        // (volume/legato/portamento), the tempo double, the envelopes and
        // the render buffers; guarded, nothing changes.
        const uint8_t mixOpBefore = v.patch().mix_op;
        const uint8_t filterModeBefore = v.patch().filter[0].mode;
        for (unsigned a = sizeof (Patch); a <= 255u; ++a)
            v.set_patch_data (static_cast<uint8_t> (a), 0xFF);

        v.ProcessBlock();
        CHECK (v.patch().osc[0].shape == WAVEFORM_FM
                   && v.patch().mix_op == mixOpBefore
                   && v.patch().filter[0].mode == filterModeBefore
                   && v.vca() == vcaBefore && v.envelopesDead(),
               "all addresses 112..255 rejected (patch/part/vca unchanged)");
    }

    // ------------------------------------------------------------------
    // [2] Mod-matrix accessors: fixed 31-source / 19-destination arrays.
    // ------------------------------------------------------------------
    printf("[2] modulation source/destination accessor guards\n");
    {
        Voice v;
        v.Init();

        CHECK (v.modulation_source (kNumModulationSources) == 0
                   && v.modulation_source (200) == 0,
               "OOB modulation_source() reads 0");
        CHECK (v.modulation_destination (kNumModulationDestinations) == 0
                   && v.modulation_destination (200) == 0,
               "OOB modulation_destination() reads 0");

        v.set_modulation_source (kNumModulationSources, 0xAB);   // dropped
        v.set_modulation_source (200, 0xCD);                     // dropped
        v.set_modulation_source (0, 42);                         // lands
        CHECK (v.modulation_source (kNumModulationSources) == 0
                   && v.modulation_source (200) == 0
                   && v.modulation_source (0) == 42,
               "OOB set_modulation_source dropped; in-range write lands");
    }

    // ------------------------------------------------------------------
    // [3] mutable_envelope clamps to the last of the fixed 3 slots; an
    //     out-of-range envelope TRIGGER stage sinks to DEAD (both were
    //     unchecked indexes into 3/5-slot arrays).
    // ------------------------------------------------------------------
    printf("[3] mutable_envelope clamp + envelope stage guard\n");
    {
        Voice v;
        v.Init();
        CHECK (v.mutable_envelope (kNumEnvelopes) == v.mutable_envelope (kNumEnvelopes - 1)
                   && v.mutable_envelope (200) == v.mutable_envelope (kNumEnvelopes - 1)
                   && v.mutable_envelope (0) != nullptr,
               "mutable_envelope(i > 2) clamps to envelope_[2] (no OOB pointer)");

        // Hostile stage (NUM_SEGMENTS..255) must land THE TRIGGERED envelope
        // in DEAD (silent) instead of reading stage_target_[stage] out of
        // bounds (the sibling envelopes keep their ATTACK stage).
        v.Trigger (60 * 128, 127, 0);
        v.TriggerEnvelope (0, 200);   // was: stage_target_[200] OOB read
        v.ProcessBlock();
        CHECK (v.envelope (0).stage() == DEAD,
               "TriggerEnvelope(0, 200) sinks envelope 0 to DEAD (no OOB stage index)");

        // Hostile envelope index clamps to the last envelope.
        v.Trigger (60 * 128, 127, 0);
        v.TriggerEnvelope (200, ATTACK);
        v.ProcessBlock();
        CHECK (v.envelope (kNumEnvelopes - 1).stage() == ATTACK,
               "TriggerEnvelope(200, ATTACK) clamps to envelope_[2]");
    }

    // ------------------------------------------------------------------
    // [4] ResourcesManager::Lookup(resource, i): resource id bounds.
    // ------------------------------------------------------------------
    printf("[4] ResourcesManager resource-id guard\n");
    {
        CHECK ((ResourcesManager::Lookup<uint16_t, int> (static_cast<int> (kNumLookupTables), 0) == 0),
               "resource id == kNumLookupTables reads 0 (one past the table)");
        CHECK ((ResourcesManager::Lookup<uint16_t, int> (-1, 0) == 0),
               "negative resource id reads 0");
        CHECK ((ResourcesManager::Lookup<uint16_t, int> (200, 0) == 0),
               "wild resource id 200 reads 0");
        CHECK ((ResourcesManager::Lookup<uint16_t, int> (LUT_RES_OSCILLATOR_INCREMENTS, 0)
                   == lut_res_oscillator_increments[0]),
               "in-range resource id still resolves (firmware parity)");
    }

    // ------------------------------------------------------------------
    // [5] FxChain setters reject slot/param indices outside the fixed
    //     3-slot / 5-param arrays (regression guard).
    // ------------------------------------------------------------------
    printf("[5] FxChain setter bounds guards\n");
    {
        FxChain chain;
        chain.prepare (48000.0, 64);
        chain.setSlotParam (0, 0, 0.25f);
        chain.setSlotParam (-1, 0, 0.9f);    // dropped
        chain.setSlotParam (3, 0, 0.9f);     // dropped
        chain.setSlotParam (0, 5, 0.9f);     // dropped
        chain.setSlotParam (0, -1, 0.9f);    // dropped
        CHECK (chain.debugGetParam (0, 0) == 0.25f,
               "OOB setSlotParam dropped; in-range param kept");

        chain.setSlotDryWet (1, 0.5f);
        chain.setSlotDryWet (-1, 0.9f);      // dropped
        chain.setSlotDryWet (3, 0.9f);       // dropped
        CHECK (chain.debugGetDryWet (1) == 0.5f,
               "OOB setSlotDryWet dropped; in-range dry/wet kept");
    }

    return true;
}
