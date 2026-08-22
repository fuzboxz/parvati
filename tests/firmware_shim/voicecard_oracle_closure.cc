// Desktop shim: the firmware VOICECARD closure, wrapped in namespace fwvc.
//
// The controller oracle (controller/resources.cc) and the voicecard build
// (voicecard/resources.cc) define the same five ambika:: resource-table
// symbol names with DIFFERENT generated contents, so linking both trees into
// one binary requires renaming one side. This TU wraps the whole voicecard
// closure — voice, oscillator, resources, audio_out, and its own private copy
// of avrlib/random.cc — inside namespace fwvc, renaming every symbol it
// defines or references. The private RNG is a feature: the voicecard oracle's
// noise stream cannot be perturbed by controller-side scenarios (it is
// re-seeded explicitly for lockstep by fw_voicecard::SeedRandom).
//
// Firmware-only TU (same contract as shims.cc: never include Parvati headers
// here). The include paths resolve firmware headers through the shim first
// (tests/firmware_shim precedes ambika_reference in the target's include
// dirs).
//
// Standard headers FIRST, at global scope: the wrapped vendor chain pulls
// <cstring>/<cstdint>/... transitively, and a standard header included
// INSIDE a namespace would re-declare its contents there (unresolved
// using-declarations, broken size_t...). Pre-including them makes the
// in-namespace re-includes no-ops via their include guards.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fwvc
{
#include "voicecard/voice.cc"
#include "voicecard/oscillator.cc"
#include "voicecard/resources.cc"
#include "voicecard/audio_out.cc"
#include "avrlib/random.cc"
}  // namespace fwvc
