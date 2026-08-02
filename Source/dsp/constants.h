// Copyright 2011 Emilie Gillet.
//
// Faithful C++17 port of Ambika engine constants (voicecard/voicecard.h,
// voicecard/voice.h pitch constants, voicecard/oscillator.h zone constants).
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef AMBIKA_DSP_CONSTANTS_H_
#define AMBIKA_DSP_CONSTANTS_H_

#include <cstdint>

#include "dsp/patch.h"

namespace ambika::dsp {

// ---- From voicecard/voicecard.h ----

// One control signal sample is generated for each 40 audio samples.
// (The modulation matrix / per-block CV update runs once per block.)
static constexpr uint8_t kControlRate = 40;

// The latency is 1ms, with a buffer storing 4ms of audio.
static constexpr uint8_t kAudioBlockSize = kControlRate;

static constexpr uint8_t kSystemVersion = 0x10;

// ---- From voicecard/voice.h (MIDI -> oscillator increment conversion) ----

// Pitches are stored on 14 bits: the 7 highest bits are the MIDI note value,
// the 7 lowest bits are used for fine-tuning (1/128 semitone units).
static constexpr int16_t kLowestNote = 0 * 128;
static constexpr int16_t kHighestNote = 120 * 128;
static constexpr int16_t kOctave = 12 * 128;
static constexpr int16_t kPitchTableStart = 116 * 128;

// ---- From voicecard/oscillator.h (bandlimited multi-zone selection) ----

static constexpr uint8_t kNumZonesFullSampleRate = 6;
static constexpr uint8_t kNumZonesHalfSampleRate = 5;

// ---- Engine internal sample rate (voicecard PWM carrier) ----
// The voicecard drives the analog filter/DAC from phase-correct 8-bit PWM at
// F_CPU/(2*TOP) = 20000000/(2*255) = 39215.7 Hz. The pitch table
// (lut_res_oscillator_increments) was generated for this rate. Back-solving
// MIDI note 116 (~6645 Hz) from the table gives ~39218 Hz, confirming it.
// The integer engine runs here; the L3 wrapper resamples to the host rate.
static constexpr double kInternalSampleRate = 39216.0;

// ---- MIDI-clock tempo-synced LFO / arpeggiator timing (controller/part.cc) ----
// midi_clock_tick_per_step[rate] = cycle length in 24-PPQN MIDI-clock ticks,
// indexed by the synced LFO rate (0..kNumSyncedLfoRates-1) and by arp resolution.
// (kNumSyncedLfoRates itself is defined in patch.h.)
static constexpr uint8_t midi_clock_tick_per_step[kNumSyncedLfoRates] = {
    96, 72, 64, 48, 36, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1
};

// ---- Layout invariant: the Patch struct must be byte-identical to firmware ----
//
// The firmware indexes `Patch` as a flat `uint8_t*` array (voice.cc uses
// `patch_data_` = reinterpret_cast<uint8_t*>(&patch_)), so sizeof(Patch) is
// a hard contract. See the byte-offset map in patch.h.
static_assert(sizeof(Patch) == 112,
              "Ambika Patch struct must be exactly 112 bytes to match firmware.");

// Modulation sources/destinations are used as array indices sized by these
// enum tails; sanity-check the counts match the enum cardinalities.
static_assert(kNumModulationSources == 31, "MOD_SRC_LAST must be 31");
static_assert(kNumModulationDestinations == 19, "MOD_DST_LAST must be 19");

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_CONSTANTS_H_
