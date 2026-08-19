// iOS open-in routing regressions (bug hunt 2026-08-19, key: open-in).
//
// Completes the open-in loop's DETERMINISTIC half. Document types/UTIs are
// declared (ios/parvati_filetypes.plist) so iOS offers "Open in Parvati" for
// .parvati/.PRO/.MUL, and Source/ui/IosOpenIn.{h,mm} route the
// handed file into Parvati's shared storage. The Obj-C++ delegate shim
// (application:openURL:options: added to JUCE's JuceAppStartupDelegate) is
// proven by the iOS-simulator Parvati_Standalone build; THIS test drives the
// pure core compiled for desktop:
//   [1] routeOpenedFile presets: outside .parvati/.PRO/.MUL → USER tree
//       (exists, bytes identical, no *_temp leftovers, collision overwrite);
//       an already-inside file returns ITSELF without duplicating; a .txt
//       returns invalid and imports nothing.
//   [2] former tuning kinds: .scl/.kbm (removed with the custom-tuning
//       subsystem, 2026-08-19) route NOWHERE — the kind predicate returns
//       None and nothing is parked or created.
//   [3] openInKindForFile table: the extension → kind predicate.
//   [4] Idempotence/route failures: a nonexistent preset file routes nowhere
//       (invalid File), a hostile extension never touches the trees.
//
// Built by default. Run: ./build/parvati_ios_openin_test

#include <cstdio>
#include <cstring>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "ui/IosOpenIn.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool filesByteEqual (const juce::File& a, const juce::File& b)
{
    juce::MemoryBlock ma, mb;
    if (! a.loadFileAsData (ma) || ! b.loadFileAsData (mb))
        return false;
    return ma == mb;
}

int countTempFragments (const juce::File& dir)
{
    juce::Array<juce::File> entries;
    dir.findChildFiles (entries, juce::File::findFiles, true);
    int n = 0;
    for (const auto& e : entries)
        if (e.getFileName().contains ("_temp"))
            ++n;
    return n;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("parvati_ios_openin_test");
    tmp.deleteRecursively();
    const auto groupRoot = tmp.getChildFile ("group");   // stands in for the app-group root
    const auto userDir   = groupRoot.getChildFile ("Parvati").getChildFile ("USER");
    const auto inbox     = tmp.getChildFile ("Inbox");  // stands in for iOS's copy-in location
    check (userDir.createDirectory(), "test setup: USER tree created");
    check (inbox.createDirectory(),   "test setup: Inbox stand-in created");

    // ------------------------------------------------------------------
    std::printf ("[1] routeOpenedFile presets -> USER tree\n");
    {
        const auto srcParvati = inbox.getChildFile ("SentPatch.parvati");
        check (srcParvati.replaceWithText (
                   "format: parvati-multi\nversion: 1\nname: \"Sent\"\nparts:\n"
                   "  - name: \"A\"\n    voice_slots: 4\n"), "setup: .parvati written");
        const auto srcPro = inbox.getChildFile ("Sent.PRO");
        check (srcPro.replaceWithText ("pro-bytes-here"), "setup: .PRO written");
        const auto srcMul = inbox.getChildFile ("Sent.MUL");
        check (srcMul.replaceWithText ("mul-bytes-here"), "setup: .MUL written");

        const auto r1 = parvati::routeOpenedFile (srcParvati, userDir);
        check (r1 == userDir.getChildFile ("SentPatch.parvati") && r1.existsAsFile(),
               "[1] .parvati routed into USER/<name>");
        check (filesByteEqual (r1, srcParvati), "[1] routed .parvati bytes identical");

        const auto r2 = parvati::routeOpenedFile (srcPro, userDir);
        check (r2 == userDir.getChildFile ("Sent.PRO") && r2.existsAsFile()
                   && filesByteEqual (r2, srcPro),
               "[1] .PRO routed + identical");
        const auto r3 = parvati::routeOpenedFile (srcMul, userDir);
        check (r3 == userDir.getChildFile ("Sent.MUL") && r3.existsAsFile()
                   && filesByteEqual (r3, srcMul),
               "[1] .MUL routed + identical");
        check (countTempFragments (userDir) == 0, "[1] no *_temp fragments in USER");

        // Collision overwrite: the user re-sends an updated patch with the
        // same name — the route replaces the stale copy atomically.
        check (srcParvati.replaceWithText (
                   "format: parvati-multi\nversion: 1\nname: \"Sent2\"\nparts:\n"
                   "  - name: \"A\"\n    voice_slots: 4\n"), "setup: updated .parvati");
        const auto r1b = parvati::routeOpenedFile (srcParvati, userDir);
        check (r1b.existsAsFile() && filesByteEqual (r1b, srcParvati),
               "[1] same-name re-route OVERWRITES with the new bytes");

        // Already-inside: deliver itself, do not duplicate.
        const auto rInside = parvati::routeOpenedFile (r1b, userDir);
        check (rInside == r1b, "[1] already-inside preset returns ITSELF");
        check (countTempFragments (userDir) == 0, "[1] inside-route duplicated nothing");

        // Non-document: invalid, imports nothing.
        const auto srcTxt = inbox.getChildFile ("readme.txt");
        check (srcTxt.replaceWithText ("not a patch"), "setup: .txt written");
        const auto rTxt = parvati::routeOpenedFile (srcTxt, userDir);
        check (! rTxt.existsAsFile(), "[1] .txt routes to an INVALID File");
        check (! userDir.getChildFile ("readme.txt").existsAsFile(),
               "[1] .txt imported NOTHING into USER");
    }

    // ------------------------------------------------------------------
    std::printf ("[2] former tuning kinds route nowhere (custom tuning removed)\n");
    {
        const auto srcScl = inbox.getChildFile ("19edo.scl");
        check (srcScl.replaceWithText ("! 19edo.scl\n!\n 19\n 63.15789473684\n"),
               "setup: .scl written");
        const auto srcKbm = inbox.getChildFile ("map19.kbm");
        check (srcKbm.replaceWithText ("! map19.kbm\n 12\n 0\n 127\n 60\n 60\n 261.6255653\n 0\n"),
               "setup: .kbm written");

        const auto tuningDir = groupRoot.getChildFile ("Parvati").getChildFile ("Tuning");
        const auto rs = parvati::routeOpenedFile (srcScl, userDir);
        check (rs == juce::File() && ! rs.existsAsFile(),
               "[2] .scl routes to an INVALID File (no parking)");
        const auto rk = parvati::routeOpenedFile (srcKbm, userDir);
        check (rk == juce::File() && ! rk.existsAsFile(),
               "[2] .kbm routes to an INVALID File (no parking)");
        check (! tuningDir.exists(), "[2] no Parvati/Tuning directory is created");

        // Nothing landed inside USER either (menu stays clean).
        juce::Array<juce::File> leaves;
        userDir.findChildFiles (leaves, juce::File::findFiles, true);
        bool sclInUser = false;
        for (const auto& l : leaves)
            if (l.hasFileExtension (".scl") || l.hasFileExtension (".kbm"))
                sclInUser = true;
        check (! sclInUser, "[2] no .scl/.kbm landed inside USER");
    }

    // ------------------------------------------------------------------
    std::printf ("[3] openInKindForFile extension table\n");
    {
        using K = parvati::OpenInKind;
        struct Row { const char* name; K kind; };
        const Row rows[] = {
            { "x.parvati", K::Preset }, { "x.PRO", K::Preset },
            { "x.Pro", K::Preset },     { "x.pro", K::Preset },
            { "x.MUL", K::Preset },     { "x.mul", K::Preset },
            { "x.scl", K::None },       { "x.SCL", K::None },
            { "x.kbm", K::None },       { "x.KBM", K::None },
            { "x.txt", K::None },       { "x.mid", K::None },
            { "x", K::None },           { "x.wav", K::None },
        };
        for (const auto& r : rows)
        {
            const juce::File f = tmp.getChildFile (r.name);
            char msg[96];
            (void) std::snprintf (msg, sizeof (msg), "[3] %s -> kind %d", r.name, (int) r.kind);
            check (parvati::openInKindForFile (f) == r.kind, msg);
        }
    }

    // ------------------------------------------------------------------
    std::printf ("[4] route failures are inert\n");
    {
        const auto ghost = inbox.getChildFile ("ghost.parvati");   // never written
        check (! ghost.existsAsFile(), "setup: ghost really absent");
        const auto rGhost = parvati::routeOpenedFile (ghost, userDir);
        check (! rGhost.existsAsFile(), "[4] nonexistent preset routes INVALID");
        check (! userDir.getChildFile ("ghost.parvati").existsAsFile(),
               "[4] nothing materialized in USER");
        // Empty user dir (the no-app-group fallback shape): everything inert.
        const auto rNoDir = parvati::routeOpenedFile (inbox.getChildFile ("Sent.PRO"), {});
        check (! rNoDir.existsAsFile(), "[4] empty userPatchDir routes INVALID");
    }

    tmp.deleteRecursively();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "IOS OPEN-IN TEST: FAILURES" : "IOS OPEN-IN TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
