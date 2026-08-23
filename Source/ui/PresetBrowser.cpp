// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// PresetBrowser implementation. See PresetBrowser.h for the class contract
// and the design comments (the bodies moved here unchanged from the header).

#include "PresetBrowser.h"

#include "PatchFile.h"   // AmbikaProgram + parseAmbikaProgramFile (.PRO names)

//==============================================================================
PresetBrowser::PresetBrowser (juce::File templatesDir, juce::File userDir,
                              juce::File factoryDir, juce::File factoryMultiDir, OnSelect onSelect)
    : templatesDir_ (std::move (templatesDir)), userDir_ (std::move (userDir)),
      factoryDir_ (std::move (factoryDir)), factoryMultiDir_ (std::move (factoryMultiDir)),
      onSelect_ (std::move (onSelect))
{
    nameBtn_.setButtonText ("(select a patch)");
    nameBtn_.onClick = [this] { showMenu(); };
    addAndMakeVisible (nameBtn_);
}

void PresetBrowser::setCurrentName (const juce::String& name)
{
    nameBtn_.setButtonText (name.isEmpty() ? TRANS ("(select a patch)") : name);
}

void PresetBrowser::invalidate() { cacheValid_ = false; }

juce::File PresetBrowser::selectNext() { return stepSelection (+1); }

juce::File PresetBrowser::selectPrev() { return stepSelection (-1); }

void PresetBrowser::resized() { nameBtn_.setBounds (getLocalBounds()); }

bool PresetBrowser::debugTreeHasLeafLabel (const juce::String& label) const
{
    return treeHasLeafLabel (cachedTree_, label);
}

int PresetBrowser::debugTreeLeafCount() const { return treeLeafCount (cachedTree_); }

juce::File PresetBrowser::importIntoUserTree (const juce::File& file, const juce::File& userDir)
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

int PresetBrowser::syncTreeNewestWins (const juce::File& sourceDir, const juce::File& destDir)
{
    if (sourceDir.getFullPathName().isEmpty() || ! sourceDir.isDirectory())
        return 0;
    int copied = 0;
    juce::Array<juce::File> entries;
    sourceDir.findChildFiles (entries,
                              juce::File::findFiles | juce::File::findDirectories,
                              false);
    for (const auto& e : entries)
    {
        if (e.isSymbolicLink())
            continue;   // never follow links out of the managed tree
        if (e.isDirectory())
        {
            copied += syncTreeNewestWins (e, destDir.getChildFile (e.getFileName()));
            continue;
        }
        const auto name = e.getFileName();
        if (name.contains ("_temp"))
            continue;   // another process's in-flight atomic write
        const juce::File dest = destDir.getChildFile (name);
        const juce::Time srcTime = e.getLastModificationTime();
        if (dest.existsAsFile() && ! (srcTime > dest.getLastModificationTime()))
            continue;   // absent, or dest already current — newest wins
        if (! dest.getParentDirectory().createDirectory())
            continue;
        juce::TemporaryFile temp (dest);
        if (e.copyFileTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary())
        {
            // Propagate the source mtime so the NEXT comparison is exact
            // (a copy-time mtime would make every later save ambiguous).
            dest.setLastModificationTime (srcTime);
            ++copied;
        }
        // A failed copy is non-fatal: the shared tree stays the source of
        // truth and the next launch retries (additive, never destructive).
    }
    return copied;
}

void PresetBrowser::buildMenu (juce::PopupMenu& menu)
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

void PresetBrowser::showMenu()
{
    juce::PopupMenu menu;
    buildMenu (menu);
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (&nameBtn_));
}

void PresetBrowser::scanInto (MenuNode& root)
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

void PresetBrowser::recordDir (const juce::File& dir)
{
    dirMtimes_[dir.getFullPathName()] = { dir.getLastModificationTime(), true };
}

void PresetBrowser::recordRoot (const juce::File& dir)
{
    const bool present = dir.isDirectory();
    dirMtimes_[dir.getFullPathName()] = { dir.getLastModificationTime(), present };
}

bool PresetBrowser::watchedDirsChanged() const
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

PresetBrowser::MenuNode& PresetBrowser::scanFlatInto (MenuNode& node, const juce::File& dir,
                                                      const juce::String& wildcard, bool parseName)
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

void PresetBrowser::scanRecursiveInto (MenuNode& node, const juce::File& dir, int depth)
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

juce::PopupMenu PresetBrowser::menuFromNode (const MenuNode& node)
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

void PresetBrowser::addSubIfAny (juce::PopupMenu& parent, const juce::String& title, juce::PopupMenu sub)
{
    if (sub.getNumItems() > 0)
        parent.addSubMenu (title, std::move (sub));
}

bool PresetBrowser::treeHasLeafLabel (const MenuNode& node, const juce::String& label)
{
    for (const auto& l : node.leaves)
        if (l.label == label) return true;
    for (const auto& s : node.subs)
        if (treeHasLeafLabel (s, label)) return true;
    return false;
}

int PresetBrowser::treeLeafCount (const MenuNode& node)
{
    int n = (int) node.leaves.size();
    for (const auto& s : node.subs)
        n += treeLeafCount (s);
    return n;
}

void PresetBrowser::addLeaf (juce::PopupMenu& m, const juce::File& f, const juce::String& label)
{
    // SafePointer guard: the leaf action runs after the async menu
    // dismisses — the PresetBrowser (editor-owned) may already be deleted
    // if the host closed the plugin window while the popup was open.
    juce::Component::SafePointer<PresetBrowser> safe (this);
    m.addItem (label, [safe, f] { if (safe != nullptr) safe->selectLeaf (f); });
}

void PresetBrowser::selectLeaf (const juce::File& f)
{
    currentFile_ = f;
    if (onSelect_)
        onSelect_ (f);
}

void PresetBrowser::flattenLeaves (const MenuNode& node, std::vector<Leaf>& out)
{
    for (const auto& s : node.subs)
        flattenLeaves (s, out);
    out.insert (out.end(), node.leaves.begin(), node.leaves.end());
}

juce::File PresetBrowser::stepSelection (int dir)
{
    if (! cacheValid_ || watchedDirsChanged())
        scanInto (cachedTree_);
    std::vector<Leaf> flat;
    flattenLeaves (cachedTree_, flat);
    if (flat.empty())
        return {};
    const juce::String cur = currentFile_.getFullPathName();
    int idx = -1;
    for (int i = 0; i < (int) flat.size(); ++i)
        if (flat[(size_t) i].file.getFullPathName() == cur) { idx = i; break; }
    // Not-in-list + next => first; not-in-list + prev => last (the ends
    // the step is heading toward); otherwise move by dir and WRAP.
    int next = (idx < 0) ? (dir > 0 ? 0 : (int) flat.size() - 1)
                         : (idx + dir + (int) flat.size()) % (int) flat.size();
    const auto& leaf = flat[(size_t) next];
    selectLeaf (leaf.file);
    return leaf.file;
}

juce::String PresetBrowser::patchLabel (const juce::File& f, bool parseName)
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
