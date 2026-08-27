// Copyright (c) 2026 805Labs Kft. / Hellcat.  See PatchFile.h.

#include "PatchFile.h"

#include <cstring>

namespace
{
uint32_t readLE32 (const uint8_t* p)
{
    return static_cast<uint32_t> (p[0])
         | (static_cast<uint32_t> (p[1]) << 8)
         | (static_cast<uint32_t> (p[2]) << 16)
         | (static_cast<uint32_t> (p[3]) << 24);
}

// Validate a RIFF "MBKS" container and return the body end offset (8 + body
// size, clamped to `size`); returns 0 if it is not an MBKS container.
size_t mbksBodyEnd (const void* data, size_t size)
{
    if (size < 12)
        return 0;
    const auto* b = static_cast<const uint8_t*> (data);
    if (std::memcmp (b, "RIFF", 4) != 0 || std::memcmp (b + 8, "MBKS", 4) != 0)
        return 0;
    const uint32_t bodySize = readLE32 (b + 4);
    // Wrap-proof on any size_t width (bug hunt 2026-08-18, F-state-3): on a
    // 32-bit size_t, `8 + bodySize` can wrap for a hostile bodySize near
    // UINT32_MAX. `size >= 12` here, so `size - 8` cannot underflow, and
    // `bodySize > size - 8` <=> `8 + bodySize > size`; only in the FALSE
    // branch do we compute the sum — where it is <= size, hence wrap-free.
    if (bodySize > size - 8)
        return size;
    return static_cast<size_t> (8) + bodySize;
}

// Shared RIFF "MBKS" chunk walker. Invokes `nameCb(bodyPtr, chunkSize)` for each
// `name` chunk and `objCb(typePrefix, payloadPtr, payloadLen)` for each `obj`
// chunk (the 4-byte type prefix is stripped). `end` is the value from
// mbksBodyEnd. Templated so callers pass lambdas without allocation.
template <typename NameCb, typename ObjCb>
void walkMbks (const uint8_t* b, size_t size, size_t end, NameCb nameCb, ObjCb objCb)
{
    size_t off = 12;  // skip "RIFF" + size + "MBKS"
    while (off + 8 <= end)
    {
        const char* tag = reinterpret_cast<const char*> (b + off);
        const uint32_t csz = readLE32 (b + off + 4);
        const uint8_t* bodyPtr = b + off + 8;

        // Wrap-proof on any size_t width (bug hunt 2026-08-18, F-state-3):
        // `off + 8 + csz` wraps on 32-bit size_t for a hostile csz near
        // UINT32_MAX, passing the guard and reading OOB. Subtract instead.
        if (csz > size || size - csz < off + 8)
            break;  // truncated

        if (std::memcmp (tag, "name", 4) == 0 && csz > 0)
        {
            nameCb (bodyPtr, csz);
        }
        else if (tag[0] == 'o' && tag[1] == 'b' && tag[2] == 'j' && csz >= 4)
        {
            const uint32_t typePrefix = readLE32 (bodyPtr);
            objCb (typePrefix, bodyPtr + 4, csz - 4);
        }

        off += static_cast<size_t> (8) + csz + (csz & 1u);  // RIFF word-alignment
    }
}

// Ambika pads the 16-byte ASCII name with spaces; trim trailing space/null.
juce::String trimName (const uint8_t* p, uint32_t csz)
{
    size_t n = csz;
    while (n > 0 && (p[n - 1] == 0 || p[n - 1] == ' '))
        --n;
    return juce::String::createStringFromData (p, static_cast<int> (n));
}
}  // namespace

//==============================================================================
bool parseAmbikaProgram (const void* data, size_t size, AmbikaProgram& out)
{
    out = AmbikaProgram{};

    const size_t end = mbksBodyEnd (data, size);
    if (end == 0)
        return false;

    const auto* b = static_cast<const uint8_t*> (data);
    walkMbks (b, size, end,
        [&out] (const uint8_t* p, uint32_t csz) { out.name = trimName (p, csz); },
        [&out] (uint32_t /*typePrefix*/, const uint8_t* payload, uint32_t payloadLen)
        {
            if (payloadLen == 112)       // sizeof(Patch)
            {
                std::memcpy (out.patch.data(), payload, 112);
                out.hasPatch = true;
            }
            else if (payloadLen == 84)   // sizeof(PartData)
            {
                std::memcpy (out.part.data(), payload, 84);
                out.hasPart = true;
            }
        });

    return out.hasPatch;
}

bool parseAmbikaProgramFile (const juce::File& file, AmbikaProgram& out)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb))
        return false;
    return parseAmbikaProgram (mb.getData(), mb.getSize(), out);
}

//==============================================================================
namespace
{
// Copy @p name into the 16-byte, space-padded, NUL@15 name chunk WITHOUT ever
// splitting a UTF-8 code point: names longer than 16 bytes (e.g. "Ünïcödé...")
// are truncated at a code-point boundary so the written file always contains
// valid UTF-8 (a raw 16-byte memcpy can cut a multi-byte sequence in half).
// Control characters (newlines etc.) are dropped — the hardware LCD shows
// printable text and a stray newline byte has no use.
void writeName16 (std::vector<uint8_t>& b, const juce::String& name)
{
    char name16[16];
    std::memset (name16, ' ', 16);
    const char* const raw = name.toRawUTF8();
    const size_t bytes = name.getNumBytesAsUTF8();

    // Length of the code point starting at raw[i] (1..4; an invalid lead byte
    // counts as 1 so a malformed sequence never splits a real one after it).
    const auto cpLen = [] (uint8_t c) -> size_t
    {
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    };

    size_t out = 0;
    for (size_t i = 0; i < bytes; )
    {
        const size_t cp = cpLen (static_cast<uint8_t> (raw[i]));
        const bool printable = static_cast<uint8_t> (raw[i]) >= 0x20;
        if (printable)
        {
            if (out + cp > 16) break;   // would split the code point: stop before it
            std::memcpy (name16 + out, raw + i, cp);
            out += cp;
        }
        i += cp;   // control bytes are skipped, not copied
    }

    // Hardware-exact terminator: the firmware writes its 16-byte name buffer
    // verbatim, and every reference file shows string + space pad + NUL at the
    // LAST byte (name[15]) — verified across 000.PRO "Junon", 001.PRO "Moof?"
    // and 000.MUL "TekDrums". A full 16-byte name leaves no room for the NUL
    // (matches C semantics on hardware).
    if (out < 16)
        name16[15] = '\0';
    b.insert (b.end(), name16, name16 + 16);
}

// One MBKS "obj " chunk: a 4-byte type prefix plus a fixed payload.
struct MbksObjectChunk
{
    uint32_t typePrefix;
    const uint8_t* data;
    size_t size;
};

// Serialize and write one MBKS (RIFF) file: the header, the name chunk, then
// the object chunks in order. The body size is computed from the chunk list;
// for the program layout below it evaluates to the same 248 the firmware
// writes. The write is atomic via a temp file (safe even if the target file is
// open elsewhere).
bool writeMbksFile (const juce::File& file, const juce::String& name,
                    const std::vector<MbksObjectChunk>& objects)
{
    size_t total = 12u + (8u + 16u);   // RIFF+MBKS header + name chunk
    for (const auto& o : objects)
        total += 8u + 4u + o.size;
    std::vector<uint8_t> b;
    b.reserve (total);

    const auto push4 = [&b] (const char* tag) { b.insert (b.end(), tag, tag + 4); };
    const auto le32  = [&b] (uint32_t x) {
        b.push_back (static_cast<uint8_t> (x));
        b.push_back (static_cast<uint8_t> (x >> 8));
        b.push_back (static_cast<uint8_t> (x >> 16));
        b.push_back (static_cast<uint8_t> (x >> 24));
    };

    // RIFF / MBKS header. The body size covers everything after the 8-byte
    // "RIFF"+size header: the "MBKS" form type plus every chunk.
    push4 ("RIFF");
    le32 (static_cast<uint32_t> (total - 8u));
    push4 ("MBKS");

    // NAME chunk: 16-byte ASCII name, space-padded (matches trimName on parse).
    push4 ("name");
    le32 (16u);
    writeName16 (b, name);

    for (const auto& o : objects)
    {
        push4 ("obj ");
        le32 (static_cast<uint32_t> (o.size + 4u));
        le32 (o.typePrefix);
        b.insert (b.end(), o.data, o.data + o.size);
    }

    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.write (b.data(), b.size()))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}
}  // namespace

bool writeAmbikaProgramFile (const juce::File& file, const AmbikaProgram& prog)
{
    // Layout — byte-for-byte verified against a real Ambika factory .PRO, and a
    // faithful transcription of the firmware RIFF writer (storage.cc Save() +
    // RIFFWriteObject()):
    //
    //   "RIFF" + LE32(248) + "MBKS"                 RIFF/MBKS header  (12 B)
    //   "name" + LE32(16) + name[16]                name, space-padded (24 B)
    //   "obj " + LE32(116) + LE32(0x00000001) + patch[112]   Patch obj  (124 B)
    //   "obj " + LE32(88)  + LE32(0x00000005) + part[84]      Part obj   (96 B)
    //
    //   file = 256 B; RIFF body size = 248 (= 256 - 8). The 4-byte type prefix is
    //   { object_type + 1, alias, 0, 0 }: Patch = STORAGE_OBJECT_PATCH(0)+1 = 1,
    //   PartData = STORAGE_OBJECT_PART(4)+1 = 5 (alias 0 for a single program).
    //   parseAmbikaProgram identifies Patch/Part purely by payload length, so this
    //   round-trips exactly. All chunk sizes here are even, so no RIFF padding.
    return writeMbksFile (file, prog.name,
        { { 0x00000001u, prog.patch.data(), prog.patch.size() },
          { 0x00000005u, prog.part.data(),  prog.part.size() } });
}

//==============================================================================
bool parseAmbikaMulti (const void* data, size_t size, AmbikaMulti& out)
{
    out = AmbikaMulti{};

    const size_t end = mbksBodyEnd (data, size);
    if (end == 0)
        return false;

    const auto* b = static_cast<const uint8_t*> (data);
    walkMbks (b, size, end,
        [&out] (const uint8_t* p, uint32_t csz) { out.name = trimName (p, csz); },
        [&out] (uint32_t typePrefix, const uint8_t* payload, uint32_t payloadLen)
        {
            // type prefix = (partIndex_1based << 8) | objectType.
            // 0x01 = Patch, 0x05 = PartData, 0x04 = MultiData (part 0).
            const uint32_t partIdx1 = (typePrefix >> 8) & 0xFFu;

            if (partIdx1 == 0 && payloadLen == 56)   // MultiData
            {
                std::memcpy (out.multiData.data(), payload, 56);
                out.hasMultiData = true;
            }
            else if (payloadLen == 112)              // Patch (part partIdx1)
            {
                const int idx = static_cast<int> (partIdx1) - 1;
                if (idx >= 0 && idx < 6)
                {
                    std::memcpy (out.parts[(size_t) idx].patch.data(), payload, 112);
                    out.parts[(size_t) idx].hasPatch = true;
                }
            }
            else if (payloadLen == 84)               // PartData (part partIdx1)
            {
                const int idx = static_cast<int> (partIdx1) - 1;
                if (idx >= 0 && idx < 6)
                {
                    std::memcpy (out.parts[(size_t) idx].part.data(), payload, 84);
                    out.parts[(size_t) idx].hasPart = true;
                }
            }
        });

    out.ok = out.hasMultiData;
    return out.ok;
}

bool parseAmbikaMultiFile (const juce::File& file, AmbikaMulti& out)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb))
        return false;
    return parseAmbikaMulti (mb.getData(), mb.getSize(), out);
}

//==============================================================================
bool writeAmbikaMultiFile (const juce::File& file, const AmbikaMulti& multi)
{
    // Layout — byte-exact inverse of parseAmbikaMulti, and a faithful
    // transcription of the firmware .MUL writer (storage.cc Save() for
    // STORAGE_OBJECT_MULTI). Verified: a written file re-parses to the identical
    // multi, and the byte size matches a real factory .MUL (1416-byte RIFF body
    // / 1424-byte file).
    //
    //   "RIFF" + LE32(bodySize) + "MBKS"          RIFF/MBKS header   (12 B)
    //   "name" + LE32(16) + name[16]              name, space-padded (24 B)
    //   "obj " + LE32(60)  + LE32(0x00000004) + multiData[56]   MultiData   (68 B)
    //   for part i = 1..6:
    //     "obj " + LE32(116) + LE32((i<<8)|0x01) + patch[112]    Patch   (124 B)
    //     "obj " + LE32(88)  + LE32((i<<8)|0x05) + part[84]      PartData (96 B)
    //
    //   bodySize = 4 (MBKS) + 24 + 68 + 6*(124+96) = 1416.
    //   The 4-byte type prefix encodes (partIndex_1based << 8) | objectType:
    //   MultiData = part 0, type 0x04; part-i Patch = (i<<8)|0x01; part-i
    //   PartData = (i<<8)|0x05. parseAmbikaMulti routes by partIdx1 (high byte)
    //   and identifies Patch/Part by payload length, so this round-trips exactly.
    //   All chunk sizes are even, so no RIFF word-padding is needed.
    std::vector<MbksObjectChunk> objects;
    objects.reserve (1u + 2u * 6u);

    // MultiData object (type prefix 0x00000004, part 0). Always written: a
    // zeroed MultiData keeps the file well-formed (loadMultiFile needs it) and a
    // reference .MUL always carries it, so the round-trip is exact.
    objects.push_back ({ 0x00000004u, multi.multiData.data(), multi.multiData.size() });

    // Six Parts (1-based index in the type prefix), each an interleaved Patch +
    // PartData object.
    for (uint32_t i = 1; i <= 6u; ++i)  // NOLINT(bugprone-infinite-loop): ++i ends it; clang-tidy FP on the uint counter
    {
        const auto& p = multi.parts[i - 1u];
        objects.push_back ({ (i << 8) | 0x00000001u, p.patch.data(), p.patch.size() });
        objects.push_back ({ (i << 8) | 0x00000005u, p.part.data(),  p.part.size() });
    }
    return writeMbksFile (file, multi.name, objects);
}
