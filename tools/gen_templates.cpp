// tools/gen_templates.cpp
//
// Generates the 5 stock init templates as full-fidelity .parvati multis into
// presets/TEMPLATES/. Each template — including Drum Kit (GM) — is produced by
// applying the corresponding ARRANGEMENT preset (Source/ui/PatchArrangement.cpp
// — the same table the Patch page's arrangement selector drives) to a FRESH
// processor, so loading one shows its arrangement name in the Patch page
// instead of "Custom". The Drum Kit (GM) file adds the bespoke drum CONTENT
// (part names + tuned percussive patches) on top of the arrangement's routing
// (6 parts x 1 mono voice, Omni, single GM note zones).
// Run from the repo root whenever the arrangement table or the drum kit
// changes; the output is embedded into the plugin binary at build time.
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
#include "SynthEngine.h"
#include "dsp/patch.h"
#include "ui/PatchArrangement.h"

namespace
{
// One processBlock over silence: services the deferred voice-allocation
// rebuild (applyArrangement marks it dirty) so the serialized state is
// consistent — serializeParvatiMulti reads engine storage directly.
void flushDeferredRebuild (ParvatiAudioProcessor& proc)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    proc.processBlock (buf, midi);
}

// Serialize the processor as a multi and write it to @p out ATOMICALLY
// (TemporaryFile): either the full new file or none of it.
bool writeMultiFile (ParvatiAudioProcessor& proc, const juce::File& out)
{
    const juce::String yaml = parvati::preset::serializeParvatiMulti (proc);
    juce::TemporaryFile tmp (out);
    {
        juce::FileOutputStream os (tmp.getFile());
        if (! os.openedOk())
        {
            std::fprintf (stderr, "gen_templates: cannot open %s\n", out.getFullPathName().toRawUTF8());
            return false;
        }
        if (! os.writeText (yaml, false, false, nullptr))
        {
            std::fprintf (stderr, "gen_templates: cannot write %s\n", out.getFullPathName().toRawUTF8());
            return false;
        }
        os.flush();
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        std::fprintf (stderr, "gen_templates: cannot write %s\n", out.getFullPathName().toRawUTF8());
        return false;
    }
    return true;
}

// ---- Named templates: applyArrangement is the SINGLE SOURCE OF TRUTH ----
//
// No APVTS edits and NO syncAllParamsToEngine() here: applyArrangement writes
// polyphony/spread engine-direct (applyPartByte), and a bulk APVTS->engine
// sync would clobber those with the fresh processor's init parameter values.
// Instead the APVTS is refreshed FROM the engine (loadPartIntoApvts) — the
// same one-way re-sync PatchPage::onArrangementChanged performs after a UI
// apply. Serialization reads engine storage, so the bytes that round-trip are
// exactly the arrangement's.
struct ArrangementSpec
{
    const char*  file;
    Arrangement  id;
};

bool writeArrangementTemplate (const ArrangementSpec& s, const juce::File& outDir)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);   // seeds the init patch + single-part default + Hardware

    SynthEngine& engine = proc.getEngine();
    applyArrangement (engine, s.id);

    // The multi's `name:` — the arrangement's own label ("Mono" / "Poly" /
    // "Unison" / "Multitimbral"), which also matches the Patch page combo.
    proc.setLoadedProgramName (arrangementLabel (s.id));
    proc.loadPartIntoApvts (engine.getCurrentPart());   // engine -> APVTS (one-way)
    flushDeferredRebuild (proc);

    const juce::File out = outDir.getChildFile (s.file);
    if (! writeMultiFile (proc, out))
        return false;

    std::printf ("  wrote %-22s  (arrangement: %s)\n",
                 out.getFileName().toRawUTF8(), arrangementLabel (s.id));
    return true;
}

// ---- Drum Kit (GM) ----
// A 6-part GM-mapped drum multi on top of the built-in Drum Kit ARRANGEMENT
// preset (single source of truth): 6 parts x 1 mono voice (a repeat retriggers
// the single voice — the latest hit always sounds), Omni channel (works on any
// incoming MIDI channel), each Part mapped to one GM percussion note via
// keyzone low == high, and a short percussive patch per drum (seeded from the
// init patch, then tuned). Concurrency comes from the 6 independent parts
// (a beat = one voice in each struck drum's part).
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

    // Routing + polyphony FIRST: the built-in Drum Kit ARRANGEMENT preset is
    // the single source of truth (6 parts x 1 voice, MONO, Omni, single GM
    // note zones {36,38,39,42,46,45} in part order) — the same table the
    // Patch page's selector drives, so the loaded template re-infers as
    // "Drum Kit" (not "Custom"). Exactly like the four named templates.
    applyArrangement (engine, Arrangement::DrumKit);

    // Bespoke drum CONTENT on top: part names + tuned percussive patches.
    for (int p = 0; p < kNumDrums; ++p)
    {
        const DrumSpec& d = kDrums[p];

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
    }
    proc.setLoadedProgramName ("Drum Kit (GM)");
    flushDeferredRebuild (proc);

    const juce::File out = outDir.getChildFile ("Drum Kit (GM).parvati");
    if (! writeMultiFile (proc, out))
        return false;
    std::printf ("  wrote %-22s  (arrangement: Drum Kit; 6 GM drums: 36/38/39/42/45/46)\n",
                 out.getFileName().toRawUTF8());
    return true;
}

// ---- Round-trip verification ----
// The contract the Patch page relies on: loading a template through the REAL
// load path must leave the engine in a state inferArrangement maps back to its
// arrangement (else the page would show "Custom"). The drum kit is verified
// the SAME way (it must re-infer as Drum Kit) plus its bespoke content
// (1 mono voice / GM zones / names per part).
int g_verifyFailures = 0;
void verify (bool ok, const juce::String& msg)
{
    std::printf ("  %-4s %s\n", ok ? "PASS" : "FAIL", msg.toRawUTF8());
    if (! ok) ++g_verifyFailures;
}

int verifyArrangementTemplate (const ArrangementSpec& s, const juce::File& outDir)
{
    ParvatiAudioProcessor chk;
    chk.prepareToPlay (48000.0, 256);
    const juce::File f = outDir.getChildFile (s.file);
    const bool loaded = chk.loadParvatiMultiFile (f);
    flushDeferredRebuild (chk);   // service the deferred allocation rebuild

    const Arrangement inferred = inferArrangement (chk.getEngine());
    const bool pass = loaded && inferred == s.id;
    verify (pass, juce::String (s.file) + ": loads + infers back to '"
                      + arrangementLabel (s.id) + "' (got '" + arrangementLabel (inferred) + "')");
    return pass ? 1 : 0;
}

int verifyDrumKitTemplate (const juce::File& outDir)
{
    ParvatiAudioProcessor chk;
    chk.prepareToPlay (48000.0, 256);
    const juce::File f = outDir.getChildFile ("Drum Kit (GM).parvati");
    const bool loaded = chk.loadParvatiMultiFile (f);
    flushDeferredRebuild (chk);
    SynthEngine& engine = chk.getEngine();
    verify (loaded, "Drum Kit (GM).parvati loads");

    // THE point of this template: it must re-infer as the built-in Drum Kit
    // arrangement (the Patch page shows "Drum Kit", not "Custom").
    const Arrangement inferred = inferArrangement (engine);
    const bool arrPass = loaded && inferred == Arrangement::DrumKit;
    verify (arrPass, "Drum Kit (GM).parvati: loads + infers back to 'Drum Kit' (got '"
                        + juce::String (arrangementLabel (inferred)) + "')");

    int okCount = arrPass ? 2 : (loaded ? 1 : 0);
    for (int p = 0; p < kNumDrums; ++p)
    {
        const DrumSpec& d = kDrums[p];
        const bool slots = engine.getPartVoiceSlots (p) == 1;
        const bool mono  = engine.getPart (p).partBytes[15] == 0;
        const bool zone  = engine.getPartKeyrangeLow (p) == d.note
                        && engine.getPartKeyrangeHigh (p) == d.note;
        const bool named = engine.getPartName (p) == d.name;
        const bool pass  = slots && mono && zone && named;
        verify (pass, juce::String ("drum part ") + juce::String (p) + " (" + d.name + "): 1slot="
                      + juce::String ((int) slots) + " MONO=" + juce::String ((int) mono)
                      + " zone lo=" + juce::String (engine.getPartKeyrangeLow (p))
                      + " hi=" + juce::String (engine.getPartKeyrangeHigh (p))
                      + " (want " + juce::String (d.note) + ") named=" + juce::String ((int) named));
        if (pass) ++okCount;
    }
    return okCount == 2 + kNumDrums ? 1 : 0;
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

    const std::vector<ArrangementSpec> specs = {
        { "Mono.parvati",         Arrangement::Mono         },
        { "Poly.parvati",         Arrangement::Poly         },
        { "Unison.parvati",       Arrangement::Unison       },
        { "Multitimbral.parvati", Arrangement::Multitimbral },
    };

    std::printf ("Generating %zu arrangement templates + Drum Kit (GM) -> %s\n",
                 specs.size(), outDir.getFullPathName().toRawUTF8());
    int ok = 0;
    for (const auto& s : specs)
        if (writeArrangementTemplate (s, outDir))
            ++ok;
    const bool drumOk = writeDrumKitTemplate (outDir);
    if (drumOk) ++ok;

    const int total = (int) specs.size() + 1;
    std::printf ("gen_templates: %d/%d written\n", ok, total);
    if (ok != total)
        return 1;

    // ---- Round-trip verification through the REAL load path. ----
    std::printf ("Verifying round-trip ...\n");
    int verified = 0;
    for (const auto& s : specs)
        verified += verifyArrangementTemplate (s, outDir);
    verified += verifyDrumKitTemplate (outDir);

    std::printf ("gen_templates verify: %d/%d passed\n", verified, total);
    if (g_verifyFailures > 0 || verified != total)
        return 2;
    return 0;
}
