// Copyright (c) 2026 805Labs Kft. / Hellcat.

#include "ThemeManager.h"

ThemeManager::ThemeManager()
    : themes_ (getBuiltinThemes()), currentIndex_ (0)
{
}

const HellcatTheme& ThemeManager::getCurrentTheme() const
{
    return themes_[static_cast<size_t> (currentIndex_)];
}

const HellcatTheme& ThemeManager::getTheme (int index) const
{
    const auto clamped = juce::jlimit (0, static_cast<int> (themes_.size()) - 1, index);
    return themes_[static_cast<size_t> (clamped)];
}

const std::vector<juce::String> ThemeManager::getThemeNames() const
{
    std::vector<juce::String> names;
    names.reserve (themes_.size());
    for (const auto& t : themes_)
        names.push_back (t.name);
    return names;
}

bool ThemeManager::selectByName (const juce::String& name)
{
    for (int i = 0; i < static_cast<int> (themes_.size()); ++i)
    {
        if (themes_[static_cast<size_t> (i)].name == name)
        {
            currentIndex_ = i;
            sendChangeMessage();
            return true;
        }
    }
    return false; // unknown -> no-op, no broadcast
}


