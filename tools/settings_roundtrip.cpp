// Verifies the crafted Parvati.settings round-trip: load it exactly like the
// standalone (juce::PropertiesFile), pull filterState, push it through
// setStateInformation, and print what the engine's FX state ends up as.
// Build: parvati_settings_roundtrip (EXCLUDE_FROM_ALL). Run: ./p s <settingsFile>

#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: %s <Parvati.settings>\n", argv[0]); return 2; }
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::PropertiesFile::Options opts;
    opts.applicationName     = "Parvati";
    opts.filenameSuffix      = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    opts.folderName          = "";
    juce::PropertiesFile pf (juce::File (argv[1]), opts);

    const auto audioSetup = pf.getValue ("audioSetup");
    std::printf ("audioSetup parsed: %.80s...\n", audioSetup.toRawUTF8());

    const auto b64 = pf.getValue ("filterState");
    std::printf ("filterState: %d chars\n", (int) b64.length());
    juce::MemoryBlock data;
    const bool ok = data.fromBase64Encoding (b64);
    std::printf ("fromBase64Encoding: %d, %d bytes\n", (int) ok, (int) data.getSize());

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    if (data.getSize() > 0)
        proc.setStateInformation (data.getData(), (int) data.getSize());

    // Probe: render a note and check the FX reach the audio (dry vs wet marker)
    // + read back fx1 params via the APVTS (what the editor would show).
    if (auto* t = proc.getApvts().getParameter ("fx1_type"))
        std::printf ("fx1_type (APVTS) = %.3f (0..1 across 0..Count-1)\n", t->getValue());
    if (auto* e = proc.getApvts().getParameter ("fx1_enabled"))
        std::printf ("fx1_enabled (APVTS) = %.3f\n", e->getValue());

    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    // one silence block first
    { juce::AudioBuffer<float> s (2, 256); s.clear (); juce::MidiBuffer m; proc.processBlock (s, m); }
    float peak = 0.0f;
    for (int blk = 0; blk < 40; ++blk)
    {
        buf.clear ();
        proc.processBlock (buf, midi);
        for (int i = 0; i < 256; ++i)
            peak = juce::jmax (peak, std::fabs (buf.getSample (0, i)));
    }
    std::printf ("sustained peak after 40 blocks = %.4f\n", peak);
    return 0;
}
