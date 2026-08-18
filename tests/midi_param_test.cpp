// MIDI CC/NRPN -> parameter mapping verification (spec F.4).
//
// Proves incoming MIDI CC (performance) and NRPN (editor) messages drive the
// plugin's APVTS parameters — and reach the engine's patch/part bytes — exactly
// as the Ambika hardware does (firmware controller/part.cc::OnControlChange,
// controller/parameter.cc midi_cc_map / midi_nrpn_map).
//
// NRPN: address == NRPN LSB; value = DIRECT param value (clamped).
// CC:   address = param.offset + param.stride*instance; value = 7-bit SCALED.

#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Push an NRPN (address, value) into a MIDI buffer as the standard CC sequence:
//   CC99 (NRPN MSB) = 0, CC98 (NRPN LSB) = address, CC38 (data-entry LSB) = value.
void addNrpn (juce::MidiBuffer& buf, int address, int value)
{
    buf.addEvent (juce::MidiMessage::controllerEvent (1, 99, 0),              0);
    buf.addEvent (juce::MidiMessage::controllerEvent (1, 98, address & 0x7f), 1);
    buf.addEvent (juce::MidiMessage::controllerEvent (1, 38, value & 0x7f),   2);
}

// Push a single CC (controller, value).
void addCc (juce::MidiBuffer& buf, int controller, int value)
{
    buf.addEvent (juce::MidiMessage::controllerEvent (1, controller, value & 0x7f), 0);
}

// Run the MIDI through the processor (handleBuffer runs first inside processBlock).
void processMidi (ParvatiAudioProcessor& proc, const juce::MidiBuffer& midi)
{
    juce::AudioBuffer<float> buf (2, 64);
    buf.clear();
    proc.processBlock (buf, const_cast<juce::MidiBuffer&> (midi));
}

float rawVal (ParvatiAudioProcessor& proc, const char* id)
{
    return proc.getApvts().getRawParameterValue (id)->load();
}

uint8_t patchByte (ParvatiAudioProcessor& proc, int offset)
{
    return proc.getEngine().getPart (0).patchBytes[(size_t) offset];
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== Parvati MIDI CC/NRPN parameter mapping (spec F.4) ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 64);

    // ---- NRPN: address == byte; value is the DIRECT param value (clamped) ----
    std::printf ("\n[1] NRPN sets the direct parameter value\n");
    {
        juce::MidiBuffer m;
        addNrpn (m, 0, 2);   // addr 0 = osc1_shape; value 2 = SQUARE
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "osc1_shape"));
        std::printf ("     NRPN(0,2) -> osc1_shape = %d (expect 2)\n", v);
        check (v == 2, "NRPN addr 0 -> osc1_shape = 2 (SQUARE)");
    }
    {
        juce::MidiBuffer m;
        addNrpn (m, 16, 96);   // addr 16 = filter1_cutoff; value 96
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "filter1_cutoff"));
        std::printf ("     NRPN(16,96) -> filter1_cutoff = %d (expect 96)\n", v);
        check (v == 96, "NRPN addr 16 -> filter1_cutoff = 96");
    }

    // End-to-end: the NRPN-set value reaches the engine patch byte.
    std::printf ("\n[2] NRPN value reaches the engine patch byte\n");
    proc.syncAllParamsToEngine();
    {
        const int b = patchByte (proc, 0);   // osc1_shape byte
        std::printf ("     engine part0 patchBytes[0] = %d (expect 2)\n", b);
        check (b == 2, "NRPN osc1_shape reached engine patch byte 0");
    }

    // ---- NRPN stride: addr 32 = env2_attack (env_lfo instance 1) ----
    std::printf ("\n[3] NRPN addresses each env-lfo slot (stride)\n");
    {
        juce::MidiBuffer m;
        addNrpn (m, 32, 50);   // addr 32 = env2_attack (byte 32)
        processMidi (proc, m);
        proc.syncAllParamsToEngine();
        const int b = patchByte (proc, 32);
        std::printf ("     NRPN(32,50) -> engine patchBytes[32] (env2_attack) = %d (expect 50)\n", b);
        check (b == 50, "NRPN addr 32 -> env2_attack byte 32 = 50");
    }

    // ---- CC: value is 7-bit SCALED to the parameter range ----
    std::printf ("\n[4] CC scales 7-bit to the parameter range\n");
    {
        juce::MidiBuffer m;
        addCc (m, 74, 64);   // CC74 = filter1_cutoff; range 0..127 -> 64
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "filter1_cutoff"));
        std::printf ("     CC(74,64) -> filter1_cutoff = %d (expect 64)\n", v);
        check (v == 64, "CC74 -> filter1_cutoff = 64 (scaled 1:1 for 0..127 range)");
    }
    {
        juce::MidiBuffer m;
        addCc (m, 7, 100);   // CC7 = part_volume; range 0..127 -> 100
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "part_volume"));
        std::printf ("     CC(7,100) -> part_volume = %d (expect 100)\n", v);
        check (v == 100, "CC7 -> part_volume = 100");
    }

    // ---- CC stride: CC81 -> env2_attack (param 25, instance 1, addr 32) ----
    std::printf ("\n[5] CC decodes stride instances (CC81 -> env2_attack)\n");
    {
        juce::MidiBuffer m;
        addCc (m, 81, 60);   // env_attack base CC=73; 73+8=81 -> instance 1 (env2)
        processMidi (proc, m);
        proc.syncAllParamsToEngine();
        const int b = patchByte (proc, 32);
        std::printf ("     CC(81,60) -> engine patchBytes[32] (env2_attack) = %d (expect 60)\n", b);
        check (b == 60, "CC81 -> env2_attack byte 32 = 60 (stride decode)");
    }

    // ---- Signed CC scaling: CC14 -> osc1_range (-24..24) ----
    std::printf ("\n[6] CC scales to a signed range (osc1_range -24..24)\n");
    {
        juce::MidiBuffer m;
        addCc (m, 14, 0);   // range 49, scaled ((49*0)>>8) + (-24) = -24
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "osc1_range"));
        std::printf ("     CC(14,0) -> osc1_range = %d (expect -24)\n", v);
        check (v == -24, "CC14 v=0 -> osc1_range = -24 (signed scale min)");
    }
    {
        juce::MidiBuffer m;
        addCc (m, 14, 127);  // ((49*254)>>8) + (-24) = 48 - 24 = 24
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "osc1_range"));
        std::printf ("     CC(14,127) -> osc1_range = %d (expect 24)\n", v);
        check (v == 24, "CC14 v=127 -> osc1_range = 24 (signed scale max)");
    }

    // ---- Unmapped CC is ignored ----
    std::printf ("\n[7] Unmapped CC is ignored (midi_cc_map[0] = 0xff)\n");
    {
        const int before = static_cast<int> (rawVal (proc, "osc1_shape"));
        juce::MidiBuffer m;
        addCc (m, 0, 5);   // CC0 is unmapped
        processMidi (proc, m);
        const int after = static_cast<int> (rawVal (proc, "osc1_shape"));
        std::printf ("     osc1_shape before CC0 = %d, after = %d (expect unchanged)\n", before, after);
        check (after == before, "CC0 (unmapped) leaves osc1_shape unchanged");
    }

    // ---- Unmapped NRPN address is ignored ----
    std::printf ("\n[8] Unmapped NRPN address is ignored (addr 127 = polyphony, not exposed)\n");
    {
        const int before = static_cast<int> (rawVal (proc, "filter1_cutoff"));
        juce::MidiBuffer m;
        addNrpn (m, 127, 9);   // addr 127 = polyphony_mode; no Parvati descriptor
        processMidi (proc, m);
        const int after = static_cast<int> (rawVal (proc, "filter1_cutoff"));
        std::printf ("     filter1_cutoff before = %d, after NRPN(127) = %d (expect unchanged)\n", before, after);
        check (after == before, "NRPN addr 127 (polyphony, unexposed) is ignored");
    }

    // ---- NRPN signed (INT8) parameters: a two's-complement negative byte ----
    // Firmware parameter.Clamp (parameter.cc UNIT_INT8) reads the data byte as
    // int8_t, so byte 0xC0 == -64. Pre-fix, applyValue clamped the raw 192
    // against the -64..64 APVTS range and saturated it to +64 — every negative
    // detune/mod amount was unreachable over NRPN.
    std::printf ("\n[NRPN signed] INT8 params honour two's complement\n");
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::controllerEvent (1, 99, 0), 0);   // NRPN MSB = 0 (patch space)
        m.addEvent (juce::MidiMessage::controllerEvent (1, 98, 3), 1);   // NRPN LSB = addr 3 (osc1_detune, -64..64)
        m.addEvent (juce::MidiMessage::controllerEvent (1, 6, 1),  2);   // data-entry MSB = 1 -> flag 128
        m.addEvent (juce::MidiMessage::controllerEvent (1, 38, 64), 3);  // data-entry LSB = 64 -> byte 192 (0xC0)
        processMidi (proc, m);
        const int v = static_cast<int> (rawVal (proc, "osc1_detune"));
        std::printf ("     NRPN(3, 0xC0) -> osc1_detune = %d (expect -64)\n", v);
        check (v == -64, "NRPN negative INT8 byte -> -64 (not saturated to +64)");

        proc.syncAllParamsToEngine();
        const int b = patchByte (proc, 3);
        std::printf ("     engine patchBytes[3] = %d (0x%02X; expect 192 = 0xC0)\n", b, b);
        check (b == 192, "negative detune reaches the engine byte as 0xC0");

        // Positive control: the same LSB WITHOUT the MSB flag stays +64.
        juce::MidiBuffer m2;
        m2.addEvent (juce::MidiMessage::controllerEvent (1, 99, 0), 0);
        m2.addEvent (juce::MidiMessage::controllerEvent (1, 98, 3), 1);
        m2.addEvent (juce::MidiMessage::controllerEvent (1, 38, 64), 2);   // no CC6 -> byte 64
        processMidi (proc, m2);
        const int v2 = static_cast<int> (rawVal (proc, "osc1_detune"));
        std::printf ("     NRPN(3, 0x40) -> osc1_detune = %d (expect +64)\n", v2);
        check (v2 == 64, "positive control: plain byte 64 -> +64");
    }

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
