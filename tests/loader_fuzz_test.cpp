// Deterministic tooling T1 — LOADER FUZZER + ROLLBACK CHECKER.
//
// Property under test (the "validate-before-mutate" class this exists to pin):
//   (P1) ANY load call that returns FALSE must leave the processor state
//        BIT-IDENTICAL (getStateInformation snapshot compared byte-for-byte).
//        Historical instances of the class: loadParvatiMultiFile ran
//        resetAllVoices + resetVoiceSlotsToInit before its pre-parse guard
//        existed; restoreState applied a truncated blob part-way; failed
//        .parvati patch loads killed sounding voices.
//   (P2) ANY load call that returns TRUE must render 32 processBlocks inside a
//        10 s watchdog with FINITE output. Historical instance: a .MUL with
//        arpOctave byte 0 hung the audio thread in Arpeggiator::stepArpeggio's
//        Random loop (now clamped at staging — this harness re-proves it from
//        the raw-byte side, i.e. it also fails if the clamp ever regresses).
//   (P3) A truncated engine-state blob (setStateInformation) must neither crash
//        nor wedge the processor: the restore itself completes, the renderer
//        stays finite, and a SUBSEQUENT full restore brings the processor back
//        to the exact full-state snapshot (two-phase restoreState contract:
//        clean apply or untouched — never a corrupt hybrid).
//
// Corpus: five files built through the REAL save paths (saveProgramFile .PRO,
// saveMultiFile .MUL x2, saveParvatiMultiFile .parvati-multi with custom
// tunings + part names + FX slot types, saveParvatiPatchFile .parvati-patch),
// so every mutational case exercises the exact bytes a user's disk file has.
//
// Mutations (~300 cases, all deterministic — fixed seeds, no wall clock):
//   - truncation at 16 evenly-spaced fractions + {1 byte, size-1, full} per
//     corpus file;
//   - single-bit flips confined to the semantically interesting byte regions
//     of the binary formats (.PRO patch/part bodies; .MUL MultiData routing
//     bytes + each part's PartData spread/raga/arp/poly bytes);
//   - structural YAML edits on the text formats (drop a part, empty parts
//     array, scalar entries, removed routing/name/params keys, bogus tail,
//     out-of-range scalars, broken quotes).
//
// Rollback exemptions on false==identical: NONE. A violating input is a REAL
// bug — do not weaken the assertion; report the case label + first-diff offset.
//
// Built by default. Run with: ./build_release/parvati_loader_fuzz_test

#include <atomic>
#include "unified_test_runner.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;
int g_cases = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

//==============================================================================
// Deterministic RNG (xorshift32) — fixed seed, no <random> implementation
// variance across stdlibs.
struct XorShift32
{
    uint32_t s;
    explicit XorShift32 (uint32_t seed) : s (seed ? seed : 1u) {}
    uint32_t next()
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    int below (int n) { return static_cast<int> (next() % static_cast<uint32_t> (n)); }
};

//==============================================================================
// State snapshot: the FULL host-visible processor state (APVTS tree + the
// 6-part engine blob). Deterministic for identical state — proven by the
// canary below before any mutation runs (if two snapshots of the same state
// ever differ, the harness itself would be invalid and must abort).
std::vector<uint8_t> snapshot (ParvatiAudioProcessor& proc)
{
    juce::MemoryBlock mb;
    proc.getStateInformation (mb);
    return { static_cast<const uint8_t*> (mb.getData()),
             static_cast<const uint8_t*> (mb.getData()) + mb.getSize() };
}

// Byte-for-byte compare; on mismatch prints sizes + the first differing
// offset (actionable failure output).
bool bytesIdentical (const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size())
    {
        std::printf ("      [diff] size %zu vs %zu\n", a.size(), b.size());
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
        {
            std::printf ("      [diff] first differing byte at offset %zu (0x%02x vs 0x%02x)\n",
                         i, a[i], b[i]);
            return false;
        }
    return true;
}

//==============================================================================
// Render watchdog: 32 processBlocks on a worker thread inside a 10 s budget.
// A load whose bytes wedge the render loop (the historical arpOctave-0 Random
// loop) is caught as a timeout. The finite flag additionally catches corrupt
// staged bytes exploding into inf/NaN. On a REAL hang the worker cannot be
// reclaimed, so the harness reports and hard-exits (the spinning thread would
// otherwise block process exit and hide the failure in CI).
struct WatchResult { bool completed; bool finite; };

WatchResult watchdog (ParvatiAudioProcessor& proc, int blocks = 32)
{
    std::atomic<bool> finite { true };
    auto fut = std::async (std::launch::async, [&proc, blocks, &finite]() {
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer empty;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            proc.processBlock (buf, empty);
            if (! finite.load())
                break;
            for (int ch = 0; ch < 2 && finite.load(); ++ch)
                for (int i = 0; i < 256; ++i)
                    if (! std::isfinite (buf.getSample (ch, i))) { finite.store (false); break; }
        }
    });
    if (fut.wait_for (std::chrono::seconds (10)) != std::future_status::ready)
    {
        std::printf ("      [HANG] processBlock did not finish 32 blocks within 10 s\n");
        std::fflush (stdout);
        std::_Exit (97);   // cannot join a wedged audio thread
    }
    fut.get();
    return { true, finite.load() };
}

//==============================================================================
// File helpers.
juce::File tempFile (const char* name)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
}

std::vector<uint8_t> readFileBytes (const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    return { static_cast<const uint8_t*> (mb.getData()),
             static_cast<const uint8_t*> (mb.getData()) + mb.getSize() };
}

void writeFileBytes (const juce::File& f, const std::vector<uint8_t>& b)
{
    f.replaceWithData (b.data(), b.size());
}

juce::String readFileText (const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    return juce::String::fromUTF8 (static_cast<const char*> (mb.getData()),
                                   static_cast<int> (mb.getSize()));
}

//==============================================================================
// Loader dispatch: which real load entry point a corpus uses.
// 0 = loadProgramFile, 1 = loadMultiFile, 2 = loadParvatiMultiFile,
// 3 = loadParvatiPatchFile.
const char* loaderName (int l)
{
    switch (l)
    {
        case 0: return "loadProgramFile";
        case 1: return "loadMultiFile";
        case 2: return "loadParvatiMultiFile";
        default: return "loadParvatiPatchFile";
    }
}

bool dispatchLoad (ParvatiAudioProcessor& proc, int loader, const juce::File& f)
{
    switch (loader)
    {
        case 0:  return proc.loadProgramFile (f);
        case 1:  return proc.loadMultiFile (f);
        case 2:  return proc.loadParvatiMultiFile (f);
        default: return proc.loadParvatiPatchFile (f);
    }
}

//==============================================================================
// Re-seed the TEST processor to a distinctive, deterministic state before every
// case (a non-default state is what makes a partial mutation DETECTABLE: a
// failed load that resets slots to init / clears a custom table / drops a part
// name would otherwise produce a byte-identical false pass). Different from
// every corpus state so successful loads also move the state.
void reseed (ParvatiAudioProcessor& proc)
{
    auto& e = proc.getEngine();
    e.setCurrentPart (3);
    e.setPartVoiceSlots (0, 11);
    e.setPartName (0, "seed0");
    e.setPartName (4, "seed4");
    e.setPartChannel (1, 5);
    e.setPartKeyrange (2, 10, 90);
    e.getPart (3).partBytes[4] = 11;   // raga preset pollution (byte-4 shadow)
    // One distinctive APVTS param so APVTS-tree-only mutations are visible in
    // the snapshot too (osc1_shape choice index 4 = Noise).
    proc.getApvts().getParameterAsValue ("osc1_shape") = 4.0f;
}

//==============================================================================
// One mutated-file case: P1 (rollback) or P2 (watchdog), never both.
void runCase (ParvatiAudioProcessor& proc, int loader, const char* label,
              const juce::File& f)
{
    reseed (proc);
    const auto pre = snapshot (proc);
    const bool ok = dispatchLoad (proc, loader, f);
    if (! ok)
    {
        const auto post = snapshot (proc);
        if (! bytesIdentical (pre, post))
        {
            std::printf ("  FAIL [%s] rollback: %s mutated state and still returned false\n",
                         loaderName (loader), label);
            ++g_failures;
        }
    }
    else
    {
        const WatchResult w = watchdog (proc);
        if (! w.completed || ! w.finite)
        {
            std::printf ("  FAIL [%s] watchdog: %s loaded OK but render %s\n",
                         loaderName (loader), label,
                         w.completed ? "produced non-finite audio" : "hung");
            ++g_failures;
        }
    }
    ++g_cases;
}

// Convenience for binary mutations: apply, run, restore the original bytes so
// the corpus file stays pristine for the next case.
void runBinaryMutation (ParvatiAudioProcessor& proc, int loader, const char* label,
                        const juce::File& f, const std::vector<uint8_t>& original,
                        size_t offset, int bit)
{
    std::vector<uint8_t> mutated = original;
    mutated[offset] = static_cast<uint8_t> (mutated[offset] ^ (1u << bit));
    writeFileBytes (f, mutated);
    runCase (proc, loader, label, f);
    writeFileBytes (f, original);
}

//==============================================================================
// Corpus builders — all through the REAL save paths.
struct Corpus
{
    const char* file;
    int loader;
};

// (a) .PRO — single program, distinctive patch/part bytes via the APVTS bridge.
Corpus buildPro()
{
    ParvatiAudioProcessor p;
    p.prepareToPlay (48000.0, 256);
    auto& apvts = p.getApvts();
    apvts.getParameterAsValue ("part_select")    = 1.0f;   // part 0
    apvts.getParameterAsValue ("osc1_shape")     = 2.0f;   // SQUARE
    apvts.getParameterAsValue ("part_polyphony") = 2.0f;   // UNISON_2X
    apvts.getParameterAsValue ("arp_mode")       = 1.0f;   // Arp
    apvts.getParameterAsValue ("arp_octave")     = 3.0f;
    apvts.getParameterAsValue ("seq_length_1")   = 9.0f;
    p.setLoadedProgramName ("FuzzPro");
    const juce::File f = tempFile ("parvati_fuzz_c1.pro");
    if (! p.saveProgramFile (f))
        { std::printf ("FATAL: corpus .PRO save failed\n"); std::_Exit (90); }
    return { "parvati_fuzz_c1.pro", 0 };
}

// (b) .MUL — 3+3 voicecard split with routing + part names (the classic
// factory-multi shape). strategyInt 0 = plain single file.
Corpus buildMul (const char* fileName, uint8_t alloc0, uint8_t alloc1,
                 uint8_t alloc2, int ch1)
{
    ParvatiAudioProcessor p;
    p.prepareToPlay (48000.0, 256);
    auto& e = p.getEngine();
    e.setPartVoiceAllocation (0, alloc0);
    e.setPartVoiceAllocation (1, alloc1);
    e.setPartVoiceAllocation (2, alloc2);
    for (int i = 3; i < SynthEngine::getNumParts(); ++i)
        e.setPartVoiceAllocation (i, 0);
    e.setPartChannel (0, 1);
    e.setPartChannel (1, static_cast<uint8_t> (ch1));
    e.setPartKeyrange (1, 48, 127);
    e.setPartName (0, "LeadA");
    e.setPartName (1, "PadB");
    p.setLoadedProgramName ("FuzzMul");
    const juce::File f = tempFile (fileName);
    if (! p.saveMultiFile (f))
        { std::printf ("FATAL: corpus .MUL save failed\n"); std::_Exit (90); }
    return { fileName, 1 };
}

// (c) .parvati MULTI — the RICH corpus: custom tunings, part names, FX slot
// TYPES + params, per-part poly/spread/arp bytes, mixed slot counts (incl. a
// disabled part). Exercises every extended field the serializer emits.
Corpus buildParvatiMulti()
{
    ParvatiAudioProcessor p;
    p.prepareToPlay (48000.0, 256);
    auto& e = p.getEngine();
    const int slots[6] = { 16, 8, 4, 2, 1, 0 };
    for (int i = 0; i < 6; ++i)
        e.setPartVoiceSlots (i, slots[i]);
    e.setPartName (0, "Kick");
    e.setPartName (1, "Snare");
    e.setPartName (2, "Clap");
    e.setPartChannel (2, 0);                       // one Omni part
    e.setPartKeyrange (0, 36, 36);                 // one drum-zone part
    e.getPart (0).partBytes[4] = 5;                // raga byte (rides params: part_raga)
    e.getPart (3).partBytes[4] = 5;
    // FX: part 0 slot 0 Resonator, part 3 slot 1 Ensemble (slot TYPES are the
    // Wave-1 bug class — the corpus must carry them).
    e.setCurrentPart (0);
    e.setFxSlotType (0, static_cast<uint8_t> (FxType::Resonator));
    e.setFxSlotEnabled (0, 1);
    e.setFxSlotDryWet (0, 90);
    e.setFxSlotParam (0, 0, 64);
    e.setCurrentPart (3);
    e.setFxSlotType (1, static_cast<uint8_t> (FxType::Ensemble));
    e.setFxSlotEnabled (1, 1);
    e.setFxSlotDryWet (1, 70);
    // PartData bytes via the message-thread byte bridge: spread/raga/arp/poly.
    const int saved = e.getCurrentPart();
    for (int part = 0; part < 6; ++part)
    {
        e.setCurrentPart (part);
        e.applyPartByte (3, static_cast<uint8_t> (part * 7));   // spread
        e.applyPartByte (4, static_cast<uint8_t> (part == 1 ? 5 : 0));  // raga
        e.applyPartByte (7, static_cast<uint8_t> (part == 2 ? 1 : 0));  // arp mode
        e.applyPartByte (9, static_cast<uint8_t> (1 + (part % 4)));   // arp octave (valid 1..4)
        e.applyPartByte (15, static_cast<uint8_t> (part % 5));   // polyphony
    }
    e.setCurrentPart (saved);
    p.setLoadedProgramName ("FuzzRich");
    const juce::File f = tempFile ("parvati_fuzz_c3.parvati");
    if (! p.saveParvatiMultiFile (f))
        { std::printf ("FATAL: corpus .parvati multi save failed\n"); std::_Exit (90); }
    return { "parvati_fuzz_c3.parvati", 2 };
}

// (d) .parvati PATCH — current-part-only program.
Corpus buildParvatiPatch()
{
    ParvatiAudioProcessor p;
    p.prepareToPlay (48000.0, 256);
    p.getApvts().getParameterAsValue ("part_select") = 1.0f;
    p.getApvts().getParameterAsValue ("osc1_shape")  = 1.0f;
    p.getApvts().getParameterAsValue ("part_raga")   = 2.0f;
    p.setLoadedProgramName ("FuzzPatch");
    const juce::File f = tempFile ("parvati_fuzz_c4.parvati");
    if (! p.saveParvatiPatchFile (f))
        { std::printf ("FATAL: corpus .parvati patch save failed\n"); std::_Exit (90); }
    return { "parvati_fuzz_c4.parvati", 3 };
}

//==============================================================================
// Truncation sweep at evenly-spaced cut points + the 1-byte / size-1 / full
// edges (the full-length pass is the unmutated baseline: must load true and
// pass the watchdog).
void runTruncations (ParvatiAudioProcessor& proc, const Corpus& c)
{
    const juce::File f = tempFile (c.file);
    const auto original = readFileBytes (f);
    const size_t size = original.size();
    const double fracs[] = { 1.0 / 64.0, 0.05, 1.0 / 8.0, 3.0 / 16.0, 0.25, 5.0 / 16.0,
                             3.0 / 8.0, 7.0 / 16.0, 0.5, 9.0 / 16.0, 5.0 / 8.0, 11.0 / 16.0,
                             0.75, 13.0 / 16.0, 7.0 / 8.0, 15.0 / 16.0 };
    for (double fr : fracs)
    {
        const size_t cut = static_cast<size_t> (static_cast<double> (size) * fr);
        if (cut == 0 || cut >= size) continue;
        writeFileBytes (f, { original.begin(), original.begin() + (long) cut });
        char label[64]; std::snprintf (label, sizeof (label), "%s trunc@%zu", c.file, cut);
        runCase (proc, c.loader, label, f);
    }
    // Edge cuts: 1 byte, size-1, and full (baseline).
    writeFileBytes (f, { original.begin(), original.begin() + 1 });
    { char l[64]; std::snprintf (l, sizeof (l), "%s trunc@1", c.file); runCase (proc, c.loader, l, f); }
    writeFileBytes (f, { original.begin(), original.begin() + (long) (size - 1) });
    { char l[64]; std::snprintf (l, sizeof (l), "%s trunc@size-1", c.file); runCase (proc, c.loader, l, f); }
    writeFileBytes (f, original);
    { char l[64]; std::snprintf (l, sizeof (l), "%s full(baseline)", c.file); runCase (proc, c.loader, l, f); }
}

//==============================================================================
// Bit flips confined to the semantic byte regions of the binary formats
// (offsets from the byte-exact writers in PatchFile.cpp):
//   .PRO (256 B): patch[112] @ 48, PartData[84] @ 172.
//   .MUL (1424 B): MultiData[56] (routing: 6x{ch,lo,hi,alloc}) @ 48;
//     part i patch @ 104 + i*220 + 12; part i PartData @ 104 + i*220 + 124.
void runProBitFlips (ParvatiAudioProcessor& proc, const Corpus& c)
{
    const juce::File f = tempFile (c.file);
    const auto original = readFileBytes (f);
    XorShift32 rng (0xF00DFACEu);
    // Patch body (offsets 48..159): mod slots, envelopes, mix — one draw for
    // offset AND bit keeps the label and the mutation in sync.
    for (int n = 0; n < 24; ++n)
    {
        const size_t off = 48 + static_cast<size_t> (rng.below (112));
        const int bit = rng.below (8);
        char label[64];
        std::snprintf (label, sizeof (label), "%s patchbit@%zu:%d", c.file, off, bit);
        runBinaryMutation (proc, c.loader, label, f, original, off, bit);
    }
    // PartData bytes 3..15 (offsets 175..187): spread / raga / arp / poly.
    for (int n = 0; n < 24; ++n)
    {
        const size_t off = 172 + 3 + static_cast<size_t> (rng.below (13));
        const int bit = rng.below (8);
        char label[64];
        std::snprintf (label, sizeof (label), "%s partbit@%zu:%d", c.file, off, bit);
        runBinaryMutation (proc, c.loader, label, f, original, off, bit);
    }
}

void runMulBitFlips (ParvatiAudioProcessor& proc, const Corpus& c)
{
    const juce::File f = tempFile (c.file);
    const auto original = readFileBytes (f);
    XorShift32 rng (0x5EED0001u);
    auto flip = [&] (size_t off, const char* region)
    {
        const int bit = rng.below (8);
        char label[64];
        std::snprintf (label, sizeof (label), "%s %s@%zu:%d", c.file, region, off, bit);
        runBinaryMutation (proc, c.loader, label, f, original, off, bit);
    };
    for (int n = 0; n < 20; ++n)   // MultiData routing bytes
        flip (48 + static_cast<size_t> (rng.below (56)), "routingbit");
    for (int n = 0; n < 20; ++n)   // part 0 PartData bytes 3..15
        flip (104 + 0 * 220 + 124 + 3 + static_cast<size_t> (rng.below (13)), "p0partbit");
    for (int part = 1; part < 6; ++part)   // parts 1..5 PartData bytes 3..15
        for (int n = 0; n < 4; ++n)
            flip (104 + static_cast<size_t> (part) * 220 + 124 + 3
                      + static_cast<size_t> (rng.below (13)), "partbit");
}

//==============================================================================
// Structural YAML edits for the text formats (line surgery, deterministic).
using TextEdit = std::pair<const char*, std::function<juce::String (const juce::String&)>>;

juce::StringArray linesOf (const juce::String& text)
{
    juce::StringArray a;
    a.addLines (text);
    return a;
}

juce::String removeLinesContaining (const juce::String& text, const juce::String& needle)
{
    juce::String out;
    for (const auto& l : linesOf (text))
        if (! l.contains (needle))
            out += l + "\n";
    return out;
}

juce::String dropLastPartEntry (const juce::String& text)
{
    // Truncate at the START of the 6th "  - channel:" list item.
    juce::StringArray ls = linesOf (text);
    int lastEntryStart = -1, seen = 0;
    for (int i = 0; i < ls.size(); ++i)
        if (ls[i].contains ("- channel:"))
        {
            ++seen;
            lastEntryStart = i;
        }
    if (seen < 6 || lastEntryStart < 0) return text;
    juce::String out;
    for (int i = 0; i < lastEntryStart; ++i) out += ls[i] + "\n";
    return out;
}

juce::String replaceFirst (const juce::String& text, const juce::String& from, const juce::String& to)
{
    const int pos = text.indexOf (from);
    if (pos < 0) return text;
    return text.substring (0, pos) + to + text.substring (pos + from.length());
}

std::vector<TextEdit> multiEdits()
{
    return {
        { "dropLastPart",      [] (const juce::String& t) { return dropLastPartEntry (t); } },
        { "emptyPartsArray",   [] (const juce::String& t)
                               { return replaceFirst (t, "parts:", "parts: []"); } },
        { "scalarParts",       [] (const juce::String&)
                               { return juce::String ("format: parvati-multi\nversion: 1\nname: \"S\"\nparts:\n  - 7\n  - 9\n"); } },
        { "dropChannelKeys",   [] (const juce::String& t) { return removeLinesContaining (t, "channel:"); } },
        { "dropKeyzoneLow",    [] (const juce::String& t) { return removeLinesContaining (t, "keyzone_low:"); } },
        { "dropKeyzoneHigh",   [] (const juce::String& t) { return removeLinesContaining (t, "keyzone_high:"); } },
        { "dropPartNames",     [] (const juce::String& t) { return removeLinesContaining (t, "name: \""); } },
        { "dropVoiceSlots",    [] (const juce::String& t) { return removeLinesContaining (t, "voice_slots:"); } },
        { "dropTuningMode",    [] (const juce::String& t) { return removeLinesContaining (t, "tuning_mode:"); } },
        { "dropParamsBlocks",  [] (const juce::String& t)
                               { juce::String out;
                                 for (const auto& l : linesOf (t))
                                     if (! l.contains ("params:") && ! l.startsWith ("      "))
                                         out += l + "\n";
                                 return out; } },
        { "bogusYamlTail",     [] (const juce::String& t) { return t + "  ::: ][ nonsense\n\t- x:\n"; } },
        { "version99",         [] (const juce::String& t) { return replaceFirst (t, "version: 1", "version: 99"); } },
        { "dropFormatLine",    [] (const juce::String& t) { return removeLinesContaining (t, "format:"); } },
        { "legacyTune999",     [] (const juce::String& t)
                               {   // Inject a legacy (pre-removal) tuning_mode
                                   // key with a hostile value; must parse+clamp,
                                   // never crash (accepted-and-ignored contract).
                                   return replaceFirst (t, "    params:", "    tuning_mode: 999\n    params:"); } },
        { "raga300",           [] (const juce::String& t) { return replaceFirst (t, "part_raga:", "part_raga: 300 #"); } },
        { "brokenTopName",     [] (const juce::String& t) { return replaceFirst (t, "name: \"", "name: \"bad \"quoted\" "); } },
        { "dupPartsKey",       [] (const juce::String& t) { return t + "\nparts:\n  - channel: 9\n"; } },
        { "deepNest",           [] (const juce::String&)
                               {   // Hostile deep nesting (bug hunt 2026-08-18,
                                   // F-state-1): thousands of increasing-indent
                                   // lines made parseParvatiYaml recurse once
                                   // per level (stack exhaustion -> host crash).
                                   // With the depth cap the parse refuses; the
                                   // load must FAIL CLEANLY and leave the state
                                   // untouched (the runCase contract).
                                   juce::String t = "format: parvati-multi\nversion: 1\nname: \"D\"\nparts:\n";
                                   for (int k = 0; k < 5000; ++k)
                                       t += juce::String::repeatedString (" ", 1 + k) + "a:\n";
                                   return t; } },
    };
}

std::vector<TextEdit> patchEdits()
{
    return {
        { "bogusYamlTail",  [] (const juce::String& t) { return t + "  ::: ][ nonsense\n"; } },
        { "version99",      [] (const juce::String& t) { return replaceFirst (t, "version: 1", "version: 99"); } },
        { "dropFormatLine", [] (const juce::String& t) { return removeLinesContaining (t, "format:"); } },
        { "paramsScalar",   [] (const juce::String& t) { return replaceFirst (t, "params:", "params: 7"); } },
        { "brokenName",     [] (const juce::String& t) { return replaceFirst (t, "name: \"", "name: \"x \"y "); } },
        { "raga300",        [] (const juce::String& t) { return replaceFirst (t, "part_raga:", "part_raga: 300 #"); } },
        { "nullParams",     [] (const juce::String& t) { return t + "params:\n  - null\n"; } },
        { "hugeSeqLength",  [] (const juce::String& t) { return t + "params_extra:\n  seq_length_1: 999\n"; } },
    };
}

void runTextEdits (ParvatiAudioProcessor& proc, const Corpus& c,
                   const std::vector<TextEdit>& edits)
{
    const juce::File f = tempFile (c.file);
    const juce::String original = readFileText (f);
    for (const auto& e : edits)
    {
        const juce::String mutated = e.second (original);
        f.replaceWithText (mutated);
        char label[80];
        std::snprintf (label, sizeof (label), "%s %s", c.file, e.first);
        runCase (proc, c.loader, label, f);
    }
    f.replaceWithText (original);   // restore for any later use
}
}  // namespace

//==============================================================================
TEST(loader_fuzz_test)
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    std::printf ("=== Parvati Loader Fuzzer + Rollback Checker (T1) ===\n");

    // ------------------------------------------------------------------
    // [C] Canary self-checks — the harness must PROVE it detects the
    // conditions it exists to catch, or a silent harness bug would pass
    // every future regression.
    // ------------------------------------------------------------------
    std::printf ("\n[C] canaries\n");
    {
        // (c1) snapshot determinism: two snapshots of the same state must be
        // byte-identical, or every later comparison would be meaningless.
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        reseed (p);
        const auto s1 = snapshot (p);
        reseed (p);
        const auto s2 = snapshot (p);
        check (bytesIdentical (s1, s2), "canary: snapshot is deterministic (reseed -> same bytes)");

        // (c2) the comparator itself: a 1-byte alteration must be detected.
        auto altered = s1;
        check (altered.size() > 8, "canary: snapshot has payload to alter");
        altered[altered.size() / 2] = static_cast<uint8_t> (altered[altered.size() / 2] ^ 0x01);
        check (! bytesIdentical (s1, altered), "canary: comparator rejects a 1-byte diff");

        // (c3) the watchdog actually fires on a hang: a bounded synthetic
        // infinite loop must trip the timeout (then be released via the stop
        // flag so nothing leaks).
        std::atomic<bool> stop { false };
        auto fut = std::async (std::launch::async, [&stop]() { while (! stop.load()) {} });
        const bool timedOut = fut.wait_for (std::chrono::seconds (2)) != std::future_status::ready;
        stop.store (true);
        fut.get();
        check (timedOut, "canary: watchdog times out on a non-terminating worker");
    }

    // ------------------------------------------------------------------
    // [F] Corpus — five files via the real save paths.
    // ------------------------------------------------------------------
    std::printf ("\n[F] corpus via real save paths\n");
    const Corpus pro   = buildPro();
    const Corpus mul1  = buildMul ("parvati_fuzz_c2.mul", 0x07, 0x38, 0x00, 2);
    const Corpus mul2  = buildMul ("parvati_fuzz_c5.mul", 0x03, 0x0c, 0x30, 3);
    const Corpus pmul  = buildParvatiMulti();
    const Corpus ppat  = buildParvatiPatch();
    {
        ParvatiAudioProcessor v;
        v.prepareToPlay (48000.0, 256);
        check (v.loadProgramFile (tempFile (pro.file)),  "corpus: .PRO loads");
        check (v.loadMultiFile (tempFile (mul1.file)),   "corpus: .MUL(3+3) loads");
        check (v.loadMultiFile (tempFile (mul2.file)),   "corpus: .MUL(2+2+2) loads");
        check (v.loadParvatiMultiFile (tempFile (pmul.file)), "corpus: rich .parvati multi loads");
        check (v.loadParvatiPatchFile (tempFile (ppat.file)), "corpus: .parvati patch loads");
    }

    // ------------------------------------------------------------------
    // [1] Truncation sweep over every corpus.
    // ------------------------------------------------------------------
    std::printf ("\n[1] truncation sweep\n");
    {
        ParvatiAudioProcessor t;
        t.prepareToPlay (48000.0, 256);
        runTruncations (t, pro);
        runTruncations (t, mul1);
        runTruncations (t, mul2);
        runTruncations (t, pmul);
        runTruncations (t, ppat);
    }

    // ------------------------------------------------------------------
    // [2] Bit flips in the semantic byte regions of the binary formats.
    // ------------------------------------------------------------------
    std::printf ("\n[2] bit-flip sweep (routing / arp / part-param bytes)\n");
    {
        ParvatiAudioProcessor t;
        t.prepareToPlay (48000.0, 256);
        runProBitFlips (t, pro);
        runMulBitFlips (t, mul1);
        runMulBitFlips (t, mul2);
    }

    // ------------------------------------------------------------------
    // [3] Structural YAML edits on the text formats.
    // ------------------------------------------------------------------
    std::printf ("\n[3] structural .parvati edits\n");
    {
        ParvatiAudioProcessor t;
        t.prepareToPlay (48000.0, 256);
        runTextEdits (t, pmul, multiEdits());
        runTextEdits (t, ppat, patchEdits());
    }

    // ------------------------------------------------------------------
    // [4] Truncated ENGINE-STATE blobs (host recall path): setStateInformation
    // with a cut state must neither crash, hang the renderer, produce
    // non-finite audio, nor WEDGE the processor (a subsequent full restore
    // must still land bit-identical — the two-phase restoreState contract:
    // clean apply or untouched, never a half-applied hybrid).
    // ------------------------------------------------------------------
    std::printf ("\n[4] truncated engine-state blobs\n");
    {
        // Rich state A: capture the FULL state bytes. The reference is
        // CANONICALIZED by one round-trip first: a freshly seeded processor's
        // APVTS tree can carry un-canonicalized float artifacts from its
        // seeding writes (e.g. env3_lfo_rate="62.99999618530273"), while a
        // restored processor's tree receives the canonical engine-derived
        // values via loadPartIntoApvts (="63.0"). Comparing a never-restored
        // tree against a restored one would diff those artifacts — a snapshot
        // REPRESENTATION asymmetry, not a state difference. Restoring once and
        // requiring the result to be STABLE (idempotent restore — a second
        // restore changes nothing) fixes the reference AND pins that extra
        // invariant.
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        reseed (a);   // distinctive state (same seed as the mutation harness)
        auto raw0 = snapshot (a);
        a.setStateInformation (raw0.data(), static_cast<int> (raw0.size()));
        const auto full = snapshot (a);
        a.setStateInformation (full.data(), static_cast<int> (full.size()));
        check (bytesIdentical (snapshot (a), full),
               "state restore is idempotent (2nd restore == 1st, canonical)");

        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        reseed (b);
        const double fracs[] = { 0.05, 0.2, 0.35, 0.5, 0.65, 0.8, 0.92 };
        for (double fr : fracs)
        {
            const size_t cut = static_cast<size_t> (static_cast<double> (full.size()) * fr);
            if (cut == 0 || cut >= full.size()) continue;
            char label[48];
            std::snprintf (label, sizeof (label), "blob trunc@%zu/%zu", cut, full.size());
            // Restore on the message thread (it owns the APVTS); corruption in
            // the restored bytes can only hang the RENDER, which the watchdog
            // below covers.
            b.setStateInformation (full.data(), static_cast<int> (cut));
            // No crash + renderer healthy after the truncated restore.
            const WatchResult w = watchdog (b);
            bool ok = w.completed && w.finite;
            // Not wedged: a full restore must still land exactly.
            b.setStateInformation (full.data(), static_cast<int> (full.size()));
            ok = ok && bytesIdentical (snapshot (b), full);
            check (ok, label);
            ++g_cases;
            reseed (b);
        }
        // size-1 edge.
        b.setStateInformation (full.data(), 1);
        const WatchResult w1 = watchdog (b);
        b.setStateInformation (full.data(), static_cast<int> (full.size()));
        check (w1.completed && w1.finite && bytesIdentical (snapshot (b), full),
               "blob trunc@1 byte");
        ++g_cases;
    }

    // Cleanup corpus files.
    for (const char* n : { pro.file, mul1.file, mul2.file, pmul.file, ppat.file })
        tempFile (n).deleteFile();

    std::printf ("\n%d mutation cases, %d failures\n", g_cases, g_failures);
    if (g_failures == 0) std::printf ("LOADER FUZZ TEST: ALL CHECKS PASSED\n");
    else                 std::printf ("LOADER FUZZ TEST: FAILURES (%d)\n", g_failures);
    return g_failures == 0;
}
