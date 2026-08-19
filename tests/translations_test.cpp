// Chrome-translation verification (ui/Translations.cpp).
//
// The FR/DE tables are hand-synced (~120 quoted pairs each); a key that
// drifts out of one table silently falls back to English in that language
// and nothing catches it. This test pins:
//   [1] the selectable-language list (order matters — the Settings combo is
//       seeded from it);
//   [2] FR/DE KEY-SET PARITY: both tables, parsed through the same
//       juce::LocalisedStrings machinery installLanguage uses, carry the
//       exact same key set (a missing key in either is reported by name);
//   [3] runtime lookups: a translated string, an unknown-key passthrough,
//       and the uninstall path ("en" / unrecognised code -> null mappings,
//       raw English).
//
// Key sets are read from LocalisedStrings::getCurrentMappings() AFTER
// installLanguage() — the exact table the editor ships, not a copy.
//
// Built by default. Run with: ./build/parvati_translations_test

#include <cstdio>
#include <set>

#include <juce_core/juce_core.h>

#include "ui/Translations.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// The keys of the CURRENTLY installed mappings (empty set when mappings are
// null — the English identity path installs nothing).
std::set<juce::String> installedKeys()
{
    std::set<juce::String> keys;
    if (const auto* mappings = juce::LocalisedStrings::getCurrentMappings())
        for (const auto& k : mappings->getMappings().getAllKeys())
            keys.insert (k.toLowerCase());
    return keys;
}
}  // namespace

int main()
{
    std::printf ("[1] Available languages\n");
    {
        const auto& langs = getAvailableLanguages();
        check (langs.size() == 4, "4 selectable languages");
        const char* const expected[] = { "auto", "en", "fr", "de" };
        bool orderOk = langs.size() == 4;
        for (size_t i = 0; i < langs.size() && i < 4; ++i)
            if (langs[i].first != expected[i]) orderOk = false;
        check (orderOk, "language order is { auto, en, fr, de } (Settings combo order)");
    }

    std::printf ("\n[2] FR/DE key-set parity (hand-synced tables)\n");
    juce::StringArray frOnly, deOnly;
    {
        installLanguage ("fr");
        const auto frKeys = installedKeys();
        installLanguage ("de");
        const auto deKeys = installedKeys();
        std::printf ("     fr keys: %zu, de keys: %zu\n", frKeys.size(), deKeys.size());
        check (! frKeys.empty() && frKeys.size() == deKeys.size(),
               "both tables non-empty and the same size");

        for (const auto& k : frKeys) if (deKeys.count (k) == 0) frOnly.add (k);
        for (const auto& k : deKeys) if (frKeys.count (k) == 0) deOnly.add (k);
        if (! frOnly.isEmpty())
            for (const auto& k : frOnly) std::printf ("       fr-only: \"%s\"\n", k.toRawUTF8());
        if (! deOnly.isEmpty())
            for (const auto& k : deOnly) std::printf ("       de-only: \"%s\"\n", k.toRawUTF8());
        check (frOnly.isEmpty() && deOnly.isEmpty(), "identical key sets (no silent English fallback)");
    }

    std::printf ("\n[3] Runtime lookups\n");
    installLanguage ("fr");
    check (juce::LocalisedStrings::getCurrentMappings() != nullptr,
           "installLanguage(\"fr\") installs mappings");
    check (juce::translate ("Settings") == juce::CharPointer_UTF8 ("Réglages"),
           "TRANS(\"Settings\") == \"Réglages\" under fr");
    check (juce::translate ("not a key") == "not a key",
           "unknown key passes through untranslated");
    installLanguage ("de");
    check (juce::translate ("Settings") == "Einstellungen",
           "TRANS(\"Settings\") == \"Einstellungen\" under de");
    // Uninstall paths: "en" and any unrecognised code both clear the mappings
    // (identity English — byte-identical to the un-localised build).
    installLanguage ("en");
    check (juce::LocalisedStrings::getCurrentMappings() == nullptr,
           "installLanguage(\"en\") clears mappings (identity)");
    installLanguage ("zz");
    check (juce::LocalisedStrings::getCurrentMappings() == nullptr,
           "unrecognised code (\"zz\") is safe: no mappings");
    check (juce::translate ("Settings") == "Settings",
           "raw English after uninstall");

    // Restore the identity state for anything that runs after this test.
    installLanguage ("en");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
