// State donor: builds a Hellcat processor state with FX1 = <type> enabled at
// full wet and writes the getStateInformation blob to a file (for feeding the
// real Standalone's Hellcat.settings filterState).
// Build: hellcat_state_donor (EXCLUDE_FROM_ALL). Run: ./hellcat_state_donor <fxType> <outFile>

#include <cstdio>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
void setInt (HellcatAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}
void setChoice (HellcatAudioProcessor& proc, const char* id, int index)
{
    if (auto* param = proc.getApvts().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (index)));
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: %s <fxType 0..24> <outFile>\n", argv[0]); return 2; }
    const int fx = std::atoi (argv[1]);

    juce::ScopedJuceInitialiser_GUI juceInit;
    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);   // SAW
    if (const char* oct = std::getenv ("HELLCAT_DONOR_OCTAVE"))
        setInt (proc, "part_octave", std::atoi (oct));
    if (fx != 0)
    {
        setChoice (proc, "fx1_type", fx);
        setInt (proc, "fx1_enabled", 1);
        setInt (proc, "fx1_drywet", 127);
        std::vector<int> p { 64, 64, 64, 64, 64 };
        if (fx == 4)  { p[2] = 0; }
        if (fx == 5)  { p[2] = 0; p[3] = 127; }
        if (fx == 6)  { p[3] = 0; }
        if (fx == 14) { p[1] = 0; }
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str (), p[static_cast<size_t> (k)]);
    }
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);
    juce::File (argv[2]).replaceWithData (blob.getData(), blob.getSize());
    // JUCE-flavour base64 ("<len>.<6-bit-packed>") for Hellcat.settings
    juce::File (juce::String (argv[2]) + ".jb64").replaceWithText (blob.toBase64Encoding ());
    std::printf ("wrote %d bytes for fx=%d -> %s (+ .jb64)\n", (int) blob.getSize(), fx, argv[2]);
    return 0;
}
