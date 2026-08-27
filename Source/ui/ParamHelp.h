// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// ParamHelp — paramID -> curated help/tooltip text for every Hellcat APVTS
// parameter.
//
// The patch surface exposed by getPatchParamDescriptors() is 262 parameters:
//   198 concrete "curated" entries (oscillators, mixer, filters, the 3
//   env+lfo units, voice LFO, 14 modulation rows, 4 modifiers, part params,
//   sequencer lengths, arpeggiator, the synth options, the per-part FX slots
//   + chain/master/EQ params, and the 16 FX-mod rows — slot/fxmod families are
//   loop-generated inside buildHelpMap() mirroring the descriptor table's
//   addFx loops so they can never drift) live in the static map returned by
//   hellcatParamHelp(); the remaining 64 step-sequencer entries
//   (seq1_step*, seq2_step*, seqnote_step*, seqnote_vel*) are generated on the
//   fly by getParamHelp() so they can never drift from the descriptor table.
//
// getParamHelp() returns non-empty for all 262 paramIDs; it returns an empty
// string only for genuinely unknown IDs.

#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <unordered_map>

// Curated paramID -> description map (the 198 non-seq-step entries).
// Returned by reference to a function-local static so it is constructed once.
const std::unordered_map<std::string, std::string>& hellcatParamHelp();

// Returns the help text for a paramID, or an empty string if unknown.
// Map hit first; on miss, generates text for the 64 step-sequencer params.
juce::String getParamHelp (const juce::String& paramID);

// True iff getParamHelp() would return non-empty text for paramID.
bool hasParamHelp (const juce::String& paramID);
