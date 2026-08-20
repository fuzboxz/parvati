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
        // ---- top-bar button tooltips (shortcut hints) ----
        "\"Load a patch (Cmd/Ctrl+O)\" = \"Charger un programme (Cmd/Ctrl+O)\"\n"
        "\"Save the current patch (Cmd/Ctrl+S)\" = \"Enregistrer le programme (Cmd/Ctrl+S)\"\n"
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
        "\"Delete modulation\" = \"Supprimer la modulation\"\n"
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
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unisson 2x\"\n"
        "\"Cyclic\" = \"Cyclique\"\n"
        "\"Chain\" = \"Chaîne\"\n"
        "\"Tune\" = \"Accordage\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Thème\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Tooltips\" = \"Info-bulles\"\n"
        "\"Parameter Smoothing\" = \"Lissage des paramètres\"\n"
        "\"Filter Quality\" = \"Qualité du filtre\"\n"
        "\"Language\" = \"Langue\"\n"
        // ---- Arp Clock (manual tempo) row ----
        "\"Arp Clock\" = \"Horloge arpège\"\n"
        "\"Host tempo: \" = \"Tempo hôte : \"\n"
        "\" BPM\" = \" BPM\"\n"
        "\" BPM (manual ignored)\" = \" BPM (manuel ignoré)\"\n"
        "\"No host tempo - manual tempo active\" = \"Pas de tempo hôte - tempo manuel actif\"\n"
        "\"No host tempo - arp clock: manual BPM (Settings)\" = \"Pas de tempo hôte - horloge arpège : BPM manuel (Réglages)\"\n"
        // ---- Filter Quality (oversampling) combo items ----
        "\"Standard (1×)\" = \"Standard (1×)\"\n"
        "\"High (2×)\" = \"Élevée (2×)\"\n"
        "\"Maximum (4×)\" = \"Maximum (4×)\"\n"
        "\"Ultra (8×)\" = \"Ultra (8×)\"\n"
        // ---- group-panel titles ----
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
        "\"Ambika Multi (.MUL)\" = \"Multi Ambika (.MUL)\"\n"
        "\"Save Ambika Multi (.MUL)\" = \"Enregistrer le multi Ambika (.MUL)\"\n"
        "\"Could not save file:\" = \"Impossible d'enregistrer le fichier :\"\n"
        "\"Could not load file:\" = \"Impossible de charger le fichier :\"\n"
        // ---- FX slot cards / wheels / export preview (suffix-key fragments) ----
        // The FX number / source label stays OUTSIDE the fragments ( untranslated
        // proper nouns); the keys carry their leading/trailing spaces so the
        // concatenation reads correctly in FR.
        "\"FX \" = \"FX \"\n"
        "\" algorithm\" = \" algorithme\"\n"
        "\" previous algorithm\" = \" algorithme précédent\"\n"
        "\" next algorithm\" = \" algorithme suivant\"\n"
        "\" enable / bypass\" = \" activer / bypass\"\n"
        "\"Drag onto a knob to assign \" = \"Glisser sur un potentiomètre pour assigner \"\n"
        "\" as a modulation source\" = \" comme source de modulation\"\n"
        "\"Octave down (Z)\" = \"Octave en dessous (Z)\"\n"
        "\"Octave up (X)\" = \"Octave au-dessus (X)\"\n"
        // ---- Accessibility names (row containers / wheels; suffix-key idiom so
        // the trailing slot number concatenates after the translated fragment) ----
        "\"Pitch Wheel\" = \"Molette de pitch\"\n"
        "\"Mod Wheel\" = \"Molette de modulation\"\n"
        "\"Mod \" = \"Mod \"\n"
        "\"FX Mod \" = \"Mod FX \"\n"
        "\"(this file)\" = \"(ce fichier)\"\n"
        // ---- MulExportDialog (Save Multi fallback dialog) ----
        "\"This setup uses more voices than one Ambika has (6 voicecards).\" = \"Cette configuration utilise plus de voix qu'un Ambika n'en possède (6 cartes vocales).\"\n"
        "\"Choose how to fit it onto the hardware:\" = \"Choisissez comment l'adapter au matériel :\"\n"
        "\"How to fit it\" = \"Comment l'adapter\"\n"
        "\"Cancel\" = \"Annuler\"\n"
        "\"Export to Ambika\" = \"Exporter vers Ambika\"\n"
        "\"Ambika\" = \"Ambika\"\n"
        "\"Voicecards per unit\" = \"Cartes vocales par unité\"\n"
        "\"Voicecards per part\" = \"Cartes vocales par partie\"\n"
        "\"Share the voicecards fairly (recommended)\" = \"Partager les cartes vocales équitablement (recommandé)\"\n"
        "\"Each part gets voicecards in proportion to how many voices it uses now — the busiest parts keep the most polyphony.\" = \"Chaque partie reçoit des cartes vocales proportionnellement au nombre de voix qu'elle utilise — les parties les plus actives conservent le plus de polyphonie.\"\n"
        "\"Give every part the same\" = \"Donner autant à chaque partie\"\n"
        "\"Every active part gets an equal number of voicecards, no matter how many voices it requested.\" = \"Chaque partie active reçoit le même nombre de cartes vocales, quel que soit le nombre de voix demandé.\"\n"
        "\"Let the first parts win\" = \"Priorité aux premières parties\"\n"
        "\"Part 1 keeps as many voices as it can use, then Part 2, and so on — later parts get whatever is left over.\" = \"La partie 1 garde autant de voix qu'elle peut en utiliser, puis la partie 2, etc. — les parties suivantes reçoivent ce qui reste.\"\n"
        "\"Keep them fat instead of polyphonic\" = \"Les garder épaisses plutôt que polyphoniques\"\n"
        "\"Shares fairly like the first option, but every part that loses voices switches to Mono: all of its voicecards then play each note together (the classic unison character), so nothing sounds thin.\" = \"Répartition équitable comme la première option, mais chaque partie qui perd des voix passe en mono : toutes ses cartes vocales jouent alors chaque note ensemble (le caractère unisson classique), donc rien ne sonne fin.\"\n"
        "\"Use two or more chained Ambikas\" = \"Utiliser deux Ambikas ou plus en chaîne\"\n"
        "\"Writes one extra file per additional Ambika (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Connect the units by MIDI, load one file into each, and they play as one big synth — keeping every voice.\" = \"Écrit un fichier supplémentaire par Ambika additionnel (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Reliez les unités par MIDI, chargez un fichier dans chacune, et elles jouent comme un seul grand synthé — en conservant toutes les voix.\"\n"
        "\"Keep the current card assignment\" = \"Conserver l'attribution actuelle des cartes\"\n"
        "\"Exports the voicecards exactly as assigned on the Patch page and simply leaves the extra voice settings out — nothing is re-arranged.\" = \"Exporte les cartes vocales exactement telles qu'attribuées sur la page Patch et laisse simplement de côté les réglages de voix superflus — rien n'est réarrangé.\"\n"
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
        // ---- top-bar button tooltips (shortcut hints) ----
        "\"Load a patch (Cmd/Ctrl+O)\" = \"Programm laden (Cmd/Ctrl+O)\"\n"
        "\"Save the current patch (Cmd/Ctrl+S)\" = \"Programm speichern (Cmd/Ctrl+S)\"\n"
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
        "\"Delete modulation\" = \"Modulation löschen\"\n"
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
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unison 2x\"\n"
        "\"Cyclic\" = \"Zyklisch\"\n"
        "\"Chain\" = \"Kette\"\n"
        "\"Tune\" = \"Stimmung\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Design\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Tooltips\" = \"Quickinfo\"\n"
        "\"Parameter Smoothing\" = \"Parameterglättung\"\n"
        "\"Filter Quality\" = \"Filterqualität\"\n"
        "\"Language\" = \"Sprache\"\n"
        // ---- Arp Clock (manual tempo) row ----
        "\"Arp Clock\" = \"Arp-Takt\"\n"
        "\"Host tempo: \" = \"Host-Tempo: \"\n"
        "\" BPM\" = \" BPM\"\n"
        "\" BPM (manual ignored)\" = \" BPM (manuell ignoriert)\"\n"
        "\"No host tempo - manual tempo active\" = \"Kein Host-Tempo - manuelles Tempo aktiv\"\n"
        "\"No host tempo - arp clock: manual BPM (Settings)\" = \"Kein Host-Tempo - Arp-Takt: manuelle BPM (Einstellungen)\"\n"
        // ---- Filter Quality (oversampling) combo items ----
        "\"Standard (1×)\" = \"Standard (1×)\"\n"
        "\"High (2×)\" = \"Hoch (2×)\"\n"
        "\"Maximum (4×)\" = \"Maximum (4×)\"\n"
        "\"Ultra (8×)\" = \"Ultra (8×)\"\n"
        // ---- group-panel titles ----
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
        "\"Ambika Multi (.MUL)\" = \"Ambika-Multi (.MUL)\"\n"
        "\"Save Ambika Multi (.MUL)\" = \"Ambika-Multi (.MUL) speichern\"\n"
        "\"Could not save file:\" = \"Datei konnte nicht gespeichert werden:\"\n"
        "\"Could not load file:\" = \"Datei konnte nicht geladen werden:\"\n"
        // ---- FX slot cards / wheels / export preview (suffix-key fragments) ----
        // The FX number / source label stays OUTSIDE the fragments ( untranslated
        // proper nouns); the keys carry their leading/trailing spaces so the
        // concatenation reads correctly in DE.
        "\"FX \" = \"FX \"\n"
        "\" algorithm\" = \" Algorithmus\"\n"
        "\" previous algorithm\" = \" vorheriger Algorithmus\"\n"
        "\" next algorithm\" = \" nächster Algorithmus\"\n"
        "\" enable / bypass\" = \" aktivieren / Bypass\"\n"
        "\"Drag onto a knob to assign \" = \"Auf einen Drehregler ziehen, um \"\n"
        "\" as a modulation source\" = \" als Modulationsquelle zuzuweisen\"\n"
        "\"Octave down (Z)\" = \"Oktave tiefer (Z)\"\n"
        "\"Octave up (X)\" = \"Oktave höher (X)\"\n"
        // ---- Accessibility names (row containers / wheels; suffix-key idiom so
        // the trailing slot number concatenates after the translated fragment) ----
        "\"Pitch Wheel\" = \"Pitchrad\"\n"
        "\"Mod Wheel\" = \"Modulationsrad\"\n"
        "\"Mod \" = \"Mod \"\n"
        "\"FX Mod \" = \"FX-Mod. \"\n"
        "\"(this file)\" = \"(diese Datei)\"\n"
        // ---- MulExportDialog (Save Multi fallback dialog) ----
        "\"This setup uses more voices than one Ambika has (6 voicecards).\" = \"Dieses Setup benötigt mehr Stimmen als ein Ambika hat (6 Voicecards).\"\n"
        "\"Choose how to fit it onto the hardware:\" = \"Wählen Sie, wie es auf die Hardware passt:\"\n"
        "\"How to fit it\" = \"Wie es passt\"\n"
        "\"Cancel\" = \"Abbrechen\"\n"
        "\"Export to Ambika\" = \"Zu Ambika exportieren\"\n"
        "\"Ambika\" = \"Ambika\"\n"
        "\"Voicecards per unit\" = \"Voicecards pro Gerät\"\n"
        "\"Voicecards per part\" = \"Voicecards pro Part\"\n"
        "\"Share the voicecards fairly (recommended)\" = \"Voicecards fair aufteilen (empfohlen)\"\n"
        "\"Each part gets voicecards in proportion to how many voices it uses now — the busiest parts keep the most polyphony.\" = \"Jeder Part erhält Voicecards im Verhältnis zu seinen aktuell genutzten Stimmen — die aktivsten Parts behalten die meiste Polyphonie.\"\n"
        "\"Give every part the same\" = \"Jedem Part dasselbe geben\"\n"
        "\"Every active part gets an equal number of voicecards, no matter how many voices it requested.\" = \"Jeder aktive Part erhält gleich viele Voicecards, egal wie viele Stimmen er angefordert hat.\"\n"
        "\"Let the first parts win\" = \"Die ersten Parts gewinnen lassen\"\n"
        "\"Part 1 keeps as many voices as it can use, then Part 2, and so on — later parts get whatever is left over.\" = \"Part 1 behält so viele Stimmen wie möglich, dann Part 2 usw. — spätere Parts bekommen, was übrig bleibt.\"\n"
        "\"Keep them fat instead of polyphonic\" = \"Fett statt polyphon halten\"\n"
        "\"Shares fairly like the first option, but every part that loses voices switches to Mono: all of its voicecards then play each note together (the classic unison character), so nothing sounds thin.\" = \"Teilt fair wie die erste Option, aber jeder Part, der Stimmen verliert, wechselt auf Mono: alle seine Voicecards spielen dann jede Note zusammen (der klassische Unison-Charakter), sodass nichts dünn klingt.\"\n"
        "\"Use two or more chained Ambikas\" = \"Zwei oder mehr verkettete Ambikas verwenden\"\n"
        "\"Writes one extra file per additional Ambika (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Connect the units by MIDI, load one file into each, and they play as one big synth — keeping every voice.\" = \"Schreibt eine zusätzliche Datei pro zusätzlichem Ambika (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Verbinden Sie die Geräte per MIDI, laden Sie in jedes eine Datei — sie spielen dann als ein großer Synth und behalten jede Stimme.\"\n"
        "\"Keep the current card assignment\" = \"Aktuelle Karten-Zuweisung behalten\"\n"
        "\"Exports the voicecards exactly as assigned on the Patch page and simply leaves the extra voice settings out — nothing is re-arranged.\" = \"Exportiert die Voicecards genau wie auf der Patch-Seite zugewiesen und lässt die überzähligen Stimmen-Einstellungen einfach weg — nichts wird neu verteilt.\"\n"
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
