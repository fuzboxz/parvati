// Deterministic tooling T8 — GOLDEN ROUND-TRIP BYTES.
//
// Property under test: SAVING is a fixed point of LOADING — for every save
// format pair, "save A -> load into a fresh B -> save B" must reproduce
// byte-identical files, and the loaded program TITLE must equal the
// format-documented normalized form of the saved name. Historical instance of
// the class: the .parvati top-level name was emitted UN-escaped (a `"` or a
// newline in a patch name corrupted the document so the file could not be
// reloaded at all — silent data loss on save).
//
// Corpus: adversarial loadedProgramName_ values (plain / double-quote /
// backslash / both / four UTF-8 kinds / a 16-byte-boundary UTF-8 truncation /
// 40 chars / leading+trailing spaces / newline+tab control chars), each saved
// over a rich seeded state (custom tunings on parts 0+3, part names, a staged
// FX slot type, voice slots + routing, part-0 params via the APVTS bridge,
// one global option).
//
// Per format:
//   .PRO / .MUL (16-byte name chunk): full fixed point per corpus name;
//     expected title = control-strip + code-point-safe 16-byte truncation +
//     trailing-space trim (writeName16 / trimName, independently re-derived
//     below so WRITER DRIFT turns this red).
//   .parvati (patch + multi): the loader re-titles from the FILENAME
//     (documented), so the fixed point is asserted with title == file
//     basename (both saves use the basename "rt"); the adversarial corpus
//     then asserts (a) every name produces a file that LOADS through the
//     real loader and (b) the parsed top-level `name:` equals the
//     control-stripped input (no escaping loss).
//
// Canary self-check (required): the byte comparator reports a 1-flipped-byte
// file with its exact offset; the title comparator flags a wrong expectation.
//
// Built by default. Run with: ./build_release/parvati_roundtrip_golden_test

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

//==============================================================================
// File helpers.
bool readFileBytes (const juce::File& f, std::vector<uint8_t>& out)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
        return false;
    out.assign (static_cast<const uint8_t*> (mb.getData()),
                static_cast<const uint8_t*> (mb.getData()) + mb.getSize());
    return true;
}

struct FileCmp
{
    bool   equal = false;
    size_t sizeA = 0, sizeB = 0;
    long   firstDiff = -1;
};

FileCmp compareFiles (const juce::File& a, const juce::File& b)
{
    FileCmp r;
    std::vector<uint8_t> va, vb;
    if (! readFileBytes (a, va) || ! readFileBytes (b, vb))
        return r;
    r.sizeA = va.size();
    r.sizeB = vb.size();
    const size_t n = std::min (va.size(), vb.size());
    for (size_t i = 0; i < n; ++i)
        if (va[i] != vb[i])
        {
            r.firstDiff = static_cast<long> (i);
            return r;
        }
    if (va.size() != vb.size())
    {
        r.firstDiff = static_cast<long> (n);
        return r;
    }
    r.equal = true;
    return r;
}

juce::File tempCaseDir (int idx, const juce::String& sub = {})
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("parvati_rt_golden")
                 .getChildFile ("case" + juce::String (idx));
    if (sub.isNotEmpty())
        d = d.getChildFile (sub);
    if (d.exists())
        d.deleteRecursively();
    d.createDirectory();
    return d;
}

void renderBlocks (ParvatiAudioProcessor& proc, int n)
{
    for (int i = 0; i < n; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
    }
}

//==============================================================================
// DOCUMENTED name normalizations, independently re-derived in the test (the
// property's oracle — deliberately NOT linked against the writers, so any
// writer drift turns the assertions red):
//   .PRO/.MUL name chunk: control chars (< 0x20) dropped; truncated to 16
//     bytes on a UTF-8 code-point boundary; space-padded; the parser trims
//     TRAILING spaces/NULs.
//   .parvati top-level name: control chars dropped, quote/backslash escaped
//     in the raw text; the loader's parser unescapes, so the parsed value ==
//     input minus control chars (no length limit).
juce::String stripControl (const juce::String& s)
{
    juce::String o;
    for (int i = 0; i < s.length(); ++i)
        if (s[i] >= 0x20)
            o += s[i];
    return o;
}

// UTF-8 lead-byte length (1..4; invalid lead counts as 1 — the same rule
// writeName16 applies so a malformed sequence never splits a real one).
size_t cpLen (uint8_t c)
{
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

juce::String proTitle16 (const juce::String& s)
{
    const char* const raw = s.toRawUTF8();
    const size_t bytes = s.getNumBytesAsUTF8();
    std::string keep;
    for (size_t i = 0; i < bytes;)
    {
        const size_t cp = cpLen (static_cast<uint8_t> (raw[i]));
        if (static_cast<uint8_t> (raw[i]) >= 0x20)
        {
            if (keep.size() + cp > 16)
                break;   // never split a code point
            keep.append (raw + i, cp);
        }
        i += cp;
    }
    while (! keep.empty() && keep.back() == ' ')
        keep.pop_back();   // parse-side trailing trim
    return juce::String::fromUTF8 (keep.data(), static_cast<int> (keep.size()));
}

//==============================================================================
// Rich seed state (idiom shared with parvati_shadow_state_test so every
// mirrored surface carries non-default bytes a round trip must preserve).
void seedRichState (ParvatiAudioProcessor& proc)
{
    SynthEngine& e = proc.getEngine();
    const int nParts = SynthEngine::getNumParts();

    int16_t custom[12] = {};
    for (int c = 0; c < 12; ++c)
        custom[(size_t) c] = static_cast<int16_t> ((c + 1) * 10 - 30);
    e.setPartTuningCustom (0, custom);
    e.setPartTuningCustom (3, custom);

    for (int p = 0; p < nParts; ++p)
        e.setPartName (p, juce::String ("Alias") + juce::String (p + 1));

    e.stagePartFxSlotType (2, 0, static_cast<int> (FxType::Resonator));

    e.setPartVoiceSlots (1, 8);
    e.setPartMidiChannel (1, 5);
    e.setPartKeyZone (1, 20, 40);

    // Part-0 params through the real APVTS bridge (part_select FIRST so the
    // writes land on part 0) + one global option (serialized by .parvati).
    auto setP = [&] (const char* id, float v) {
        proc.getApvts().getParameterAsValue (id) = v;
    };
    setP ("part_select", 1.0f);
    setP ("osc1_shape", 4.0f);
    setP ("filter1_cutoff", 90.0f);
    setP ("mix_noise", 40.0f);
    setP ("vca_curve", 1.0f);

    renderBlocks (proc, 2);   // service tuning/fx/config/allocation dirty flags
}

//==============================================================================
struct NameCase
{
    const char* label;
    juce::String name;
};

const std::vector<NameCase>& corpus()
{
    static const std::vector<NameCase> c = {
        { "plain",        "Plain Patch" },
        { "double-quote", "He said \"hi\"" },
        { "backslash",    "back\\slash" },
        { "both",         "mix \"q\" and \\ b" },
        { "utf8 e-acute", "Caf" "\xC3\xA9" },
        { "utf8 note",    "\xE2\x99\xAA" " music" },
        { "utf8 ellipsis","dot dot dot " "\xE2\x80\xA6" },
        { "utf8 emoji",   "emoji " "\xF0\x9F\x8E\xB9" },
        { "utf8 >16B",    "\xC3\xA9" "\xC3\xA9" "\xC3\xA9" "\xC3\xA9"
                          "\xC3\xA9" "\xC3\xA9" "\xC3\xA9" "\xC3\xA9"
                          "\xC3\xA9" },
        { "40 chars",     "0123456789012345678901234567890123456789" },
        { "pad spaces",   "  padded  " },
        { "control",      "line\nbreak\ttab" },
    };
    return c;
}

//==============================================================================
// .PRO / .MUL: full fixed point per corpus name + title round trip.
template <typename SaveFn, typename LoadFn>
void roundtripAmbika (const char* fmt, int idx, const juce::String& title,
                      SaveFn save, LoadFn load)
{
    const auto dir = tempCaseDir (idx);

    ParvatiAudioProcessor a;
    a.prepareToPlay (48000.0, 256);
    seedRichState (a);
    a.setLoadedProgramName (title);

    const juce::File fa = dir.getChildFile ("rtA.bin");
    if (! save (a, fa))
    {
        check (false, "save A");
        return;
    }

    ParvatiAudioProcessor b;
    b.prepareToPlay (48000.0, 256);
    if (! load (b, fa))
    {
        check (false, "load B");
        return;
    }
    renderBlocks (b, 2);   // service the deferred allocation/config rebuilds

    const juce::File fb = dir.getChildFile ("rtB.bin");
    if (! save (b, fb))
    {
        check (false, "save B");
        return;
    }

    const FileCmp cmp = compareFiles (fa, fb);
    {
        char msg[192];
        if (cmp.equal)
            std::snprintf (msg, sizeof (msg), "%s [%s]: fixed point bytes(A)==bytes(B) (%zu B)",
                           fmt, title.substring (0, 24).toRawUTF8(), cmp.sizeA);
        else
            std::snprintf (msg, sizeof (msg),
                           "%s [%s]: fixed point VIOLATED (sizes %zu vs %zu, first diff @%ld)",
                           fmt, title.substring (0, 24).toRawUTF8(), cmp.sizeA, cmp.sizeB, cmp.firstDiff);
        check (cmp.equal, msg);
    }

    {
        const juce::String expected = proTitle16 (title);
        char msg[192];
        std::snprintf (msg, sizeof (msg), "%s [%s]: title round-trips to the 16-byte form [%s]",
                       fmt, title.substring (0, 24).toRawUTF8(), expected.substring (0, 24).toRawUTF8());
        check (b.getLoadedProgramName() == expected, msg);
    }
}

// .parvati: fixed point with title == file basename (the documented
// filename-retitling behavior means the doc name line only round-trips when
// the two agree — both saves use the basename "rt").
template <typename SaveFn, typename LoadFn>
void roundtripParvatiFixedPoint (const char* fmt, int idx, SaveFn save, LoadFn load)
{
    const auto dirA = tempCaseDir (idx, "a");
    const auto dirB = tempCaseDir (idx, "b");

    ParvatiAudioProcessor a;
    a.prepareToPlay (48000.0, 256);
    seedRichState (a);
    a.setLoadedProgramName ("rt");   // == the basename of both save targets

    const juce::File fa = dirA.getChildFile ("rt.parvati");
    if (! save (a, fa))
    {
        check (false, "save A");
        return;
    }

    ParvatiAudioProcessor b;
    b.prepareToPlay (48000.0, 256);
    if (! load (b, fa))
    {
        check (false, "load B");
        return;
    }
    renderBlocks (b, 2);

    const juce::File fb = dirB.getChildFile ("rt.parvati");
    if (! save (b, fb))
    {
        check (false, "save B");
        return;
    }

    const FileCmp cmp = compareFiles (fa, fb);
    char msg[192];
    if (cmp.equal)
        std::snprintf (msg, sizeof (msg), "%s: fixed point bytes(A)==bytes(B) (%zu B, title==basename)",
                       fmt, cmp.sizeA);
    else
        std::snprintf (msg, sizeof (msg), "%s: fixed point VIOLATED (sizes %zu vs %zu, first diff @%ld)",
                       fmt, cmp.sizeA, cmp.sizeB, cmp.firstDiff);
    check (cmp.equal, msg);
}

// .parvati: adversarial-name corpus — the file must LOAD (the un-escaped-name
// class corrupted the document before the escaping fix) and the parsed
// top-level name must equal the control-stripped input.
template <typename SaveFn, typename LoadFn>
void parvatiNameEscaping (const char* fmt, int idx, const NameCase& nc,
                          SaveFn save, LoadFn load)
{
    const auto dir = tempCaseDir (idx);
    ParvatiAudioProcessor a;
    a.prepareToPlay (48000.0, 256);
    seedRichState (a);
    a.setLoadedProgramName (nc.name);

    const juce::File f = dir.getChildFile ("c.parvati");
    if (! save (a, f))
    {
        check (false, "save A");
        return;
    }

    {
        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%s [%s]: adversarial-name file LOADS", fmt, nc.label);
        check (load (b, f), msg);
    }

    {
        juce::String text;
        if (juce::FileInputStream in (f); in.openedOk())
            text = in.readEntireStreamAsString();
        const juce::var tree = parvati::preset::parseParvatiYaml (text);
        const juce::String parsed = tree.isObject() ? tree["name"].toString() : juce::String();
        char msg[192];
        std::snprintf (msg, sizeof (msg), "%s [%s]: parsed doc name == control-stripped input [%s]",
                       fmt, nc.label, stripControl (nc.name).substring (0, 24).toRawUTF8());
        check (parsed == stripControl (nc.name), msg);
    }
}
}  // namespace

//==============================================================================
int main ()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== TOOL 8: golden round-trip bytes ===\n");

    auto savePRO  = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.saveProgramFile (f); };
    auto loadPRO  = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.loadProgramFile (f); };
    auto saveMUL  = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.saveMultiFile (f); };
    auto loadMUL  = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.loadMultiFile (f); };
    auto savePPat = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.saveParvatiPatchFile (f); };
    auto loadPPat = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.loadParvatiPatchFile (f); };
    auto savePMul = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.saveParvatiMultiFile (f); };
    auto loadPMul = [] (ParvatiAudioProcessor& p, const juce::File& f) { return p.loadParvatiMultiFile (f); };

    // ---- canary 1: the byte comparator reports a 1-flipped-byte file ----
    std::printf ("\n[canary] byte comparator detects a 1-byte flip\n");
    {
        const auto dir = tempCaseDir (9001);
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        seedRichState (a);
        a.setLoadedProgramName ("canary");
        const juce::File good = dir.getChildFile ("good.parvati");
        const juce::File bad  = dir.getChildFile ("bad.parvati");
        check (savePMul (a, good), "canary: save the reference file");

        std::vector<uint8_t> bytes;
        check (readFileBytes (good, bytes), "canary: read it back");
        constexpr size_t kFlipOffset = 200;   // deep inside the params block
        check (bytes.size() > kFlipOffset, "canary: flip offset inside the file");
        bytes[kFlipOffset] = static_cast<uint8_t> (bytes[kFlipOffset] ^ 0x01);
        bad.replaceWithData (bytes.data(), bytes.size());

        const FileCmp cmp = compareFiles (good, bad);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "canary: comparator flags the flip (first diff @%ld, want %zu)",
                       cmp.firstDiff, kFlipOffset);
        check (! cmp.equal && cmp.firstDiff == static_cast<long> (kFlipOffset), msg);
    }

    // ---- canary 2: the title comparator flags a wrong expectation ----
    std::printf ("\n[canary] title comparator detects a wrong expectation\n");
    {
        const juce::String loaded = proTitle16 (corpus().front().name);
        const juce::String wrong  = loaded + "X";
        check (loaded != wrong, "canary: title comparator flags a mismatched expectation");
    }

    // ---- the corpus over the two Ambika pairs + the two .parvati names ----
    int idx = 0;
    for (const auto& nc : corpus())
    {
        std::printf ("\n[%s] name = \"%s\"\n", nc.label,
                     nc.name.replace ("\n", "\\n").replace ("\t", "\\t")
                         .substring (0, 32).toRawUTF8());
        roundtripAmbika (".PRO", idx, nc.name, savePRO, loadPRO);
        roundtripAmbika (".MUL", idx, nc.name, saveMUL, loadMUL);
        parvatiNameEscaping (".parvati-patch", idx + 1000, nc, savePPat, loadPPat);
        parvatiNameEscaping (".parvati-multi", idx + 2000, nc, savePMul, loadPMul);
        ++idx;
    }

    // ---- .parvati fixed points (title == basename) ----
    std::printf ("\n[fixed point] .parvati pairs with title == basename\n");
    roundtripParvatiFixedPoint (".parvati-patch", 3001, savePPat, loadPPat);
    roundtripParvatiFixedPoint (".parvati-multi", 3002, savePMul, loadPMul);

    // ---- summary ----
    std::printf ("\n=== GOLDEN ROUND-TRIP: %s (%d failures) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
