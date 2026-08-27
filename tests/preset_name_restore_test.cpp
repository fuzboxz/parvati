// preset_name_restore_test — the loaded preset NAME survives a state round-trip.
//
// getStateInformation must serialize loadedProgramName_ (root property
// "loaded_program_name") and setStateInformation must restore it, so a host
// or standalone session reload shows the last loaded preset in the patch
// browser instead of "Init". A legacy state WITHOUT the property must keep
// the name the load path already set (no reset to "Init").

#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "unified_test_runner.h"

namespace
{
// Serialize @p src's state, restore it into @p dst, return true on success.
bool transferState (HellcatAudioProcessor& src, HellcatAudioProcessor& dst)
{
    juce::MemoryBlock block;
    src.getStateInformation (block);
    dst.setStateInformation (block.getData(),
                             static_cast<int> (block.getSize()));
    return block.getSize() > 0;
}

// Re-serialize @p block with the "loaded_program_name" property REMOVED —
// the shape of a state saved by an older build (pre-persistence of the name).
juce::MemoryBlock stripLoadedName (const juce::MemoryBlock& block)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    CHECK(xml != nullptr, "saved state parses as XML");
    if (xml == nullptr)
        return {};
    if (xml->hasAttribute ("loaded_program_name"))
        xml->removeAttribute ("loaded_program_name");
    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary (*xml, out);
    return out;
}
}  // namespace

TEST(preset_name_restore_test)
{
    // ---- [1] The name round-trips through a full state transfer ----
    {
        HellcatAudioProcessor a, b;
        a.prepareToPlay (48000.0, 256);
        b.prepareToPlay (48000.0, 256);

        a.setLoadedProgramName ("Pad - Emberdrift");
        CHECK(transferState (a, b), "state transfer succeeds");
        CHECK(b.getLoadedProgramName() == "Pad - Emberdrift",
              "restored processor carries the saved preset name (got '"
                  + b.getLoadedProgramName() + "')");
    }

    // ---- [2] The property is present in the serialized tree ----
    {
        HellcatAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        a.setLoadedProgramName ("Lead - Buzzsaw");

        juce::MemoryBlock block;
        a.getStateInformation (block);
        auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
        CHECK(xml != nullptr, "state XML parses");
        if (xml != nullptr)
            CHECK(xml->getStringAttribute ("loaded_program_name") == "Lead - Buzzsaw",
                  "the state tree carries loaded_program_name");
    }

    // ---- [3] Backward compatibility: a legacy state without the property
    //          keeps the CURRENT name (no reset to "Init") ----
    {
        HellcatAudioProcessor a, b;
        a.prepareToPlay (48000.0, 256);
        b.prepareToPlay (48000.0, 256);

        a.setLoadedProgramName ("Anything");
        juce::MemoryBlock block;
        a.getStateInformation (block);
        const juce::MemoryBlock legacy = stripLoadedName (block);
        CHECK(legacy.getSize() > 0, "legacy (stripped) state re-serialized");

        // b has a name a load path already set; the legacy restore must keep it.
        b.setLoadedProgramName ("Keys - Rhodie");
        b.setStateInformation (legacy.getData(),
                              static_cast<int> (legacy.getSize()));
        CHECK(b.getLoadedProgramName() == "Keys - Rhodie",
              "a legacy state without the property keeps the current name (got '"
                  + b.getLoadedProgramName() + "')");
    }

    // ---- [4] A restore with the property REPLACES a different name ----
    {
        HellcatAudioProcessor a, b;
        a.prepareToPlay (48000.0, 256);
        b.prepareToPlay (48000.0, 256);

        a.setLoadedProgramName ("FX - Uprise");
        b.setLoadedProgramName ("Bass - FMee");
        CHECK(transferState (a, b), "state transfer succeeds");
        CHECK(b.getLoadedProgramName() == "FX - Uprise",
              "a state that carries a name replaces a different current name");
    }

    return true;
}
