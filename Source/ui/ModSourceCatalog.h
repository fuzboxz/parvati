// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ModSourceCatalog — the single source of truth for the 31 MOD_SRC_* modulation
// sources (ambika::dsp::ModulationSource, see dsp/patch.h). The CentralModBar
// builds itself from this catalogue so every mod source appears in exactly one
// cluster, with a stable short label / full name / accent colour, and the same
// drag payload ("parvatiModSrc:<enum>") the rest of the editor already emits.
//
// The catalogue is AUTHORITATIVE for the bar. The legacy name/colour helpers in
// ModMatrixView / PluginEditor (sourceNameForSlot, categoryColourForSourceName)
// are intentionally left untouched; a later phase may refactor them to read here.
//
// This file is header-only data + thin accessors: no JUCE app state, no
// allocations after the first call. clusterAccent() takes a ParvatiTheme so it
// re-resolves on a theme switch.

#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>   // juce::Colour

#include "ParvatiTheme.h"
#include "dsp/patch.h"                          // ambika::dsp::MOD_SRC_* / MOD_SRC_LAST

namespace parvati
{

//==============================================================================
// The seven functional clusters a modulation source belongs to. Display order
// (left -> right in the bar) == declaration order.
enum class Cluster
{
    // --- Generators (editable; LEFT half of the bar — click opens the editor) ---
    Env,    // MOD_SRC_ENV_1..3
    Lfo,    // MOD_SRC_LFO_1..4 (LFO 4 == per-voice LFO)
    SeqArp, // MOD_SRC_SEQ_1..2 + MOD_SRC_ARP_STEP (+ bar-only Note Sequencer)
    Mod,    // MOD_SRC_OP_1..4 modifier outputs
    // --- Drag-only sources (RIGHT half of the bar — click highlights routings) ---
    Perf,   // Velocity/Aftertouch/Bend/Wheels/Expression/Note
    Util,   // Gate/Noise/Random
    Const   // MOD_SRC_CONSTANT_256..4
};

// A cluster whose pills are EDITABLE generators (open an editor on click) vs
// drag-only inputs. Drives the bar's left/right split (generators on the left,
// drag-only on the right) and the larger separator gap between the two halves.
inline constexpr bool isGeneratorCluster (Cluster c)
{
    return c == Cluster::Env || c == Cluster::Lfo
        || c == Cluster::SeqArp || c == Cluster::Mod;
}

// One catalogue entry. enumValue is the ambika::dsp::ModulationSource value;
// shortLabel is the compact pill caption; fullName is the tooltip / drag label;
// cluster drives the accent colour; isGenerator flags whether the source is a
// live generator (Env/LFO/Seq/Arp/Op) or a drag-only input (Perf/Util/Const).
struct SourceEntry
{
    int         enumValue;
    const char* shortLabel;
    const char* fullName;
    Cluster     cluster;
    bool        isGenerator;
};

// Bar-only sentinel for the Note Sequencer (seq3: params seqnote_step* /
// seqnote_vel*, groups "Note Pitch" + "Note Velocity"). Unlike every other
// catalogue entry this is NOT a real ambika::dsp::ModulationSource value — the
// Note Sequencer is a melodic note-on/off sequencer that fires note events and
// is NEVER written to modulation_sources_[] (only SEQ_1/SEQ_2/ARP_STEP are
// injected, see SynthEngine.cpp). The negative enumValue therefore marks a
// BAR-ONLY entry: it is rendered as a SeqArp generator pill (solid border +
// active underline) and is CLICK-ONLY (opens its editor), NEVER draggable (a
// drag would route a non-existent source). Because it has no MOD_SRC_* enum it
// is deliberately absent from ParameterLayout's mod-source choice array, so it
// cannot leak into the mod-matrix source dropdown.
inline constexpr int kNoteSeqSentinel = -1;

//==============================================================================
// All catalogue entries in CLUSTER display order (generators first: Env, Lfo,
// SeqArp, Mod — then drag-only: Perf, Util, Const). The array holds the 31 real
// bar-only sentinel (kNoteSeqSentinel == -1, the Note Sequencer) appended to
// the SeqArp cluster. The array is NOT ordered by enum value (Modifiers live
// between LFO and SEQ in the enum), so look up by enum via entryFor().
inline constexpr std::array<SourceEntry, 32> kAllSources =
{{
    // ===== Generators (LEFT half of the bar — click opens the editor) =====
    // --- Env (generators) ---
    // Short labels name the envelope's DESTINATION (Mod / Filter / Amp — the
    // semantic mapping checked against the generator-page registration and
    // the Env group headers); the fullName tooltips keep the "Env n" identity
    // for the status strip / hover / tests.
    { ambika::dsp::MOD_SRC_ENV_1, "MOD",  "Env 1",   Cluster::Env,    true },
    { ambika::dsp::MOD_SRC_ENV_2, "FLTR", "Env 2",   Cluster::Env,    true },
    { ambika::dsp::MOD_SRC_ENV_3, "AMP",  "Env 3",   Cluster::Env,    true },
    // --- Lfo (generators) ---
    { ambika::dsp::MOD_SRC_LFO_1, "L1",  "LFO 1",   Cluster::Lfo,    true },
    { ambika::dsp::MOD_SRC_LFO_2, "L2",  "LFO 2",   Cluster::Lfo,    true },
    { ambika::dsp::MOD_SRC_LFO_3, "L3",  "LFO 3",   Cluster::Lfo,    true },
    { ambika::dsp::MOD_SRC_LFO_4, "vL4", "Voice LFO", Cluster::Lfo,  true },
    // --- SeqArp (generators) ---
    { ambika::dsp::MOD_SRC_SEQ_1,   "S1",  "Seq 1",     Cluster::SeqArp, true },
    { ambika::dsp::MOD_SRC_SEQ_2,   "S2",  "Seq 2",     Cluster::SeqArp, true },
    { ambika::dsp::MOD_SRC_ARP_STEP, "ARP", "Arp Step",  Cluster::SeqArp, true },
    // Bar-only Note Sequencer (see kNoteSeqSentinel above): generator-style
    // pill in the SEQ cluster, but click-only (NOT draggable). enumValue < 0.
    { kNoteSeqSentinel,              "NOTE", "Note Sequencer", Cluster::SeqArp, true },
    // --- Mod / modifier outputs (generators) ---
    { ambika::dsp::MOD_SRC_OP_1, "M1", "Modifier 1 Output", Cluster::Mod, true },
    { ambika::dsp::MOD_SRC_OP_2, "M2", "Modifier 2 Output", Cluster::Mod, true },
    { ambika::dsp::MOD_SRC_OP_3, "M3", "Modifier 3 Output", Cluster::Mod, true },
    { ambika::dsp::MOD_SRC_OP_4, "M4", "Modifier 4 Output", Cluster::Mod, true },
    // ===== Drag-only sources (RIGHT half of the bar — click highlights routings) =====
    // --- Perf (drag-only) ---
    { ambika::dsp::MOD_SRC_VELOCITY,   "VEL", "Velocity",   Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_AFTERTOUCH, "AT",  "Aftertouch", Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_PITCH_BEND, "PB",  "Pitch Bend", Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_WHEEL,      "MW",  "Wheel",      Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_WHEEL_2,    "W2",  "Wheel 2",    Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_EXPRESSION, "EXP", "Expression", Cluster::Perf, false },
    { ambika::dsp::MOD_SRC_NOTE,       "KEY", "Note",       Cluster::Perf, false },
    // --- Util (drag-only) ---
    { ambika::dsp::MOD_SRC_GATE,   "GATE", "Gate",   Cluster::Util, false },
    { ambika::dsp::MOD_SRC_NOISE,  "NOIZ", "Noise",  Cluster::Util, false },
    { ambika::dsp::MOD_SRC_RANDOM, "RND",  "Random", Cluster::Util, false },
    // --- Const (drag-only). Labels show the ACTUAL value each constant writes
    // (CONSTANT_256 writes 255, etc. — see dsp/voice.cpp) so a pill reads as a
    // modulation amount, not an arbitrary index. Ascending by value. ---
    { ambika::dsp::MOD_SRC_CONSTANT_4,   "4",   "Constant 4",   Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_8,   "8",   "Constant 8",   Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_16,  "16",  "Constant 16",  Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_32,  "32",  "Constant 32",  Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_64,  "64",  "Constant 64",  Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_128, "128", "Constant 128", Cluster::Const, false },
    { ambika::dsp::MOD_SRC_CONSTANT_256, "255", "Constant 255", Cluster::Const, false },
}};

// Look up the catalogue entry for a MOD_SRC_* enum value (0..MOD_SRC_LAST-1)
// OR the bar-only kNoteSeqSentinel sentinel (-1). A linear scan of kAllSources
// (which now includes the -1 sentinel) finds both; on an unknown value it
// asserts. Returns a stable reference into kAllSources.
const SourceEntry& entryFor (int modSrcEnum);

// The accent colour for a cluster, resolved from the active theme.
juce::Colour clusterAccent (Cluster c, const ParvatiTheme& theme);

// The 7 clusters in display order (a stable reference into a function-local
// static, safe to hold for the call).
const std::array<Cluster, 7>& clustersInOrder();

}  // namespace parvati
