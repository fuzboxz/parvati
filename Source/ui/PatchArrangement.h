// Phase 1 of the "Patch page" feature: arrangement templates + inference.
//
// Pure translation unit depending only on SynthEngine.h — NO JUCE GUI. Drives
// the engine purely through its EXISTING public setters (the engine internals,
// voice allocators, file formats and audio-thread code are untouched).
//
// Design: /tmp/parvati_patch_design.md ("Phase 1").
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
// (Polyphony READ previously forced inferArrangement to be non-const, because
//  SynthEngine had no const polyphony accessor. Resolved by adding the additive
//  const SynthEngine::getPartPolyphony(int), so inference is now const-correct.)
// ----------------------------------------------------------------------------
#pragma once

#include "SynthEngine.h"

// Arrangement templates. `Custom` is infer-only (applyArrangement(Custom) is a
// no-op): it labels engine state that does not match any built-in template.
enum class Arrangement
{
    Single,
    Stack,
    Split2,
    Layer2,
    Multi6,
    Custom
};

// Human label: "Single" / "Stack" / "Split 2" / "Layer 2" / "Multi 6" / "Custom".
const char* arrangementLabel (Arrangement a);

// Number of built-in (selectable, non-Custom) arrangement templates (= 5).
int arrangementCount();

// Write the template's full 6-Part state into the engine via its EXISTING public
// setters: voicecard bitmasks (setPartVoiceAllocation), MIDI channels
// (setPartMidiChannel), key zones (setPartKeyZone) and polyphony (setPartByte
// equivalent: setCurrentPart + applyPartByte(15,mode)). Card bitmasks are
// assigned CONTIGUOUSLY in part order from the per-part counts (part 0 gets the
// first N0 cards, part 1 the next N1, ...). Polyphony is written LAST. The
// original currentPart is restored on return. applyArrangement(Custom) is a
// no-op (Custom is inferred, never applied).
void applyArrangement (SynthEngine& engine, Arrangement a);

// Best-effort template inference from engine state; Custom if nothing matches.
// Reads per-part popcount(voiceAllocation), midiChannel, keyrange and polyphony
// (PartData byte 15, via the const getPartPolyphony getter). Read-only.
Arrangement inferArrangement (const SynthEngine& engine);
