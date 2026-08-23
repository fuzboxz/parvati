// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// PresetBrowser — a button showing the current patch name that opens a
// HIERARCHICAL juce::PopupMenu, replacing the flat patch ComboBox. Categories:
//   Templates ▸ (.parvati templates)
//   User ▸ (recursively scanned nested folders; .PRO/.MUL/.parvati)
//   Factory ▸ A / B / F / S ▸ (.PRO patches per bank)
//   Multi ▸ (factory .MUL multis)
// The menu tree is rebuilt from a disk CACHE (W10, lane-A finding 5): the
// scan + the per-.PRO name parse run at most once per GENERATION — the editor
// bumps the generation via invalidate() after every successful save, and the
// cache also self-invalidates when any previously-seen directory's mtime
// changed (external adds/removes/renames, e.g. the iOS Files app). Each leaf
// carries its own File -> onSelect callback (no external ID map).
// Load… / drag-drop are unaffected (handled by the editor).
//
// Implementation lives in PresetBrowser.cpp (the class previously carried its
// full ~400-line implementation in this header).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <vector>

class PresetBrowser : public juce::Component
{
public:
    using OnSelect = std::function<void (const juce::File&)>;

    PresetBrowser (juce::File templatesDir, juce::File userDir,
                   juce::File factoryDir, juce::File factoryMultiDir, OnSelect onSelect);

    /** Update the button label to the loaded program name (empty => placeholder). */
    void setCurrentName (const juce::String& name);

    /** Drop the cached preset tree (W10): the NEXT buildMenu()/showMenu()
        rescans the directories and re-parses .PRO names. The editor calls
        this after every SUCCESSFUL save (a new/overwritten file must appear
        at the next open); loads need no invalidation (they change no files).
        Externally modified directories (Files app) are caught separately by
        the mtime check, so they do not depend on this call. */
    void invalidate();

    // ---- prev/next stepping (header [ ] / Cmd+[/] shortcuts) ----
    /** The file currently marked as loaded in this browser (set by a menu
        pick, setCurrentFile(), or a step). Invalid until something sets it.
        The editor mirrors every successful load (menu pick, Load... picker,
        drag-drop, template) through setCurrentFile(). */
    const juce::File& getCurrentFile() const noexcept { return currentFile_; }

    /** Inform the browser of a load that happened OUTSIDE the menu (Load...
        dialog, drag-drop, host state restore) so stepping continues from the
        ACTUAL current preset. No-op-safe for a file not in the tree (stepping
        then starts from the list ends — see selectNext). */
    void setCurrentFile (const juce::File& f) { currentFile_ = f; }

    /** Step to the next preset in the flattened menu order (Factory A/B/F/S,
        Multi, User recursive, Templates), selecting it via the SAME onSelect_
        callback a menu pick fires. WRAPS at the end (back to the first leaf);
        when the current file is not in the list, the FIRST leaf is selected.
        @returns the newly selected file, or an invalid File when the tree has
        no leaves at all. */
    juce::File selectNext();

    /** Step to the previous preset — selectNext's mirror (wraps to the LAST
        leaf; a current file not in the list selects the last leaf).
        @returns the newly selected file, or an invalid File when empty. */
    juce::File selectPrev();

    void resized() override;

    // ---- cache observables (headless test seams; not used by the UI) ----
    /** Number of full disk scans performed since construction (a cached
        rebuild does not bump this). */
    int debugScanCount() const noexcept { return scanCount_; }
    /** Number of .PRO name parses performed since construction (parsing is
        cached with the tree — a cached rebuild does not re-parse). */
    int debugParseCount() const noexcept { return parseCount_; }
    /** True if the cached tree carries a leaf labelled @p label (exact). */
    bool debugTreeHasLeafLabel (const juce::String& label) const;

    /** Total number of leaves in the cached tree, counted recursively (the
        symlink-refusal test uses it: a followed directory link would double
        every leaf under it). */
    int debugTreeLeafCount() const;

    /** iOS picker compensation (F-ios-files-1, iOS hunt 2026-08-19).

        On iOS the save/load FileChooser can only write into document-provider
        locations (On My iPad / iCloud / third-party) — the shared App-Group
        container this browser's USER tree lives in is NOT part of any provider
        tree, so a preset saved through the picker NEVER appeared in the preset
        menu. The editor's applyPatchFile compensates on the LOAD side: after a
        successful load of a file OUTSIDE the USER tree it atomically imports a
        copy here, then invalidate()s the cache so the next open shows it.

        Static + parameterized by @p userDir (not this browser's dir) so the
        headless tests drive the exact same code path with a temp tree; also
        compiled on desktop (the editor's CALL is JUCE_IOS-gated — desktop users
        organize files deliberately).

        Copy semantics: same FILENAME at the USER root (provider folders are not
        USER banks); an existing file of that name is OVERWRITTEN (the user just
        deliberately picked this file); the write is ATOMIC (juce::TemporaryFile
        + rename — the house pattern), so an interrupted copy can never leave a
        torn preset at the visible destination.

        @returns the imported destination file, or an invalid File when nothing
        was imported (already inside the tree, not a preset extension, or the
        copy failed — all non-fatal; the load itself already succeeded). */
    static juce::File importIntoUserTree (const juce::File& file, const juce::File& userDir);

    /** F-ios-lc-4 (bug hunt 2026-08-19): one-way ADDITIVE mirror of a source
        tree into @p destDir — the containing app's Documents/Parvati/USER in
        production use. Presets saved from inside an AUv3 host (AUM/GB) land
        in the shared App-Group USER tree, but the per-save Documents mirror
        writes into the EXTENSION's private container (invisible to Files);
        this sync, run at STANDALONE launch, publishes them.

        Semantics (deliberately conservative):
          - a file is copied when absent in dest OR source mtime > dest mtime
            (newest wins; a re-run with no changes copies NOTHING — idempotent);
          - the dest copy's mtime is SET TO the source's, so comparisons stay
            exact across syncs instead of racing copy-time;
          - `*_temp*` files are skipped (another process's in-flight atomic
            write — the F-ios-files-4 lesson); symlinks are not followed;
          - NOTHING in dest is ever deleted: Files-app users may organize/
            delete mirrored copies deliberately, and a launch hook must never
            be destructive.
        Returns the number of files copied (test observable). */
    static int syncTreeNewestWins (const juce::File& sourceDir, const juce::File& destDir);

    /** Build the preset menu from the cache (rescanning first if the cache
        was invalidated or a watched directory changed). Public so the
        headless tests drive the exact showMenu() menu-build path — only the
        final showMenuAsync needs a desktop. */
    void buildMenu (juce::PopupMenu& menu);

    void showMenu();

private:
    // ---- the cached menu tree (pure data; PopupMenu is built from it) ----
    struct Leaf { juce::File file; juce::String label; };
    struct MenuNode
    {
        juce::String title;                       // sub-menu title (folder / bank name)
        std::vector<MenuNode> subs;
        std::vector<Leaf> leaves;
    };

    // F-ios-perf-4 (iOS hunt 2026-08-19): bounds for the recursive USER scan.
    // An unbounded walk stalled the message thread for seconds when the USER
    // tree was large or linked (measured 3.8-5.1 s against a big tree); the
    // app-managed tree is small, but a user-copied folder structure (Files
    // app into the mirrored Documents, a desktop library moved in) is not.
    static constexpr int kScanMaxDepth          = 8;     // folders below this stay unscanned
    static constexpr int kScanMaxEntriesPerDir  = 512;   // sorted entries beyond this stay unscanned

    // subs layout produced by scanInto (parallel to kBanks + the fixed tail):
    //   0..3 = factory banks A/B/F/S, 4 = factory multi, 5 = user, 6 = templates.
    void scanInto (MenuNode& root);

    void recordDir (const juce::File& dir);
    // F-w10-4 (bug hunt 2026-08-18): record a watch ROOT even when it does
    // not (yet) exist — external creation mid-session then flips `present`
    // and invalidates the cache. A root that stays absent is NOT a change
    // (the empty subtree stays honestly empty).
    void recordRoot (const juce::File& dir);
    // A watched directory's mtime moved, appeared or vanished — the entry
    // list can no longer match the cache. One stat per previously-seen dir;
    // NO filesystem watcher threads. Catches external adds/removes/renames
    // (including NEW folders: creating a child changes the parent's mtime)
    // and, since W11, externally created ROOT directories (they start absent
    // and flip `present`). RESIDUAL, accepted by design: a write that
    // PRESERVES the directory mtime exactly (same-millisecond on coarse
    // filesystems; rsync -a style timestamp restores) stays cached-stale
    // until the next save-invalidate or any other real change — the editor
    // save paths call invalidate() precisely for that case.
    bool watchedDirsChanged() const;

    MenuNode& scanFlatInto (MenuNode& node, const juce::File& dir, const juce::String& wildcard, bool parseName);

    void scanRecursiveInto (MenuNode& node, const juce::File& dir, int depth = 0);

    juce::PopupMenu menuFromNode (const MenuNode& node);

    static void addSubIfAny (juce::PopupMenu& parent, const juce::String& title, juce::PopupMenu sub);

    static bool treeHasLeafLabel (const MenuNode& node, const juce::String& label);

    static int treeLeafCount (const MenuNode& node);

    void addLeaf (juce::PopupMenu& m, const juce::File& f, const juce::String& label);

    // A leaf became the loaded preset — either from a menu pick or a step.
    // Records it (so stepping continues from here) and fires the editor's
    // onSelect_ seam (the exact path a menu pick takes).
    void selectLeaf (const juce::File& f);

    // ---- flattened stepping ----
    // The step order mirrors the MENU order exactly: for each node, submenus
    // first (recursively), then leaves — the same traversal menuFromNode
    // uses, over subs[0..6] (Factory banks A/B/F/S, Multi, User, Templates).
    static void flattenLeaves (const MenuNode& node, std::vector<Leaf>& out);

    juce::File stepSelection (int dir);

    juce::String patchLabel (const juce::File& f, bool parseName);

    juce::TextButton nameBtn_;
    juce::File templatesDir_, userDir_, factoryDir_, factoryMultiDir_;
    OnSelect onSelect_;
    // The file the editor last loaded (stepping's anchor). Only ever set via
    // selectLeaf (a menu pick / a step) or setCurrentFile (an out-of-menu
    // load the editor reports) — never guessed from the button label.
    juce::File currentFile_;

    // ---- the cache (W10) ----
    MenuNode cachedTree_;
    bool cacheValid_ = false;
    std::map<juce::String, std::pair<juce::Time, bool>> dirMtimes_;   // dir path -> {mtime at scan time, existed then} (F-w10-4 roots track presence)
    int scanCount_  = 0;
    int parseCount_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};
