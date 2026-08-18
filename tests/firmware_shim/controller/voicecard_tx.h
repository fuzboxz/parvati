// Desktop shim: controller/voicecard_tx.h — RECORDING replacement for the
// firmware's SPI voicecard transmitter (tests/firmware_parity_test).
//
// The firmware Part talks to its voicecards exclusively through this class:
// Trigger / Release / Kill / WriteData / WriteLfo / RetriggerEnvelope /
// Reset(AllControllers) / PrepareForBlockWrite + WriteBlock. That protocol
// stream IS the controller's observable response surface, so on the host we
// replace the hardware transmitter with a recorder and diff the recorded
// stream against what our SynthEngine does for the same stimulus.
//
// The API mirrors the real class signature-for-signature (everything the
// vendored controller/multi.cc + controller/part.cc call); the SpiMaster /
// AddressBus / RingBuffer internals become recorder storage. The real header
// (ambika_reference/controller/voicecard_tx.h) is shadowed through the shim
// include path; the firmware tree stays untouched.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/protocol.h"   // COMMAND_WRITE_* -> VOICECARD_DATA_* values
#include "controller/controller.h"

// The firmware's part.cc busy-waits via ConstantDelay (avrlib/time.h) inside
// TouchPatch; nothing in its include closure declares the macro on the host,
// so provide the no-op here (this header is included before the use site).
#include "avrlib/time.h"

namespace ambika
{

enum VoicecardDataType
{
    VOICECARD_DATA_PATCH      = COMMAND_WRITE_PATCH_DATA,
    VOICECARD_DATA_PART       = COMMAND_WRITE_PART_DATA,
    VOICECARD_DATA_MODULATION = COMMAND_WRITE_MOD_MATRIX,
};

// One recorded controller->voicecard command.
struct FwVoicecardEvent
{
    enum Kind
    {
        kTrigger,              // voice, note, velocity, legato
        kRelease,              // voice
        kKill,                 // voice
        kWriteData,            // voice, dataType, address, value
        kWriteLfo,             // voice, lfo index, value
        kRetriggerEnvelope,    // voice, envelope index
        kReset,                // voice
        kResetAllControllers,  // voice
        kBlockWrite,           // voice (patch block upload)
    };
    Kind       kind      = kRelease;
    uint8_t    voice     = 0;
    uint16_t   note      = 0;    // Trigger: the tuned 14-bit note
    uint8_t    velocity  = 0;
    uint8_t    legato    = 0;
    uint8_t    dataType  = 0;    // WriteData
    uint8_t    address   = 0;    // WriteData / WriteLfo / RetriggerEnvelope
    uint8_t    value     = 0;    // WriteData / WriteLfo
};

class VoicecardProtocolTx
{
public:
    VoicecardProtocolTx() {}

    static void Init() {}

    static void Trigger (uint8_t voice_id, uint16_t note, uint8_t velocity, uint8_t legato)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kTrigger;
        e.voice = voice_id; e.note = note; e.velocity = velocity; e.legato = legato;
        log().push_back (e);
    }

    static void WriteData (uint8_t voice_id, uint8_t data_type, uint8_t address, uint8_t value)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kWriteData;
        e.voice = voice_id; e.dataType = data_type; e.address = address; e.value = value;
        log().push_back (e);
    }

    static void Release (uint8_t voice_id)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kRelease;
        e.voice = voice_id;
        log().push_back (e);
    }

    static void Kill (uint8_t voice_id)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kKill;
        e.voice = voice_id;
        log().push_back (e);
    }

    static void Reset (uint8_t voice_id)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kReset;
        e.voice = voice_id;
        log().push_back (e);
    }

    static void ResetAllControllers (uint8_t voice_id)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kResetAllControllers;
        e.voice = voice_id;
        log().push_back (e);
    }

    static void WriteLfo (uint8_t voice_id, uint8_t lfo_index, uint8_t value)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kWriteLfo;
        e.voice = voice_id; e.address = lfo_index; e.value = value;
        log().push_back (e);
    }

    static void RetriggerEnvelope (uint8_t voice_id, uint8_t envelope_index)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kRetriggerEnvelope;
        e.voice = voice_id; e.address = envelope_index;
        log().push_back (e);
    }

    // Patch block upload (TouchPatch). The 112-byte body is not recorded —
    // only the command (the parity oracle compares the command stream).
    static void PrepareForBlockWrite (uint8_t voice_id)
    {
        FwVoicecardEvent e;
        e.kind = FwVoicecardEvent::kBlockWrite;
        e.voice = voice_id;
        log().push_back (e);
    }
    static void WriteBlock (uint8_t voice_id, const uint8_t*, uint8_t)
    {
        // Folded into the kBlockWrite event recorded by PrepareForBlockWrite.
        (void) voice_id;
    }

    static void BeginSdCard() {}
    static void EndSdCard() {}

    // ---- recorder plumbing (test-side; not part of the firmware API) ----
    static std::vector<FwVoicecardEvent>& log()
    {
        static std::vector<FwVoicecardEvent> g_log;
        return g_log;
    }
    static void clearLog() { log().clear(); }

private:
    // The real class is all-statics with private hardware members; the
    // recorder needs none. Keep a DISALLOW-style deletion for parity of use.
    VoicecardProtocolTx (const VoicecardProtocolTx&);
    void operator= (const VoicecardProtocolTx&);
};

extern VoicecardProtocolTx voicecard_tx;

}  // namespace ambika
