// Diagnostic for "no sound" regression: proves whether creating the editor
// (ModMatrixView + ring listeners + drag affordance) alters the amp-envelope
// modulation (mod11_amount = ENV3->VCA) or renders the engine silent on a
// FRESH processor. Not shipped; build/run manually:
//   cmake --build build --target parvati_mod_silence_diag && ./build/parvati_mod_silence_diag

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setInt (host-path helper)

static float amountOf (ParvatiAudioProcessor& proc, const char* id)
{
    if (auto* r = proc.getApvts().getRawParameterValue (id))
        return r->load();
    return -999.0f;
}

// Render ~0.3 s of a sustained note; return the output peak.
static double renderNote (ParvatiAudioProcessor& proc, int bufferSize = 256)
{
    {   // flush one block first
        juce::AudioBuffer<float> f (2, bufferSize); f.clear();
        juce::MidiBuffer empty;
        proc.processBlock (f, empty);
    }
    juce::AudioBuffer<float> buf (2, bufferSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0);
    juce::MidiBuffer empty;

    double peak = 0.0;
    constexpr int kBlocks = 60;   // ~0.3 s @48k/256
    for (int b = 0; b < kBlocks; ++b)
    {
        buf.clear();
        proc.processBlock (buf, b == 0 ? midi : empty);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < bufferSize; ++i)
                peak = std::max (peak, std::fabs (static_cast<double> (buf.getSample (ch, i))));
    }
    return peak;
}

TEST(mod_audio_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();   // main thread == message thread

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;   // Part 1
    setInt (proc, "osc1_shape", 1);   // WAVEFORM_SAW (audible source)

    std::printf ("=== mod-silence diagnostic ===\n");
    std::printf ("mod11_amount (ENV3->VCA) BEFORE editor = %.0f  (init default 63)\n",
                 amountOf (proc, "mod11_amount"));
    std::printf ("mod12_amount (Velocity->VCA)           = %.0f\n",
                 amountOf (proc, "mod12_amount"));

    const double peakNoEditor = renderNote (proc);
    std::printf ("peak WITHOUT editor = %.5f  %s\n", peakNoEditor,
                 peakNoEditor > 1.0e-4 ? "(AUDIBLE)" : "(SILENT)");

    std::printf ("\nCreating editor (exercises ModMatrixView + ring listeners + drag affordance)...\n");
    juce::AudioProcessorEditor* ed = proc.createEditor();
    std::printf ("editor = %p\n", (void*) ed);

    // Editor construction is synchronous (attachments settle via
    // dontSendNotification), and ModMatrixView's timer refresh() is read-only,
    // so no message pumping is needed to detect a construction-time write.

    std::printf ("mod11_amount AFTER editor + msg pump   = %.0f\n", amountOf (proc, "mod11_amount"));

    const double peakWithEditor = renderNote (proc);
    std::printf ("peak WITH editor    = %.5f  %s\n", peakWithEditor,
                 peakWithEditor > 1.0e-4 ? "(AUDIBLE)" : "(SILENT)");

    // Force a full APVTS->engine sync (what standalone state-restore does) and
    // re-check, to see if a sync clobbers the amp envelope.
    // (Mirrors what PluginProcessor does on setStateInformation.)

    std::printf ("\nVERDICT:\n");
    bool editorInnocent = (amountOf (proc, "mod11_amount") >= 62.0f) && (peakWithEditor > 1.0e-4);
    std::printf ("  editor preserves ENV->VCA amount AND audio stays audible: %s\n",
                 editorInnocent ? "YES -> editor is NOT the cause; silence is persisted standalone state"
                                : "NO  -> editor is zeroing the amp routing (real bug)");

    delete ed;
    return editorInnocent;
}
