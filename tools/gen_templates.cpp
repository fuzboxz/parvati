// tools/gen_templates.cpp
//
// Generates the 5 stock init templates as full-fidelity .parvati multis into
// presets/TEMPLATES/. Each template is produced by configuring a FRESH
// processor's init state (polyphony mode / per-part voice allocation / global
// Voice Mode), flushing the deferred allocation rebuild, then
// parvati::preset::serializeParvatiMulti(). Run from the repo root via the
// `parvati_gen_templates` CMake target whenever the init state or the set of
// templates changes; the output is embedded into the plugin binary at build time.
//
//   cmake --build build --target parvati_gen_templates && ./build/parvati_gen_templates

#include <cstdint>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
// One stock init template. part0Poly is the `part_polyphony` value
// (0 = MONO, 1 = POLY, 2 = UNISON_2X). voiceMode is 0 = Hardware (6 voices),
// 1 = Extended (16 voices).
struct TemplateSpec
{
    const char* file;
    const char* name;
    int     part0Poly;
    uint8_t part0Alloc;
    uint8_t part1Alloc;
    int     voiceMode;
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

    // Per-part voice allocation. Single-part templates own all 6 voicecards on
    // Part 0 (0x3f); the Multitimbral template splits them 3+3 (0x15 / 0x2a).
    proc.getEngine().setPartVoiceAllocation (0, s.part0Alloc);
    proc.getEngine().setPartVoiceAllocation (1, s.part1Alloc);
    for (int p = 2; p < SynthEngine::getNumParts(); ++p)
        proc.getEngine().setPartVoiceAllocation (p, 0);

    // Global Voice Mode (Hardware 6 / Extended 16) — carried under .parvati options:.
    proc.setUiVoiceMode (s.voiceMode);
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

    std::printf ("  wrote %-22s  (%d bytes, voice_mode=%d, alloc0=0x%02x alloc1=0x%02x, poly0=%d)\n",
                 out.getFileName().toRawUTF8(), (int) yaml.length(),
                 s.voiceMode, (unsigned) s.part0Alloc, (unsigned) s.part1Alloc, s.part0Poly);
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
        { "Mono.parvati",         "Monotimbral Mono",   0, 0x3f, 0x00, 0 },
        { "Poly 6.parvati",       "Poly 6 (Hardware)",  1, 0x3f, 0x00, 0 },
        { "Poly 16.parvati",      "Poly 16 (Extended)", 1, 0x3f, 0x00, 1 },
        { "Unison.parvati",       "Unison 2x",          2, 0x3f, 0x00, 1 },
        { "Multitimbral.parvati", "Multitimbral (3+3)", 1, 0x15, 0x2a, 0 },
    };

    std::printf ("Generating %zu templates -> %s\n", specs.size(), outDir.getFullPathName().toRawUTF8());
    int ok = 0;
    for (const auto& s : specs)
        if (writeTemplate (s, outDir))
            ++ok;

    std::printf ("gen_templates: %d/%zu written\n", ok, (int) specs.size());
    if (ok != (int) specs.size())
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
        const int vm      = chk.getUiVoiceMode();
        // Polyphony lives in PartData byte 15 (synced into Part.polyphonyMode by
        // the rebuild above); read it from the engine's Part 0 storage.
        chk.getApvts().getParameterAsValue ("part_select") = 1.0f;
        chk.syncAllParamsToEngine();
        const int poly0   = chk.getEngine().getPart (0).polyphonyMode;
        const bool pass   = loaded && vm == s.voiceMode && poly0 == s.part0Poly;
        std::printf ("  %-22s load=%d pb15pre=%d apvtsPoly=%.2f voice_mode=%d (exp %d) poly0=%d (exp %d)  %s\n",
                     s.file, (int) loaded, pb15pre, (double) apvtsPoly, vm, s.voiceMode, poly0, s.part0Poly,
                     pass ? "OK" : "FAIL");
        if (pass) ++verified;
    }
    std::printf ("gen_templates verify: %d/%zu passed\n", verified, (int) specs.size());
    return verified == (int) specs.size() ? 0 : 2;
}
