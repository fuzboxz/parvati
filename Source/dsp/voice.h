// Faithful C++17 port of Ambika's `voicecard/voice.{h,cc}` — the per-voice
// synthesis engine: the modulation matrix + the full ProcessBlock signal flow.
//
// Original: ambika_reference/voicecard/voice.{h,cc} (Emilie Gillet, GPL3).
//
// CONVERSION NOTES (read these — this is the highest-risk module):
//
//  * The firmware `Voice` and its globals (osc_1/osc_2/sub_osc/transient_generator)
//    are STATIC singletons (one voicecard = one voice). Here `Voice` is an
//    INSTANCE class so the synth can run 16 of them; every former `static`
//    member and global is now a per-instance member. The DSP math is otherwise
//    byte-for-byte identical (integer 8-bit, centred at 128).
//
//  * OUTPUT BOUNDARY: the firmware wrote the rendered 8-bit samples into a
//    global `audio_buffer` ring via `audio_buffer.Overwrite2(a,b)`. Here the
//    block is written into the per-instance `output_[kAudioBlockSize]` buffer
//    (helper `Overwrite2()`). `ProcessBlock()` always produces exactly
//    `kAudioBlockSize` (40) samples. The 8-bit→float conversion + VCA gain +
//    analog filter are applied by the L3 wrapper (AmbikaVoice), NOT here —
//    faithfully matching the hardware, where the VCA/filter are ANALOG and
//    applied after the digital audio output. `vca()` only gates the early-out.
//
//  * LFO_1/2/3 are rendered IN-ENGINE here (see voice.cpp LoadSources /
//    UpdateDestinations) — the firmware received them from the controller over
//    SPI. This plugin has no controller, so each Voice owns three `Lfo`
//    instances driven from patch_.env_lfo[i].shape / .rate. See the note in
//    voice.cpp for the free-running vs synced-rate handling.
//
//  * The `static uint8_t ops[9]` scratch in the firmware LoadSources is a
//    local array here (per-voice; it must not be shared across voices).

#ifndef PARVATI_DSP_VOICE_H_
#define PARVATI_DSP_VOICE_H_

#include <cstdint>

#include "dsp/constants.h"
#include "dsp/patch.h"

#include "dsp/envelope.h"
#include "dsp/lfo.h"
#include "dsp/oscillator.h"
#include "dsp/sub_oscillator.h"
#include "dsp/transient_generator.h"

namespace ambika::dsp {

class Voice {
 public:
    Voice() = default;

    // One-time power-up initialisation (loads the init patch, arms envelopes).
    void Init();

    // Called whenever a new note is played (manually or via the arpeggiator).
    // `note` is a 14-bit pitch (7 bits MIDI note : 7 bits fine, 1/128 semitone).
    void Trigger(uint16_t note, uint8_t velocity, uint8_t legato);

    // Move this voice to the release stage.
    void Release();

    // Instantly silence the voice.
    void Kill() { TriggerEnvelope(DEAD); }

    // Render one kAudioBlockSize (40-sample) block into output().
    void ProcessBlock();

    // ---- the 8-bit values the firmware sent to the ANALOG filter / VCA ----
    // (read by the L3 wrapper to drive the emulated filter + VCA gain).
    inline uint8_t cutoff() const {
        return static_cast<uint8_t>(modulation_destinations_[MOD_DST_FILTER_CUTOFF]);
    }
    inline uint8_t vca() const {
        return static_cast<uint8_t>(modulation_destinations_[MOD_DST_VCA]);
    }
    inline uint8_t crush() const {
        return static_cast<uint8_t>(modulation_destinations_[MOD_DST_MIX_CRUSH]);
    }
    inline uint8_t resonance() const {
        return static_cast<uint8_t>(modulation_destinations_[MOD_DST_FILTER_RESONANCE]);
    }
    // filter[0].mode selects the analog filter output (LP/BP/HP/NOTCH).
    inline uint8_t mode() const { return patch_.filter[0].mode; }

    // True when ALL three envelopes have reached DEAD (the release tail is
    // done). A robust voice-free condition independent of the VCA routing: even
    // a patch with no ENV->VCA modulation (so vca() never collapses to <2)
    // reaches DEAD after Release() (every segment advances — attack/decay/
    // release increments are always >=1, so a released voice always finishes).
    bool envelopesDead() const {
        return envelope_[0].stage() == DEAD && envelope_[1].stage() == DEAD
            && envelope_[2].stage() == DEAD;
    }

    // The last rendered 40-sample 8-bit block (centred at 128).
    const uint8_t* output() const { return output_; }

    inline uint8_t modulation_source(uint8_t i) const { return modulation_sources_[i]; }
    inline uint8_t modulation_destination(uint8_t i) const {
        return static_cast<uint8_t>(modulation_destinations_[i]);
    }
    inline void set_modulation_source(uint8_t i, uint8_t value) {
        modulation_sources_[i] = value;
    }

    // Per-voice pitch-bend offset, in the same 1/128-semitone units as the
    // 14-bit note pitch (pitch_value_). Applied directly to the oscillator
    // pitch in RenderOscillators() so the bend is always audible regardless of
    // the mod-matrix routing. Set by the L3 wrapper from the host pitch wheel /
    // MPE per-note bend (0 = no bend). (Minimal hook for MPE / pitch-wheel fix.)
    void set_pitch_bend_offset(int16_t offset) { pitch_bend_offset_ = offset; }

    // ---- patch / part byte access (the firmware indexes Patch as uint8_t*) ----
    void set_patch_data(uint8_t address, uint8_t value) {
        reinterpret_cast<uint8_t*>(&patch_)[address] = value;
    }
    void set_part_data(uint8_t address, uint8_t value) {
        if (address < sizeof(part_))   // Part is 7 bytes; reject controller-side offsets (arp/seq/polyphony) that would write OOB.
            reinterpret_cast<uint8_t*>(&part_)[address] = value;
    }
    uint8_t* mutable_patch_data() { return reinterpret_cast<uint8_t*>(&patch_); }
    const Patch& patch() const { return patch_; }

    Envelope* mutable_envelope(uint8_t i) { return &envelope_[i]; }
    void TriggerEnvelope(uint8_t stage);
    void TriggerEnvelope(uint8_t index, uint8_t stage);

    void ResetAllControllers();

    // Host transport tempo (BPM) for tempo-synced LFOs (rate < kNumSyncedLfoRates).
    void setTempo(double bpm) { tempo_bpm_ = bpm; }

 private:
    // Mirrors `audio_buffer.Overwrite2(a, b)` — writes a pair into output_.
    inline void Overwrite2(uint8_t a, uint8_t b) {
        output_[write_index_++] = a;
        output_[write_index_++] = b;
    }

    void LoadSources();
    void ProcessModulationMatrix();
    void UpdateDestinations();
    void RenderOscillators();

    // Reset SLAVE-mode LFOs to phase 0 on a fresh note-on (firmware
    // Part::RetriggerLfos). Only affects lfo_[0..2] (MOD_SRC_LFO_1..3).
    void RetriggerLfos();

    // The patch & part are indexed as flat byte arrays (firmware contract).
    Patch patch_ {};
    Part  part_  {};

    // Host transport tempo (BPM) for tempo-synced LFOs (rate < 15).
    double tempo_bpm_ { 120.0 };

    // Envelope generators (3) + the voice LFO (MOD_SRC_LFO_4) + the three
    // in-engine LFOs (MOD_SRC_LFO_1/2/3).
    Envelope envelope_[kNumEnvelopes] {};
    Lfo voice_lfo_ {};
    Lfo lfo_[kNumLfos] {};

    // Oscillators + auxiliary generators.
    Oscillator osc_1 {};
    Oscillator osc_2 {};
    SubOscillator sub_osc {};
    TransientGenerator transient_generator {};

    uint8_t gate_ {};

    // 14-bit pitch portamento state.
    int16_t pitch_increment_ {};
    int16_t pitch_target_ {};
    int16_t pitch_value_ {};

    // Per-voice pitch-bend offset (1/128-semitone units). Added to base_pitch
    // in RenderOscillators() (direct osc pitch, independent of the mod matrix).
    int16_t pitch_bend_offset_ {};

    // Modulation matrix working storage.
    uint8_t modulation_sources_[kNumModulationSources] {};
    int8_t  modulation_destinations_[kNumModulationDestinations] {};
    int16_t dst_[kNumModulationDestinations] {};

    // Per-block render buffers (all kAudioBlockSize).
    uint8_t buffer_[kAudioBlockSize] {};
    uint8_t osc2_buffer_[kAudioBlockSize] {};
    uint8_t sync_state_[kAudioBlockSize] {};
    uint8_t no_sync_[kAudioBlockSize] {};
    uint8_t dummy_sync_state_[kAudioBlockSize] {};

    // Final 8-bit output block + write cursor.
    uint8_t output_[kAudioBlockSize] {};
    uint8_t write_index_ {};

    // (No copy: a Voice owns oscillator/envelope state.)
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
};

}  // namespace ambika::dsp

#endif  // PARVATI_DSP_VOICE_H_
