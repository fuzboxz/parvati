// Deterministic tooling T7: UNDO ROUND-TRIP PROPERTY.
//
//   Property: undo returns the FULL host-visible processor state to the exact
//   pre-op snapshot (byte-identical getStateInformation), for every op class
//   in the battery:
//     - a plain patch-byte float write (osc1_param)
//     - a choice write into PartData (part_polyphony — byte 15, the
//       poly-mode/mirror surface)
//     - a controller-side choice write (arp_mode — pendingConfig_/seqlock)
//     - a SIGNED patch-byte write (mod1_amount, -63..63)
//     - an FX param write (fx1_param1 — fxState atomics)
//     - an FX TYPE change (fx1_type — W10: seeding lives ONLY at the UI seams
//       (the type-combo popup pick / keyboard arrows / stepType chevrons, via
//       seedEngagementDefaultsForType, all BEFORE the type write in ONE
//       transaction); the listener NEVER seeds (the same parameterChanged
//       fires for host automation / NRPN / undo replay / part loads, where
//       seeding would clobber live values) — so a replayed type write is
//       trivially seed-free and one transaction undoes as ONE step)
//
//   Plus the two corruption classes the waves found:
//     [3] the W7 lane-A seed clobber: undoing a type switch must restore the
//         PREVIOUS type's USER values, not its engagement defaults. (W10
//         note: the old isPerformingUndoRedo guard is GONE — subsumed by the
//         no-listener-seeding contract; this case stays pinned so a future
//         re-introduction of listener-side seeding fails here.)
//     [4] the W2 cross-part doctrine: a part switch invalidates the stack —
//         undoSafe() must sweep to canUndo()==false (NO replay into the new
//         part), the new part's bytes stay untouched, and the old part keeps
//         its edit.
//
//   CANARY (required): the byte comparator must flag a 1-byte doctored
//   snapshot — a comparator that cannot fail proves nothing. Mutation-tested:
//   removing the part-switch invalidation and skipping engine pushes during
//   undo replay both FAIL this suite (real teeth). Removing the W7
//   isPerformingUndoRedo guard does NOT fail it: JUCE's UndoManager::perform
//   DISCARDS actions attempted while an undo/redo is in flight (verified in
//   the vendored juce_UndoManager.cpp — the SetPropertyAction never runs), so
//   a getParameterAsValue write during an undo replay is a structural no-op
//   in Release (and a jassert trap in Debug — the guard remains correct
//   defense-in-depth and keeps Debug builds quiet). This harness therefore
//   pins the OBSERVABLE property (user values survive the undo) rather than
//   the guard itself.
//
//   Snapshots are taken from the SAME processor instance around each op (no
//   restore in between), so the tree representation is stable and no
//   canonicalization round-trip is needed (the T1 cross-instance asymmetry
//   — seeded float artifacts vs loadPartIntoApvts's canonical values —
//   cannot occur within one instance).
//
// Run: ./build_unified/hellcat_unified_tests undo_property_test

#include <cstdint>
#include "unified_test_runner.h"
#include <cstdio>
#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"              // setCurrentTopPage (the FX-card tree hunt)
#include "SynthEngine.h"
#include "ui/FxSlotCard.h"             // the seeding seam (W10)
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
// Full host-visible state snapshot (APVTS tree + the 6-part engine blob + ui
// prefs), exactly what a host saves. Deterministic within one instance for
// identical state — proven by the canary before any property runs.
std::vector<uint8_t> snapshot (HellcatAudioProcessor& proc)
{
    juce::MemoryBlock mb;
    proc.getStateInformation (mb);
    return { static_cast<const uint8_t*> (mb.getData()),
             static_cast<const uint8_t*> (mb.getData()) + mb.getSize() };
}

// Byte-for-byte compare with actionable failure output (size + first diff).
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
            std::printf ("      [diff] first differing byte @ %zu (0x%02x vs 0x%02x)\n",
                         i, (unsigned) a[i], (unsigned) b[i]);
            return false;
        }
    return true;
}

// Raw APVTS value. Every id used here is a choice/int parameter whose stored
// value is an exact integer-in-float, so callers compare with juce::approximatelyEqual.
float rawValue (HellcatAudioProcessor& proc, const char* id)
{
    return proc.getApvts().getRawParameterValue (id)->load();
}

bool rawIs (HellcatAudioProcessor& proc, const char* id, float exactIntValue)
{
    return juce::approximatelyEqual (rawValue (proc, id), exactIntValue);
}

// A USER edit: the APVTS-bound editor widgets write through getParameterAsValue
// (tree property write recorded by the UndoManager -> synchronous
// parameterChanged -> engine staging). This is the real UI write path.
void writeParam (HellcatAudioProcessor& proc, const char* id, float v)
{
    proc.getApvts().getParameterAsValue (id) = v;
}

// Editor doctrine: each discrete edit site opens a fresh transaction, so one
// undo = one logical edit (PluginEditor.cpp idiom).
void newTransaction (HellcatAudioProcessor& proc)
{
    proc.getUndoManager().beginNewTransaction();
}

// One engine part's full storage (patch + part bytes) for the part-isolation
// assertions ([4]) — the surfaces undo could corrupt across parts.
struct PartBytes
{
    std::array<uint8_t, 112> patch {};
    std::array<uint8_t, 84>  part {};
};

PartBytes readPartBytes (HellcatAudioProcessor& proc, int i)
{
    PartBytes out;
    out.patch = {};
    proc.getEngine().getPart (i).patchBytes.copyTo (out.patch);
    proc.getEngine().getPart (i).partBytes.copyTo (out.part);
    return out;
}

bool partBytesEqual (const PartBytes& a, const PartBytes& b)
{
    return a.patch == b.patch && a.part == b.part;
}
}  // namespace

//==============================================================================
TEST(undo_property_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== Hellcat undo round-trip property (T7) ===\n");

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    // The fx-type ENGAGEMENT SEEDING lives in FxSlotCard's APVTS listener (a
    // UI component — the W7 guard is there), so the fx-type cases need a live
    // card: create the editor (headless, same as tests/editor_test.cpp). One
    // processor throughout; declared after proc so it is destroyed first.
    // Construction is display-neutral for the state snapshot (any
    // loadPartIntoApvts writes are the W2 non-undoable path and settle BEFORE
    // the canary takes its first snapshot).
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    check (editor != nullptr, "editor created (live FxSlotCards for the seeding cases)");

    // Locate the FX1 card for the seeding seam (W10): engagement defaults are
    // seeded ONLY at the UI seams now, so the fx cases drive
    // seedEngagementDefaultsForType + the type write explicitly. The
    // pageSelector_ TabbedComponent unparents non-current tab contents, so the
    // hunt runs on the FX page (then restores SYNTH).
    FxSlotCard* fx1 = nullptr;
    {
        auto* parEd = dynamic_cast<HellcatEditor*> (editor.get());
        if (parEd != nullptr) parEd->setCurrentTopPage (1);
        std::function<void (juce::Component*)> hunt = [&] (juce::Component* c)
        {
            if (c == nullptr || fx1 != nullptr) return;
            if (auto* card = dynamic_cast<FxSlotCard*> (c)) { fx1 = card; return; }
            for (int i = 0; i < c->getNumChildComponents(); ++i)
                hunt (c->getChildComponent (i));
        };
        hunt (editor.get());
        if (parEd != nullptr) parEd->setCurrentTopPage (0);
    }
    check (fx1 != nullptr, "FX1 card found (the seeding seam is reachable)");

    // ------------------------------------------------------------------
    // [0] CANARY: the comparator itself. Two snapshots of the same state
    // must be identical (else the harness is invalid), and a 1-byte-doctored
    // snapshot must be REJECTED (a comparator that cannot fail proves
    // nothing — every later PASS depends on this).
    // ------------------------------------------------------------------
    std::printf ("\n[0] comparator canary\n");
    {
        const auto s1 = snapshot (proc);
        const auto s2 = snapshot (proc);
        check (bytesIdentical (s1, s2), "canary: snapshot deterministic (same state -> same bytes)");
        check (s1.size() > 64, "canary: snapshot has payload to doctor");

        auto doctored = s1;
        doctored[doctored.size() / 2] ^= 0x01;
        check (! bytesIdentical (s1, doctored), "canary: comparator rejects a 1-byte-doctored snapshot");
    }

    // ------------------------------------------------------------------
    // [1] Ops battery: one op, one transaction, one undoSafe -> the FULL
    // state must return byte-identical to pre-op. Each case also asserts the
    // op REALLY changed the state mid-flight (no vacuous passes) and that the
    // single transaction was fully consumed (canUndo false after undo).
    // ------------------------------------------------------------------
    struct OpCase
    {
        const char* id;
        float v;
        const char* label;
    };
    // osc1_param: plain patch byte. part_polyphony: choice -> PartData byte 15
    // (poly mode + the displayVersion mirror surface). arp_mode: choice ->
    // controller-side pendingConfig_. mod1_amount: SIGNED patch byte.
    // fx1_param1: per-part FX atomics.
    const OpCase ops[] = {
        { "osc1_param",    77.0f, "float patch-byte write (osc1_param)" },
        { "part_polyphony", 2.0f, "choice write -> PartData byte 15 (part_polyphony)" },
        { "arp_mode",       1.0f, "choice write -> controller arp config (arp_mode)" },
        { "mod1_amount",   21.0f, "signed patch-byte write (mod1_amount)" },
        { "fx1_param1",    44.0f, "FX param write (fx1_param1)" },
    };

    std::printf ("\n[1] ops battery: undo restores the full state byte-exactly\n");
    for (const auto& op : ops)
    {
        const float pre = rawValue (proc, op.id);
        if (juce::approximatelyEqual (pre, op.v))
        {
            std::printf ("  SKIP: %s already at %g (value collision with default — adjust the case)\n", op.id, op.v);
            ++g_failures;   // a vacuous case must fail loudly, not pass silently
            continue;
        }

        newTransaction (proc);
        const auto before = snapshot (proc);
        writeParam (proc, op.id, op.v);

        const auto mid = snapshot (proc);
        check (! bytesIdentical (before, mid), "op changed the visible state (mid != pre)");

        proc.undoSafe();
        const auto after = snapshot (proc);
        check (bytesIdentical (before, after), op.label);
        check (! proc.getUndoManager().canUndo(), "single transaction fully consumed by one undo");
    }

    // ------------------------------------------------------------------
    // [2] FX TYPE write: the engagement seeding re-writes 7 sibling params
    // re-entrantly inside the SAME transaction; one undo must still return
    // the full state exactly (type + seeds).
    // ------------------------------------------------------------------
    std::printf ("\n[2] fx1_type: plain write seeds NOTHING; the UI seam seeds + undoes atomically\n");
    {
        // Default slot: type None -> { enabled 0, drywet 0, param1..5 0 }.
        check (rawIs (proc, "fx1_type", 0.0f), "precondition: fx1 slot starts as None");

        // (a) W10: a PLAIN param write (host automation / NRPN stand-in) must
        //     NOT seed — the listener never seeds anymore.
        newTransaction (proc);
        writeParam (proc, "fx1_type", (float) FxType::Overdrive);
        check (rawIs (proc, "fx1_type", (float) FxType::Overdrive), "plain write: type moved");
        check (rawIs (proc, "fx1_enabled", 0.0f) && rawIs (proc, "fx1_drywet", 0.0f)
                   && rawIs (proc, "fx1_param1", 0.0f),
               "plain write: NO engagement seeding (automation cannot clobber)");
        proc.undoSafe();
        check (rawIs (proc, "fx1_type", 0.0f), "plain write undone (back to None)");

        // (b) The UI-pick shape (W10): the seam seeds BEFORE the type write,
        //     all in ONE transaction. Overdrive engagement defaults
        //     (FxSlotCard.cpp fxTypeDefaults): enabled=1, drywet=80,
        //     param1=50. Pinning the exact table values proves the SEEDING
        //     ran, not just that "something changed".
        newTransaction (proc);
        const auto before = snapshot (proc);
        if (fx1 != nullptr)
            fx1->seedEngagementDefaultsForType (static_cast<int> (FxType::Overdrive));
        writeParam (proc, "fx1_type", (float) FxType::Overdrive);
        check (rawIs (proc, "fx1_enabled", 1.0f),  "engagement seed: fx1_enabled -> 1");
        check (rawIs (proc, "fx1_drywet", 80.0f), "engagement seed: fx1_drywet -> 80");
        check (rawIs (proc, "fx1_param1", 50.0f), "engagement seed: fx1_param1 -> 50");

        proc.undoSafe();
        const auto after = snapshot (proc);
        check (bytesIdentical (before, after), "seed writes + type undone in ONE step (full state byte-equal)");
    }

    // ------------------------------------------------------------------
    // [3] W7 lane-A class: undoing a type switch must restore the previous
    // type's USER values, not re-apply engagement defaults on the replayed
    // type write. (W10: the listener never seeds at all now, so the replay
    // is trivially silent — pinned anyway: re-introducing listener-side
    // seeding must fail here.)
    // ------------------------------------------------------------------
    std::printf ("\n[3] type-switch undo restores USER values, not seeds\n");
    {
        // Start clean: Chorus via the UI seam (seeds param1=45), then a
        // DISTINCTIVE user param.
        newTransaction (proc);
        if (fx1 != nullptr)
            fx1->seedEngagementDefaultsForType (static_cast<int> (FxType::Chorus));
        writeParam (proc, "fx1_type", (float) FxType::Chorus);
        newTransaction (proc);
        writeParam (proc, "fx1_param1", 33.0f);                  // the user's value
        check (rawIs (proc, "fx1_type", (float) FxType::Chorus), "precondition: type is Chorus");
        check (rawIs (proc, "fx1_param1", 33.0f), "precondition: user param1 = 33");

        // Switch to Flanger via the seam in its own transaction: seeds
        // param1=40 (and the other Flanger defaults) INSIDE that transaction.
        newTransaction (proc);
        if (fx1 != nullptr)
            fx1->seedEngagementDefaultsForType (static_cast<int> (FxType::Flanger));
        writeParam (proc, "fx1_type", (float) FxType::Flanger);
        check (rawIs (proc, "fx1_param1", 40.0f), "Flanger switch seeded param1 = 40 (engagement ran)");

        proc.undoSafe();
        check (rawIs (proc, "fx1_type", (float) FxType::Chorus), "undo: type back to Chorus");
        // THE bug class: the replayed type write must NOT re-seed. 40 = the
        // Flanger seed (clobber), 45 = the Chorus seed (also wrong — the
        // user's 33 must survive). Assert the exact user value.
        check (rawIs (proc, "fx1_param1", 33.0f),
               "undo: param1 back to the USER value 33 (not Flanger's 40, not Chorus's seed 45)");
    }

    // ------------------------------------------------------------------
    // [4] W2 doctrine: undo cannot cross a part switch. After switching to
    // part B, undoSafe() must sweep to canUndo()==false (no replay into B),
    // part B's engine bytes stay untouched, part A keeps its edit, and the
    // switch itself survives.
    // ------------------------------------------------------------------
    std::printf ("\n[4] part-switch doctrine: stack invalidated, no cross-part replay\n");
    {
        // Edit part A (0, the current part): a distinctive patch byte.
        const uint8_t preEditByte = proc.getEngine().getPart (0).patchBytes[1];
        newTransaction (proc);
        writeParam (proc, "osc1_param", 99.0f);
        const uint8_t postEditByte = proc.getEngine().getPart (0).patchBytes[1];
        check (postEditByte != preEditByte, "precondition: part A's edit landed (patch byte moved)");

        const auto partBBefore = readPartBytes (proc, 1);

        // Switch to part B (2 = 1-based part 2 = index 1). onPartSelect runs
        // the W2 guard: non-undoable display dump + clearUndoHistory + the
        // undoInvalidatedByPartSwitch_ flag for the straggler sweep.
        newTransaction (proc);
        writeParam (proc, "part_select", 2.0f);
        check (proc.getEngine().getCurrentPart() == 1, "precondition: switched to part B (index 1)");

        proc.undoSafe();
        check (! proc.getUndoManager().canUndo(), "undoSafe swept the stack: canUndo == false");

        const auto partBAfter = readPartBytes (proc, 1);
        check (partBytesEqual (partBBefore, partBAfter),
               "part B bytes UNTOUCHED by the swept undo (no cross-part replay)");

        check (proc.getEngine().getPart (0).patchBytes[1] == postEditByte,
               "part A retains its edit (undo did not replay into part A either)");

        check (rawIs (proc, "part_select", 2.0f),
               "the switch itself survives (not undoable by design)");
    }

    const bool ok = g_failures == 0;
    std::printf ("\n%s\n", ok ? "UNDO PROPERTY TEST: ALL CHECKS PASSED (0 failures)"
                              : "UNDO PROPERTY TEST: FAILURES");
    if (! ok) std::printf ("UNDO PROPERTY TEST: %d failures\n", g_failures);
    return ok;
}
