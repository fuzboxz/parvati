// Desktop shim: voicecard audio-oracle implementation (firmware-only TU).
//
// Drives the REAL firmware voicecard (fwvc::ambika::Voice statics, rendered
// bytes drained from the fwvc::ambika::audio_buffer ring buffer) for
// firmware_parity_test's mix-gain-glide scenario. The voicecard closure
// lives inside namespace fwvc (see voicecard_oracle_closure.cc — the
// controller and voicecard resource tables share symbol names with different
// contents), so this TU re-includes the voicecard headers inside the same
// namespace to talk to it.
//
// Like shims.cc, this TU deliberately includes ONLY shim + firmware headers
// — never any Parvati headers — so the oracle stays an independent
// implementation (no shared code with the port under test).
#include "firmware_shim/voicecard_oracle.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fwvc
{
#include "avrlib/random.h"
#include "voicecard/voice.h"
#include "voicecard/audio_out.h"
}

namespace fw_voicecard
{

void Init() { fwvc::ambika::Voice::Init(); }

void SetPatchByte (int address, uint8_t value)
{
    fwvc::ambika::Voice::set_patch_data (static_cast<uint8_t> (address), value);
}

void Trigger (uint16_t note, uint8_t velocity, uint8_t legato)
{
    fwvc::ambika::Voice::Trigger (note, velocity, legato);
}

void ProcessBlock (uint8_t out[kAudioBlockSize])
{
    fwvc::ambika::Voice::ProcessBlock();
    for (int i = 0; i < kAudioBlockSize; ++i)
        out[i] = fwvc::ambika::audio_buffer.Read();
}

void SeedRandom (uint16_t seed) { fwvc::avrlib::Random::Seed (seed); }

int Osc1ShapeOffset()  { return static_cast<int> (offsetof (fwvc::ambika::Patch, osc[0].shape)); }
int MixBalanceOffset() { return static_cast<int> (offsetof (fwvc::ambika::Patch, mix_balance)); }

uint8_t ModulationDestination (int i)
{
    return fwvc::ambika::Voice::modulation_destination (static_cast<uint8_t> (i));
}
int EnvAttackOffset (int slot)
{
    return static_cast<int> (offsetof (fwvc::ambika::Patch, env_lfo[(size_t) slot].attack));
}
int EnvSustainOffset (int slot)
{
    return static_cast<int> (offsetof (fwvc::ambika::Patch, env_lfo[(size_t) slot].sustain));
}
int FilterCutoffOffset()
{
    return static_cast<int> (offsetof (fwvc::ambika::Patch, filter[0].cutoff));
}
int FilterEnvAmountOffset()
{
    return static_cast<int> (offsetof (fwvc::ambika::Patch, filter_env));
}
uint8_t WaveformSaw()  { return fwvc::ambika::WAVEFORM_SAW; }

}  // namespace fw_voicecard
