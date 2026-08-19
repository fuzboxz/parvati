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

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PatchFile.h"   // AmbikaProgram + parseAmbikaProgramFile (.PRO names)

#include <functional>
#include <map>
#include <vector>

class PresetBrowser : public juce::Component
{
public:
    using OnSelect = std::function<void (const juce::File&)>;

    PresetBrowser (juce::File templatesDir, juce::File userDir,
                   juce::File factoryDir, juce::File factoryMultiDir, OnSelect onSelect)
        : templatesDir_ (std::move (templatesDir)), userDir_ (std::move (userDir)),
          factoryDir_ (std::move (factoryDir)), factoryMultiDir_ (std::move (factoryMultiDir)),
          onSelect_ (std::move (onSelect))
    {
        nameBtn_.setButtonText ("(select a patch)");
        nameBtn_.onClick = [this] { showMenu(); };
        addAndMakeVisible (nameBtn_);
    }

    /** Update the button label to the loaded program name (empty => placeholder). */
    void setCurrentName (const juce::String& name)
    {
        nameBtn_.setButtonText (name.isEmpty() ? TRANS ("(select a patch)") : name);
    }

    /** Drop the cached preset tree (W10): the NEXT buildMenu()/showMenu()
        rescans the directories and re-parses .PRO names. The editor calls
        this after every SUCCESSFUL save (a new/overwritten file must appear
        at the next open); loads need no invalidation (they change no files).
        Externally modified directories (Files app) are caught separately by
        the mtime check, so they do not depend on this call. */
    void invalidate() { cacheValid_ = false; }

    void resized() override { nameBtn_.setBounds (getLocalBounds()); }

    // ---- cache observables (headless test seams; not used by the UI) ----
    /** Number of full disk scans performed since construction (a cached
        rebuild does not bump this). */
    int debugScanCount() const noexcept { return scanCount_; }
    /** Number of .PRO name parses performed since construction (parsing is
        cached with the tree — a cached rebuild does not re-parse). */
    int debugParseCount() const noexcept { return parseCount_; }
    /** True if the cached tree carries a leaf labelled @p label (exact). */
    bool debugTreeHasLeafLabel (const juce::String& label) const
    {
        return treeHasLeafLabel (cachedTree_, label);
    }

    /** Total number of leaves in the cached tree, counted recursively (the
        symlink-refusal test uses it: a followed directory link would double
        every leaf under it). */
    int debugTreeLeafCount() const { return treeLeafCount (cachedTree_); }

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
    static juce::File importIntoUserTree (const juce::File& file, const juce::File& userDir)
    {
        if (userDir.getFullPathName().isEmpty() || file.isAChildOf (userDir))
            return {};
        if (! (file.hasFileExtension (".pro") || file.hasFileExtension (".mul")
               || file.hasFileExtension (".parvati")))
            return {};
        if (! file.existsAsFile())
            return {};
        juce::File dest = userDir.getChildFile (file.getFileName());
        if (! dest.getParentDirectory().createDirectory())
            return {};
        juce::TemporaryFile temp (dest);
        if (! file.copyFileTo (temp.getFile()))
            return {};
        if (! temp.overwriteTargetFileWithTemporary())
            return {};
        return dest;
    }

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

public:
    /** Build the preset menu from the cache (rescanning first if the cache
        was invalidated or a watched directory changed). Public so the
        headless tests drive the exact showMenu() menu-build path — only the
        final showMenuAsync needs a desktop. */
    void buildMenu (juce::PopupMenu& menu)
    {
        if (! cacheValid_ || watchedDirsChanged())
            scanInto (cachedTree_);
        menu = juce::PopupMenu();   // order: Factory (banks + Multi), User, Templates
        juce::PopupMenu factorySub;
        static const char* const kBanks[] = { "A", "B", "F", "S" };   // actual Ambika bank dirs
        for (size_t b = 0; b < sizeof (kBanks) / sizeof (kBanks[0]); ++b)
            if (cachedTree_.subs.size() > b && cachedTree_.subs[b].leaves.size() > 0)
                factorySub.addSubMenu (kBanks[b], menuFromNode (cachedTree_.subs[b]));
        // Factory multis (.MUL) nest at the bottom of Factory.
        addSubIfAny (factorySub, TRANS ("Multi"), menuFromNode (cachedTree_.subs[4]));
        if (factorySub.getNumItems() > 0)
            menu.addSubMenu (TRANS ("Factory"), factorySub);

        addSubIfAny (menu, TRANS ("User"), menuFromNode (cachedTree_.subs[5]));
        addSubIfAny (menu, TRANS ("Templates"), menuFromNode (cachedTree_.subs[6]));
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        buildMenu (menu);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (&nameBtn_));
    }

private:
    // subs layout produced by scanInto (parallel to kBanks + the fixed tail):
    //   0..3 = factory banks A/B/F/S, 4 = factory multi, 5 = user, 6 = templates.
    void scanInto (MenuNode& root)
    {
        root = MenuNode();
        dirMtimes_.clear();
        // F-w10-4: the four ROOTS are always recorded (present or not) so an
        // externally created root invalidates the cache. Nested dirs absent
        // at scan time are covered by their recorded parent's mtime.
        recordRoot (factoryDir_);
        recordRoot (factoryMultiDir_);
        recordRoot (userDir_);
        recordRoot (templatesDir_);
        static const char* const kBanks[] = { "A", "B", "F", "S" };
        for (const char* bank : kBanks)
            scanFlatInto (root.subs.emplace_back(), factoryDir_.getChildFile (bank), "*.PRO", true).title = bank;
        scanFlatInto (root.subs.emplace_back(), factoryMultiDir_, "*.MUL", false);
        scanRecursiveInto (root.subs.emplace_back(), userDir_);
        scanFlatInto (root.subs.emplace_back(), templatesDir_, "*.parvati", false);
        cacheValid_ = true;
        ++scanCount_;
    }

    void recordDir (const juce::File& dir)
    {
        dirMtimes_[dir.getFullPathName()] = { dir.getLastModificationTime(), true };
    }
    // F-w10-4 (bug hunt 2026-08-18): record a watch ROOT even when it does
    // not (yet) exist — external creation mid-session then flips `present`
    // and invalidates the cache. A root that stays absent is NOT a change
    // (the empty subtree stays honestly empty).
    void recordRoot (const juce::File& dir)
    {
        const bool present = dir.isDirectory();
        dirMtimes_[dir.getFullPathName()] = { dir.getLastModificationTime(), present };
    }
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
    bool watchedDirsChanged() const
    {
        for (const auto& [path, entry] : dirMtimes_)
        {
            const juce::File dir (path);
            const bool now = dir.isDirectory();
            if (now != entry.second)
                return true;
            if (entry.second && dir.getLastModificationTime() != entry.first)
                return true;
        }
        return false;
    }

    MenuNode& scanFlatInto (MenuNode& node, const juce::File& dir, const juce::String& wildcard, bool parseName)
    {
        if (! dir.isDirectory())
            return node;
        recordDir (dir);
        juce::Array<juce::File> files;
        dir.findChildFiles (files, juce::File::findFiles, false, wildcard);
        files.sort();
        for (const auto& f : files)
            node.leaves.push_back ({ f, patchLabel (f, parseName) });
        return node;
    }

    void scanRecursiveInto (MenuNode& node, const juce::File& dir, int depth = 0)
    {
        if (! dir.isDirectory())
            return;
        // F-ios-perf-4: depth cap — silently stop descending past kScanMaxDepth
        // (pathological nesting must not walk the whole tree).
        if (depth >= kScanMaxDepth)
            return;
        recordDir (dir);
        juce::Array<juce::File> entries;
        dir.findChildFiles (entries, juce::File::findDirectories | juce::File::findFiles, false);
        entries.sort();
        // F-ios-perf-4: per-directory entry cap — only the first
        // kScanMaxEntriesPerDir SORTED entries are considered (deterministic
        // which ones), so a 6000-file folder cannot stall the scan.
        const int n = juce::jmin (entries.size(), kScanMaxEntriesPerDir);
        for (int ei = 0; ei < n; ++ei)
        {
            const auto& e = entries[ei];
            // F-ios-perf-4: symlink refusal — never follow links (provider
            // aliases / cycle-shaped user trees would loop or explode the walk).
            if (e.isSymbolicLink())
                continue;
            if (e.isDirectory())
            {
                auto& sub = node.subs.emplace_back();
                sub.title = e.getFileName();
                scanRecursiveInto (sub, e, depth + 1);
            }
            else if (e.hasFileExtension (".pro") || e.hasFileExtension (".mul") || e.hasFileExtension (".parvati"))
            {
                node.leaves.push_back ({ e, patchLabel (e, e.hasFileExtension (".pro")) });
            }
        }
    }

    juce::PopupMenu menuFromNode (const MenuNode& node)
    {
        juce::PopupMenu sub;
        for (const auto& s : node.subs)
        {
            auto child = menuFromNode (s);
            if (child.getNumItems() > 0)
                sub.addSubMenu (s.title, child);
        }
        for (const auto& l : node.leaves)
            addLeaf (sub, l.file, l.label);
        return sub;
    }

    static void addSubIfAny (juce::PopupMenu& parent, const juce::String& title, juce::PopupMenu sub)
    {
        if (sub.getNumItems() > 0)
            parent.addSubMenu (title, std::move (sub));
    }

    static bool treeHasLeafLabel (const MenuNode& node, const juce::String& label)
    {
        for (const auto& l : node.leaves)
            if (l.label == label) return true;
        for (const auto& s : node.subs)
            if (treeHasLeafLabel (s, label)) return true;
        return false;
    }

    static int treeLeafCount (const MenuNode& node)
    {
        int n = (int) node.leaves.size();
        for (const auto& s : node.subs)
            n += treeLeafCount (s);
        return n;
    }

    void addLeaf (juce::PopupMenu& m, const juce::File& f, const juce::String& label)
    {
        // SafePointer guard: the leaf action runs after the async menu
        // dismisses — the PresetBrowser (editor-owned) may already be deleted
        // if the host closed the plugin window while the popup was open.
        juce::Component::SafePointer<PresetBrowser> safe (this);
        m.addItem (label, [safe, f] { if (safe != nullptr && safe->onSelect_) safe->onSelect_ (f); });
    }

    juce::String patchLabel (const juce::File& f, bool parseName)
    {
        if (parseName)
        {
            ++parseCount_;   // .PRO parsing is cached with the tree (W10)
            AmbikaProgram prog;
            if (parseAmbikaProgramFile (f, prog) && prog.name.isNotEmpty())
                return prog.name;
        }
        return f.getFileNameWithoutExtension();
    }

    juce::TextButton nameBtn_;
    juce::File templatesDir_, userDir_, factoryDir_, factoryMultiDir_;
    OnSelect onSelect_;

    // ---- the cache (W10) ----
    MenuNode cachedTree_;
    bool cacheValid_ = false;
    std::map<juce::String, std::pair<juce::Time, bool>> dirMtimes_;   // dir path -> {mtime at scan time, existed then} (F-w10-4 roots track presence)
    int scanCount_  = 0;
    int parseCount_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};
