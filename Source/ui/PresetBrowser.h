// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// PresetBrowser — a button showing the current patch name that opens a
// HIERARCHICAL juce::PopupMenu, replacing the flat patch ComboBox. Categories:
//   Templates ▸ (.parvati templates)
//   User ▸ (recursively scanned nested folders; .PRO/.MUL/.parvati)
//   Factory ▸ A / B / F / S ▸ (.PRO patches per bank)
//   Multi ▸ (factory .MUL multis)
// The menu is rebuilt on every open (so newly saved user presets appear) and
// each leaf carries its own File -> onSelect callback (no external ID map).
// .PRO leaf labels use the embedded program name (AmbikaProgram); others use the
// file name. Load… / drag-drop are unaffected (handled by the editor).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PatchFile.h"   // AmbikaProgram + parseAmbikaProgramFile (.PRO names)

#include <functional>

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

    void resized() override { nameBtn_.setBounds (getLocalBounds()); }

private:
    void showMenu()
    {
        juce::PopupMenu menu;
        // Order: Templates, User, Factory (banks), Multi.
        addSubIfAny (menu, "Templates", buildFlatSub (templatesDir_, "*.parvati", false));
        menu.addSubMenu (TRANS ("User"), buildRecursiveSub (userDir_));

        juce::PopupMenu factorySub;
        static const char* const kBanks[] = { "A", "B", "F", "S" };   // actual Ambika bank dirs
        for (const char* bank : kBanks)
        {
            auto bankSub = buildFlatSub (factoryDir_.getChildFile (bank), "*.PRO", true);
            if (bankSub.getNumItems() > 0)
                factorySub.addSubMenu (bank, bankSub);
        }
        menu.addSubMenu (TRANS ("Factory"), factorySub);

        addSubIfAny (menu, "Multi", buildFlatSub (factoryMultiDir_, "*.MUL", false));

        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (&nameBtn_));
    }

    static void addSubIfAny (juce::PopupMenu& parent, const juce::String& title, juce::PopupMenu sub)
    {
        if (sub.getNumItems() > 0)
            parent.addSubMenu (title, std::move (sub));
    }

    // One level: list files matching @p wildcard in @p dir, sorted.
    juce::PopupMenu buildFlatSub (const juce::File& dir, const juce::String& wildcard, bool parseName)
    {
        juce::PopupMenu sub;
        if (! dir.isDirectory())
            return sub;
        juce::Array<juce::File> files;
        dir.findChildFiles (files, juce::File::findFiles, false, wildcard);
        files.sort();
        for (const auto& f : files)
            addLeaf (sub, f, parseName);
        return sub;
    }

    // Recursive: a folder becomes a submenu; a preset file becomes a leaf. Lets
    // the user organize USER/ into nested categories.
    juce::PopupMenu buildRecursiveSub (const juce::File& dir)
    {
        juce::PopupMenu sub;
        if (! dir.isDirectory())
            return sub;
        juce::Array<juce::File> entries;
        dir.findChildFiles (entries, juce::File::findDirectories | juce::File::findFiles, false);
        entries.sort();
        for (const auto& e : entries)
        {
            if (e.isDirectory())
            {
                auto childSub = buildRecursiveSub (e);
                if (childSub.getNumItems() > 0)
                    sub.addSubMenu (e.getFileName(), childSub);
            }
            else if (e.hasFileExtension (".pro") || e.hasFileExtension (".mul") || e.hasFileExtension (".parvati"))
            {
                addLeaf (sub, e, e.hasFileExtension (".pro"));
            }
        }
        return sub;
    }

    void addLeaf (juce::PopupMenu& m, const juce::File& f, bool parseName)
    {
        m.addItem (patchLabel (f, parseName), [this, f] { if (onSelect_) onSelect_ (f); });
    }

    static juce::String patchLabel (const juce::File& f, bool parseName)
    {
        if (parseName)
        {
            AmbikaProgram prog;
            if (parseAmbikaProgramFile (f, prog) && prog.name.isNotEmpty())
                return prog.name;
        }
        return f.getFileNameWithoutExtension();
    }

    juce::TextButton nameBtn_;
    juce::File templatesDir_, userDir_, factoryDir_, factoryMultiDir_;
    OnSelect onSelect_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};
