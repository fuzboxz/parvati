// Copyright (c) 2024 805LABS / Parvati.

#include "ThemeManager.h"

ThemeManager::ThemeManager()
    : themes_ (getBuiltinThemes()), currentIndex_ (0)
{
}

int ThemeManager::getNumThemes() const
{
    return static_cast<int> (themes_.size());
}

const ParvatiTheme& ThemeManager::getCurrentTheme() const
{
    return themes_[static_cast<size_t> (currentIndex_)];
}

const ParvatiTheme& ThemeManager::getTheme (int index) const
{
    const auto clamped = juce::jlimit (0, static_cast<int> (themes_.size()) - 1, index);
    return themes_[static_cast<size_t> (clamped)];
}

int ThemeManager::getCurrentIndex() const
{
    return currentIndex_;
}

juce::String ThemeManager::getCurrentThemeName() const
{
    return getCurrentTheme().name;
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

void ThemeManager::selectByIndex (int index)
{
    currentIndex_ = juce::jlimit (0, static_cast<int> (themes_.size()) - 1, index);
    sendChangeMessage();
}

juce::ValueTree ThemeManager::toValueTree() const
{
    juce::ValueTree vt ("ParvatiUI");
    vt.setProperty ("theme", getCurrentThemeName(), nullptr);
    return vt;
}

bool ThemeManager::fromValueTree (const juce::ValueTree& vt)
{
    if (vt.getType() != juce::Identifier ("ParvatiUI"))
        return false;

    const auto name = vt.getProperty ("theme", "Carbon").toString();
    return selectByName (name.isEmpty() ? "Carbon" : name);
}
