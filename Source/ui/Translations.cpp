// Copyright (c) 2024 805LABS / Parvati.  See Translations.h.

#include "Translations.h"

#include <juce_core/juce_core.h>   // LocalisedStrings, SystemStats

namespace
{
// The selectable languages: {persisted code, user-facing label}. Order matters
// (the SettingsPanel combo is seeded from this order). "Auto" is first so it is
// the natural default. Built with CharPointer_UTF8 so the accented characters
// are decoded as UTF-8 regardless of the compiler's source-encoding assumption.
const std::vector<std::pair<juce::String, juce::String>> kAvailableLanguages = {
    { "auto", "Auto" },
    { "en",   "English" },
    { "fr",   juce::CharPointer_UTF8 ("Français") },
    { "de",   "Deutsch" }
};

// The French chrome translation table, in the LocalisedStrings text format
// (language:/countries: headers, then quoted "key" = "value" pairs; the key is
// the exact English chrome string used with TRANS()). Covers every visible
// editor-chrome string: top-bar buttons, captions, tab names, page headings,
// and the Settings panel labels/combos. Parameter NAMES are hardware terms and
// are intentionally absent (they stay raw). CharPointer_UTF8 + raw UTF-8 bytes
// makes the accented characters and the U+00D7 MULTIPLICATION SIGN ("×")
// unambiguous (the source file itself is UTF-8).
const juce::String& frenchChromeStrings()
{
    static const juce::String text = juce::CharPointer_UTF8 (
        // language: / countries: headers (informational; the table is installed
        // directly, not auto-selected by JUCE).
        "language: French\n"
        "countries: fr be mc ch lu ca\n"
        "\n"
        // ---- top-bar buttons ----
        "\"Settings\" = \"Réglages\"\n"
        "\"Load...\" = \"Charger...\"\n"
        "\"Save...\" = \"Enregistrer...\"\n"
        "\"Undo\" = \"Annuler\"\n"
        "\"Redo\" = \"Rétablir\"\n"
        // ---- top-bar captions ----
        "\"Patch:\" = \"Programme :\"\n"
        "\"Part:\" = \"Partie :\"\n"
        // ---- tab names + page headings ----
        "\"Oscillators\" = \"Oscillateurs\"\n"
        "\"Mixer\" = \"Mixeur\"\n"
        "\"Filter\" = \"Filtre\"\n"
        "\"Envelopes\" = \"Enveloppes\"\n"
        "\"LFOs\" = \"LFO\"\n"
        "\"Mod Matrix\" = \"Matrice de modulation\"\n"
        "\"Modifiers\" = \"Modificateurs\"\n"
        "\"Arp\" = \"Arpège\"\n"
        "\"Sequencer\" = \"Séquenceur\"\n"
        "\"Global\" = \"Général\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Multi / Setup\" = \"Multi / Configuration\"\n"
        // ---- Multi page labels ----
        "\"MIDI Channel\" = \"Canal MIDI\"\n"
        "\"Key Zone Low\" = \"Zone de clavier (bas)\"\n"
        "\"Key Zone High\" = \"Zone de clavier (haut)\"\n"
        "\"Voice Allocation (voicecards)\" = \"Allocation de voix (cartes sons)\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Thème\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Tooltips\" = \"Info-bulles\"\n"
        "\"Parameter Smoothing\" = \"Lissage des paramètres\"\n"
        "\"Filter Quality\" = \"Qualité du filtre\"\n"
        "\"Language\" = \"Langue\"\n"
        // ---- Filter Quality (oversampling) combo items ----
        "\"Standard (1×)\" = \"Standard (1×)\"\n"
        "\"High (2×)\" = \"Élevée (2×)\"\n"
        "\"Maximum (4×)\" = \"Maximum (4×)\"\n"
    );
    return text;
}

// The German chrome translation table, in the LocalisedStrings text format
// (language:/countries: headers, then quoted "key" = "value" pairs; the key is
// the exact English chrome string used with TRANS()). Covers every visible
// editor-chrome string: top-bar buttons, captions, tab names, page headings,
// and the Settings panel labels/combos. Parameter NAMES are hardware terms and
// are intentionally absent (they stay raw). CharPointer_UTF8 + raw UTF-8 bytes
// makes the accented characters (ü, ä) and the U+00D7 MULTIPLICATION SIGN ("×")
// unambiguous (the source file itself is UTF-8).
const juce::String& germanChromeStrings()
{
    static const juce::String text = juce::CharPointer_UTF8 (
        // language: / countries: headers (informational; the table is installed
        // directly, not auto-selected by JUCE).
        "language: German\n"
        "countries: de at ch li lu\n"
        "\n"
        // ---- top-bar buttons ----
        "\"Settings\" = \"Einstellungen\"\n"
        "\"Load...\" = \"Laden...\"\n"
        "\"Save...\" = \"Speichern...\"\n"
        "\"Undo\" = \"Rückgängig\"\n"
        "\"Redo\" = \"Wiederholen\"\n"
        // ---- top-bar captions ----
        "\"Patch:\" = \"Programm:\"\n"
        "\"Part:\" = \"Part:\"\n"
        // ---- tab names + page headings ----
        "\"Oscillators\" = \"Oszillatoren\"\n"
        "\"Mixer\" = \"Mixer\"\n"
        "\"Filter\" = \"Filter\"\n"
        "\"Envelopes\" = \"Hüllkurven\"\n"
        "\"LFOs\" = \"LFO\"\n"
        "\"Mod Matrix\" = \"Mod-Matrix\"\n"
        "\"Modifiers\" = \"Modifikatoren\"\n"
        "\"Arp\" = \"Arp\"\n"
        "\"Sequencer\" = \"Sequenzer\"\n"
        "\"Global\" = \"Global\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Multi / Setup\" = \"Multi / Einrichtung\"\n"
        // ---- Multi page labels ----
        "\"MIDI Channel\" = \"MIDI-Kanal\"\n"
        "\"Key Zone Low\" = \"Tastaturzone (tief)\"\n"
        "\"Key Zone High\" = \"Tastaturzone (hoch)\"\n"
        "\"Voice Allocation (voicecards)\" = \"Stimmenzuweisung (Voicecards)\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Design\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Tooltips\" = \"Quickinfo\"\n"
        "\"Parameter Smoothing\" = \"Parameterglättung\"\n"
        "\"Filter Quality\" = \"Filterqualität\"\n"
        "\"Language\" = \"Sprache\"\n"
        // ---- Filter Quality (oversampling) combo items ----
        "\"Standard (1×)\" = \"Standard (1×)\"\n"
        "\"High (2×)\" = \"Hoch (2×)\"\n"
        "\"Maximum (4×)\" = \"Maximum (4×)\"\n"
    );
    return text;
}
}  // namespace

const std::vector<std::pair<juce::String, juce::String>>& getAvailableLanguages()
{
    return kAvailableLanguages;
}

void installLanguage (const juce::String& code)
{
    // "auto" defers to the OS locale: a French locale ("fr", "fr-FR", ...) ->
    // French; a German locale ("de", "de-DE", ...) -> German; anything else ->
    // English (identity).
    juce::String resolved = code;
    if (resolved == "auto")
    {
        const auto lang = juce::SystemStats::getUserLanguage();   // e.g. "en", "fr", "de"
        if (lang.startsWithIgnoreCase ("de"))
            resolved = "de";
        else if (lang.startsWithIgnoreCase ("fr"))
            resolved = "fr";
        else
            resolved = "en";
    }

    if (resolved == "fr")
    {
        // setCurrentMappings owns + deletes the object when no longer needed.
        juce::LocalisedStrings::setCurrentMappings (
            new juce::LocalisedStrings (frenchChromeStrings(), true));   // ignoreCaseOfKeys
    }
    else if (resolved == "de")
    {
        juce::LocalisedStrings::setCurrentMappings (
            new juce::LocalisedStrings (germanChromeStrings(), true));   // ignoreCaseOfKeys
    }
    else
    {
        // English (and any unrecognised code): no mappings => TRANS() returns
        // the raw English string => byte-identical to the un-localised build.
        juce::LocalisedStrings::setCurrentMappings (nullptr);
    }
}
