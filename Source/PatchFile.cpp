// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See PatchFile.h.

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
    return std::min<size_t> (size, static_cast<size_t> (8) + bodySize);
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

        if (static_cast<size_t> (off) + 8u + csz > size)
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
    std::vector<uint8_t> b;
    b.reserve (256);

    const auto push4 = [&b] (const char* tag) { b.insert (b.end(), tag, tag + 4); };
    const auto le32  = [&b] (uint32_t x) {
        b.push_back (static_cast<uint8_t> (x));
        b.push_back (static_cast<uint8_t> (x >> 8));
        b.push_back (static_cast<uint8_t> (x >> 16));
        b.push_back (static_cast<uint8_t> (x >> 24));
    };

    // RIFF / MBKS header.
    push4 ("RIFF");
    le32 (248u);
    push4 ("MBKS");

    // NAME chunk: 16-byte ASCII name, space-padded (matches trimName on parse).
    push4 ("name");
    le32 (16u);
    {
        char name16[16];
        std::memset (name16, ' ', 16);
        const char* const raw = prog.name.toRawUTF8();
        const size_t len = std::min<size_t> (16, prog.name.getNumBytesAsUTF8());
        if (len > 0)
            std::memcpy (name16, raw, len);
        b.insert (b.end(), name16, name16 + 16);
    }

    // Patch object (type prefix 0x00000001).
    push4 ("obj ");
    le32 (112u + 4u);
    le32 (0x00000001u);
    b.insert (b.end(), prog.patch.begin(), prog.patch.end());

    // PartData object (type prefix 0x00000005).
    push4 ("obj ");
    le32 (84u + 4u);
    le32 (0x00000005u);
    b.insert (b.end(), prog.part.begin(), prog.part.end());

    // Atomic write via a temp file (safe even if the target is open elsewhere).
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.write (b.data(), b.size()))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
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
            const uint32_t objType  = typePrefix & 0xFFu;

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
            (void) objType;
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
    std::vector<uint8_t> b;
    b.reserve (1424);

    const auto push4 = [&b] (const char* tag) { b.insert (b.end(), tag, tag + 4); };
    const auto le32  = [&b] (uint32_t x) {
        b.push_back (static_cast<uint8_t> (x));
        b.push_back (static_cast<uint8_t> (x >> 8));
        b.push_back (static_cast<uint8_t> (x >> 16));
        b.push_back (static_cast<uint8_t> (x >> 24));
    };

    // Compute the RIFF body size up front (everything after the 8-byte
    // "RIFF"+size header): the "MBKS" form type + every chunk (8-byte header +
    // payload).
    const uint32_t bodySize = 4u     // "MBKS" form type
                           + (8u + 16u)                  // name chunk
                           + (8u + 4u + 56u)             // MultiData obj
                           + 6u * (8u + 4u + 112u)       // 6 x Patch obj
                           + 6u * (8u + 4u + 84u);       // 6 x PartData obj

    // RIFF / MBKS header.
    push4 ("RIFF");
    le32 (bodySize);
    push4 ("MBKS");

    // NAME chunk: 16-byte ASCII name, space-padded (matches trimName on parse).
    push4 ("name");
    le32 (16u);
    {
        char name16[16];
        std::memset (name16, ' ', 16);
        const char* const raw = multi.name.toRawUTF8();
        const size_t len = std::min<size_t> (16, multi.name.getNumBytesAsUTF8());
        if (len > 0)
            std::memcpy (name16, raw, len);
        b.insert (b.end(), name16, name16 + 16);
    }

    // MultiData object (type prefix 0x00000004, part 0). Always written: a
    // zeroed MultiData keeps the file well-formed (loadMultiFile needs it) and a
    // reference .MUL always carries it, so the round-trip is exact.
    push4 ("obj ");
    le32 (56u + 4u);
    le32 (0x00000004u);
    b.insert (b.end(), multi.multiData.begin(), multi.multiData.end());

    // Six Parts (1-based index in the type prefix), each an interleaved Patch +
    // PartData object.
    for (uint32_t i = 1; i <= 6u; ++i)  // NOLINT(bugprone-infinite-loop): ++i terminates it; clang-tidy FP on the uint counter
    {
        const auto& p = multi.parts[i - 1u];

        push4 ("obj ");
        le32 (112u + 4u);
        le32 ((i << 8) | 0x00000001u);
        b.insert (b.end(), p.patch.begin(), p.patch.end());

        push4 ("obj ");
        le32 (84u + 4u);
        le32 ((i << 8) | 0x00000005u);
        b.insert (b.end(), p.part.begin(), p.part.end());
    }

    // Atomic write via a temp file (safe even if the target is open elsewhere).
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.write (b.data(), b.size()))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}
