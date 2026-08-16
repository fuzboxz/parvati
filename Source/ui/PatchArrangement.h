// Phase 1 of the "Patch page" feature: arrangement templates + inference.
//
// Pure translation unit depending only on SynthEngine.h — NO JUCE GUI. Drives
// the engine purely through its EXISTING public setters (the engine internals,
// voice allocators, file formats and audio-thread code are untouched).
//
// VOICE-FIRST MODEL (Task 2, 2026-08): the six templates are VOICE-BUDGET
// presets over the engine's 96-voice pool — each Part carries a voice count
// (1..kMaxVoicesPerPart) and the 6-voicecard bitmask is DERIVED from those
// counts (mul_export::deriveMasks; contiguous proportional share, min 1 per
// active Part). The old card-count presets (Stack 2/Split 2/Layer 2 card
// splits) are gone: with any combination of counts legal (pool = 6x16), a
// preset is just a convenient starting point of counts + zones + channels +
// polyphony. "Mono" is a TRUE mono preset: 1 voice + MONO polyphony = one
// retriggering voice, no unison.
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
    Single,
    DualLayer,
    DualSplit,
    QuadSplit,
    Multi6,
    Custom
};

// Human label: "Mono" / "Single" / "Dual Layer" / "Dual Split" / "Quad Split"
// / "Multi 6" / "Custom".
const char* arrangementLabel (Arrangement a);

// Number of built-in (selectable, non-Custom) arrangement templates (= 6).
int arrangementCount();

// Write the template's full 6-Part state into the engine via its EXISTING public
// setters: voice counts (setPartVoiceSlots 1..16; 0 = disable via the legacy
// setPartVoiceAllocation(part,0) path), MIDI channels (setPartMidiChannel), key
// zones (setPartKeyZone) and polyphony (setCurrentPart + applyPartByte(15)).
// The voicecard bitmasks are NOT written — they are DERIVED from the voice
// counts by the engine (rebuildVoiceAllocation). Polyphony is written LAST.
// The original currentPart is restored on return. applyArrangement(Custom) is a
// no-op (Custom is inferred, never applied).
void applyArrangement (SynthEngine& engine, Arrangement a);

// Exact-preset inference from engine state; Custom if nothing matches. A state
// matches iff EVERY part's voice count, MIDI channel, key zone and polyphony
// (PartData byte 15) equal the template's table (the applyArrangement round-trip
// is exact by construction). Reads getPartVoiceSlots / getPartChannel /
// getPartKeyrangeLow/High / getPartPolyphony (all const). Read-only.
Arrangement inferArrangement (const SynthEngine& engine);
