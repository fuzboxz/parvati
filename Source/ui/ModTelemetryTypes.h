// Copyright (c) 2026 Jozsef Otticsak / Parvati.
//
// ModTelemetryTypes — the dependency-light shared contract for the LIVE
// modulation feedback system (docs/LIVE_MOD_FEEDBACK_DESIGN.md):
//
//   * ModTelemetrySnapshot — the seqlock-guarded block the SynthEngine's audio
//     thread writes (history ring + current mod-source values + envelope /
//     filter observables) and the message thread reads for the UI.
//   * LiveEnvStage / LiveFilterValues — the small provider payloads the
//     EnvelopeDisplay / FilterResponseDisplay live overlays consume.
//   * isBipolarModSource() — the display polarity map, mirroring the voice
//     mod-matrix AC coupling (voice.cpp): LFO 1..4, Pitch Bend and Note are
//     bipolar (128 = neutral); everything else is unipolar (0 = floor).
//
// This header deliberately includes NOTHING but <cstdint> so the engine
// (SynthEngine.h) and every UI component can share it without dragging JUCE
// module headers into the DSP shard.

#pragma once

#include <cstddef>   // size_t without dragging JUCE / DSP headers into this shard
#include <cstdint>

namespace parvati
{

//==============================================================================
// One consistent telemetry frame. Trivially copyable (the engine memcpy-copies
// it under a seqlock). kNumSources is pinned to the Ambika MOD_SRC_LAST count
// by a static_assert in SynthEngine.h.
struct ModTelemetrySnapshot
{
    static constexpr int kNumSources = 32;    // >= ambika::dsp::MOD_SRC_LAST (31): every real
                                               // source index fits; the spare slot carries the
                                               // NOTE-SEQ preview (kNoteSeqSlot below)
    static constexpr int kHistoryLen = 256;   // ~3.13 s at the ~81.7 Hz append rate
    // (2026-08-22: 128 -> 256, user feedback — the strips scrolled visibly
    // faster than the previews/indicators; doubling the window halves the
    // apparent scroll speed. Append rate is unchanged — time fidelity of the
    // samples themselves is exact — only how much history one strip spans.)

    // The append cadence the engine runs (kInternalSampleRate / kAudioBlockSize
    // / kUiTelDecimBlocks == 39216 / 40 / 12 ~= 81.7 appends/s; SynthEngine.h's
    // kUiTelDecimBlocks is the other side of this pair — keep them in sync).
    // The UI uses it to render the strips' scroll position as a pure function
    // of WALL TIME (smooth, tick-jitter-free motion).
    static constexpr double kAppendHz = 39216.0 / (40.0 * 12.0);

    // The bar-only Note Sequencer pill has NO MOD_SRC_* enum (its output is
    // note events, not a modulation bus value), so its live preview rides the
    // one spare slot: the tracked part's currently-sounding sequencer note
    // (0..127 -> 0..254, 0 = rest/gap) — a melody trace with rests as gaps.
    static constexpr int kNoteSeqSlot = kNumSources - 1;

    // MT-authoritative validity epoch: bumped by SynthEngine::resetUiTelemetry
    // on patch load / part switch / init. A snapshot whose epoch does not match
    // the engine's live epoch is STALE (readUiTelemetry reports invalid).
    uint32_t epoch = 0;

    // The multitimbral part this frame describes (0..5, -1 = none yet).
    int part = -1;

    // CURRENT effective mod-source values (0..255) of the tracked part's
    // representative (most-recently-triggered active) voice — exactly what the
    // FX mod matrix consumes.
    uint8_t sources[(size_t) kNumSources] {};

    // Recent history per source, OLDEST -> NEWEST, 0..255. historyCount == 0
    // means "no history yet" (e.g. after a reset); fewer than kHistoryLen
    // samples are left-aligned from index 0. The ENGINE's internal storage is
    // a ring; readUiTelemetry linearizes it into this layout.
    // historyHead is ENGINE-INTERNAL ring metadata (the next-write position,
    // written under the same seqlock critical sections as the samples so the
    // reader's copy is always self-consistent); the LINEARIZED frame handed
    // to the UI zeroes it — consumers read history[0..historyCount).
    uint8_t history[(size_t) kNumSources * (size_t) kHistoryLen] {};
    int     historyCount = 0;
    int     historyHead  = 0;   // engine-internal ring next-write index

    // Envelopes 1..3 of the representative voice.
    uint8_t envStage[3] {};      // 0..4: ATTACK / DECAY / SUSTAIN / RELEASE / DEAD
    float   envProgress[3] {};   // 0..1 progress within the current stage
    float   envLevel[3] {};      // 0..1 current envelope output

    // Filter (representative voice, EFFECTIVE = modulation-applied values).
    uint16_t effCutoff     = 0;  // 0..255 (modulation_destinations domain)
    uint16_t effResonance  = 0;  // 0..255
    uint8_t  filterMode    = 0;  // 0..3 LP/BP/HP/Notch

    // Oscillator parameters (representative voice, EFFECTIVE = the
    // modulation-applied bytes UpdateDestinations feeds osc_1/osc_2's
    // set_parameter() — dst_[MOD_DST_PARAMETER_{1,2}] >> 7, 0..127). Drives
    // the OSC waveform preview's live overlay (same discipline as the filter
    // bytes above; meaningful while voiceActive).
    uint8_t  effOscParam[2] {};

    // True while the tracked part has an active representative voice. False =>
    // the envelope/filter observables are the held tail values and the UI hides
    // its live markers.
    bool voiceActive = false;
};

//==============================================================================
// Live stage of one envelope, as consumed by EnvelopeDisplay's marker overlay.
struct LiveEnvStage
{
    bool  active   = false;
    int   stage    = 4;      // 0..4 ATTACK/DECAY/SUSTAIN/RELEASE/DEAD
    float progress = 0.0f;   // 0..1 within the stage
};

// Live effective filter state, as consumed by FilterResponseDisplay's live
// curve overlay. cutoff01/reso01 are normalized to the 0..255 effective-byte
// domain (the same domain as the display's base curve bytes).
struct LiveFilterValues
{
    bool  active  = false;
    float cutoff01 = 0.5f;
    float reso01   = 0.0f;
};

// Live effective OSC parameter for ONE oscillator (index 0/1), as consumed by
// OscPreviewDisplay's live overlay (2026-08-23 parity pass — the osc preview
// now follows the engine exactly like the filter preview). param01 is the
// 0..127 effective byte normalized to 0..1 (the same domain as the display's
// base param byte), so the display's temporal activity gate can byte-diff it
// against its own quantization of the target.
struct LiveOscValues
{
    bool  active  = false;
    float param01 = 0.0f;
};

//==============================================================================
// Display polarity for a MOD_SRC_* enum (mirrors the AC/DC coupling of the
// voice mod matrix, voice.cpp): bipolar sources rest at 128 and swing both
// ways; everything else is a unipolar 0..255 level.
inline constexpr bool isBipolarModSource (int modSrcEnum) noexcept
{
    // MOD_SRC_LFO_1..4, MOD_SRC_PITCH_BEND, MOD_SRC_NOTE.
    // Enum order (dsp/patch.h): ENV 0..2, LFO 3..6, OP 7..10, SEQ 11..12,
    // ARP 13, VELOCITY 14, AFTERTOUCH 15, PITCH_BEND 16, WHEEL 17, WHEEL_2 18,
    // EXPRESSION 19, NOTE 20, GATE 21, NOISE 22, RANDOM 23, CONSTANTS 24..30.
    return (modSrcEnum >= 3 && modSrcEnum <= 6)      // LFO 1..4
        || modSrcEnum == 16                          // PITCH_BEND
        || modSrcEnum == 20;                         // NOTE
}

//==============================================================================
// ALWAYS-ON telemetry contract (2026-08-21 user request): the history strips
// start at zero, keep scrolling forever, and always show the modulator's
// ACTUAL state — including while the tracked part is idle (no active voice).
// Only the live-per-voice generators stop existing when nothing sounds; the
// classes below keep their true value instead of freezing mid-air:
//   * PITCH_BEND / WHEEL / WHEEL_2 / EXPRESSION — persisted controller values
//     (a held wheel position IS the modulator's state; they never decay).
//   * CONSTANT_* — literal constants (a constant's state is its value).
// Everything else (ENV, LFO, OP, SEQ, ARP, VELOCITY, AFTERTOUCH, NOTE, GATE,
// NOISE, RANDOM and the NOTE-SEQ spare slot) exists only inside active
// voices in this engine: idle = ZERO (the strips fall to the floor, exactly
// the user's "LFO goes to zero when no key is held").
inline constexpr bool telemetrySourcePersistsWhenIdle (int modSrcEnum) noexcept
{
    // PITCH_BEND 16, WHEEL 17, WHEEL_2 18, EXPRESSION 19, CONSTANTS 24..30
    // (dsp/patch.h order — this header deliberately includes no DSP headers).
    return (modSrcEnum >= 16 && modSrcEnum <= 19)
        || (modSrcEnum >= 24 && modSrcEnum <= 30);
}

// The literal byte a CONSTANT_* source carries (modulation_sources_ init,
// voice.cpp: enum 24..30 -> 255/128/64/32/16/8/4 = min(255, 256 >> n)).
inline constexpr uint8_t telemetryConstantByte (int modSrcEnum) noexcept
{
    return modSrcEnum <= 24
        ? uint8_t { 255 }   // CONSTANT_256 (a byte cannot hold 256; the voice uses 255)
        : static_cast<uint8_t> (256 >> (modSrcEnum - 24));
}

// Build the IDLE telemetry row for @p lastSources (the part's persisted
// lastModSources_): persisting classes keep their value, everything else 0.
inline constexpr void telemetryIdleRow (uint8_t* row, int rowLen,
                                        const uint8_t* lastSources) noexcept
{
    for (int src = 0; src < rowLen; ++src)
    {
        if (! telemetrySourcePersistsWhenIdle (src))            row[src] = 0;
        else if (src >= 24 && src < rowLen)                     row[src] = telemetryConstantByte (src);
        else if (lastSources != nullptr)                        row[src] = lastSources[src];
        else                                                    row[src] = 0;
    }
}

}  // namespace parvati
