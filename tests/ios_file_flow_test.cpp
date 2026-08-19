// iOS file-flow regressions (bug hunt 2026-08-19, key: ios-files).
//
// Covers the four remediations from the iOS hunt:
//   [1] F-ios-files-1  import-on-load: a picker-loaded preset that lives
//       OUTSIDE the USER tree is atomically imported into it (same filename,
//       collision overwrites, no *_temp fragments), and the import is visible
//       to the PresetBrowser scan on the next buildMenu (mtime pickup — the
//       same observable the editor's invalidate() produces on iOS).
//   [2] F-ios-files-4  the installer's stale-template sweep must NOT delete
//       another process's in-flight atomic write (JUCE TemporaryFile names
//       "<name>_temp<hex>" in the TARGET directory).
//   [3] F-ios-perf-5   the installer's version-marker fast path: a completed
//       pass writes the marker; a subsequent run still content-syncs the
//       TEMPLATES (the fast path skips only the bank walk).
//   [4] F-ios-perf-4   PresetBrowser scan bounds: depth cap, per-directory
//       entry cap and symlink refusal — a hostile USER tree cannot stall the
//       message thread.
//
// The editor-side glue (applyPatchFile's import call + invalidate +
// mirrorUserSaveToDocumentsIOS) is JUCE_IOS-gated and thus not compiled here;
// the SHARED static helper (PresetBrowser::importIntoUserTree) is driven
// directly, which is the exact code the iOS path calls.
//
// Built by default. Run: ./build/parvati_ios_file_flow_test

#include <chrono>
#include <cstdio>
#include <cstring>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/FactoryPresetInstaller.h"
#include "ui/PresetBrowser.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// A minimal but valid .parvati multi document (the format the picker loads and
// the import copies — the exact text shape used by parvati_preset_test).
const char* kParvatiMultiText =
    "format: parvati-multi\n"
    "version: 1\n"
    "name: \"FlowTest\"\n"
    "parts:\n"
    "  - name: \"A\"\n    voice_slots: 4\n"
    "  - name: \"B\"\n    voice_slots: 4\n"
    "  - name: \"C\"\n    voice_slots: 4\n"
    "  - name: \"D\"\n    voice_slots: 4\n"
    "  - name: \"E\"\n    voice_slots: 4\n"
    "  - name: \"F\"\n    voice_slots: 4\n";

bool filesByteEqual (const juce::File& a, const juce::File& b)
{
    juce::MemoryBlock ma, mb;
    if (! a.loadFileAsData (ma) || ! b.loadFileAsData (mb))
        return false;
    return ma == mb;
}

int countTempFragments (const juce::File& dir)
{
    juce::Array<juce::File> files;
    dir.findChildFiles (files, juce::File::findFiles, true);
    int n = 0;
    for (const auto& f : files)
        if (f.getFileName().contains ("_temp"))
            ++n;
    return n;
}

// Fresh temp workspace with the four roots the PresetBrowser wants.
struct Workspace
{
    juce::File root, user, factory, factoryMulti, templates;

    explicit Workspace (const juce::String& tag)
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("parvati_iosfileflow_" + tag);
        root.deleteRecursively();
        root.createDirectory();
        user         = root.getChildFile ("USER");
        factory      = root.getChildFile ("FACTORY");
        factoryMulti = root.getChildFile ("FACTORY_MULTI");
        templates    = root.getChildFile ("TEMPLATES");
        user.createDirectory();
        templates.createDirectory();
    }

    ~Workspace() { root.deleteRecursively(); }
};
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInit;

    // ------------------------------------------------------------------
    std::printf ("[1] import-on-load into the USER tree (F-ios-files-1)\n");
    {
        Workspace ws ("import");

        // A .parvati picked from a provider location OUTSIDE the USER tree.
        const juce::File picked = ws.root.getChildFile ("OnMyiPad").getChildFile ("Picked.parvati");
        picked.getParentDirectory().createDirectory();
        picked.replaceWithText (kParvatiMultiText);

        // Extension guard: a non-preset file is never imported.
        const juce::File note = ws.root.getChildFile ("OnMyiPad").getChildFile ("readme.txt");
        note.replaceWithText ("not a patch");

        // (a) The import copies the file into USER, byte-identical.
        const juce::File imported = PresetBrowser::importIntoUserTree (picked, ws.user);
        check (imported == ws.user.getChildFile ("Picked.parvati"),
               "[1] destination is USER/<same filename>");
        check (imported.existsAsFile(), "[1] imported file exists");
        check (filesByteEqual (imported, picked), "[1] contents byte-identical to the picked file");
        check (countTempFragments (ws.user) == 0, "[1] atomic: no *_temp fragments left in USER");

        // (b) Collision: a pre-existing USER file of the same name is
        //     OVERWRITTEN (the user just deliberately picked this file).
        ws.user.getChildFile ("Picked.parvati").replaceWithText ("stale older copy");
        const juce::File reimported = PresetBrowser::importIntoUserTree (picked, ws.user);
        check (reimported.existsAsFile() && filesByteEqual (reimported, picked),
               "[1] name collision overwrites with the picked content");

        // (c) Guards: files already inside USER / non-preset extensions return
        //     an invalid File and copy nothing.
        check (! PresetBrowser::importIntoUserTree (imported, ws.user).existsAsFile(),
               "[1] a file already inside USER is not re-imported");
        check (! PresetBrowser::importIntoUserTree (note, ws.user).existsAsFile(),
               "[1] a non-preset extension is not imported");
        check (! ws.user.getChildFile ("readme.txt").existsAsFile(),
               "[1] the non-preset file was not copied into USER");

        // (d) The browser contract: after the import, the next buildMenu
        //     RESCANS (the USER mtime moved) and the new leaf is present —
        //     the exact observable the iOS editor's invalidate() produces.
        PresetBrowser browser (ws.templates, ws.user, ws.factory, ws.factoryMulti,
                               [] (const juce::File&) {});
        juce::PopupMenu m1;
        browser.buildMenu (m1);   // scan 1: sees Picked.parvati
        check (browser.debugScanCount() == 1, "[1] first open scans once");
        check (browser.debugTreeHasLeafLabel ("Picked"),
               "[1] the imported preset is in the browser's USER tree");

        // A LATER external pick appears at the next open the same way.
        const juce::File later = ws.root.getChildFile ("OnMyiPad").getChildFile ("Later.parvati");
        later.replaceWithText (kParvatiMultiText);
        const juce::File laterImported = PresetBrowser::importIntoUserTree (later, ws.user);
        check (laterImported.existsAsFile(), "[1] second import lands");
        ws.user.setLastModificationTime (juce::Time::getCurrentTime()
                                             + juce::RelativeTime::seconds (2));
        juce::PopupMenu m2;
        browser.buildMenu (m2);   // mtime moved -> rescan
        check (browser.debugScanCount() == 2, "[1] post-import open rescans (the invalidate seam's observable)");
        check (browser.debugTreeHasLeafLabel ("Later"),
               "[1] the second imported preset appears at the next open");
    }

    // ------------------------------------------------------------------
    std::printf ("\n[2] installer sweep never deletes an in-flight temp (F-ios-files-4)\n");
    {
        Workspace ws ("sweep");
        // Decoy: the documented JUCE TemporaryFile shape in the TARGET dir —
        // another process's atomic template write mid-rename. Pre-fix the
        // stale-template sweep deleted it (it is not in the embedded set).
        const juce::File decoy = ws.templates.getChildFile ("Poly_tempdeadbeef.parvati");
        decoy.replaceWithText ("in-flight atomic write");

        parvati::ensureFactoryPresetsInstalled (ws.factory, ws.factoryMulti, ws.templates, ws.user);
        check (decoy.existsAsFile(),
               "[2] the *_temp decoy SURVIVES the stale-template sweep");

        // Control: a genuinely stale non-temp template is still removed.
        const juce::File stale = ws.templates.getChildFile ("Zzz_deffo_stale.parvati");
        stale.replaceWithText ("not embedded");
        parvati::resetInstallOnceForTest();
        parvati::ensureFactoryPresetsInstalled (ws.factory, ws.factoryMulti, ws.templates, ws.user);
        check (! stale.existsAsFile(), "[2] control: a stale non-temp template IS removed");
        check (decoy.existsAsFile(), "[2] control: the temp decoy still survives");
    }

    // ------------------------------------------------------------------
    std::printf ("\n[3] installer version-marker fast path (F-ios-perf-5)\n");
    {
        Workspace ws ("marker");

        // First run (no marker): full pass writes the banks + templates and
        // the completion marker. (The once-guard is process-wide — [2] already
        // ran an install on ITS dirs — so arm a fresh pass here.)
        parvati::resetInstallOnceForTest();
        parvati::ensureFactoryPresetsInstalled (ws.factory, ws.factoryMulti, ws.templates, ws.user);
        const juce::File marker = ws.factory.getChildFile (".factory-install");
        check (marker.existsAsFile(), "[3] a completed pass writes the marker");
        {
            juce::String txt;
            if (juce::FileInputStream in (marker); in.openedOk())
                txt = in.readEntireStreamAsString().trim();
            check (txt == "installed=" + juce::String (parvati::kFactoryInstallVersion),
                   "[3] marker content carries the install version");
        }

        // A bank file + a template exist from the full pass.
        juce::Array<juce::File> pros;
        ws.factory.findChildFiles (pros, juce::File::findFiles, true, "*.PRO");
        check (pros.size() > 0, "[3] factory .PRO files installed by the full pass");
        juce::Array<juce::File> pars;
        ws.templates.findChildFiles (pars, juce::File::findFiles, false, "*.parvati");
        check (pars.size() > 0, "[3] stock templates installed by the full pass");
        const juce::String someTemplate = pars[0].getFileName();
        const juce::MemoryBlock goodTemplate = [&]
        {
            juce::MemoryBlock mb;
            pars[0].loadFileAsData (mb);
            return mb;
        }();

        // Corrupt the template; second run with a VALID marker takes the FAST
        // path but the TEMPLATES content-sync must still restore it.
        pars[0].replaceWithText ("user-corrupted template content");
        parvati::resetInstallOnceForTest();
        parvati::ensureFactoryPresetsInstalled (ws.factory, ws.factoryMulti, ws.templates, ws.user);
        {
            juce::MemoryBlock after;
            pars[0].loadFileAsData (after);
            check (after == goodTemplate,
                   "[3] fast path still content-syncs TEMPLATES (corruption restored)");
        }
        check (marker.existsAsFile(), "[3] marker intact after the fast-path run");
        juce::Array<juce::File> pros2;
        ws.factory.findChildFiles (pros2, juce::File::findFiles, true, "*.PRO");
        check (pros2.size() == pros.size(), "[3] fast path leaves the installed banks alone");
        juce::ignoreUnused (someTemplate);

        // A CORRUPT marker must fall back to the full self-healing pass
        // (observable: a deleted bank file is re-extracted).
        if (! pros.isEmpty())
        {
            pros[0].deleteFile();
            marker.replaceWithText ("installed=garbage");
            parvati::resetInstallOnceForTest();
            parvati::ensureFactoryPresetsInstalled (ws.factory, ws.factoryMulti, ws.templates, ws.user);
            check (pros[0].existsAsFile(),
                   "[3] corrupt marker -> full pass re-extracts a deleted bank file");
        }
    }

    // ------------------------------------------------------------------
    std::printf ("\n[4] PresetBrowser scan bounds (F-ios-perf-4)\n");
    {
        Workspace ws ("bounds");

        // (a) Depth: a 20-level nested chain (short dir names stay far below
        //     PATH_MAX — a longer chain would truncate on the OS side and the
        //     check would pass vacuously) with a preset at the bottom (past
        //     the cap of 8) and one at depth 3 (inside it).
        juce::File deep = ws.user;
        for (int i = 0; i < 20; ++i)
        {
            deep = deep.getChildFile ("x" + juce::String (i));
            deep.createDirectory();
        }
        (void) deep.getChildFile ("TooDeep.parvati").replaceWithText (kParvatiMultiText);
        juce::File shallow = ws.user;
        for (int i = 0; i < 3; ++i)
            shallow = shallow.getChildFile ("s" + juce::String (i));
        shallow.createDirectory();
        (void) shallow.getChildFile ("Shallow.parvati").replaceWithText (kParvatiMultiText);

        {
            PresetBrowser browser (ws.templates, ws.user, ws.factory, ws.factoryMulti,
                                   [] (const juce::File&) {});
            const auto t0 = std::chrono::steady_clock::now();
            juce::PopupMenu m;
            browser.buildMenu (m);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now() - t0).count();
            char msg[128];
            std::snprintf (msg, sizeof (msg), "[4] deep tree scan completes fast (%lld ms)", (long long) ms);
            check (ms < 2000, msg);
            check (browser.debugScanCount() == 1, "[4] deep tree: exactly one scan");
            check (browser.debugTreeHasLeafLabel ("Shallow"),
                   "[4] a preset within the depth cap IS found");
            check (! browser.debugTreeHasLeafLabel ("TooDeep"),
                   "[4] a preset past the depth cap is NOT scanned");
        }

        // (b) Width: 6000 sibling files in the USER root; the first 512
        //     sorted entries are considered, the rest are not.
        {
            for (int i = 0; i < 6000; ++i)
                ws.user.getChildFile ("cap_" + juce::String (i).paddedLeft ('0', 4) + ".parvati")
                    .replaceWithText (kParvatiMultiText);

            PresetBrowser browser (ws.templates, ws.user, ws.factory, ws.factoryMulti,
                                   [] (const juce::File&) {});
            const auto t0 = std::chrono::steady_clock::now();
            juce::PopupMenu m;
            browser.buildMenu (m);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now() - t0).count();
            char msg[128];
            std::snprintf (msg, sizeof (msg), "[4] wide tree scan completes fast (%lld ms)", (long long) ms);
            check (ms < 2000, msg);
            check (browser.debugScanCount() == 1, "[4] wide tree: exactly one scan");
            check (browser.debugTreeHasLeafLabel ("cap_0000"),
                   "[4] within the entry cap: the first sorted file IS found");
            check (! browser.debugTreeHasLeafLabel ("cap_0600"),
                   "[4] past the entry cap: a later file is NOT scanned");
            // The Shallow preset from (a) shares the root's 512-entry budget
            // with the cap_* files — its absence here is the cap, not a bug.
        }

        // (c) Symlink refusal: a directory link into a real preset folder is
        //     not followed.
        {
            Workspace ws2 ("symlink");
            const juce::File real = ws2.user.getChildFile ("real");
            real.createDirectory();
            real.getChildFile ("InsideReal.parvati").replaceWithText (kParvatiMultiText);
            // File::createSymbolicLink(linkAt, overwrite): the RECEIVER is
            // the target — real.createSymbolicLink(link) creates `link -> real`.
            const juce::File link = ws2.user.getChildFile ("link");
            link.deleteRecursively (false);
            const bool linked = real.createSymbolicLink (link, true);

            PresetBrowser browser (ws2.templates, ws2.user, ws2.factory, ws2.factoryMulti,
                                   [] (const juce::File&) {});
            juce::PopupMenu m;
            browser.buildMenu (m);
            check (browser.debugTreeHasLeafLabel ("InsideReal"),
                   "[4] the real directory's preset IS found");
            if (linked)
                check (browser.debugTreeLeafCount() == 1,
                       "[4] a directory symlink is not followed (leaf count stays 1)");
            else
                std::printf ("     (symlink creation unavailable on this system — sub-check skipped)\n");
        }
    }

    // ------------------------------------------------------------------
    // [5] F-ios-lc-4: launch-sync publishes the shared USER tree into the
    //     containing app's Documents (Files-app visibility for AUv3-host
    //     saves). The sync is one-way ADDITIVE, newest-wins, atomic, and
    //     idempotent — pinned here against two temp dirs with controlled
    //     mtimes (deterministic: setLastModificationTime, no sleeps).
    // ------------------------------------------------------------------
    std::printf ("\n[5] syncTreeNewestWins: additive newest-wins launch mirror\n");
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_usersync_test");
        tmp.deleteRecursively();
        const auto shared = tmp.getChildFile ("SHARED_USER");   // the App-Group tree
        const auto docs  = tmp.getChildFile ("Documents/Parvati/USER");
        shared.createDirectory();

        const auto t0 = juce::Time (1780000000000LL);   // arbitrary fixed epoch ms
        // a.parvati: source OLDER than the dest copy -> must be SKIPPED.
        shared.getChildFile ("a.parvati").replaceWithText ("old");
        shared.getChildFile ("a.parvati").setLastModificationTime (t0);
        // b.PRO: absent in dest -> copied.
        shared.getChildFile ("b.PRO").replaceWithText ("B");
        shared.getChildFile ("b.PRO").setLastModificationTime (t0 + juce::RelativeTime::milliseconds (1000));
        // Nested: sub/c.MUL -> dest subdir created + copied.
        shared.getChildFile ("sub").createDirectory();
        shared.getChildFile ("sub/c.MUL").replaceWithText ("C");
        shared.getChildFile ("sub/c.MUL").setLastModificationTime (t0 + juce::RelativeTime::milliseconds (1000));
        // In-flight atomic write of another process -> skipped.
        shared.getChildFile ("z_tempdead.parvati").replaceWithText ("x");

        // Pre-existing dest a.parvati NEWER than source: newest wins -> kept.
        // (Create the FULL dest chain: replaceWithText does NOT create parent
        // directories — a missing USER/ dir would fail the write silently and
        // the sync would then legitimately copy the file.)
        docs.getParentDirectory().createDirectory();
        docs.createDirectory();
        docs.getChildFile ("a.parvati").replaceWithText ("dest-copy");
        docs.getChildFile ("a.parvati").setLastModificationTime (t0 + juce::RelativeTime::milliseconds (5000));

        const int copied = PresetBrowser::syncTreeNewestWins (shared, docs);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "[5] first sync copies exactly 2 (got %d)", copied);
        check (copied == 2, msg);
        check (docs.getChildFile ("b.PRO").loadFileAsString() == "B",
               "[5] absent file copied with identical bytes");
        check (docs.getChildFile ("sub/c.MUL").loadFileAsString() == "C",
               "[5] nested file copied into the created subdir");
        check (docs.getChildFile ("a.parvati").loadFileAsString() == "dest-copy",
               "[5] dest-newer file NOT overwritten (newest wins)");
        check (! docs.getChildFile ("z_tempdead.parvati").existsAsFile(),
               "[5] in-flight *_temp file skipped");
        check (docs.getChildFile ("b.PRO").getLastModificationTime()
                   == shared.getChildFile ("b.PRO").getLastModificationTime(),
               "[5] copied file carries the SOURCE mtime (exact next-sync compare)");

        // Idempotence: nothing changed -> a re-run copies NOTHING.
        const int copied2 = PresetBrowser::syncTreeNewestWins (shared, docs);
        check (copied2 == 0, "[5] re-sync with no changes copies 0 (idempotent)");

        // A later host save (source mtime moves) -> exactly that file re-syncs.
        shared.getChildFile ("b.PRO").replaceWithText ("B2");
        shared.getChildFile ("b.PRO").setLastModificationTime (t0 + juce::RelativeTime::milliseconds (9000));
        const int copied3 = PresetBrowser::syncTreeNewestWins (shared, docs);
        check (copied3 == 1 && docs.getChildFile ("b.PRO").loadFileAsString() == "B2",
               "[5] an updated preset re-syncs (newest wins, exactly one file)");

        // Destructive check: removing a SOURCE file never deletes the dest copy
        // (Files-app users may keep organized mirrors; the hook is additive).
        shared.getChildFile ("b.PRO").deleteFile();
        const int copied4 = PresetBrowser::syncTreeNewestWins (shared, docs);
        check (copied4 == 0 && docs.getChildFile ("b.PRO").existsAsFile(),
               "[5] source removal NEVER deletes the Documents copy (additive-only)");

        tmp.deleteRecursively();
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "IOS FILE FLOW TEST: FAILURES" : "IOS FILE FLOW TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
