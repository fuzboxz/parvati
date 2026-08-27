// Copyright (c) 2026 805Labs Kft. / Hellcat.  See Translations.h.

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
        "\"Synth page\" = \"Page synthé\"\n"
        "\"FX page\" = \"Page FX\"\n"
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
        "\"Voice\" = \"Voix\"\n"
        "\"MIDI\" = \"MIDI\"\n"
        "\"Export .PRO\" = \"Exporter .PRO\"\n"
        "\"Load Hellcat Patch (.yml)\" = \"Charger un patch Hellcat (.yml)\"\n"
        "\"Export .MUL\" = \"Exporter .MUL\"\n"
        "\"Export this part as an Ambika .PRO patch \" = \"Exporte cette partie en patch Ambika .PRO \"\n"
        "\"(byte-faithful, hardware-shareable; Hellcat-only options \" = \"(fidèle au format, partageable avec le matériel ; options Hellcat \"\n"
        "\"— VCA curve, filter card, arp — are not carried; use Save \" = \"— courbe VCA, carte filtre, arp — non reprises ; utilisez Enregistrer \"\n"
        "\"(.yml) for the full patch).\" = \"(.yml) pour le patch complet).\"\n"
        "\"Export the whole 6-part setup as an Ambika .MUL multi \" = \"Exporte l'ensemble des 6 parties en multi Ambika .MUL \"\n"
        "\"(hardware-shareable; if a part needs more voices than its \" = \"(partageable avec le matériel ; si une partie demande plus de voix que ses \"\n"
        "\"voicecards, the export-fallback dialog maps them onto the \" = \"cartes vocales, le dialogue de repli d'export les répartit sur les \"\n"
        "\"6 cards).\" = \"6 cartes).\"\n"
        "\"More\" = \"Plus\"\n"
        "\"Channel\" = \"Canal\"\n"
        "\"Portamento\" = \"Portamento\"\n"
        "\"Legato\" = \"Legato\"\n"
        "\"Volume\" = \"Volume\"\n"
        "\"Spread\" = \"Dispersion\"\n"
        "\"Octave\" = \"Octave\"\n"
        "\"Fine Tune\" = \"Accord fin\"\n"
        "\"Reset\" = \"Réinitialiser\"\n"
        "\"Zone Low\" = \"Zone (bas)\"\n"
        "\"Click (or tap) to rename this part — an empty name reverts to the \" = \"Cliquez (ou touchez) pour renommer cette partie — un nom vide rétablit \"\n"
        "\"default 'Part N' label.\" = \"l'étiquette par défaut « Partie N ».\"\n"
        "\"MIDI channel this part listens on (Omni responds on every \" = \"Canal MIDI écouté par cette partie (Omni répond sur tous \"\n"
        "\"channel; multitimbral stacks usually need distinct channels).\" = \"les canaux ; les stacks multitimbraux utilisent en général des canaux distincts).\"\n"
        "\"Key zone: the lowest MIDI note this part responds to (notes \" = \"Zone de clavier : note MIDI la plus basse à laquelle cette partie répond (les \"\n"
        "\"below stay silent so another part can use them).\" = \"notes plus basses restent muettes pour qu'une autre partie les utilise).\"\n"
        "\"Key zone: the highest MIDI note this part responds to (notes \" = \"Zone de clavier : note MIDI la plus haute à laquelle cette partie répond (les \"\n"
        "\"above stay silent so another part can use them).\" = \"notes plus hautes restent muettes pour qu'une autre partie les utilise).\"\n"
        "\"Zone High\" = \"Zone (haut)\"\n"
        "\"Oct\" = \"Oct\"\n"        // abbreviation (octave) — same in FR
        "\"Porta\" = \"Porta\"\n"      // portamento — same term in FR
        "\"Lgo\" = \"Lgo\"\n"          // abbreviation (legato) — same in FR
        "\"Vol\" = \"Vol\"\n"          // abbreviation (volume) — same in FR
        "\"Fine\" = \"Fin\"\n"        // fine tuning (accord fin)
        "\"Spr\" = \"Spr\"\n"          // abbreviation (spread/detune) — same in FR
        "\"Off\" = \"Off\"\n"          // gear-standard term (kept, cf. param value lists)
        "\"On\" = \"On\"\n"
        "\"Polyphony\" = \"Polyphonie\"\n"
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unisson 2x\"\n"
        "\"Cyclic\" = \"Cyclique\"\n"
        "\"Chain\" = \"Chaîne\"\n"
        "\"Tune\" = \"Accordage\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Thème\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Zoom in\" = \"Zoom avant\"\n"
        "\"Zoom out\" = \"Zoom arrière\"\n"
        "\"Reset zoom\" = \"Réinitialiser le zoom\"\n"
        "\"Tooltips\" = \"Info-bulles\"\n"
        "\"Mod Lamp Colours\" = \"Couleurs de lampe par catégorie\"\n"
        "\"Parameter Smoothing\" = \"Lissage des paramètres\"\n"
        "\"Filter Quality\" = \"Qualité du filtre\"\n"
        "\"Language\" = \"Langue\"\n"
        "\"Editor settings\" = \"Réglages de l'éditeur\"\n"
        "\"Manual tempo\" = \"Tempo manuel\"\n"
        "\"Arp clock tempo in BPM\" = \"Tempo de l'horloge arpège en BPM\"\n"
        // ---- Visual Refresh (live feedback animation cadence) ----
        "\"Visual Refresh\" = \"Rafraîchissement visuel\"\n"
        "\"Animation rate of the live modulation indicators\" = \"Fréquence d'animation des indicateurs de modulation en direct\"\n"
        "\"10 Hz\" = \"10 Hz\"\n"
        "\"15 Hz\" = \"15 Hz\"\n"
        "\"30 Hz (Default)\" = \"30 Hz (par défaut)\"\n"
        "\"60 Hz\" = \"60 Hz\"\n"
        // ---- Seq length stepper / status strip (load + thermal hints) ----
        "\"Set sequence length\" = \"Définir la longueur de la séquence\"\n"
        "\"Audio-thread realtime load (current block; near 100% = dropouts/crackle).\" = \"Charge temps réel du thread audio (bloc en cours ; proche de 100 % = décrochages/craquements).\"\n"
        "\"Thermal: reduce Filter Quality\" = \"Thermique : réduire la qualité du filtre\"\n"
        "\"Thermal: lower Filter Quality now\" = \"Thermique : réduire la qualité du filtre dès maintenant\"\n"
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
        "\"Reset this parameter to its default value\" = \"Réinitialiser ce paramètre à sa valeur par défaut\"\n"
        "\"Set this parameter to a random value\" = \"Mettre ce paramètre à une valeur aléatoire\"\n"
        "\"Omni\" = \"Omni\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Part\" = \"Partie\"\n"
        "\"Templates\" = \"Modèles\"\n"
        "\"Length\" = \"Longueur\"\n"
        "\"Ambika Patch (.PRO)\" = \"Programme Ambika (.PRO)\"\n"
        "\"Hellcat Patch (.yml)\" = \"Programme Hellcat (.yml)\"\n"
        "\"Load Patch / Multi (.PRO / .MUL / .yml)\" = \"Charger programme / multi (.PRO / .MUL / .yml)\"\n"
        "\"Save Ambika Patch (.PRO)\" = \"Enregistrer le programme Ambika (.PRO)\"\n"
        "\"Save Hellcat Patch (.yml)\" = \"Enregistrer le programme Hellcat (.yml)\"\n"
        "\"Ambika Multi (.MUL)\" = \"Multi Ambika (.MUL)\"\n"
        "\"Save Ambika Multi (.MUL)\" = \"Enregistrer le multi Ambika (.MUL)\"\n"
        "\"Could not save file:\" = \"Impossible d'enregistrer le fichier :\"\n"
        "\"Could not load file:\" = \"Impossible de charger le fichier :\"\n"
        // ---- FX routing bar (global dry/wet, master EQ, topology buttons) ----
        "\"Dry/Wet\" = \"Dry/Wet\"\n"          // gear-standard term — kept, cf. "Off"/"On"
        "\"Global FX wet/dry\" = \"Mix wet/dry global des FX\"\n"
        "\"FX master EQ \" = \"Égaliseur principal FX \"\n"
        "\"Previous FX topology\" = \"Topologie FX précédente\"\n"
        "\"Next FX topology\" = \"Topologie FX suivante\"\n"
        // ---- preset browser (submenus + name placeholder) ----
        "\"Ambika Factory\" = \"Usine Ambika\"\n"
        "\"Hellcat Factory\" = \"Usine Hellcat\"\n"
        "\"User\" = \"Utilisateur\"\n"
        "\"(select a patch)\" = \"(sélectionner un patch)\"\n"
        // step chevrons beside the name button (tooltip AND icon title)
        "\"Previous patch\" = \"Patch précédent\"\n"
        "\"Next patch\" = \"Patch suivant\"\n"
        // ---- mod matrix rows / tap-to-assign status strip ----
        "\"+ Add Modulation\" = \"+ Ajouter une modulation\"\n"
        "\"Assign the next free slot\" = \"Assigne le prochain emplacement libre\"\n"
        "\"Mute / bypass this modulation\" = \"Couper / bypasser cette modulation\"\n"
        "\"Assigned\" = \"Assignée\"\n"
        "\"Mod Matrix full\" = \"Matrice de modulation pleine\"\n"
        "\"Tap-to-assign modulation\" = \"Assignation de modulation au toucher\"\n"
        "\"Toggle the modulation pill bar\" = \"Afficher/masquer la barre de pastilles de modulation\"\n"
        // Suffix-key fragments: the source name and the exit hint follow.
        "\"MOD assign armed: \" = \"Assignation MOD armée : \"\n"
        "\" — tap a destination knob ([MOD] to exit)\" = \" — touchez un potentiomètre de destination ([MOD] pour quitter)\"\n"
        "\"Tap a mod source first\" = \"Touchez d'abord une source de modulation\"\n"
        "\"Tap a mod source, then a knob\" = \"Touchez une source de modulation, puis un potentiomètre\"\n"
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
        // ---- Patch page voices tooltip (suffix-key fragments; the sentence
        // concatenates across five TRANS fragments) ----
        "\"How many voices this part plays at once from the shared \" = \"Nombre de voix jouées à la fois par cette partie depuis la \"\n"
        "\"96-voice pool (0-16; 0 disables the part; the pool holds 6 x 16 so \" = \"banque partagée de 96 voix (0-16 ; 0 désactive la partie ; la banque tient 6 x 16 donc \"\n"
        "\"all parts can be maxed at the same time; the hardware voicecards \" = \"toutes les parties peuvent être saturées en même temps ; les cartes vocales du matériel \"\n"
        "\"are shared out automatically for the individual outputs and the \" = \"sont réparties automatiquement entre les sorties individuelles et \"\n"
        "\".MUL export).\" = \"l'export .MUL).\"\n"
        // ---- Accessibility names (row containers / wheels; suffix-key idiom so
        // the trailing slot number concatenates after the translated fragment) ----
        "\"Pitch Wheel\" = \"Molette de pitch\"\n"
        "\"Mod Wheel\" = \"Molette de modulation\"\n"
        "\"Mod \" = \"Mod \"\n"
        "\"FX Mod \" = \"Mod FX \"\n"
        "\"Toggle virtual keyboard\" = \"Afficher/masquer le clavier virtuel\"\n"
        "\"Keyboard: octave \" = \"Clavier : octave \"\n"
        "\"velocity\" = \"vélocité\"\n"
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
        "\"Each chained Ambika after the first gets one extra file (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Connect the units by MIDI. Load one file into each unit. The units then play as one big synth. Every voice stays.\" = \"Chaque Ambika en chaîne après le premier reçoit un fichier supplémentaire (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Reliez les unités par MIDI. Chargez un fichier dans chaque unité. Les unités jouent alors comme un seul grand synthé. Chaque voix est conservée.\"\n"
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
        "\"Synth page\" = \"Synth-Seite\"\n"
        "\"FX page\" = \"FX-Seite\"\n"
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
        "\"Voice\" = \"Voice\"\n"
        "\"MIDI\" = \"MIDI\"\n"
        "\"Export .PRO\" = \".PRO exportieren\"\n"
        "\"Load Hellcat Patch (.yml)\" = \"Hellcat-Patch (.yml) laden\"\n"
        "\"Export .MUL\" = \".MUL exportieren\"\n"
        "\"Export this part as an Ambika .PRO patch \" = \"Exportiert diesen Part als Ambika-.PRO-Patch \"\n"
        "\"(byte-faithful, hardware-shareable; Hellcat-only options \" = \"(bytetreu, hardwarekompatibel; Hellcat-exklusive Optionen \"\n"
        "\"— VCA curve, filter card, arp — are not carried; use Save \" = \"— VCA-Kurve, Filterkarte, Arp — werden nicht übernommen; nutze Speichern \"\n"
        "\"(.yml) for the full patch).\" = \"(.yml) für den vollständigen Patch).\"\n"
        "\"Export the whole 6-part setup as an Ambika .MUL multi \" = \"Exportiert das gesamte 6-Part-Setup als Ambika-.MUL-Multi \"\n"
        "\"(hardware-shareable; if a part needs more voices than its \" = \"(hardwarekompatibel; braucht ein Part mehr Stimmen als seine \"\n"
        "\"voicecards, the export-fallback dialog maps them onto the \" = \"Voicecards, ordnet der Export-Fallback-Dialog sie den \"\n"
        "\"6 cards).\" = \"6 Karten zu).\"\n"
        "\"More\" = \"Mehr\"\n"
        "\"Channel\" = \"Kanal\"\n"
        "\"Portamento\" = \"Portamento\"\n"
        "\"Legato\" = \"Legato\"\n"
        "\"Volume\" = \"Volume\"\n"
        "\"Spread\" = \"Verstimmung\"\n"
        "\"Octave\" = \"Oktave\"\n"
        "\"Fine Tune\" = \"Feinstimmung\"\n"
        "\"Reset\" = \"Zurücksetzen\"\n"
        "\"Zone Low\" = \"Zone (tief)\"\n"
        "\"Click (or tap) to rename this part — an empty name reverts to the \" = \"Klicken (oder tippen), um diese Part umzubenennen — ein leerer Name stellt \"\n"
        "\"default 'Part N' label.\" = \"das Standard-Label 'Part N' wieder her.\"\n"
        "\"MIDI channel this part listens on (Omni responds on every \" = \"MIDI-Kanal, auf dem diese Part lauscht (Omni antwortet auf \"\n"
        "\"channel; multitimbral stacks usually need distinct channels).\" = \"jedem Kanal; multitimbrale Setups brauchen meist eigene Kanäle).\"\n"
        "\"Key zone: the lowest MIDI note this part responds to (notes \" = \"Tastenzone: tiefste MIDI-Note, auf die diese Part reagiert (\"\n"
        "\"below stay silent so another part can use them).\" = \"tiefere Noten bleiben stumm, damit eine andere Part sie nutzt).\"\n"
        "\"Key zone: the highest MIDI note this part responds to (notes \" = \"Tastenzone: höchste MIDI-Note, auf die diese Part reagiert (\"\n"
        "\"above stay silent so another part can use them).\" = \"höhere Noten bleiben stumm, damit eine andere Part sie nutzt).\"\n"
        "\"Zone High\" = \"Zone (hoch)\"\n"
        "\"Oct\" = \"Okt\"\n"         // abbreviation (Oktave)
        "\"Porta\" = \"Porta\"\n"      // Portamento — same term in DE
        "\"Lgo\" = \"Lgo\"\n"          // abbreviation (Legato) — same in DE
        "\"Vol\" = \"Vol\"\n"          // abbreviation (Volume) — same in DE
        "\"Fine\" = \"Fein\"\n"       // Feinstimmung
        "\"Spr\" = \"Spr\"\n"          // abbreviation (Spread/Verstreuung) — same in DE
        "\"Off\" = \"Aus\"\n"
        "\"On\" = \"Ein\"\n"
        "\"Polyphony\" = \"Polyphonie\"\n"
        "\"Poly\" = \"Poly\"\n"
        "\"Unison 2x\" = \"Unison 2x\"\n"
        "\"Cyclic\" = \"Zyklisch\"\n"
        "\"Chain\" = \"Kette\"\n"
        "\"Tune\" = \"Stimmung\"\n"
        // ---- Settings panel labels / toggles ----
        "\"Theme\" = \"Design\"\n"
        "\"Zoom\" = \"Zoom\"\n"
        "\"Zoom in\" = \"Vergrößern\"\n"
        "\"Zoom out\" = \"Verkleinern\"\n"
        "\"Reset zoom\" = \"Zoom zurücksetzen\"\n"
        "\"Tooltips\" = \"Quickinfo\"\n"
        "\"Mod Lamp Colours\" = \"Kategorienfarben der Lampen\"\n"
        "\"Parameter Smoothing\" = \"Parameterglättung\"\n"
        "\"Filter Quality\" = \"Filterqualität\"\n"
        "\"Language\" = \"Sprache\"\n"
        "\"Editor settings\" = \"Editoreinstellungen\"\n"
        "\"Manual tempo\" = \"Manuelles Tempo\"\n"
        "\"Arp clock tempo in BPM\" = \"Arp-Takt-Tempo in BPM\"\n"
        // ---- Visual Refresh (live feedback animation cadence) ----
        "\"Visual Refresh\" = \"Visuelle Aktualisierung\"\n"
        "\"Animation rate of the live modulation indicators\" = \"Animationsrate der Live-Modulationsanzeigen\"\n"
        "\"10 Hz\" = \"10 Hz\"\n"
        "\"15 Hz\" = \"15 Hz\"\n"
        "\"30 Hz (Default)\" = \"30 Hz (Standard)\"\n"
        "\"60 Hz\" = \"60 Hz\"\n"
        // ---- Seq length stepper / status strip (load + thermal hints) ----
        "\"Set sequence length\" = \"Sequenzlänge festlegen\"\n"
        "\"Audio-thread realtime load (current block; near 100% = dropouts/crackle).\" = \"Echtzeitlast des Audio-Threads (aktueller Block; nahe 100 % = Aussetzer/Knistern).\"\n"
        "\"Thermal: reduce Filter Quality\" = \"Temperatur: Filterqualität reduzieren\"\n"
        "\"Thermal: lower Filter Quality now\" = \"Temperatur: Filterqualität jetzt senken\"\n"
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
        "\"Reset this parameter to its default value\" = \"Diesen Parameter auf den Standardwert zurücksetzen\"\n"
        "\"Set this parameter to a random value\" = \"Diesen Parameter auf einen Zufallswert setzen\"\n"
        "\"Omni\" = \"Omni\"\n"
        "\"Multi\" = \"Multi\"\n"
        "\"Part\" = \"Part\"\n"
        "\"Templates\" = \"Vorlagen\"\n"
        "\"Length\" = \"Länge\"\n"
        "\"Ambika Patch (.PRO)\" = \"Ambika-Programm (.PRO)\"\n"
        "\"Hellcat Patch (.yml)\" = \"Hellcat-Programm (.yml)\"\n"
        "\"Load Patch / Multi (.PRO / .MUL / .yml)\" = \"Programm / Multi laden (.PRO / .MUL / .yml)\"\n"
        "\"Save Ambika Patch (.PRO)\" = \"Ambika-Programm speichern (.PRO)\"\n"
        "\"Save Hellcat Patch (.yml)\" = \"Hellcat-Programm speichern (.yml)\"\n"
        "\"Ambika Multi (.MUL)\" = \"Ambika-Multi (.MUL)\"\n"
        "\"Save Ambika Multi (.MUL)\" = \"Ambika-Multi (.MUL) speichern\"\n"
        "\"Could not save file:\" = \"Datei konnte nicht gespeichert werden:\"\n"
        "\"Could not load file:\" = \"Datei konnte nicht geladen werden:\"\n"
        // ---- FX routing bar (global dry/wet, master EQ, topology buttons) ----
        "\"Dry/Wet\" = \"Dry/Wet\"\n"          // gear-standard term — kept
        "\"Global FX wet/dry\" = \"Globales FX-Dry/Wet\"\n"
        "\"FX master EQ \" = \"FX-Master-EQ \"\n"
        "\"Previous FX topology\" = \"Vorherige FX-Topologie\"\n"
        "\"Next FX topology\" = \"Nächste FX-Topologie\"\n"
        // ---- preset browser (submenus + name placeholder) ----
        "\"Ambika Factory\" = \"Ambika-Werk\"\n"
        "\"Hellcat Factory\" = \"Hellcat-Werk\"\n"
        "\"User\" = \"Benutzer\"\n"
        "\"(select a patch)\" = \"(Patch auswählen)\"\n"
        // step chevrons beside the name button (tooltip AND icon title)
        "\"Previous patch\" = \"Vorheriger Patch\"\n"
        "\"Next patch\" = \"Nächster Patch\"\n"
        // ---- mod matrix rows / tap-to-assign status strip ----
        "\"+ Add Modulation\" = \"+ Modulation hinzufügen\"\n"
        "\"Assign the next free slot\" = \"Weist den nächsten freien Platz zu\"\n"
        "\"Mute / bypass this modulation\" = \"Diese Modulation stummschalten / umgehen\"\n"
        "\"Assigned\" = \"Zugewiesen\"\n"
        "\"Mod Matrix full\" = \"Mod-Matrix voll\"\n"
        "\"Tap-to-assign modulation\" = \"Modulation per Tipp zuweisen\"\n"
        "\"Toggle the modulation pill bar\" = \"Modulationsleiste ein-/ausblenden\"\n"
        // Suffix-key fragments: the source name and the exit hint follow.
        "\"MOD assign armed: \" = \"MOD-Zuweisung aktiv: \"\n"
        "\" — tap a destination knob ([MOD] to exit)\" = \" — Ziel-Drehregler antippen ([MOD] zum Beenden)\"\n"
        "\"Tap a mod source first\" = \"Zuerst eine Modulationsquelle antippen\"\n"
        "\"Tap a mod source, then a knob\" = \"Zuerst eine Modulationsquelle, dann einen Drehregler antippen\"\n"
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
        // ---- Patch page voices tooltip (suffix-key fragments; the sentence
        // concatenates across five TRANS fragments) ----
        "\"How many voices this part plays at once from the shared \" = \"Wie viele Stimmen diese Part gleichzeitig aus dem gemeinsamen \"\n"
        "\"96-voice pool (0-16; 0 disables the part; the pool holds 6 x 16 so \" = \"96-Stimmen-Pool spielt (0-16; 0 deaktiviert die Part; der Pool fasst 6 x 16, sodass \"\n"
        "\"all parts can be maxed at the same time; the hardware voicecards \" = \"alle Parts gleichzeitig maximal ausgelastet sein können; die Hardware-Voicecards \"\n"
        "\"are shared out automatically for the individual outputs and the \" = \"werden automatisch für die Einzelausgänge und den \"\n"
        "\".MUL export).\" = \".MUL-Export aufgeteilt).\"\n"
        // ---- Accessibility names (row containers / wheels; suffix-key idiom so
        // the trailing slot number concatenates after the translated fragment) ----
        "\"Pitch Wheel\" = \"Pitchrad\"\n"
        "\"Mod Wheel\" = \"Modulationsrad\"\n"
        "\"Mod \" = \"Mod \"\n"
        "\"FX Mod \" = \"FX-Mod. \"\n"
        "\"Toggle virtual keyboard\" = \"Virtuelle Tastatur ein-/ausblenden\"\n"
        "\"Keyboard: octave \" = \"Tastatur: Oktave \"\n"
        "\"velocity\" = \"Anschlagstärke\"\n"
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
        "\"Each chained Ambika after the first gets one extra file (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Connect the units by MIDI. Load one file into each unit. The units then play as one big synth. Every voice stays.\" = \"Jeder verkettete Ambika nach dem ersten erhält eine zusätzliche Datei (\\\"-2.MUL\\\", \\\"-3.MUL\\\", ...). Verbinden Sie die Geräte per MIDI. Laden Sie in jedes Gerät eine Datei. Die Geräte spielen dann als ein großer Synth. Jede Stimme bleibt erhalten.\"\n"
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
