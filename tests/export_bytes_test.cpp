// Byte-level export verification for the Ambika file formats.
//
// The other export tests (roundtrip_test, export_fallback_test) verify through
// the parser (parse->write->parse field equality) or through the engine
// (reload + engine-state equality). This test goes to the RAW BYTES:
//
//   [1] Full-file byte equality against real Ambika hardware files: parse a
//       reference .PRO/.MUL, re-write it, and memcmp the ENTIRE file against
//       the reference — every chunk header, size, type prefix, name byte and
//       payload byte must match the firmware's output exactly.
//   [2] Raw chunk-level inspection via a RIFF walker: the writer's chunk
//       sequence, tags, sizes and obj type prefixes on a fresh export.
//   [3] Strategy exports at the byte level: the fallback strategies rewrite
//       ONLY the MultiData allocation bytes (offset i*4+3) and the folded
//       PartData[15] polyphony bytes — every Patch byte, every other
//       PartData byte, and the routing bytes are byte-identical across all
//       strategies and to a no-fallback export.
//   [4] .PRO parameter bytes land at their exact Patch/PartData offsets in
//       the raw file (APVTS -> byte bridge, verified in-file, not via reload).

#include <cstdio>
#include "unified_test_runner.h"
#include <cstring>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MulExport.h"
#include "ParameterLayout.h"
#include "PatchFile.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

void renderIdle (HellcatAudioProcessor& p, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

juce::File goldencard (const juce::String& rel)
{
    return juce::File (HELLCAT_SOURCE_DIR).getChildFile ("ambika_reference/controller/data/goldencard").getChildFile (rel);
}

juce::File tempDir()
{
    const auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("hellcat_export_bytes_test");
    d.createDirectory();
    return d;
}

bool readFile (const juce::File& f, std::vector<uint8_t>& out)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    out.assign (static_cast<const uint8_t*> (mb.getData()),
                static_cast<const uint8_t*> (mb.getData()) + mb.getSize());
    return true;
}

uint32_t rd32 (const uint8_t* p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// ---- Minimal RIFF/MBKS walker ------------------------------------------------
// Layout (storage.cc): "RIFF" LE32(bodySize) "MBKS", then chunks:
//   4cc tag + LE32(size) + payload[size]. "obj " payloads start with a 4-byte
//   type prefix (partEncoding<<8 | type). Sizes are even (no pad bytes here).
struct Chunk
{
    char tag[5] {};
    uint32_t size = 0;
    uint32_t objType = 0;          // valid when tag == "obj "
    const uint8_t* payload = nullptr;   // AFTER the type prefix for obj chunks
};
struct RiffFile
{
    std::vector<uint8_t> bytes;
    std::vector<Chunk> chunks;
    uint32_t riffBody = 0;

    bool parse (const juce::File& f)
    {
        if (! readFile (f, bytes)) return false;
        if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0
            || std::memcmp (bytes.data() + 8, "MBKS", 4) != 0)
            return false;
        riffBody = rd32 (bytes.data() + 4);
        size_t pos = 12;
        while (pos + 8 <= bytes.size())
        {
            Chunk c;
            std::memcpy (c.tag, bytes.data() + pos, 4);
            c.size = rd32 (bytes.data() + pos + 4);
            pos += 8;
            if (pos + c.size > bytes.size()) return false;
            const uint8_t* data = bytes.data() + pos;
            if (std::memcmp (c.tag, "obj ", 4) == 0 && c.size >= 4)
            {
                c.objType = rd32 (data);
                c.payload = data + 4;
            }
            else
            {
                c.payload = data;
            }
            pos += c.size + (c.size & 1);   // RIFF pads odd sizes
            chunks.push_back (c);
        }
        return true;
    }

    const Chunk* findObj (uint32_t objType) const
    {
        for (const auto& c : chunks)
            if (std::memcmp (c.tag, "obj ", 4) == 0 && c.objType == objType)
                return &c;
        return nullptr;
    }
};

// Configure a processor into the over-capacity state (parts 0-2: 2 cards each,
// slots 10/8/6; POLY) used by the strategy byte checks.
void setupProcessor (HellcatAudioProcessor& proc)
{
    proc.prepareToPlay (48000.0, 256);
    renderIdle (proc, 2);
    SynthEngine& e = proc.getEngine();
    e.setPartVoiceAllocation (0, 0b000011);
    e.setPartVoiceAllocation (1, 0b001100);
    e.setPartVoiceAllocation (2, 0b110000);
    e.setPartVoiceSlots (0, 10);
    e.setPartVoiceSlots (1, 8);
    e.setPartVoiceSlots (2, 6);
    renderIdle (proc, 2);
}
}  // namespace

TEST(export_bytes_test)
{
    std::printf ("EXPORT BYTES TEST\n");

    // ---- [1] Raw full-file byte equality vs hardware references ----
    {
        std::printf ("\n[1] raw file equality vs hardware references\n");
        struct Ref { const char* rel; bool isPro; };
        const Ref refs[] = {
            { "PROGRAM/BANK/A/000.PRO", true },   // name "Junon" (5 chars)
            { "PROGRAM/BANK/A/001.PRO", true },   // name "Moof?" (5 chars)
            { "MULTI/BANK/A/000.MUL",   false },  // name "TekDrums" (8 chars)
        };
        for (const auto& r : refs)
        {
            const juce::File ref = goldencard (r.rel);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s: reference exists", r.rel);
            check (ref.existsAsFile(), msg);
            if (! ref.existsAsFile()) continue;

            std::vector<uint8_t> raw, written;
            check (readFile (ref, raw), "reference reads");

            if (r.isPro)
            {
                AmbikaProgram p;
                check (parseAmbikaProgramFile (ref, p), "parses");
                const juce::File out = tempDir().getChildFile ("rawref.PRO");
                check (writeAmbikaProgramFile (out, p), "writes");
                check (readFile (out, written), "written reads");
            }
            else
            {
                AmbikaMulti m;
                check (parseAmbikaMultiFile (ref, m) && m.ok, "parses");
                const juce::File out = tempDir().getChildFile ("rawref.MUL");
                check (writeAmbikaMultiFile (out, m), "writes");
                check (readFile (out, written), "written reads");
            }
            std::snprintf (msg, sizeof (msg), "%s: re-written file is byte-identical (%zu bytes)",
                           r.rel, raw.size());
            check (raw.size() == written.size()
                   && (raw.size() == 0 || std::memcmp (raw.data(), written.data(), raw.size()) == 0),
                   msg);
        }
    }

    // ---- [2] Chunk-level layout of a fresh export (RIFF walker) ----
    {
        std::printf ("\n[2] chunk layout of a fresh .MUL export\n");
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        proc.setLoadedProgramName ("ByteLvl");
        const juce::File f = tempDir().getChildFile ("layout.MUL");
        check (proc.saveMultiFile (f), "exported");

        RiffFile rf;
        check (rf.parse (f), "RIFF walks");
        check (rf.chunks.size() == 14, "1 name + 1 MultiData + 6x2 part objs = 14 chunks");
        // RIFF body size = file size - 8 (header "RIFF" + size field).
        check (rf.riffBody == rf.bytes.size() - 8, "RIFF body size consistent");
        // Name chunk: first, 16 bytes, "ByteLvl" + pad + NUL@15.
        const auto& nameChunk = rf.chunks[0];
        check (std::memcmp (nameChunk.tag, "name", 4) == 0 && nameChunk.size == 16,
               "name chunk: tag + 16 bytes");
        char want[16];
        std::memset (want, ' ', 16);
        std::memcpy (want, "ByteLvl", 7);
        want[15] = '\0';
        check (std::memcmp (nameChunk.payload, want, 16) == 0,
               "name payload: string + space pad + NUL at byte 15 (hardware pattern)");
        // MultiData obj: type 0x04, 56 bytes.
        const Chunk* md = rf.findObj (0x04);
        check (md != nullptr && md->size == 56 + 4, "MultiData obj: type prefix 0x04, 56-byte payload");
        // Per-part objs: patch type (i+1)<<8 | 0x01 (112 B), part type (i+1)<<8 | 0x05 (84 B),
        // interleaved patch-then-part in part order.
        bool objsOk = true, orderOk = true;
        size_t mdIdx = 0;
        for (size_t i = 0; i < rf.chunks.size(); ++i)
            if (&rf.chunks[i] == md) { mdIdx = i; break; }
        for (int p = 0; p < 6; ++p)
        {
            const uint32_t patchType = (uint32_t) ((p + 1) << 8) | 0x01;
            const uint32_t partType  = (uint32_t) ((p + 1) << 8) | 0x05;
            const Chunk* pc = rf.findObj (patchType);
            const Chunk* pt = rf.findObj (partType);
            if (pc == nullptr || pc->size != 112 + 4 || pt == nullptr || pt->size != 84 + 4)
                objsOk = false;
        }
        check (objsOk, "all 12 per-part objs: correct type prefixes + payload sizes");
        for (size_t i = mdIdx + 1; i < rf.chunks.size(); i += 2)
        {
            const uint32_t expectPatch = (uint32_t) ((((i - mdIdx - 1) / 2) + 1) << 8) | 0x01;
            const uint32_t expectPart  = (uint32_t) ((((i - mdIdx - 1) / 2) + 1) << 8) | 0x05;
            if (rf.chunks[i].objType != expectPatch || rf.chunks[i + 1].objType != expectPart)
                orderOk = false;
        }
        check (orderOk, "objs interleave patch-then-part in part order after MultiData");
    }

    // ---- [3] Strategy rewrites at the byte level ----
    {
        std::printf ("\n[3] strategy byte-level rewrites\n");
        using namespace hellcat::mul_export;

        // Baseline: a no-fallback export (AUTO slots) for the untouched-bytes ref.
        HellcatAudioProcessor plain;
        plain.prepareToPlay (48000.0, 256);
        renderIdle (plain, 2);
        plain.getEngine().setPartVoiceAllocation (0, 0b000011);
        plain.getEngine().setPartVoiceAllocation (1, 0b001100);
        plain.getEngine().setPartVoiceAllocation (2, 0b110000);
        renderIdle (plain, 2);
        const juce::File plainF = tempDir().getChildFile ("plain.MUL");
        check (plain.saveMultiFile (plainF), "no-fallback export");
        RiffFile plainRf;
        check (plainRf.parse (plainF), "walks");

        const struct { int strat; const char* name; } cases[] = {
            { 0, "AsIs" }, { 1, "Proportional" }, { 2, "Priority" },
            { 3, "EvenSplit" }, { 4, "MonoFold" }, { 5, "ChainSplit" },
        };
        for (const auto& c : cases)
        {
            HellcatAudioProcessor proc;
            setupProcessor (proc);
            const juce::File f = tempDir().getChildFile (juce::String (c.name) + ".MUL");
            check (proc.saveMultiFile (f, c.strat), "strategy export saves");

            RiffFile rf;
            check (rf.parse (f), "walks");
            const auto setup = proc.getMulExportSetup();
            const auto expect = (c.strat == 5)
                ? solveChain (setup).front()
                : solve (setup, static_cast<Strategy> (c.strat));

            // (a) MultiData allocation bytes at raw offset i*4+3.
            const Chunk* md = rf.findObj (0x04);
            bool masksOk = md != nullptr;
            for (int p = 0; p < 6 && masksOk; ++p)
                masksOk = md->payload[(size_t) (p * 4 + 3)] == expect.masks[(size_t) p];
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s: MultiData[i*4+3] masks byte-exact", c.name);
            check (masksOk, msg);

            // (b) PartData[15] polyphony bytes.
            bool modesOk = true;
            for (int p = 0; p < 6 && modesOk; ++p)
            {
                const Chunk* pt = rf.findObj ((uint32_t) ((p + 1) << 8) | 0x05);
                if (pt == nullptr) { modesOk = false; break; }
                if (pt->payload[15] != expect.polyMode[(size_t) p]) modesOk = false;
            }
            std::snprintf (msg, sizeof (msg), "%s: PartData[15] modes byte-exact", c.name);
            check (modesOk, msg);

            // (c) UNTOUCHED bytes: patches + all other PartData bytes + routing.
            bool untouched = true;
            for (int p = 0; p < 6 && untouched; ++p)
            {
                const uint32_t patchType = (uint32_t) ((p + 1) << 8) | 0x01;
                const uint32_t partType  = (uint32_t) ((p + 1) << 8) | 0x05;
                const Chunk* pc = rf.findObj (patchType);
                const Chunk* pt = rf.findObj (partType);
                const Chunk* refPc = plainRf.findObj (patchType);
                const Chunk* refPt = plainRf.findObj (partType);
                if (pc == nullptr || pt == nullptr || refPc == nullptr || refPt == nullptr)
                { untouched = false; break; }
                if (std::memcmp (pc->payload, refPc->payload, 112) != 0) untouched = false;
                // PartData: everything except byte 15 must be identical.
                if (pt->payload[15] != refPt->payload[15]
                    && std::memcmp (pt->payload, refPt->payload, 84) != 0)
                {
                    // mode differs (expected under MonoFold/ChainSplit) — the
                    // other 83 bytes must still match.
                    for (int o = 0; o < 84; ++o)
                        if (o != 15 && pt->payload[o] != refPt->payload[o]) { untouched = false; break; }
                }
                // Routing (channel/lo/hi): MultiData i*4+0..2.
                const Chunk* mdC = md;
                if (mdC->payload[(size_t) (p * 4)]     != plainRf.findObj (0x04)->payload[(size_t) (p * 4)]
                 || mdC->payload[(size_t) (p * 4 + 1)] != plainRf.findObj (0x04)->payload[(size_t) (p * 4 + 1)]
                 || mdC->payload[(size_t) (p * 4 + 2)] != plainRf.findObj (0x04)->payload[(size_t) (p * 4 + 2)])
                    untouched = false;
            }
            std::snprintf (msg, sizeof (msg), "%s: patches/routing/other bytes untouched", c.name);
            check (untouched, msg);
        }

        // (d) ChainSplit sibling units carry CONTINUATION masks at the byte level.
        {
            HellcatAudioProcessor proc;
            setupProcessor (proc);
            const juce::File f = tempDir().getChildFile ("chainb.MUL");
            check (proc.saveMultiFile (f, 5), "chain export saves");
            const auto setup = proc.getMulExportSetup();
            const auto units = solveChain (setup);
            for (size_t u = 0; u < units.size(); ++u)
            {
                const juce::File uf = u == 0 ? f
                    : f.getParentDirectory().getChildFile ("chainb-" + juce::String (u + 1) + ".MUL");
                RiffFile rf;
                char msg[128];
                std::snprintf (msg, sizeof (msg), "unit %zu parses", u);
                check (rf.parse (uf), msg);
                const Chunk* md = rf.findObj (0x04);
                bool ok = md != nullptr;
                for (int p = 0; p < 6 && ok; ++p)
                    ok = md->payload[(size_t) (p * 4 + 3)] == units[u].masks[(size_t) p];
                std::snprintf (msg, sizeof (msg), "unit %zu: raw MultiData masks byte-exact", u);
                check (ok, msg);
            }
        }
    }

    // ---- [4] .PRO parameter bytes at exact raw offsets ----
    {
        std::printf ("\n[4] .PRO parameter bytes at raw offsets\n");
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        // Distinct known values through the REAL APVTS bridge.
        proc.getApvts().getParameterAsValue ("osc1_shape")  = 0.30f;   // e.g. shape 7/22
        proc.getApvts().getParameterAsValue ("part_volume") = 0.90f;
        renderIdle (proc, 2);
        const juce::File f = tempDir().getChildFile ("offsets.PRO");
        check (proc.saveProgramFile (f), "saves");

        RiffFile rf;
        check (rf.parse (f), "walks");
        const Chunk* patch = rf.findObj (0x01);
        const Chunk* part  = rf.findObj (0x05);
        check (patch != nullptr && part != nullptr, "Patch + PartData objs found by type prefix");

        // Cross-check every non-arp/non-option descriptor against its byte.
        int checked = 0;
        bool allOk = true;
        for (const auto& d : getPatchParamDescriptors())
        {
            if (d.isArp || d.isOption || d.isFx || d.byteOffset < 0) continue;
            const float raw = proc.getApvts().getRawParameterValue (d.paramID)->load();
            const uint8_t byte = d.isSequencer
                ? static_cast<uint8_t> (juce::jlimit (d.minValue, d.maxValue, (int) raw))
                : hellcatValueToPatchByte (d, raw);
            const uint8_t inFile = (d.isPart || d.isSequencer)
                ? part->payload[(size_t) d.byteOffset]
                : patch->payload[(size_t) d.byteOffset];
            ++checked;
            if (inFile != byte) { allOk = false; }
        }
        char msg[96];
        std::snprintf (msg, sizeof (msg), "every mapped descriptor lands at its raw offset (%d checked)", checked);
        check (allOk && checked > 100, msg);
    }

    tempDir().deleteRecursively();
    std::printf ("\nEXPORT BYTES TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}
