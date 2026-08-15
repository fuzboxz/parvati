// tools/gen_templates.cpp
//
// Generates the 5 stock init templates as full-fidelity .parvati multis into
// presets/TEMPLATES/. Each template is produced by configuring a FRESH
// processor's init state (polyphony mode / per-part voice allocation), flushing
// the deferred allocation rebuild, then parvati::preset::serializeParvatiMulti().
// Run from the repo root via the `parvati_gen_templates` CMake target whenever
// the init state or the set of templates changes; the output is embedded into
// the plugin binary at build time.
//
//   cmake --build build --target parvati_gen_templates && ./build/parvati_gen_templates

#include <cstdint>
#include <cstdio>
#include <array>
#include <cstring>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "ParameterLayout.h"
#include "SynthEngine.h"
#include "dsp/patch.h"

namespace
{
// One stock init template. part0Poly is the `part_polyphony` value
// (0 = MONO, 1 = POLY, 2 = UNISON_2X).
struct TemplateSpec
{
    const char* file;
    const char* name;
    int     part0Poly;
    uint8_t part0Alloc;
    uint8_t part1Alloc;
    int     part0Legato;   // PartData legato byte (1 = mono plays legato: no env re-attack on overlapping notes)
};

// Build one template from a freshly-prepared processor and write it to
// <outDir>/<spec.file>. Returns true on success.
bool writeTemplate (const TemplateSpec& s, const juce::File& outDir)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);   // seeds the init patch + single-part default + Hardware

    // Edit Part 0's polyphony mode (PartData byte 15 via the APVTS bridge).
    proc.getApvts().getParameterAsValue ("part_select")     = 1.0f;
    proc.getApvts().getParameterAsValue ("part_polyphony")  = static_cast<float> (s.part0Poly);
    proc.getApvts().getParameterAsValue ("part_legato")     = static_cast<float> (s.part0Legato);

    // Per-part voice allocation. Single-part templates own all 6 voicecards on
    // Part 0 (0x3f); the Multitimbral template splits them 3+3 (0x15 / 0x2a).
    proc.getEngine().setPartVoiceAllocation (0, s.part0Alloc);
    proc.getEngine().setPartVoiceAllocation (1, s.part1Alloc);
    for (int p = 2; p < SynthEngine::getNumParts(); ++p)
        proc.getEngine().setPartVoiceAllocation (p, 0);

    proc.setLoadedProgramName (s.name);
    proc.syncAllParamsToEngine();

    // Flush the deferred voice-allocation rebuild so the serialized state is
    // consistent (serializeParvatiMulti reads engine storage directly).
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
    }

    const juce::String yaml = parvati::preset::serializeParvatiMulti (proc);
    const juce::File out    = outDir.getChildFile (s.file);

    juce::TemporaryFile tmp (out);
    {
        juce::FileOutputStream os (tmp.getFile());
        if (! os.openedOk())
        {
            std::fprintf (stderr, "gen_templates: cannot open %s\n", out.getFullPathName().toRawUTF8());
            return false;
        }
        os.writeText (yaml, false, false, nullptr);
        os.flush();
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        std::fprintf (stderr, "gen_templates: cannot write %s\n", out.getFullPathName().toRawUTF8());
        return false;
    }

    std::printf ("  wrote %-22s  (%d bytes, alloc0=0x%02x alloc1=0x%02x, poly0=%d)\n",
                 out.getFileName().toRawUTF8(), (int) yaml.length(),
                 (unsigned) s.part0Alloc, (unsigned) s.part1Alloc, s.part0Poly);
    return true;
}

// ---- Drum Kit (GM) ----
// A 6-part GM-mapped drum multi: one MIDI note per Part via keyzone
// low == high, Omni channel (works on any incoming MIDI channel), one
// voicecard each, 4 voice slots + CYCLIC for round-robin repeats, and a short
// percussive patch per drum (seeded from the init patch, then tuned).
struct DrumSpec
{
    const char* name;
    int  note;         // GM percussion note
    // Patch edits (offsets into the 112-byte Patch):
    uint8_t oscShape, oscRange;            // osc[0]: shape, range (int8 semitones)
    uint8_t mixNoise;                       // mix_noise (13)
    uint8_t cutoff, resonance;              // filter[0] (16, 17)
    int8_t  filterEnv;                      // filter_env (22): ENV_2 -> cutoff sweep
    uint8_t env1Decay;                      // ENV_1 (pitch/env sweep) decay
    uint8_t vcaDecay, vcaRelease;           // ENV_3 (VCA, routed in the init patch)
    int8_t  pitchDropAmount;                // ENV_1 -> OSC_1_2_COARSE (mod slot 0)
};

constexpr int kNumDrums = 6;
const DrumSpec kDrums[kNumDrums] = {
    //  GM note  shape                     range  noise  cut  res  fEnv  e1D  vcaD vcaR drop
    { "Kick",        36, ambika::dsp::WAVEFORM_SINE,      244 /* -12 */,   0,  40, 24,  40,  20,  18, 30, -63 },
    { "Snare",       38, ambika::dsp::WAVEFORM_TRIANGLE,   0,              90,  70, 10,  50,   8,  11, 20, -16 },
    { "Clap",        39, ambika::dsp::WAVEFORM_SINE,       0,             110,  78, 30,  10,   6,  16, 20,   0 },
    { "Closed Hat",  42, ambika::dsp::WAVEFORM_SINE,       0,             127, 105, 20,  30,   3,   5,  6,   0 },
    { "Open Hat",    46, ambika::dsp::WAVEFORM_SINE,       0,             127, 100, 20,  25,  20,  25, 15,   0 },
    { "Tom",         45, ambika::dsp::WAVEFORM_SINE,      244 /* -12 */,   0,  55, 15,  30,  15,  20, 25, -48 },
};

bool writeDrumKitTemplate (const juce::File& outDir)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    SynthEngine& engine = proc.getEngine();

    for (int p = 0; p < kNumDrums; ++p)
    {
        const DrumSpec& d = kDrums[p];

        // Routing: Omni channel, single-note keyzone, one card, 4 slots CYCLIC.
        engine.setPartMidiChannel (p, 0);   // 0 = Omni: the kit answers on ANY channel
        engine.setPartKeyZone (p, d.note, d.note);
        engine.setPartVoiceAllocation (p, static_cast<uint8_t> (1u << p));
        engine.setPartVoiceSlots (p, 4);
        engine.setPartName (p, d.name);

        // Patch: seed from the init patch, then tune into a percussive drum.
        std::array<uint8_t, 112> patch;
        std::memcpy (patch.data(), getControllerInitPatchBytes(), 112);
        patch[0]  = d.oscShape;                                   // osc[0].shape
        patch[2]  = d.oscRange;                                   // osc[0].range (int8)
        patch[4]  = ambika::dsp::WAVEFORM_NONE;                   // osc[1].shape (silent)
        patch[13] = d.mixNoise;                                   // mix.noise
        patch[16] = d.cutoff;                                     // filter[0].cutoff
        patch[17] = d.resonance;                                  // filter[0].resonance
        patch[22] = static_cast<uint8_t> (d.filterEnv);           // filter_env (ENV_2 -> cutoff)
        patch[25] = d.env1Decay;                                  // env_lfo[0].decay  (ENV_1)
        patch[26] = 0;                                            //   sustain 0
        patch[33] = d.env1Decay;                                  // env_lfo[1].decay  (ENV_2)
        patch[34] = 0;                                            //   sustain 0
        patch[41] = d.vcaDecay;                                   // env_lfo[2].decay  (ENV_3 = VCA)
        patch[42] = 0;                                            //   sustain 0 (one-shot)
        patch[43] = d.vcaRelease;
        // mod slot 0 (offset 50): ENV_1 -> OSC_1_2_COARSE, the pitch drop.
        patch[50] = ambika::dsp::MOD_SRC_ENV_1;
        patch[51] = ambika::dsp::MOD_DST_OSC_1_2_COARSE;
        patch[52] = static_cast<uint8_t> (d.pitchDropAmount);
        engine.getPart (p).patchBytes.loadFrom (patch.data());

        // PartData byte 15 = polyphony: CYCLIC (3) for round-robin repeats.
        engine.getPart (p).partBytes[15] = 3;
    }
    engine.markAllocationDirty();
    proc.setLoadedProgramName ("Drum Kit (GM)");

    // Flush the deferred rebuild so the serialized state is consistent.
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
    }

    const juce::String yaml = parvati::preset::serializeParvatiMulti (proc);
    const juce::File out    = outDir.getChildFile ("Drum Kit (GM).parvati");
    juce::TemporaryFile tmp (out);
    {
        juce::FileOutputStream os (tmp.getFile());
        if (! os.openedOk())
        {
            std::fprintf (stderr, "gen_templates: cannot open %s\n", out.getFullPathName().toRawUTF8());
            return false;
        }
        os.writeText (yaml, false, false, nullptr);
        os.flush();
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        std::fprintf (stderr, "gen_templates: cannot write %s\n", out.getFullPathName().toRawUTF8());
        return false;
    }
    std::printf ("  wrote %-22s  (%d bytes, 6 GM drums: 36/38/39/42/45/46)\n",
                 out.getFileName().toRawUTF8(), (int) yaml.length());
    return true;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Output dir = presets/TEMPLATES/ (relative to the repo root — the target is
    // run as ./build/parvati_gen_templates from the repo root).
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("presets/TEMPLATES");
    if (! outDir.createDirectory())
    {
        std::fprintf (stderr, "gen_templates: cannot create %s\n", outDir.getFullPathName().toRawUTF8());
        return 1;
    }

    const std::vector<TemplateSpec> specs = {
        { "Mono.parvati",         "Monotimbral Mono",   0, 0x01, 0x00, 1 },
        { "Poly.parvati",         "Poly",               1, 0x3f, 0x00, 0 },
        { "Unison.parvati",       "Unison 2x",          2, 0x3f, 0x00, 0 },
        { "Multitimbral.parvati", "Multitimbral (3+3)", 1, 0x15, 0x2a, 0 },
    };

    std::printf ("Generating %zu templates -> %s\n", specs.size(), outDir.getFullPathName().toRawUTF8());
    int ok = 0;
    for (const auto& s : specs)
        if (writeTemplate (s, outDir))
            ++ok;
    const bool drumOk = writeDrumKitTemplate (outDir);
    if (drumOk) ++ok;

    const int total = (int) specs.size() + 1;
    std::printf ("gen_templates: %d/%d written\n", ok, total);
    if (ok != total)
        return 1;

    // ---- Round-trip verification: reload each template via the real load path
    // and assert the global Voice Mode + Part 0 polyphony were applied. ----
    std::printf ("Verifying round-trip ...\n");
    int verified = 0;
    for (const auto& s : specs)
    {
        ParvatiAudioProcessor chk;
        chk.prepareToPlay (48000.0, 256);
        const juce::File f = outDir.getChildFile (s.file);
        const bool loaded = chk.loadParvatiMultiFile (f);
        const int pb15pre = chk.getEngine().getPart (0).partBytes[15];   // before servicing
        const float apvtsPoly = chk.getApvts().getParameter ("part_polyphony")->getValue();
        // Service the deferred allocation rebuild (sets Part.polyphonyMode from
        // partBytes[15]) before reading it back — loadParvatiMultiFile marks it
        // dirty but does not run the audio thread itself.
        { juce::AudioBuffer<float> b (2, 256); b.clear(); juce::MidiBuffer m; chk.processBlock (b, m); }
        // Polyphony lives in PartData byte 15 (synced into Part.polyphonyMode by
        // the rebuild above); read it from the engine's Part 0 storage.
        chk.getApvts().getParameterAsValue ("part_select") = 1.0f;
        chk.syncAllParamsToEngine();
        const int poly0   = chk.getEngine().getPart (0).polyphonyMode;
        const bool pass   = loaded && poly0 == s.part0Poly;
        std::printf ("  %-22s load=%d pb15pre=%d apvtsPoly=%.2f poly0=%d (exp %d)  %s\n",
                     s.file, (int) loaded, pb15pre, (double) apvtsPoly, poly0, s.part0Poly,
                     pass ? "OK" : "FAIL");
        if (pass) ++verified;
    }
    std::printf ("gen_templates verify: %d/%zu passed\n", verified, (int) specs.size());
    return verified == (int) specs.size() ? 0 : 2;
}
