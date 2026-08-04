// Parvati-native YAML preset format round-trip + forward-compat verification.
//
// Proves the .parvati format carries EVERYTHING Parvati can do — including the
// Parvati-only `vca_curve` / `filter_card` options and the arp settings that the
// Ambika .PRO byte format silently drops. A patch sculpted with the SVF +
// exponential VCA survives a .parvati save+load intact, whereas a .PRO of the
// same state reverts them.
//
// Built by default. Run with: ./build/parvati_preset_test

#include <cmath>
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "ParvatiPreset.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Sets an APVTS parameter by raw (denormalized) value via the host notification
// path (works for AudioParameterInt and AudioParameterChoice).
void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (value)));
}

bool rawEqual (float a, float b) { return std::fabs (a - b) <= 0.5f; }

int countApvtsMismatches (ParvatiAudioProcessor& a, ParvatiAudioProcessor& b)
{
    int mism = 0;
    for (const auto& d : getPatchParamDescriptors())
    {
        const float va = a.getApvts().getRawParameterValue (d.paramID)->load();
        const float vb = b.getApvts().getRawParameterValue (d.paramID)->load();
        if (! rawEqual (va, vb)) ++mism;
    }
    return mism;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    using namespace parvati::preset;

    // ---------------------------------------------------------------------
    std::printf ("[1] YAML emit/parse round-trips a var tree\n");
    {
        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty ("format", "parvati-patch");
        root->setProperty ("version", 1);
        root->setProperty ("name", "Test");
        auto params = std::make_unique<juce::DynamicObject>();
        params->setProperty ("osc1_shape", 1);
        params->setProperty ("osc1_range", -12);
        params->setProperty ("vca_curve", 1);
        root->setProperty ("params", juce::var (params.release()));

        const juce::String text = emitParvatiYaml (juce::var (root.release()));
        const juce::var parsed = parseParvatiYaml (text);
        check (parsed.isObject(), "parsed tree is an object");
        check ((int) parsed["version"] == 1, "version round-trips as int");
        check ((int) parsed["params"]["osc1_shape"] == 1, "nested param round-trips");
        check ((int) parsed["params"]["osc1_range"] == -12, "negative int round-trips");
        check ((int) parsed["params"]["vca_curve"] == 1, "vca_curve round-trips");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[2] .parvati patch round-trip: vca_curve/filter_card/arp survive\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Set several params, INCLUDING the Parvati-only options + arp that .PRO
        // drops. Use non-default values so a drop is detectable.
        setParam (a, "osc1_shape", 1);         // Saw
        setParam (a, "osc1_range", -12);
        setParam (a, "env2_sustain", 80);
        setParam (a, "vca_curve", 1);          // Exponential  <-- .PRO drops this
        setParam (a, "filter_card", 2);        // 2-pole SVF    <-- .PRO drops this
        setParam (a, "arp_mode", 1);           // Arp           <-- .PRO drops this
        setParam (a, "arp_resolution", 10);
        a.syncAllParamsToEngine();

        const juce::String yaml = serializeParvatiPatch (a);
        check (yaml.contains ("vca_curve: 1"), "YAML contains vca_curve=1");
        check (yaml.contains ("filter_card: 2"), "YAML contains filter_card=2");
        check (yaml.contains ("arp_mode: 1"), "YAML contains arp_mode=1");
        check (yaml.contains ("parvati_version: 0.1.0"), "parvati_version is 0.1.0 (pre-release)");

        check (applyParvatiPatch (b, yaml), "applyParvatiPatch parses + applies");

        const int mism = countApvtsMismatches (a, b);
        std::printf ("     APVTS mismatches = %d\n", mism);
        check (mism == 0, "end-to-end .parvati: EVERY APVTS value matches (incl options/arp)");

        // Specifically prove the gap is closed:
        check (rawEqual (a.getApvts().getRawParameterValue ("vca_curve")->load(),
                         b.getApvts().getRawParameterValue ("vca_curve")->load()),
               "vca_curve survives .parvati round-trip");
        check (rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                         b.getApvts().getRawParameterValue ("filter_card")->load()),
               "filter_card survives .parvati round-trip");
        check (rawEqual (a.getApvts().getRawParameterValue ("arp_mode")->load(),
                         b.getApvts().getRawParameterValue ("arp_mode")->load()),
               "arp_mode survives .parvati round-trip");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[3] Contrast: .PRO of the same state DROPS the options\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        setParam (a, "vca_curve", 1);
        setParam (a, "filter_card", 2);
        a.syncAllParamsToEngine();

        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_preset_gap.PRO");
        check (a.saveProgramFile (tmp), ".PRO saved");
        check (b.loadProgramFile (tmp), ".PRO loaded");

        const bool vcaKept = rawEqual (a.getApvts().getRawParameterValue ("vca_curve")->load(),
                                       b.getApvts().getRawParameterValue ("vca_curve")->load());
        const bool cardKept = rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                                        b.getApvts().getRawParameterValue ("filter_card")->load());
        std::printf ("     .PRO keeps vca_curve=%d, filter_card=%d (expect 0)\n", vcaKept, cardKept);
        check (! vcaKept, ".PRO DROPS vca_curve (documenting the gap this format fixes)");
        check (! cardKept, ".PRO DROPS filter_card (documenting the gap this format fixes)");
        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[4] Forward-compat: unknown keys + typo'd paramIDs are ignored\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        juce::String yaml =
            "format: parvati-patch\n"
            "version: 1\n"
            "name: \"Future\"\n"
            "params:\n"
            "  future_param: 99\n"        // unknown key -> ignored
            "  osc1_shape: 2\n"           // Square
            "  typo_paramID: 5\n"         // unknown -> ignored
            "  env2_sustain: 60\n";       // known -> applied

        check (applyParvatiPatch (proc, yaml), "load succeeds with unknown keys");
        check ((int) proc.getApvts().getRawParameterValue ("osc1_shape")->load() == 2,
               "known value (osc1_shape=2) applied despite unknown keys");
        check ((int) proc.getApvts().getRawParameterValue ("env2_sustain")->load() == 60,
               "known value (env2_sustain=60) applied");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[5] .parvati MULTI round-trip (all 6 parts + routing)\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Customize a couple of parts' routing + arp directly on the engine.
        // (Write pendingConfig_ + flag configDirty_ -- the authoritative arp/seq
        // config; the live objects lag it until the audio thread services
        // configDirty_, and serializeParvatiMulti reads pendingConfig_.)
        a.getEngine().getPart (1).pendingConfig_.arpOctave = 3;
        a.getEngine().getPart (1).configDirty_.store (true);
        a.getEngine().setPartChannel (1, 3);
        a.getEngine().setPartKeyrange (1, 36, 60);
        a.getEngine().getPart (2).pendingConfig_.arpResolution = 6;
        a.getEngine().getPart (2).configDirty_.store (true);
        a.getEngine().setPartChannel (2, 5);
        // And a global option.
        setParam (a, "filter_card", 1);   // SSM2164
        a.syncAllParamsToEngine();

        const juce::String yaml = serializeParvatiMulti (a);
        check (yaml.contains ("format: parvati-multi"), "multi YAML has format tag");
        check (applyParvatiMulti (b, yaml), "applyParvatiMulti parses + applies");

        int routeMism = 0;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            if (a.getEngine().getPartChannel (i) != b.getEngine().getPartChannel (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeLow (i) != b.getEngine().getPartKeyrangeLow (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeHigh (i) != b.getEngine().getPartKeyrangeHigh (i)) ++routeMism;
            if (a.getEngine().getPartVoiceAllocation (i) != b.getEngine().getPartVoiceAllocation (i)) ++routeMism;
        }
        std::printf ("     per-part routing mismatches = %d\n", routeMism);
        check (routeMism == 0, "all 6 parts' channel/keyrange/alloc match");

        check (a.getEngine().getPart (1).pendingConfig_.arpOctave == b.getEngine().getPart (1).pendingConfig_.arpOctave,
               "Part 1 arp_octave preserved");
        check (a.getEngine().getPart (2).pendingConfig_.arpResolution == b.getEngine().getPart (2).pendingConfig_.arpResolution,
               "Part 2 arp_resolution preserved");
        check (rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                         b.getApvts().getRawParameterValue ("filter_card")->load()),
               "global filter_card option preserved");

        // Current-part (0) APVTS should match too.
        const int apvtsMism = countApvtsMismatches (a, b);
        check (apvtsMism == 0, "current-part APVTS matches after multi round-trip");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PRESET TEST: FAILURES" : "PRESET TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
