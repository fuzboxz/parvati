// Phase 1 of the "Patch page" feature: arrangement templates + inference.
//
// Pure translation unit depending only on SynthEngine.h — NO JUCE GUI. Drives
// the engine purely through its EXISTING public setters (the engine internals,
// voice allocators, file formats and audio-thread code are untouched).
//
// VOICE-FIRST MODEL (Task 2, 2026-08): the five templates are VOICE-BUDGET
// presets over the engine's 96-voice pool — each Part carries a voice count
// (0..kMaxVoicesPerPart; 0 = the Part is DISABLED) and the 6-voicecard bitmask
// is DERIVED from those counts (mul_export::deriveMasks; contiguous
// proportional share, min 1 per active Part). With any combination of counts
// legal (pool = 6x16), a preset is just a convenient starting point of counts
// + zones + channels + polyphony + spread:
//
//   Mono          — 1 part, 1 voice, MONO polyphony (one retriggering voice).
//   Poly          — 1 part, 16 voices, POLY.
//   Unison        — 1 part, 16 voices, MONO (a 16-voice unison stack) with a
//                   per-voice detune spread so the stack reads fat, not flat.
//   Multitimbral  — 6 parts, 16 voices each, all MONO, one per MIDI channel
//                   1..6 (individually addressable).
//   Drum Kit      — 6 parts, 1 voice each, all MONO, Omni, each Part mapped to
//                   a single GM drum note (36/38/39/42/46/45).
//
// ----------------------------------------------------------------------------
// Engine-API note (behaviour is identical to what the spec names; the engine
// internals, voice allocators, file formats and audio-thread code stay untouched):
//
//  1. Polyphony per part is WRITTEN via the EXISTING public pair
//       engine.setCurrentPart(part);
//       engine.applyPartByte(15, mode);     // PartData byte 15 = polyphony mode
//     The spec names a non-existent `setPartByte(part,15,mode)`. applyPartByte
//     writes the current Part's partBytes[15] and marks allocationDirty on a real
//     change — the established message-thread path (mirrors the .MUL loader).
//     applyArrangement saves and restores the original currentPart.
//
//  2. Disabling a Part (voice count 0) rides the LEGACY materialization path:
//       engine.setPartVoiceAllocation (part, 0);
//     The PUBLIC setPartVoiceSlots clamps 0 to 1 by design (it can never
//     disable), while the legacy loader path materializes a zero mask as 0
//     slots = disabled. That asymmetry is the engine's single disable entry
//     point for preset/loader code.
// ----------------------------------------------------------------------------
#pragma once

#include "SynthEngine.h"

// Arrangement templates. `Custom` is infer-only (applyArrangement(Custom) is a
// no-op): it labels engine state that does not match any built-in template.
enum class Arrangement
{
    Mono,
    Poly,
    Unison,
    Multitimbral,
    DrumKit,
    Custom
};

// Human label: "Mono" / "Poly" / "Unison" / "Multitimbral" / "Drum Kit" /
// "Custom".
const char* arrangementLabel (Arrangement a);

// Number of built-in (selectable, non-Custom) arrangement templates (= 5).
int arrangementCount();

// Write the template's full 6-Part state into the engine via its EXISTING public
// setters: voice counts (0 = disable via the legacy setPartVoiceAllocation(part,0)
// path; 1..16 via setPartVoiceSlots), MIDI channels (setPartMidiChannel), key
// zones (setPartKeyZone), polyphony and spread (setCurrentPart +
// applyPartByte(15) / applyPartByte(3)). The voicecard bitmasks are NOT written
// — they are DERIVED from the voice counts by the engine
// (rebuildVoiceAllocation). Polyphony + spread are written LAST. The original
// currentPart is restored on return. applyArrangement(Custom) is a no-op
// (Custom is inferred, never applied).
void applyArrangement (SynthEngine& engine, Arrangement a);

// Exact-preset inference from engine state; Custom if nothing matches. A state
// matches iff EVERY part's voice count, MIDI channel, key zone, polyphony
// (PartData byte 15) and spread (PartData byte 3) equal the template's table
// (the applyArrangement round-trip is exact by construction). Reads
// getPartVoiceSlots / getPartChannel / getPartKeyrangeLow/High /
// getPartPolyphony / getPartSpread (all const). Read-only.
Arrangement inferArrangement (const SynthEngine& engine);
