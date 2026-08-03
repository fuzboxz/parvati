// Copyright (c) 2024 805LABS / Parvati.  See PluginProcessor.h.

#include <array>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParvatiPreset.h"
#include "PatchFile.h"
#include "ui/FactoryPresetInstaller.h"
#include "dsp/constants.h"   // ambika::dsp::kInternalSampleRate (resampler latency)

//==============================================================================
ParvatiAudioProcessor::ParvatiAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
        .withOutput ("Main", juce::AudioChannelSet::stereo(), true)
        .withOutput ("VC1", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC2", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC3", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC4", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC5", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC6", juce::AudioChannelSet::mono(), false)),
      apvts (*this, &undoManager_, "PARAMS", createParvatiParameterLayout())
{
    // Build the paramID -> descriptor-index lookup and register a listener for
    // every parameter so host / GUI / automation edits reach the engine.
    const auto& descs = getPatchParamDescriptors();
    paramIndex_.reserve (descs.size());
    for (size_t i = 0; i < descs.size(); ++i)
    {
        paramIndex_[descs[i].paramID] = static_cast<int> (i);
        apvts.addParameterListener (descs[i].paramID, this);
    }

    // Hardware-parity MIDI CC/NRPN -> parameter mapping (spec F.4). The setter
    // routes through setValueNotifyingHost so parameterChanged fires and the
    // byte reaches the current part's voices (same path as host/GUI edits).
    midiParamMap_.initialise (descs,
        [this] (const char* id, float denormalized) {
            // setValueNotifyingHost takes a NORMALIZED 0..1 value; convert the
            // denormalized APVTS value (choice index / int) to normalized first.
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (denormalized));
        },
        [this] (const char* id) -> float { if (auto v = apvts.getRawParameterValue (id)) return v->load(); return 0.0f; });

    // Extract the embedded GPL-3.0 factory presets into the user app-data dirs
    // on first run (process-once) so the Patch combo is populated out of the
    // box. Also ensures the USER save area exists. Non-fatal: a failure just
    // leaves the combo empty.
    parvati::ensureFactoryPresetsInstalled (getFactoryPatchDir(), getFactoryMultiDir(), getUserPatchDir());
}

//==============================================================================
void ParvatiAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate_ = sampleRate;   // cache for the audio-thread latency re-report
    engine_.prepare (sampleRate, samplesPerBlock);

    // The MidiMessageCollector (UI keyboard click-play injection) must know the
    // sample rate to timestamp queued messages for the audio block.
    midiCollector_.reset (sampleRate);

    // ---- Master DC blocker (main bus) ----
    // 15 Hz high-pass per channel: kills any sub-audio/DC leakage from the
    // DC-coupled filter+VCA path (a latent low-frequency rumble source) without
    // touching audible content. Prepared + reset each prepareToPlay so a
    // sample-rate / block-size change does not leave stale filter state.
    {
        const auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 15.0);
        const juce::dsp::ProcessSpec spec { sampleRate,
                                             (juce::uint32) juce::jmax (1, samplesPerBlock),
                                             1u };
        for (auto& f : dcBlocker_)
        {
            f.prepare (spec);
            f.coefficients = coeffs;
            f.reset();
        }
    }

    // ---- Plugin latency / PDC ----
    // Total latency = Lagrange resampler latency (2 INPUT/internal samples, see
    // the formula below) + active filter-oversampling latency (a few internal
    // samples when OS is on, else 0). Both are in INPUT samples and converted to
    // host samples via hostRate/internalRate. computePluginLatency() reads the
    // OS latency from osLatencyProbe_ (rebuilt in setOversamplingFactor). Only
    // notify the host when the value changes (e.g. sample-rate switch / OS toggle).
    const int latencySamples = computePluginLatency (sampleRate);
    if (latencySamples != lastReportedLatency_)
    {
        setLatencySamples (latencySamples);
        lastReportedLatency_ = latencySamples;
    }

    // After the voices are initialized (Init() has loaded the init patch),
    // push every APVTS parameter value into them so the synth starts from the
    // user's patch rather than the silent init patch. Idempotent and safe to
    // re-run on each prepareToPlay (e.g. sample-rate changes).
    syncAllParamsToEngine();
}

bool ParvatiAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Main bus: stereo or mono only.
    const auto& mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::stereo()
        && mainOut != juce::AudioChannelSet::mono())
        return false;

    // Up to 6 optional mono aux outputs (VC1..VC6), one per voicecard. Each aux,
    // if present/enabled, must be mono; disabled auxes are always fine.
    int auxEnabled = 0;
    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto& set = layouts.outputBuses.getReference (i);
        if (set.isDisabled())
            continue;
        if (set != juce::AudioChannelSet::mono())
            return false;
        ++auxEnabled;
    }
    return auxEnabled <= SynthEngine::getNumParts();
}

void ParvatiAudioProcessor::setNonRealtime (bool isNonRealtime) noexcept
{
    // Cache the host's offline-render state. The host wrapper calls this on the
    // message thread when entering/leaving a bounce (freeze/export). A future
    // "max quality" mode can read isNonRealtimeRender() to auto-engage
    // oversampling during offline render, where CPU is unconstrained. (Wiring
    // the quality mode itself is a later phase; this is detection only.)
    nonRealtime_.store (isNonRealtime, std::memory_order_relaxed);
    // Defer to the base so its own bookkeeping (isNonRealtime()) stays in sync.
    juce::AudioProcessor::setNonRealtime (isNonRealtime);
}

void ParvatiAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Keep the offline-render flag warm: some hosts change realtime state
    // without re-calling setNonRealtime(), so poll isNonRealtime() once per
    // block as a fallback. This is the read-point a future quality mode uses.
    nonRealtime_.store (isNonRealtime(), std::memory_order_relaxed);

    // Re-report the plugin latency after a live filter-oversampling change
    // (set on the message thread in setOversamplingFactor). The probe has
    // already been rebuilt there, so computePluginLatency() reflects the new
    // factor immediately; the host gets the updated PDC the same block.
    // Uses hostSampleRate_ (cached from prepareToPlay) since getSampleRate() can
    // read 0 in standalone / headless contexts.
    if (latencyDirty_.exchange (false, std::memory_order_acq_rel))
    {
        const int latencySamples = computePluginLatency (hostSampleRate_);
        if (latencySamples != lastReportedLatency_)
        {
            setLatencySamples (latencySamples);
            lastReportedLatency_ = latencySamples;
        }
    }

    // Map incoming MIDI CC/NRPN to parameters (hardware parity, spec F.4) before
    // rendering. Scans only — does not consume messages (notes still reach the
    // engine). Order vs processTransport is irrelevant for CC (it strips notes).
    midiParamMap_.handleBuffer (midiMessages);

    buffer.clear();

    // Read the host transport tempo + playing state from AudioPlayHead.
    double bpm = 120.0;
    bool isPlaying = true;  // default: run in standalone without transport
    if (auto* playHead = getPlayHead())
    {
        if (const auto pos = playHead->getPosition())
        {
            if (const auto b = pos->getBpm())
                bpm = *b;
            isPlaying = pos->getIsPlaying();
        }
    }

    // Advance the arpeggiator transport (routes MIDI when arp is on, generates
    // arp notes before the block renders).
    engine_.processTransport (midiMessages, buffer.getNumSamples(), bpm, isPlaying);

    // Merge UI-injected MIDI (keyboard click-play) into the buffer. Placed
    // AFTER processTransport so injected notes bypass the arpeggiator and
    // always reach the synth voices directly — matching the expectation that a
    // clicked key sounds immediately regardless of the current Part's arp mode.
    // Additive: only adds messages queued by addMidiEvent (empty when no UI).
    if (buffer.getNumSamples() > 0)
        midiCollector_.removeNextBlockOfMessages (midiMessages, buffer.getNumSamples());

    // The Synthesiser consumes the (possibly note-stripped) MIDI buffer and
    // renders the active voices additively. The engine's renderVoices override
    // routes each voice to its FIXED voicecard buffer (6 mono buffers) instead
    // of this master buffer, so renderNextBlock leaves `buffer` cleared here.
    // No post-processing stage: hardware Ambika has no master limiter — the
    // voicecard output feeds the analog VCA only, so the engine's per-voice
    // VCA is the final gain stage.
    engine_.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());

    // ---- Multi-output bus mixing (Ambika hardware: 6 individual voicecard
    // outputs + a global mix) ----
    // Main bus: sum ALL six voicecard buffers into L and R. This reproduces the
    // pre-multi-out single-buffer mix exactly (each voice's mono signal added
    // to both stereo channels), so the default main-stereo path is
    // audible-identical. When the main bus is mono, only L is written.
    // Aux buses (VC1..VC6): each ENABLED aux bus copies its voicecard output.
    const int numSamples = buffer.getNumSamples();
    const auto& vcBuffers = engine_.getVoiceCardBuffers();

    if (const int mainChans = getChannelCountOfBus (false, 0); mainChans > 0)
    {
        auto mainBus = getBusBuffer (buffer, false, 0);
        for (int vc = 0; vc < SynthEngine::getNumParts(); ++vc)
        {
            const float* src = vcBuffers[(size_t) vc].getReadPointer (0);
            mainBus.addFrom (0, 0, src, numSamples);                       // main L
            if (mainChans > 1)
                mainBus.addFrom (1, 0, src, numSamples);                   // main R
        }

        // Master DC blocker (main bus only): the engine's filter+VCA are
        // DC-coupled, so any sub-audio/DC offset would leak as a low-frequency
        // rumble. The 15 Hz high-pass removes it without affecting audible
        // content. Raw aux voicecard buses are left unfiltered.
        for (int ch = 0; ch < mainChans; ++ch)
        {
            auto* data = mainBus.getWritePointer (ch);
            auto& f = dcBlocker_[(size_t) ch];
            for (int i = 0; i < numSamples; ++i)
                data[i] = f.processSample (data[i]);
        }
    }

    // Optional aux buses: bus index 1..6 == VC1..VC6 == voicecard 0..5. A
    // disabled aux contributes 0 channels and is skipped (default layout =
    // main-only, so these are no-ops for existing hosts).
    for (int vc = 0; vc < SynthEngine::getNumParts(); ++vc)
    {
        const int busIdx = vc + 1;
        if (getChannelCountOfBus (false, busIdx) <= 0)
            continue;   // host disabled this aux bus
        auto auxBus = getBusBuffer (buffer, false, busIdx);
        if (auxBus.getNumChannels() > 0)
            auxBus.copyFrom (0, 0, vcBuffers[(size_t) vc].getReadPointer (0), numSamples);
    }
}

//==============================================================================
void ParvatiAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    const auto it = paramIndex_.find (parameterID.toStdString());
    if (it == paramIndex_.end())
        return;

    const auto& d = getPatchParamDescriptors()[static_cast<size_t> (it->second)];

    // Arpeggiator params route to the controller-side arpeggiator, not the
    // patch-byte bridge.
    if (d.isArp)
    {
        applyArpParameter (d, newValue);
        return;
    }

    // Synth options (e.g. VCA curve) have no patch byte.
    if (d.isOption)
    {
        applyOptionParameter (d, newValue);
        return;
    }

    // Step-sequencer params route to the engine Sequencer.
    if (d.isSequencer)
    {
        applySequencerParameter (d, newValue);
        return;
    }

    const uint8_t byte = parvatiValueToPatchByte (d, newValue);
    if (d.isPart)
        engine_.applyPartByte (d.byteOffset, byte);
    else
        engine_.applyPatchByte (d.byteOffset, byte);
}

void ParvatiAudioProcessor::applyParameterToEngine (const PatchParamDescriptor& d)
{
    if (d.isArp)
    {
        const float raw = apvts.getRawParameterValue (d.paramID)->load();
        applyArpParameter (d, raw);
        return;
    }
    if (d.isOption)
    {
        const float raw = apvts.getRawParameterValue (d.paramID)->load();
        applyOptionParameter (d, raw);
        return;
    }
    // part_select is acted on only via live parameterChanged (onPartSelect),
    // never during a bulk sync (which would recurse / thrash).
    if (d.paramID == "part_select")
        return;
    if (d.isSequencer)
    {
        const float raw = apvts.getRawParameterValue (d.paramID)->load();
        applySequencerParameter (d, raw);
        return;
    }

    const float raw = apvts.getRawParameterValue (d.paramID)->load();
    const uint8_t byte = parvatiValueToPatchByte (d, raw);
    if (d.isPart)
        engine_.applyPartByte (d.byteOffset, byte);
    else
        engine_.applyPatchByte (d.byteOffset, byte);
}

void ParvatiAudioProcessor::applyArpParameter (const PatchParamDescriptor& d, float rawValue)
{
    const int v = static_cast<int> (rawValue);
    if (d.paramID == "arp_mode")
        engine_.setArpMode (static_cast<uint8_t> (v));
    else if (d.paramID == "arp_direction")
        engine_.setArpDirection (static_cast<uint8_t> (v));
    else if (d.paramID == "arp_octave")
        engine_.setArpOctave (static_cast<uint8_t> (juce::jlimit (1, 4, v)));
    else if (d.paramID == "arp_pattern")
        engine_.setArpPattern (static_cast<uint8_t> (v));
    else if (d.paramID == "arp_resolution")
        engine_.setArpResolution (static_cast<uint8_t> (v));
}

void ParvatiAudioProcessor::applyOptionParameter (const PatchParamDescriptor& d, float rawValue)
{
    if (d.paramID == "vca_curve")
        engine_.setVcaExponential (static_cast<int> (rawValue) != 0);  // 0=Linearized, 1=Exponential
    else if (d.paramID == "part_select")
        onPartSelect (static_cast<int> (rawValue));
    else if (d.paramID == "filter_card")
    {
        using FT = ambika::dsp::FilterTopology;
        const int v = static_cast<int> (rawValue);
        const FT t = (v == 2) ? FT::TWO_POLE_SVF
                    : (v == 1) ? FT::FOUR_POLE_SSM2164
                               : FT::FOUR_POLE_LADDER;
        engine_.setFilterTopology (t);   // global: every voice, every part
    }
}

void ParvatiAudioProcessor::applySequencerParameter (const PatchParamDescriptor& d, float rawValue)
{
    const int v = static_cast<int> (rawValue);
    if (d.paramID == "seq_length_1")
        engine_.setSequenceLength (0, static_cast<uint8_t> (v));
    else if (d.paramID == "seq_length_2")
        engine_.setSequenceLength (1, static_cast<uint8_t> (v));
    else if (d.paramID == "seq_length_3")
        engine_.setSequenceLength (2, static_cast<uint8_t> (v));
    else
        // Step params: byteOffset is the controller PartData offset; the
        // Sequencer's sequence_data[] is offset by -16 within PartData.
        engine_.setSequenceDataByte (d.byteOffset - 16, static_cast<uint8_t> (v));
}

void ParvatiAudioProcessor::syncAllParamsToEngine()
{
    for (const auto& d : getPatchParamDescriptors())
        applyParameterToEngine (d);
}

//==========================================================================
// Multitimbral Part selection. Edits always route to the current Part's
// objects/voices (see SynthEngine::applyPatchByte), so each Part's storage is
// always up to date — switching Parts only needs to LOAD the new Part's stored
// values back into the APVTS (so the editor reflects it).
void ParvatiAudioProcessor::onPartSelect (int newPart1Based)
{
    const int newPart = juce::jlimit (0, SynthEngine::getNumParts() - 1, newPart1Based - 1);
    if (newPart == currentPart_)
        return;

    currentPart_ = newPart;
    engine_.setCurrentPart (newPart);
    loadPartIntoApvts (newPart);
    syncAllParamsToEngine();   // ensure the new Part's voices match (idempotent)
}

void ParvatiAudioProcessor::loadPartIntoApvts (int part)
{
    auto& p = engine_.getPart (part);
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isOption)              // part_select / vca_curve are global
            continue;

        float value = 0.0f;
        if (d.isArp)
        {
            if (d.paramID == "arp_mode")            value = (float) p.arp.getMode();
            else if (d.paramID == "arp_direction")  value = (float) p.arp.getDirection();
            else if (d.paramID == "arp_octave")     value = (float) p.arp.getOctave();
            else if (d.paramID == "arp_pattern")    value = (float) p.arp.getPattern();
            else if (d.paramID == "arp_resolution") value = (float) p.arp.getResolution();
        }
        else if (d.isSequencer)
        {
            if (d.paramID == "seq_length_1")      value = (float) p.seq.getSequenceLength (0);
            else if (d.paramID == "seq_length_2") value = (float) p.seq.getSequenceLength (1);
            else if (d.paramID == "seq_length_3") value = (float) p.seq.getSequenceLength (2);
            else                                   value = (float) p.seq.getSequenceDataByte (d.byteOffset - 16);
        }
        else
        {
            const uint8_t byte = d.isPart ? p.partBytes[(size_t) d.byteOffset]
                                           : p.patchBytes[(size_t) d.byteOffset];
            value = parvatiPatchByteToValue (d, byte);
        }

        apvts.getParameterAsValue (d.paramID) = value;
    }
}

//==========================================================================
// Ambika .PRO patch loading.
bool ParvatiAudioProcessor::loadProgramFromBytes (const uint8_t* patch112, const uint8_t* part84)
{
    if (patch112 == nullptr || part84 == nullptr)
        return false;

    // Push every patch/part byte back into the APVTS via the inverse mapping;
    // the GUI reads from the APVTS so it updates too.
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isArp || d.isOption)
            continue;  // not stored in the patch/part structs

        const uint8_t byte = (d.isPart || d.isSequencer ? part84 : patch112)[d.byteOffset];
        apvts.getParameterAsValue (d.paramID) = parvatiPatchByteToValue (d, byte);
    }

    // Apply the full patch atomically (the per-param listener writes may be
    // ValueTree/timer-deferred, so an explicit sync guarantees correctness).
    syncAllParamsToEngine();
    return true;
}

bool ParvatiAudioProcessor::loadProgramFile (const juce::File& file)
{
    AmbikaProgram prog;
    if (! parseAmbikaProgramFile (file, prog) || ! prog.hasPatch)
        return false;

    static constexpr std::array<uint8_t, 84> kSilentPart{};
    const uint8_t* partPtr = prog.hasPart ? prog.part.data() : kSilentPart.data();
    if (! loadProgramFromBytes (prog.patch.data(), partPtr))
        return false;

    loadedProgramName_ = prog.name.isNotEmpty() ? prog.name
                                                 : file.getFileNameWithoutExtension();
    return true;
}

bool ParvatiAudioProcessor::saveProgramFile (const juce::File& file)
{
    // The byte-exact inverse of loadProgramFromBytes: iterate the same set of
    // descriptors with the same skip rule (isArp / isOption carry no patch or
    // part byte) and the same patch/part routing, and store each one's current
    // APVTS value as its faithful byte.
    AmbikaProgram prog;
    prog.name = loadedProgramName_.isNotEmpty() ? loadedProgramName_ : "Parvati";
    // prog.patch / prog.part are zero-initialised (the bytes no parameter maps
    // to — e.g. PartData's MIDI channel / key zone / voice allocation, which live
    // in the .MUL MultiData, not the .PRO — stay 0, exactly as on load).

    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isArp || d.isOption)
            continue;  // no patch / part byte

        const float raw = apvts.getRawParameterValue (d.paramID)->load();

        // parvatiValueToPatchByte does not cover sequencer params (they live in
        // the controller PartData, not the Patch struct, and the shared helper
        // early-returns 0 for them), so convert their AudioParameterInt value to
        // the byte directly — otherwise the sequence would not round-trip.
        const uint8_t byte = d.isSequencer
            ? static_cast<uint8_t> (juce::jlimit (d.minValue, d.maxValue, static_cast<int> (raw)))
            : parvatiValueToPatchByte (d, raw);

        const int off = d.byteOffset;
        if (d.isPart || d.isSequencer)
        {
            if (off >= 0 && off < 84) prog.part[(size_t) off] = byte;
        }
        else
        {
            if (off >= 0 && off < 112) prog.patch[(size_t) off] = byte;
        }
    }
    prog.hasPatch = true;
    prog.hasPart  = true;

    return writeAmbikaProgramFile (file, prog);
}

juce::File ParvatiAudioProcessor::getFactoryPatchDir()
{
    // Per-user app-data location (user-writable on macOS, unlike
    // ~/Library/Audio/Presets which is often root-owned). The factory banks are
    // extracted here as subfolders: <appdata>/Parvati/FACTORY/{A,B,F,S}/.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Parvati/FACTORY");
}

//==========================================================================
// Ambika .MUL (multi) loading — configures all 6 Parts at once.
bool ParvatiAudioProcessor::loadMultiFile (const juce::File& file)
{
    AmbikaMulti multi;
    if (! parseAmbikaMultiFile (file, multi) || ! multi.ok)
        return false;

    // Load every Part's patch + PartData bytes, plus its MIDI channel, key
    // zone, and voice allocation (from MultiData.part_mapping_[i]:
    // {midi_channel, low, high, alloc}). Also push each Part's arp/seq settings
    // from its PartData into its Arpeggiator + Sequencer (so ALL 6 Parts carry
    // their .MUL settings, not just the current Part).
    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        auto& part = engine_.getPart (i);
        if (multi.parts[i].hasPatch) part.patchBytes = multi.parts[i].patch;
        if (multi.parts[i].hasPart)  part.partBytes  = multi.parts[i].part;

        if (multi.hasMultiData)
        {
            const uint8_t* pm = multi.multiData.data() + static_cast<size_t> (i * 4);
            engine_.setPartChannel        (i, pm[0]);        // midi_channel (0 = Omni)
            engine_.setPartKeyrange       (i, pm[1], pm[2]); // keyrange_low, keyrange_high
            engine_.setPartVoiceAllocation (i, pm[3]);       // firmware 6-voicecard bitmask
        }

        if (multi.parts[i].hasPart)
        {
            const uint8_t* pb = part.partBytes.data();
            // arp_sequencer_mode@7 drives both the arp and the note-sequencer.
            part.arp.setMode (pb[7]);              part.seq.setMode (pb[7]);
            part.arp.setDirection (pb[8]);         part.arp.setOctave  (pb[9]);
            part.arp.setPattern  (pb[10]);         part.arp.setResolution (pb[11]);
            part.seq.setSequenceLength (0, pb[12]);
            part.seq.setSequenceLength (1, pb[13]);
            part.seq.setSequenceLength (2, pb[14]);
            for (int o = 0; o < 64; ++o)
                part.seq.setSequenceDataByte (o, pb[16 + o]);
        }
    }

    // Defer the patch/part-byte push + voice-allocation rebuild to the audio
    // thread. setPartVoiceAllocation (above) already marked the engine dirty; this
    // explicit mark also covers the polyphony / patch bytes that loadMultiFile
    // wrote directly into Part storage. The audio thread rebuilds + pushes next
    // block (one-block latency, invisible).
    engine_.markAllocationDirty();

    // Show Part 0 in the editor and re-apply its parameters.
    currentPart_ = 0;
    engine_.setCurrentPart (0);
    loadPartIntoApvts (0);
    syncAllParamsToEngine();

    loadedProgramName_ = multi.name.isNotEmpty() ? multi.name
                                                 : file.getFileNameWithoutExtension();
    return true;
}

juce::File ParvatiAudioProcessor::getFactoryMultiDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Parvati/FACTORY_MULTI");
}

juce::File ParvatiAudioProcessor::getUserPatchDir()
{
    // User-writable area for the user's own saved patches/multis. Created on
    // first run by the factory installer. <appdata>/Parvati/USER/.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Parvati/USER");
}

bool ParvatiAudioProcessor::saveMultiFile (const juce::File& file)
{
    // The byte-exact inverse of loadMultiFile. Builds an AmbikaMulti from the
    // live engine + APVTS state: the CURRENT part's Patch/PartData bytes come
    // from the APVTS (captures uncommitted edits), the other 5 parts come from
    // engine storage, and MultiData.part_mapping_ is rebuilt from the engine's
    // per-part channel / keyrange / voice-allocation.
    AmbikaMulti multi;
    multi.name = loadedProgramName_.isNotEmpty() ? loadedProgramName_ : "Parvati";

    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        auto& mp = multi.parts[i];

        if (i == currentPart_)
        {
            // Same byte-bridge as saveProgramFile: iterate every descriptor
            // (skip arp/option; sequencer special-case) and route each byte into
            // patch[off<112] / part[off<84]. Captures live APVTS edits.
            for (const auto& d : getPatchParamDescriptors())
            {
                if (d.isArp || d.isOption)
                    continue;

                const float raw = apvts.getRawParameterValue (d.paramID)->load();
                const uint8_t byte = d.isSequencer
                    ? static_cast<uint8_t> (juce::jlimit (d.minValue, d.maxValue, static_cast<int> (raw)))
                    : parvatiValueToPatchByte (d, raw);

                const int off = d.byteOffset;
                if (d.isPart || d.isSequencer)
                {
                    if (off >= 0 && off < 84) mp.part[(size_t) off] = byte;
                }
                else
                {
                    if (off >= 0 && off < 112) mp.patch[(size_t) off] = byte;
                }
            }

        }
        else
        {
            // Non-current parts: read straight from engine storage.
            mp.patch = engine_.getPart (i).patchBytes;
            mp.part  = engine_.getPart (i).partBytes;
        }

        // Arp (PartData 7..11) and sequencer (12..14 lengths, 16..79 step
        // bytes) live in the per-part Arpeggiator/Sequencer OBJECTS -- the
        // engine setters do not mirror them into partBytes, and the descriptor
        // loop above skips isArp. Serialize them from the live objects for EVERY
        // part (the exact inverse of loadMultiFile's read at ~483-495), making
        // saveMultiFile authoritative regardless of how the patch/part bytes
        // above were sourced. Without this, non-current parts' arp/seq edits
        // were lost on save.
        {
            const auto& p = engine_.getPart (i);
            mp.part[7]  = p.arp.getMode();
            mp.part[8]  = p.arp.getDirection();
            mp.part[9]  = p.arp.getOctave();
            mp.part[10] = p.arp.getPattern();
            mp.part[11] = p.arp.getResolution();
            mp.part[12] = p.seq.getSequenceLength (0);
            mp.part[13] = p.seq.getSequenceLength (1);
            mp.part[14] = p.seq.getSequenceLength (2);
            for (int o = 0; o < 64; ++o)
                mp.part[(size_t) (16 + o)] = p.seq.getSequenceDataByte (o);
        }

        mp.hasPatch = true;
        mp.hasPart  = true;
    }

    // MultiData.part_mapping_[i] = { midi_channel, keyrange_low, keyrange_high,
    // voice_allocation } (4 bytes per part, 6 parts = 24; rest of the 56-byte
    // MultiData stays zero).
    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        const int off = i * 4;
        multi.multiData[(size_t) off + 0] = engine_.getPartChannel (i);
        multi.multiData[(size_t) off + 1] = engine_.getPartKeyrangeLow (i);
        multi.multiData[(size_t) off + 2] = engine_.getPartKeyrangeHigh (i);
        multi.multiData[(size_t) off + 3] = engine_.getPartVoiceAllocation (i);
    }
    multi.hasMultiData = true;
    multi.ok = true;

    return writeAmbikaMultiFile (file, multi);
}

//==========================================================================
// Parvati-native preset format (.parvati) — full-fidelity YAML.
bool ParvatiAudioProcessor::saveParvatiPatchFile (const juce::File& file)
{
    const juce::String text = parvati::preset::serializeParvatiPatch (*this);
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.writeText (text, false, false, "\n"))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}

bool ParvatiAudioProcessor::loadParvatiPatchFile (const juce::File& file)
{
    juce::String text;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk()) return false;
        text = in.readEntireStreamAsString();
    }
    if (! parvati::preset::applyParvatiPatch (*this, text))
        return false;
    // Derive a display name from the file (the in-document name is applied via
    // the loaded-program title separately by the editor).
    loadedProgramName_ = file.getFileNameWithoutExtension();
    return true;
}

bool ParvatiAudioProcessor::saveParvatiMultiFile (const juce::File& file)
{
    const juce::String text = parvati::preset::serializeParvatiMulti (*this);
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.writeText (text, false, false, "\n"))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}

bool ParvatiAudioProcessor::loadParvatiMultiFile (const juce::File& file)
{
    juce::String text;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk()) return false;
        text = in.readEntireStreamAsString();
    }
    if (! parvati::preset::applyParvatiMulti (*this, text))
        return false;
    loadedProgramName_ = file.getFileNameWithoutExtension();
    return true;
}

//==============================================================================
juce::AudioProcessorEditor* ParvatiAudioProcessor::createEditor()
{
    return new ParvatiEditor (*this);
}

//==========================================================================
void ParvatiAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialize the full APVTS state plus the persisted UI preferences
    // (theme/zoom/tooltips) as root-level properties on the state tree. This is
    // backward compatible: old hosts that ignore unknown properties are
    // unaffected, and old saved states restore with UI defaults (Carbon/1.0/true).
    auto tree = apvts.copyState();
    tree.setProperty ("ui_theme", uiThemeName_, nullptr);
    tree.setProperty ("ui_zoom", uiZoom_, nullptr);
    tree.setProperty ("ui_tooltips", uiTooltips_, nullptr);
    tree.setProperty ("ui_smoothing", uiSmoothing_, nullptr);
    tree.setProperty ("ui_oversampling", uiOversampling_, nullptr);
    tree.setProperty ("ui_language", uiLanguage_, nullptr);
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void ParvatiAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Restore the APVTS, then push every parameter into the engine. If the
    // voices are not yet initialized the byte writes are stored harmlessly and
    // the next prepareToPlay re-syncs after Init().
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.isValid())
        {
            // Read UI preferences (with defaults for backward compatibility with
            // states saved before Phase 4a) before replaceState — the parsed tree
            // is untouched at this point.
            uiThemeName_ = tree.getProperty ("ui_theme", "Carbon").toString();
            uiZoom_ = static_cast<double> (tree.getProperty ("ui_zoom", 1.0));
            uiTooltips_ = static_cast<bool> (tree.getProperty ("ui_tooltips", true));
            uiSmoothing_ = static_cast<bool> (tree.getProperty ("ui_smoothing", false));
            uiOversampling_ = static_cast<int> (tree.getProperty ("ui_oversampling", 1));
            uiLanguage_ = tree.getProperty ("ui_language", "auto").toString();
            apvts.replaceState (tree);
        }
        syncAllParamsToEngine();
        // Restore the audio-side smoothing pref (covers headless / no-editor
        // hosts; the editor re-applies it on construction for GUI hosts).
        engine_.setParameterSmoothing (uiSmoothing_);
        // Restore + propagate the filter-oversampling factor (rebuilds the
        // latency probe + voices; the next prepareToPlay / processBlock reports
        // the matching latency).
        setOversamplingFactor (uiOversampling_);
    }
}

//==========================================================================
void ParvatiAudioProcessor::setParameterSmoothing (bool smoothing)
{
    uiSmoothing_ = smoothing;          // persist
    engine_.setParameterSmoothing (smoothing);
}

//==========================================================================
void ParvatiAudioProcessor::rebuildOsLatencyProbe()
{
    // The probe mirrors the per-voice Oversampling config (1 channel, min-phase
    // IIR half-band, max quality, integer latency) so its getLatencyInSamples()
    // exactly matches what every voice adds. Never fed audio, so rebuilding on
    // the message thread is race-free.
    if (uiOversampling_ > 1)
    {
        const size_t factorExp = (uiOversampling_ >= 4) ? 2 : 1;
        osLatencyProbe_ = std::make_unique<juce::dsp::Oversampling<float>> (
            1u,
            factorExp,
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandPolyphaseIIR,
            true, true);
    }
    else
    {
        osLatencyProbe_.reset();
    }
}

int ParvatiAudioProcessor::computePluginLatency (double hostSampleRate) const
{
    // Lagrange resampler: 2 INPUT (internal) samples of latency
    // (LagrangeTraits::algorithmicLatency == 2.0). latency_host = 2 * hostRate/internalRate.
    int latencySamples = juce::roundToInt (2.0 * hostSampleRate
                                           / ambika::dsp::kInternalSampleRate);

    // Filter oversampling: getLatencyInSamples() is in INPUT (internal) samples;
    // convert to host samples with the same ratio as the resampler latency.
    if (osLatencyProbe_)
    {
        const double osInputSamples = osLatencyProbe_->getLatencyInSamples();
        latencySamples += juce::roundToInt (osInputSamples * hostSampleRate
                                            / ambika::dsp::kInternalSampleRate);
    }

    return juce::jlimit (0, 4096, latencySamples);
}

void ParvatiAudioProcessor::setOversamplingFactor (int factor)
{
    if (factor != 1 && factor != 2 && factor != 4)
        factor = (factor <= 1) ? 1 : (factor <= 2 ? 2 : 4);

    uiOversampling_ = factor;                 // persist
    rebuildOsLatencyProbe();                  // message-thread safe (no audio)
    engine_.setOversamplingFactor (factor);   // per-voice: deferred to audio thread
    latencyDirty_.store (true, std::memory_order_release);   // report next block
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ParvatiAudioProcessor();
}
