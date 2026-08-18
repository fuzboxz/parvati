// Deterministic tooling T2 — SHADOW-STATE DEFAULTS PROPERTY.
//
// Property under test (the "stale shadow state survives a load" class this
// exists to pin): loading a DEFAULTS-ONLY .parvati multi must reset EVERY
// piece of per-part shadow state — the previous session's custom tuning
// tables, part names/aliases, staged FX slot types, voice slots, routing and
// the mirrored PartData bytes must all return to the fresh-engine values.
// The file is the whole truth; nothing the user did before the load may leak
// through. Historical instances of the class:
//   - a stale customTuningActive flag kept playing the OLD microtonal table
//     after loading a 12-EDO multi (fixed by the byte-4==0 clear on load);
//   - part names ("Kick") survived .MUL/.PRO loads that replaced the content;
//   - .parvati multi loads wrote FX slot TYPES into fxState but never staged
//     them into the DSP chains (loaded FX silently absent / previous effect);
//   - a defaults file must also restore voice slots, channel, key zone and
//     PartData bytes 3 (spread) / 4 (raga) / 15 (polyphony) per part.
//
// Harness: proc A (fresh, prepareToPlay) saves a defaults multi through the
// REAL path (saveParvatiMultiFile). Proc D is POLLUTED on every mirrored
// surface (custom tunings on parts 0+3, names on all parts, a staged
// non-None FX type on part 2 slot 0, arp config, slots/channel/zone on part
// 1, bytes 3/4/15 on parts 1+4). CANARY: the diff collector must REPORT the
// pollution (>=1 diff in EVERY category) — proving the comparator detects
// the bug condition. Proc B is polluted identically, then loads the defaults
// file + one flush block: diff(B, fresh C) must be EXACTLY zero on every
// mirrored surface.
//
// If a future serializer change omits a key and the defaults file can no
// longer reset a surface, the zero-diff check FAILS with the diff lines —
// that is a real finding to report, not an assertion to delete.
//
// Deterministic: fixed pollution values (chosen against the fresh-engine
// values read at runtime, never wall-clock/random), a handful of blocks.
// Built by default. Run with: ./build_release/parvati_shadow_state_test

#include <cstdint>
#include <cstdio>
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

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr int kRate  = 48000;
constexpr int kBlock = 256;

//==============================================================================
// Render n empty blocks (services the staged config/fx/tuning dirty flags +
// consumes the pending FX type swaps — one block is enough; render a few so
// the assertion is not sensitive to the exact service cadence).
void renderBlocks (ParvatiAudioProcessor& proc, int blocks)
{
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
    }
}

// juce::String + int is ambiguous (int / int64 overloads) — every int in a
// diff message goes through this explicit wrapper.
juce::String J (int v) { return juce::String (v); }

//==============================================================================
// Diff collector: every mirrored surface the defaults load must reset, tagged
// by category ("name", "tune", "fx", "slots", "ch", "zone", "spread", "raga",
// "poly") so the canary can prove EACH category is observable. Compares two
// processors' ENGINES only (the file-load contract ends at engine storage).
std::vector<juce::String> collectDiffs (ParvatiAudioProcessor& a,
                                        ParvatiAudioProcessor& b)
{
    SynthEngine& ea = a.getEngine();
    SynthEngine& eb = b.getEngine();
    std::vector<juce::String> diffs;

    const int nParts = SynthEngine::getNumParts();
    for (int p = 0; p < nParts; ++p)
    {
        if (ea.getPartName (p) != eb.getPartName (p))
            diffs.push_back ("name[" + J (p) + "]: \"" + ea.getPartName (p)
                             + "\" vs \"" + eb.getPartName (p) + "\"");
        if (ea.resolvedTuningMode (p) != eb.resolvedTuningMode (p))
            diffs.push_back ("tune[" + J (p) + "]: " + J (ea.resolvedTuningMode (p))
                             + " vs " + J (eb.resolvedTuningMode (p)));
        for (int s = 0; s < kNumFxSlots; ++s)
            if (ea.fxChainSlotTypeForTest (p, s) != eb.fxChainSlotTypeForTest (p, s))
                diffs.push_back ("fx[" + J (p) + ":" + J (s) + "]: installed "
                                 + J ((int) ea.fxChainSlotTypeForTest (p, s)) + " vs "
                                 + J ((int) eb.fxChainSlotTypeForTest (p, s)));
        if (ea.getPartVoiceSlots (p) != eb.getPartVoiceSlots (p))
            diffs.push_back ("slots[" + J (p) + "]: " + J (ea.getPartVoiceSlots (p))
                             + " vs " + J (eb.getPartVoiceSlots (p)));
        if (ea.getPartChannel (p) != eb.getPartChannel (p))
            diffs.push_back ("ch[" + J (p) + "]: " + J ((int) ea.getPartChannel (p))
                             + " vs " + J ((int) eb.getPartChannel (p)));
        if (ea.getPartKeyrangeLow (p) != eb.getPartKeyrangeLow (p)
            || ea.getPartKeyrangeHigh (p) != eb.getPartKeyrangeHigh (p))
            diffs.push_back ("zone[" + J (p) + "]: "
                             + J ((int) ea.getPartKeyrangeLow (p)) + ".." + J ((int) ea.getPartKeyrangeHigh (p))
                             + " vs " + J ((int) eb.getPartKeyrangeLow (p)) + ".."
                             + J ((int) eb.getPartKeyrangeHigh (p)));
        // Mirrored PartData bytes: 3 = spread, 4 = raga preset, 15 = polyphony.
        if (ea.getPart (p).partBytes[3] != eb.getPart (p).partBytes[3])
            diffs.push_back ("spread[" + J (p) + "]: "
                             + J ((int) ea.getPart (p).partBytes[3]) + " vs "
                             + J ((int) eb.getPart (p).partBytes[3]));
        if (ea.getPart (p).partBytes[4] != eb.getPart (p).partBytes[4])
            diffs.push_back ("raga[" + J (p) + "]: "
                             + J ((int) ea.getPart (p).partBytes[4]) + " vs "
                             + J ((int) eb.getPart (p).partBytes[4]));
        if (ea.getPart (p).partBytes[15] != eb.getPart (p).partBytes[15])
            diffs.push_back ("poly[" + J (p) + "]: "
                             + J ((int) ea.getPart (p).partBytes[15]) + " vs "
                             + J ((int) eb.getPart (p).partBytes[15]));
    }
    return diffs;
}

int countCategory (const std::vector<juce::String>& diffs, const char* cat)
{
    int n = 0;
    for (const auto& d : diffs)
        if (d.startsWith (cat))
            ++n;
    return n;
}

//==============================================================================
// Pollute EVERY mirrored surface of a freshly-prepared processor with values
// guaranteed different from the fresh-engine state (byte values are chosen
// against the reference's current value, so no default can collide). Uses
// only public message-thread setters — the exact paths a user session drives.
void pollute (ParvatiAudioProcessor& proc)
{
    SynthEngine& e = proc.getEngine();
    const int nParts = SynthEngine::getNumParts();

    // Custom tuning tables (the customTuningActive shadow) on parts 0 and 3.
    int16_t custom[12] = {};
    for (int c = 0; c < 12; ++c)
        custom[(size_t) c] = static_cast<int16_t> ((c + 1) * 10 - 30);   // -30..80 (tuning_test idiom)
    e.setPartTuningCustom (0, custom);
    e.setPartTuningCustom (3, custom);

    // Names/aliases on all parts.
    for (int p = 0; p < nParts; ++p)
        e.setPartName (p, juce::String ("Alias") + juce::String (p + 1));

    // A staged non-None FX slot TYPE on part 2 slot 0 (chain staging + one
    // flush block so fxChainSlotTypeForTest observes the installed type).
    e.stagePartFxSlotType (2, 0, static_cast<int> (FxType::Resonator));

    // Arp config (pendingConfig shadow) — drive through the engine setters.
    e.setCurrentPart (2);
    e.setArpMode (1);
    e.setArpOctave (2);

    // Voice slots + routing on part 1.
    e.setPartVoiceSlots (1, 8);
    e.setPartMidiChannel (1, 5);
    e.setPartKeyZone (1, 20, 40);

    // PartData bytes on parts 1 and 4 — picked against the reference values
    // so the pollution is always a real diff (3=spread, 4=raga, 15=poly).
    const auto flipByte = [&e] (int part, int offset, int notEqual)
    {
        e.setCurrentPart (part);
        const uint8_t cur = e.getPart (part).partBytes[(size_t) offset];
        // Ternary ints stay <= 41 (value-preserving); the explicit cast keeps
        // the int->uint8_t conversion visible to -Wsign-conversion.
        const int chosen = (cur == notEqual) ? notEqual + 1 : notEqual;
        e.applyPartByte (offset, static_cast<uint8_t> (chosen));
    };
    flipByte (1, 3, 40);   // spread
    flipByte (1, 4, 5);    // raga preset
    flipByte (4, 15, 2);   // polyphony

    renderBlocks (proc, 2);   // service tuning/fx/config dirty flags
}
}  // namespace

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== Parvati shadow-state defaults property ===\n");

    // ------------------------------------------------------------------
    // [0] Build the defaults file through the REAL save path (proc A fresh).
    // ------------------------------------------------------------------
    std::printf ("[0] save a defaults-only .parvati multi from a fresh proc\n");
    juce::File defaultsFile;
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (kRate, kBlock);
        defaultsFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_shadow_defaults.parvati");
        (void) defaultsFile.deleteFile();
        const bool saved = a.saveParvatiMultiFile (defaultsFile);
        check (saved && defaultsFile.existsAsFile(), "defaults multi saved via the real path");
    }

    // Reference: a fresh processor (what the polluted proc must become).
    ParvatiAudioProcessor c;
    c.prepareToPlay (kRate, kBlock);
    renderBlocks (c, 1);

    // ------------------------------------------------------------------
    // [1] CANARY: the comparator must REPORT every pollution category. This
    //     proves the diff collector detects the bug condition (a shadow-state
    //     leak); without it, a silently-blind comparator would pass [2] free.
    // ------------------------------------------------------------------
    std::printf ("\n[1] canary: polluted engine (no load) diffs a fresh one\n");
    {
        ParvatiAudioProcessor d;
        d.prepareToPlay (kRate, kBlock);
        renderBlocks (d, 1);
        pollute (d);

        // The FX staging must have taken (installed type observable) — a
        // sanity precondition for the canary's fx category.
        check (d.getEngine().fxChainSlotTypeForTest (2, 0)
                   == static_cast<uint8_t> (FxType::Resonator),
               "canary precondition: polluted FX type installed on part 2 slot 0");
        check (d.getEngine().resolvedTuningMode (0) == 33
                   && d.getEngine().resolvedTuningMode (3) == 33,
               "canary precondition: custom tuning active on parts 0 and 3 (mode 33)");

        const auto diffs = collectDiffs (d, c);
        std::printf ("     comparator reported %d diffs\n", (int) diffs.size());
        for (const auto& s : diffs)
            std::printf ("       - %s\n", s.toRawUTF8());

        static const char* kCats[] = { "name", "tune", "fx", "slots", "ch",
                                       "zone", "spread", "raga", "poly" };
        for (const char* cat : kCats)
        {
            char msg[96];
            std::snprintf (msg, sizeof (msg), "canary: comparator sees the %s pollution", cat);
            check (countCategory (diffs, cat) > 0, msg);
        }
    }

    // ------------------------------------------------------------------
    // [2] THE PROPERTY: pollute a proc, load the DEFAULTS multi, and require
    //     zero diffs against the fresh reference on every mirrored surface.
    //     Any reported diff is a real shadow-state leak — a finding.
    // ------------------------------------------------------------------
    std::printf ("\n[2] defaults load resets every polluted surface\n");
    {
        ParvatiAudioProcessor b;
        b.prepareToPlay (kRate, kBlock);
        renderBlocks (b, 1);
        pollute (b);

        const bool loaded = b.loadParvatiMultiFile (defaultsFile);
        check (loaded, "defaults multi loads into the polluted proc");
        renderBlocks (b, 4);   // flush: consume staged FX swaps + dirty flags

        const auto diffs = collectDiffs (b, c);
        if (diffs.empty())
        {
            check (true, "after the defaults load: ZERO diffs vs a fresh engine (all 9 surfaces)");
        }
        else
        {
            for (const auto& s : diffs)
                std::printf ("     LEAK: %s\n", s.toRawUTF8());
            char msg[96];
            std::snprintf (msg, sizeof (msg),
                           "after the defaults load: %d shadow-state leaks remain (see above)",
                           (int) diffs.size());
            check (false, msg);
        }
    }

    (void) defaultsFile.deleteFile();

    std::printf ("\n%s\n", g_failures == 0
                   ? "SHADOW-STATE TEST: ALL CHECKS PASSED (0 failures)"
                   : "SHADOW-STATE TEST: FAILURES (see above)");
    return g_failures == 0 ? 0 : 1;
}
