// Desktop shim: voicecard audio-oracle facade.
//
// Narrow API over the REAL firmware voicecard statics (ambika::Voice +
// the ambika::audio_buffer ring) so tests/firmware_parity_test.cpp can drive
// the firmware Voice::ProcessBlock block-by-block WITHOUT including any
// voicecard header in its own TU: the voicecard headers collide with both
// the controller headers (ambika::Part, kControlRate) and the port's DSP
// headers (LUT_RES_* macros vs constexpr ids) when included together.
//
// The .cc is a firmware-only TU (same contract as shims.cc: it must never
// include Parvati headers — the oracle must not share code with the
// implementation under test).
#pragma once

#include <cstdint>

namespace fw_voicecard
{

constexpr int kAudioBlockSize = 40;   // == ambika::kAudioBlockSize (voicecard)

void Init();
void SetPatchByte (int address, uint8_t value);
void Trigger (uint16_t note, uint8_t velocity, uint8_t legato);

// Renders one 40-sample block and drains it from the audio ring buffer
// (the vca()<2 silence path also writes 40 bytes, so drains match writes).
void ProcessBlock (uint8_t out[kAudioBlockSize]);

// Re-seed the firmware's global RNG (lockstep with the port's dsp::random()).
void SeedRandom (uint16_t seed);

// Patch-byte addresses in the ambika::Patch layout (computed in the .cc from
// the firmware struct; the port's Patch copy is byte-identical — sizeof==112
// pinned in Source/dsp/constants.h — and the oracle's byte-equality checks
// themselves trip loudly on any layout mismatch, so plain functions suffice
// here: the header must stay free of firmware includes to keep this TU
// composable with the controller + port headers in the parity test).
int Osc1ShapeOffset();
int MixBalanceOffset();
// The filter-cutoff CV byte (MOD_DST_FILTER_CUTOFF destination readout — the
// byte the hardware drives the analog VCF with; @p i is the port-side enum
// value, identical in the firmware).
uint8_t ModulationDestination (int i);
// Env-slot offsets (env_lfo[i]: attack/decay/sustain/release) + filter_env.
int EnvAttackOffset (int slot);
int EnvSustainOffset (int slot);
int FilterCutoffOffset();
int FilterEnvAmountOffset();
uint8_t WaveformSaw();

}  // namespace fw_voicecard
