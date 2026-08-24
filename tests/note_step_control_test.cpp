// NoteStepControl slider<->byte mapping + readout verification.
//
// The note-sequencer step knob remaps the raw 0..255 byte param onto a
// discrete 0..128 slider range (0 = Rest, 1..128 = MIDI notes 0..127 with
// the gate bit set). This pins the two statics that implement the
// recomposition — including the deliberately ASYMMETRIC decode (any
// gate-off byte, i.e. < 128, collapses to Rest: the note bits of a
// gate-off byte are data-loss by design, firmware Sequencer.h semantics) —
// plus the on-screen readout through a REAL constructed control, and the
// midiNoteName edges it renders with.
//
// Run: ./build_unified/parvati_unified_tests note_step_control_test

#include <cstdio>
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ui/NoteName.h"
#include "ui/NoteStepControl.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void checkEq (int got, int want, const char* msg)
{
    const bool ok = got == want;
    std::printf ("  %s: %s (got %d, want %d)\n", ok ? "ok  " : "FAIL", msg, got, want);
    if (! ok) ++g_failures;
}

// Exposes the protected ParamControl::slider_ readout lambda (the control's
// on-screen text) without touching production code.
class ReadoutProbe : public NoteStepControl
{
public:
    using NoteStepControl::NoteStepControl;
    juce::String readout (double sliderValue)
    {
        return slider_->textFromValueFunction (sliderValue);
    }
};
}  // namespace

TEST(note_step_control_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("[1] sliderToByte (slider 0..128 -> byte)\n");
    checkEq (NoteStepControl::sliderToByte (0.0), 0, "0 -> Rest (0)");
    checkEq (NoteStepControl::sliderToByte (1.0), 0x80, "1 -> note 0 + gate (0x80)");
    checkEq (NoteStepControl::sliderToByte (128.0), 0x80 | 127, "128 -> note 127 + gate (0xFF)");
    checkEq (NoteStepControl::sliderToByte (61.0), 0x80 | 60, "61 -> note 60 + gate (0xBC)");
    checkEq (NoteStepControl::sliderToByte (-1.0), 0, "negative clamps to Rest (0)");
    checkEq (NoteStepControl::sliderToByte (129.0), 0x80,
             "129 wraps to note 0 (the &0x7f mask, not a clamp)");

    std::printf ("\n[2] byteToSlider (byte -> slider 0..128)\n");
    checkEq (NoteStepControl::byteToSlider (0), 0, "0 -> Rest");
    checkEq (NoteStepControl::byteToSlider (0x80), 1, "0x80 -> 1 (note 0)");
    checkEq (NoteStepControl::byteToSlider (0x80 | 60), 61, "0x80|60 -> 61 (note 60)");
    checkEq (NoteStepControl::byteToSlider (0xFF), 128, "0xFF -> 128 (note 127)");
    // The asymmetric decode: a gate-OFF byte (any value < 128) is Rest — its
    // note bits cannot be recovered through the slider (by design; the raw
    // byte param still carries them for serialization).
    checkEq (NoteStepControl::byteToSlider (60), 0, "60 (gate off) -> Rest (asymmetric, data-loss by design)");
    checkEq (NoteStepControl::byteToSlider (127), 0, "127 (gate off, max) -> Rest");
    checkEq (NoteStepControl::byteToSlider (128), 1, "128 (lowest gate-on) -> 1");

    std::printf ("\n[3] Round-trip where it is bijective (gate-on bytes)\n");
    bool roundTripOk = true;
    for (int b = 0x80; b <= 0xFF; ++b)
        if (NoteStepControl::sliderToByte (static_cast<double> (NoteStepControl::byteToSlider (b))) != b)
            roundTripOk = false;
    check (roundTripOk, "sliderToByte(byteToSlider(b)) == b for all 128 gate-on bytes");

    std::printf ("\n[4] midiNoteName edges\n");
    check (midiNoteName (-1).isEmpty(), "note -1 -> empty");
    check (midiNoteName (128).isEmpty(), "note 128 -> empty");
    check (midiNoteName (60) == "C4", "note 60 -> C4");
    check (midiNoteName (0) == "C-1", "note 0 -> C-1");
    check (midiNoteName (127) == "G9", "note 127 -> G9");

    std::printf ("\n[5] Constructed control readout (real slider textFromValue)\n");
    {
        ParvatiAudioProcessor proc;
        const PatchParamDescriptor* stepDesc = nullptr;
        for (const auto& d : getPatchParamDescriptors())
            if (d.paramID == "seqnote_step0") { stepDesc = &d; break; }
        check (stepDesc != nullptr, "seqnote_step0 descriptor found");
        if (stepDesc != nullptr)
        {
            ReadoutProbe probe (proc, *stepDesc);
            check (probe.readout (0.0) == "Rest", "readout at slider 0 == \"Rest\"");
            check (probe.readout (61.0) == "C4", "readout at slider 61 == \"C4\" (note 60)");
            check (probe.readout (62.0) == "C#4", "readout at slider 62 == \"C#4\" (note 61)");
            check (probe.readout (128.0) == "G9", "readout at slider 128 == \"G9\" (note 127)");
        }
    }

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
