// Host-visible parameter TEXT + GROUPING verification (ParameterLayout.cpp).
//
// Pins the two host-integration QoL seams:
//   [1] GROUP STRUCTURE  — the 13 AudioProcessorParameterGroups (VST3 Units /
//       AU grouped parameter lists) exist in first-appearance order, and the
//       recursive parameter count equals the descriptor-table count.
//   [2] WITHIN-GROUP ORDER — each group's parameter sequence is the
//       descriptor-table order restricted to that group's members (hosts that
//       sort by group keep the stable Parvati ordering inside every unit).
//   [3] FLATTENED ORDER — the two reorder spans are contained: the prefix
//       through modif4_op and the suffix from fx1_type onward are IDENTICAL
//       to the pre-grouping descriptor (index) order; only the 23 env+lfo
//       params and the 84-param part..global span permute internally. Every
//       shipped wrapper format references parameters by string/hash ID
//       (verified in juce_audio_plugin_client), not by ordinal.
//   [4] VALUE-TO-TEXT — representative AudioParameterInt params carry the
//       pure formatters (Hz / ms / ∞ / cents / % / dB / On-Off), including the
//       fx{N}_paramK SIBLING lookup (text follows the slot's CURRENT FxType)
//       and the master-EQ hoisted readouts (fxEqLowToString/fxEqDbToString).
//       Choice params keep their choice-list text untouched.
//   [5] TEXT-TO-VALUE — typed entry maps through the DISPLAYED unit (typing
//       "100" into Dry/Wet = 100% = 127; "+6" into FX EQ Mid = +6 dB = 96),
//       and plain-integer entry still works everywhere.
//   [6] part_select stays non-automatable (regression: the grouping rewrite
//       must keep .withAutomatable(false)).
//
// Built by default. Run with: ./build/parvati_host_param_text_test

#include <cstdio>
#include "unified_test_runner.h"
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

juce::String textFor (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        return param->getText (param->convertTo0to1 (static_cast<float> (value)), 64);
    return "<missing>";
}

int valueForText (ParvatiAudioProcessor& proc, const char* id, const char* text)
{
    if (auto* param = proc.getApvts().getParameter (id))
        return juce::roundToInt (param->convertFrom0to1 (param->getValueForText (text)));
    return -999;
}

void setChoice (ParvatiAudioProcessor& proc, const char* id, int index)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
            choice->setValueNotifyingHost (choice->convertTo0to1 (static_cast<float> (index)));
}
}  // namespace

TEST(host_param_text_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto& descs = getPatchParamDescriptors();

    std::printf ("[1] Group structure (VST3 units)\n");
    ParvatiAudioProcessor proc;

    const auto& root = proc.getParameterTree();
    const auto groups = root.getSubgroups (false);
    // First-appearance order of the descriptor table.
    const char* const kExpectedGroups[] = {
        "Oscillators", "Mixer", "Filter", "Envelopes", "LFOs", "Mod Matrix",
        "Modifiers", "Part", "Sequencer", "Arpeggiator", "Global", "FX", "FX Mod" };
    check (groups.size() == 13, "13 parameter groups");
    int orderOk = 0;
    {
        int idx = 0;
        for (const auto* g : groups)
        {
            if (idx >= 13) break;
            if (g->getName() == kExpectedGroups[idx])
                ++orderOk;
            ++idx;
        }
    }
    std::printf ("     groups in first-appearance order: %d/13\n", orderOk);
    check (orderOk == 13, "group names + order match the expected 13");

    int totalInGroups = 0;
    for (const auto* g : groups)
        totalInGroups += g->getParameters (true).size();
    std::printf ("     recursive parameter count in groups: %d (descriptors: %zu)\n",
                 totalInGroups, descs.size());
    check (totalInGroups == (int) descs.size(),
           "every descriptor parameter lives in exactly one group");

    std::printf ("\n[2] Within-group order == descriptor-table order\n");
    int groupsOrdered = 0;
    for (const auto* g : groups)
    {
        // Walk the descriptor table; collect this group's ids in table order.
        std::vector<std::string> expected;
        for (const auto& d : descs)
            if (proc.getParameterTree().getGroupsForParameter (
                    proc.getApvts().getParameter (d.paramID)).getLast() == g)
                expected.push_back (d.paramID);

        std::vector<std::string> actual;
        for (auto* p : g->getParameters (false))
            if (const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (p))
                actual.push_back (withId->getParameterID().toStdString());

        if (expected == actual)
            ++groupsOrdered;
        else
            std::printf ("     group '%s': order mismatch (%zu vs %zu)\n",
                         g->getName().toStdString().c_str(), actual.size(), expected.size());
    }
    std::printf ("     groups with descriptor-order members: %d/13\n", groupsOrdered);
    check (groupsOrdered == 13, "every group preserves descriptor-table order");

    std::printf ("\n[3] Flattened order: stable outside the two reorder spans\n");
    {
        const auto& params = proc.getParameters();
        std::vector<std::string> flat;
        for (const auto* p : params)
            if (const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (p))
                flat.push_back (withId->getParameterID().toStdString());

        // Compare the flat list against the descriptor table with the two
        // reorder spans (env+lfo: positions of env1_attack..voice_lfo_rate;
        // part..global: part_volume..filter_drive) excised from BOTH sides.
        // Everything else — the prefix and the fx/fxmod tail — must match the
        // pre-grouping descriptor order EXACTLY, pinning the contained reorder.
        const auto isEnvLfoSpan = [] (const std::string& id)
        {
            const juce::String jid (id);
            return jid.startsWith ("env") || jid.startsWith ("voice_lfo");
        };
        const auto isPartGlobalSpan = [] (const std::string& id)
        {
            const juce::String jid (id);
            return jid.startsWith ("part") || jid.startsWith ("seq") || jid.startsWith ("arp")
                   || id == "vca_curve" || id == "filter_card" || id == "filter_drive";
        };

        std::vector<std::string> flatStable, descStable;
        for (const auto& id : flat)
            if (! (isEnvLfoSpan (id) || isPartGlobalSpan (id)))
                flatStable.push_back (id);
        for (const auto& d : descs)
        {
            const juce::String jid (d.paramID);
            if (! (jid.startsWith ("env") || jid.startsWith ("voice_lfo")
                   || jid.startsWith ("part") || jid.startsWith ("seq") || jid.startsWith ("arp")
                   || d.paramID == "vca_curve" || d.paramID == "filter_card" || d.paramID == "filter_drive"))
                descStable.push_back (d.paramID);
        }
        check (flatStable == descStable,
               "indices unchanged outside the env/lfo + part..global spans");

        // Stronger, positional pin: the FX tail (fx1_type onward) keeps its
        // exact historical indices.
        std::vector<std::string> suffix;
        bool started = false;
        for (const auto& d : descs)
        {
            if (d.paramID == "fx1_type") started = true;
            if (started) suffix.push_back (d.paramID);
        }
        if (flat.size() >= suffix.size())
        {
            const std::vector<std::string> flatSuffix (flat.end() - (std::ptrdiff_t) suffix.size(), flat.end());
            check (flatSuffix == suffix,
                   "index-ordered list from fx1_type onward == descriptor order");
        }
        else
            check (false, "flat parameter list covers the fx suffix");
    }

    std::printf ("\n[4] Value-to-text (host automation display)\n");
    check (textFor (proc, "osc1_detune", 64) == "+50ct", "osc1_detune 64 -> \"+50ct\"");
    // LUT is fastest-first (resources_data.cpp:31): byte 0 -> inc 65535
    // -> t = (65536*40)/(65535*39216) = 1.0200 ms -> "1ms" (the <1ms branch is
    // NOT taken). Byte 127 -> inc 1 -> 66.846 s -> "66.8s". (inc==0 never occurs
    // in the LUT; envTimeToString's infinity glyph stays defensive.)
    check (textFor (proc, "env1_attack", 0) == "1ms",
           "env1_attack 0 (fastest, inc=65535) -> \"1ms\"");
    check (textFor (proc, "env1_attack", 127) == "66.8s",
           "env1_attack 127 (slowest, inc=1) -> \"66.8s\"");
    check (textFor (proc, "mod1_amount", 63) == "+100%", "mod1_amount 63 -> \"+100%\"");
    check (textFor (proc, "part_octave", 1) == "+1oct", "part_octave 1 -> \"+1oct\"");
    {
        const auto cutoffText = textFor (proc, "filter1_cutoff", 96);
        check (cutoffText.contains ("Hz") || cutoffText.contains ("k"),
               "filter1_cutoff shows Hz (not the raw byte)");
    }
    // FX master section (hoisted readouts).
    check (textFor (proc, "fx_mix", 127) == "100%", "fx_mix 127 -> \"100%\"");
    check (textFor (proc, "fx_eq_mid", 64) == "0dB", "fx_eq_mid 64 -> \"0dB\"");
    check (textFor (proc, "fx_eq_mid", 96) == "+6dB", "fx_eq_mid 96 -> \"+6dB\"");
    check (textFor (proc, "fx_eq_low", 0) == "Off", "fx_eq_low 0 -> \"Off\"");
    check (textFor (proc, "fx_eq_low", 127).contains ("k"), "fx_eq_low 127 -> \"1k5\" (kHz)");
    // FX slot basics + fxmod amounts.
    check (textFor (proc, "fx1_enabled", 1) == "On", "fx1_enabled 1 -> \"On\"");
    check (textFor (proc, "fx1_drywet", 64) == "50%", "fx1_drywet 64 -> \"50%\"");
    check (textFor (proc, "fxmod1_amount", 63) == "+100%", "fxmod1_amount 63 -> \"+100%\"");
    check (textFor (proc, "fxmod1_amount", -63) == "-100%", "fxmod1_amount -63 -> \"-100%\"");
    check (textFor (proc, "fx_order", 3) == "3", "fx_order stays raw (by design)");

    // fx{N}_paramK text follows the slot's CURRENT type (sibling lookup).
    setChoice (proc, "fx1_type", 2 /* PitchShifter */);
    check (textFor (proc, "fx1_param1", 127) == "+12.0", "fx1_param1 (PitchShifter) 127 -> \"+12.0\"");
    setChoice (proc, "fx1_type", 16 /* Overdrive */);
    check (textFor (proc, "fx1_param1", 127) == "16.0x", "fx1_param1 (Overdrive) 127 -> \"16.0x\"");
    setChoice (proc, "fx1_type", 11 /* ClockedDelay */);
    check (textFor (proc, "fx1_param1", 127) == "1/16", "fx1_param1 (ClockedDelay) 127 -> \"1/16\"");
    setChoice (proc, "fx1_type", 0 /* None: dimensionless fallback */);
    check (textFor (proc, "fx1_param1", 64) == "50%", "fx1_param1 (None) 64 -> \"50%\" fallback");

    // Choice params untouched: text == the choice label.
    check (textFor (proc, "osc1_shape", 1) == "Saw", "osc1_shape 1 -> \"Saw\" (choice list)");

    std::printf ("\n[5] Text-to-value (typed parameter entry)\n");
    check (valueForText (proc, "fx1_drywet", "100") == 127, "drywet \"100\" -> 127 (100%)");
    check (valueForText (proc, "fx1_drywet", "0") == 0, "drywet \"0\" -> 0");
    check (valueForText (proc, "fx_eq_mid", "+6") == 96, "fx_eq_mid \"+6\" -> 96 (+6 dB)");
    check (valueForText (proc, "fx_eq_mid", "0") == 64, "fx_eq_mid \"0\" -> 64 (unity)");
    check (valueForText (proc, "fxmod1_amount", "100") == 63, "fxmod1_amount \"100\" -> 63");
    check (valueForText (proc, "fx1_enabled", "On") == 1, "fx1_enabled \"On\" -> 1");
    check (valueForText (proc, "fx1_enabled", "off") == 0, "fx1_enabled \"off\" -> 0");
    check (valueForText (proc, "osc1_param", "77") == 77, "osc1_param \"77\" -> 77 (plain int)");
    check (valueForText (proc, "part_octave", "2") == 2, "part_octave \"2\" -> 2");
    // ---- extensions: the untested parse branches ----
    check (valueForText (proc, "fx_eq_low", "off") == 0, "fx_eq_low \"off\" -> 0");
    // fx_eq_low Hz text is NOT invertible: the DISPLAY for byte 127 is \"1k5\",
    // but typed entry takes the leading integer -> 1. Pinned as documented
    // non-invertibility (semantic strings stay raw-integer typed entry).
    check (valueForText (proc, "fx_eq_low", "1k5") == 1,
           "fx_eq_low \"1k5\" -> 1 (Hz display text is non-invertible)");
    check (valueForText (proc, "fx_eq_high", "-12") == 0, "fx_eq_high \"-12\" -> 0 (-12 dB rail)");
    check (valueForText (proc, "fx_eq_high", "+12") == 127, "fx_eq_high \"+12\" -> 127 (+12 dB rail)");
    // fx{N}_paramK typed entry stays RAW-INTEGER (semantic strings like \"C4\"
    // or \"1/16\" are not generally invertible).
    check (valueForText (proc, "fx1_param1", "100") == 100,
           "fx1_param1 \"100\" -> 100 (raw int, NOT percent-mapped)");
    check (valueForText (proc, "fxmod1_amount", "-100") == -63,
           "fxmod1_amount \"-100\" -> -63 (negative typed entry)");
    // Garbage typed entry: non-numeric strings parse as 0 and land on the
    // parameter's 0 (clamped by the normalisable range) — never a wild value.
    check (valueForText (proc, "fx1_drywet", "") == 0, "garbage \"\" -> 0");
    check (valueForText (proc, "fxmod1_amount", "abc") == 0, "garbage \"abc\" -> 0");
    check (valueForText (proc, "osc1_param", "??") == 0, "garbage \"??\" -> 0");

    std::printf ("\n[6] part_select still non-automatable\n");
    check (! proc.getApvts().getParameter ("part_select")->isAutomatable(),
           "part_select excluded from host automation (isAutomatable == false)");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
