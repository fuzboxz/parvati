// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Ambika .PRO patch-file support. Ambika stores programs as RIFF "MBKS"
// containers (see ambika_reference/controller/storage.cc): a `name` chunk +
// `obj` chunks, each carrying a 4-byte type prefix followed by a raw struct
// (Patch = 112 bytes, PartData = 84 bytes). This parses them into plain byte
// arrays that the ParvatiAudioProcessor can load straight into the APVTS bridge.

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>

// One parsed Ambika program (patch + part). The patch bytes map 1:1 onto the
// dsp::Patch struct; the part bytes onto PartData.
struct AmbikaProgram
{
    juce::String name;
    std::array<uint8_t, 112> patch {};
    std::array<uint8_t, 84>  part  {};
    bool hasPatch = false;
    bool hasPart  = false;
};

// Parse a .PRO blob (RIFF "MBKS") in memory. Returns true if a Patch was found.
bool parseAmbikaProgram (const void* data, size_t size, AmbikaProgram& out);

// Convenience: parse a .PRO file from disk.
bool parseAmbikaProgramFile (const juce::File& file, AmbikaProgram& out);

// Write an Ambika .PRO program file — the byte-exact inverse of
// parseAmbikaProgramFile (RIFF "MBKS" header + a 16-byte "name" chunk + an
// "obj" Patch chunk (type prefix 0x00000001) + an "obj" PartData chunk (type
// prefix 0x00000005)). A file written here re-loads to the identical program.
bool writeAmbikaProgramFile (const juce::File& file, const AmbikaProgram& prog);

//-----------------------------------------------------------------------------
// Ambika multi (.MUL). A multi (RIFF "MBKS", see ambika_reference/controller/
// multi.h + storage.cc STORAGE_OBJECT_MULTI) holds up to 6 Parts' patches +
// PartData, plus one MultiData (56 bytes). MultiData's first 24 bytes are
// part_mapping_[6], each 4 bytes: { midi_channel, keyrange_low, keyrange_high,
// voice_allocation }.
//
// .MUL layout (verified from a real file): a `name` chunk, then an `obj` with
// type prefix 0x04 (MultiData, 56B), then for parts 1..6 interleaved: an `obj`
// type 0x0i01 (Patch, 112B) and an `obj` type 0x0i05 (PartData, 84B). The 4-byte
// type prefix encodes (partIndex_1based << 8) | objectType.
struct AmbikaMultiPart
{
    std::array<uint8_t, 112> patch {};
    std::array<uint8_t, 84>  part  {};
    bool hasPatch = false;
    bool hasPart  = false;
};

struct AmbikaMulti
{
    juce::String name;
    std::array<AmbikaMultiPart, 6> parts {};     // one per hardware Part (kNumParts=6)
    std::array<uint8_t, 56> multiData {};        // sizeof(MultiData)
    bool hasMultiData = false;
    bool ok = false;
};

// Parse a .MUL blob in memory. Returns true if a MultiData chunk was found.
bool parseAmbikaMulti (const void* data, size_t size, AmbikaMulti& out);

// Convenience: parse a .MUL file from disk.
bool parseAmbikaMultiFile (const juce::File& file, AmbikaMulti& out);

// Write an Ambika .MUL multi file — the byte-exact inverse of parseAmbikaMulti
// (RIFF "MBKS" header + 16-byte name chunk + a MultiData obj (type prefix
// 0x04, 56 B) +, for parts 1..6, an interleaved Patch obj (type prefix
// (i<<8)|0x01, 112 B) and a PartData obj (type prefix (i<<8)|0x05, 84 B)).
// The MultiData chunk is ALWAYS written (zeroed if !multi.hasMultiData) so the
// file is well-formed and loadMultiFile() accepts it; a reference .MUL always
// carries MultiData, so parse->write->parse preserves hasMultiData exactly.
bool writeAmbikaMultiFile (const juce::File& file, const AmbikaMulti& multi);
