// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See Translations.h.

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
        "\"Load\" = \"Charger\"\n"
        "\"Save\" = \"Enregistrer\"\n"
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
        // ---- Patch page: header button + arrangement + part rows ----
        "\"Patch\" = \"Patch\"\n"
        "\"Patch / arrangement\" = \"Patch / arrangement\"\n"
        "\"Patch / arrangement page\" = \"Page patch / arrangement\"\n"
        "\"Mono\" = \"Mono\"\n"
        "\"Unison\" = \"Unisson\"\n"
        "\"Multitimbral\" = \"Multitimbral\"\n"
        "\"Drum Kit\" = \"Kit de batterie\"\n"
        "\"Custom\" = \"Personnalisé\"\n"
        "\"Voices\" = \"Voix\"\n"
        "\"Ch\" = \"Canal\"\n"
        "\"Zone Low\" = \"Zone (bas)\"\n"
        "\"Zone High\" = \"Zone (haut)\"\n"
        "\"Polyphony\" = \"Polyphonie\"\n"
        "\"Mono\" = \"Mono\"\n"
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unisson 2x\"\n"
        "\"Cyclic\" = \"Cyclique\"\n"
        "\"Chain\" = \"Chaîne\"\n"
        "\"Tune\" = \"Accordage\"\n"
        // \xE2\x80\xA6 = U+2026: must byte-match TRANS("Custom\xE2\x80\xA6")
        "\"Custom…\" = \"Personnalisé…\"\n"
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
        // ---- group-panel titles ----
        "\"Mixer\" = \"Mixeur\"\n"
        "\"Sub Oscillator\" = \"Oscillateur secondaire\"\n"
        "\"Noise / Waveshaper\" = \"Bruit / Waveshaper\"\n"
        "\"Filter Mod\" = \"Modulation du filtre\"\n"
        "\"Note Sequencer\" = \"Séquenceur de notes\"\n"
        "\"Voice LFO\" = \"LFO de voix\"\n"
        "\"Part / Play\" = \"Partie / Jeu\"\n"
        "\"Other\" = \"Autre\"\n"
        "\"Osc 1\" = \"Osc 1\"\n"
        "\"Osc 2\" = \"Osc 2\"\n"
        "\"Filter 1\" = \"Filtre 1\"\n"
        "\"Filter 2\" = \"Filtre 2\"\n"
        "\"Env 1 (Mod)\" = \"Env 1 (Mod)\"\n"
        "\"Env 2 (Filter)\" = \"Env 2 (Filtre)\"\n"
        "\"Env 3 (Amp)\" = \"Env 3 (Amp)\"\n"
        "\"Sequencer 1\" = \"Séquenceur 1\"\n"
        "\"Sequencer 2\" = \"Séquenceur 2\"\n"
        // ---- context menu / Multi page / dialogs ----
        "\"Reset to default\" = \"Réinitialiser\"\n"
        "\"Randomize\" = \"Aléatoire\"\n"
        "\"Omni\" = \"Omni\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Part\" = \"Partie\"\n"
        "\"Templates\" = \"Modèles\"\n"
        "\"Length\" = \"Longueur\"\n"
        "\"Ambika Patch (.PRO)\" = \"Programme Ambika (.PRO)\"\n"
        "\"Parvati Patch (.parvati)\" = \"Programme Parvati (.parvati)\"\n"
        "\"Load Patch / Multi (.PRO / .MUL / .parvati)\" = \"Charger programme / multi (.PRO / .MUL / .parvati)\"\n"
        "\"Save Ambika Patch (.PRO)\" = \"Enregistrer le programme Ambika (.PRO)\"\n"
        "\"Save Parvati Patch (.parvati)\" = \"Enregistrer le programme Parvati (.parvati)\"\n"
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
        "\"Load\" = \"Laden\"\n"
        "\"Save\" = \"Speichern\"\n"
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
        // ---- Patch page: header button + arrangement + part rows ----
        "\"Patch\" = \"Patch\"\n"
        "\"Patch / arrangement\" = \"Patch / Arrangement\"\n"
        "\"Patch / arrangement page\" = \"Patch-/Arrangement-Seite\"\n"
        "\"Mono\" = \"Mono\"\n"
        "\"Unison\" = \"Unison\"\n"
        "\"Multitimbral\" = \"Multitimbral\"\n"
        "\"Drum Kit\" = \"Drum-Kit\"\n"
        "\"Custom\" = \"Benutzerdefiniert\"\n"
        "\"Voices\" = \"Stimmen\"\n"
        "\"Ch\" = \"Kanal\"\n"
        "\"Zone Low\" = \"Zone (tief)\"\n"
        "\"Zone High\" = \"Zone (hoch)\"\n"
        "\"Polyphony\" = \"Polyphonie\"\n"
        "\"Mono\" = \"Mono\"\n"
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unison 2x\"\n"
        "\"Cyclic\" = \"Zyklisch\"\n"
        "\"Chain\" = \"Kette\"\n"
        "\"Tune\" = \"Stimmung\"\n"
        // \xE2\x80\xA6 = U+2026: must byte-match TRANS("Custom\xE2\x80\xA6")
        "\"Custom…\" = \"Benutzerdefiniert…\"\n"
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
        // ---- group-panel titles ----
        "\"Mixer\" = \"Mixer\"\n"
        "\"Sub Oscillator\" = \"Sub-Oszillator\"\n"
        "\"Noise / Waveshaper\" = \"Rauschen / Waveshaper\"\n"
        "\"Filter Mod\" = \"Filtermod\"\n"
        "\"Note Sequencer\" = \"Notensequenzer\"\n"
        "\"Voice LFO\" = \"Stimmen-LFO\"\n"
        "\"Part / Play\" = \"Part / Wiedergabe\"\n"
        "\"Other\" = \"Sonstige\"\n"
        "\"Osc 1\" = \"Osz 1\"\n"
        "\"Osc 2\" = \"Osz 2\"\n"
        "\"Filter 1\" = \"Filter 1\"\n"
        "\"Filter 2\" = \"Filter 2\"\n"
        "\"Env 1 (Mod)\" = \"Hüll 1 (Mod)\"\n"
        "\"Env 2 (Filter)\" = \"Hüll 2 (Filter)\"\n"
        "\"Env 3 (Amp)\" = \"Hüll 3 (Amp)\"\n"
        "\"Sequencer 1\" = \"Sequenzer 1\"\n"
        "\"Sequencer 2\" = \"Sequenzer 2\"\n"
        // ---- context menu / Multi page / dialogs ----
        "\"Reset to default\" = \"Zurücksetzen\"\n"
        "\"Randomize\" = \"Zufall\"\n"
        "\"Omni\" = \"Omni\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Part\" = \"Part\"\n"
        "\"Templates\" = \"Vorlagen\"\n"
        "\"Length\" = \"Länge\"\n"
        "\"Ambika Patch (.PRO)\" = \"Ambika-Programm (.PRO)\"\n"
        "\"Parvati Patch (.parvati)\" = \"Parvati-Programm (.parvati)\"\n"
        "\"Load Patch / Multi (.PRO / .MUL / .parvati)\" = \"Programm / Multi laden (.PRO / .MUL / .parvati)\"\n"
        "\"Save Ambika Patch (.PRO)\" = \"Ambika-Programm speichern (.PRO)\"\n"
        "\"Save Parvati Patch (.parvati)\" = \"Parvati-Programm speichern (.parvati)\"\n"
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
