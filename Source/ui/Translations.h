// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Chrome (UI chrome) translations for the Hellcat editor. Only the editor
// chrome — top-bar buttons, settings labels, tab names, page headings — is
// localised. Parameter NAMES are Ambika hardware terms and are intentionally
// NOT translated (they stay raw in every language). English is the identity
// default: with no mappings installed (language "en", or "auto" resolving to a
// non-French locale) juce::translate()/TRANS() returns the raw English string,
// so the UI is byte-identical to the un-localised build.
//
// The lookup uses the JUCE LocalisedStrings machinery: installLanguage() pushes
// a LocalisedStrings (or clears it for English) via
// LocalisedStrings::setCurrentMappings(), and every chrome string is wrapped in
// TRANS() at its point of use (the English literal is the key).

#pragma once

#include <juce_core/juce_core.h>   // juce::String

#include <utility>
#include <vector>

// The selectable languages, as {code, displayLabel}. "auto" defers to the OS
// locale via juce::SystemStats::getUserLanguage() (French locale -> French,
// German locale -> German, everything else -> English). The codes are also
// what is persisted as the `ui_language` processor preference.
const std::vector<std::pair<juce::String, juce::String>>& getAvailableLanguages();

// Install the active language's LocalisedStrings (process-wide, via
// juce::LocalisedStrings::setCurrentMappings). Safe and idempotent to call on
// the message thread.
//   "en"   -> clear mappings (English = identity = raw strings).
//   "fr"   -> install the French chrome translations.
//   "de"   -> install the German chrome translations.
//   "auto" -> resolve via SystemStats::getUserLanguage(): a French locale
//             installs French, a German locale installs German, anything else
//             clears mappings (English).
// Any other/unrecognised code is treated as English (identity), so an unknown
// saved preference never breaks the UI.
void installLanguage (const juce::String& code);
