// Copyright (c) 2026 805Labs Kft. / Hellcat.  See PluginProcessor.h.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "PluginProcessor.h"

// AFTER the first JUCE-including header: JUCE_IOS is defined by the juce
// module headers, so an #if JUCE_IOS BEFORE this point silently compiles the
// block OUT even on iOS (the 2026-08-19 wave-B thermal sampler shipped that
// way and only the first real iOS-toolchain build caught it).
#if JUCE_IOS
 #include <objc/message.h>   // objc_msgSend (F-ios-perf-2 thermal sampler)
 #include <objc/runtime.h>    // objc_getClass / sel_registerName
#endif

#include "PluginEditor.h"
#include "MulExport.h"
#include "HellcatPreset.h"
#include "PatchFile.h"
#include "ui/FactoryPresetInstaller.h"
#include "ui/IosOpenIn.h"        // open-in routing (desktop: inline no-op)
#include "ui/PresetBrowser.h"     // syncTreeNewestWins (the iOS launch mirror)
#include "ui/SharedContainer.h"
#include "dsp/constants.h"   // ambika::dsp::kInternalSampleRate (resampler latency)
#include "dsp/patch_sanitizer.h"   // sanitizePatch/sanitizePartData (.MUL ingestion)

namespace
{
// Main-bus mix headroom. The processor sums ALL six voicecard buffers into the
// main stereo bus; at unity gain several loud voices (and especially Unison
// stacks) clip. Internal mixing is 32-bit float
// (voiceCardBuffers_ are AudioBuffer<float>; addFrom accumulates in float), so
// there is no INTERNAL clipping — this trim only adds headroom on the summed
// MAIN output. -6 dB (0.5x) is a reasoned default for a 6-voicecard sum; the
// raw per-voicecard AUX buses are left un-trimmed. Adjust up/down if needed.
constexpr float kMainMixHeadroomGain = 0.5f;   // -6 dB
}  // namespace

//==============================================================================
void HellcatAudioProcessor::DeferredParamRing::push (int index, float value) noexcept
{
    // Spin until the drain (message thread) releases the lock. The critical
    // section is a bounded scan + store (no allocation, no system call), so the
    // spin is bounded in practice even under contention.
    while (lock.test_and_set (std::memory_order_acquire)) {}
    // Latest-wins coalescing: an entry for the same parameter index is
    // OVERWRITTEN in place (order among DIFFERENT parameters is preserved),
    // which is exactly knob-burst semantics.
    for (uint32_t i = 0; i < count.load (std::memory_order_relaxed); ++i)
        if (slots[i].index == index)
        {
            slots[i].value = value;
            lock.clear (std::memory_order_release);
            return;
        }
    if (count.load (std::memory_order_relaxed) < slots.size())
    {
        slots[count.load (std::memory_order_relaxed)] = { index, value };
        count.store (count.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    else
        dropped.fetch_add (1, std::memory_order_relaxed);   // overflow: drop (bounded memory)
    lock.clear (std::memory_order_release);
}

juce::Array<HellcatAudioProcessor::DeferredParamRing::Entry> HellcatAudioProcessor::DeferredParamRing::drain() noexcept
{
    // FX audit finding 4: the previous drain built the juce::Array WHILE
    // HOLDING the spinlock -- juce::Array::add can MALLOC, and the audio
    // thread's push() spins unbounded against the flag: if the OS preempts
    // this (message) thread inside that malloc, the audio thread spends its
    // entire block budget in the spin -> overrun -> crackle. Copy into a
    // FIXED-capacity local array under the lock instead (a bounded copy of
    // trivially-copyable 8-byte entries -- provably allocation-free). Then
    // build the returned Array AFTER the lock is released (still the message
    // thread, where allocating is safe).
    decltype (slots) snapshot;   // same fixed capacity as the ring (64)
    uint32_t n = 0;
    while (lock.test_and_set (std::memory_order_acquire)) {}
    {
        n = count.load (std::memory_order_relaxed);
        if (n > slots.size())   // defensive: count never exceeds capacity
            n = (uint32_t) slots.size();
        for (uint32_t i = 0; i < n; ++i)
            snapshot[i] = slots[i];
        count.store (0, std::memory_order_relaxed);
    }
    lock.clear (std::memory_order_release);

    juce::Array<Entry> out;
    out.ensureStorageAllocated ((int) n);   // one reservation, then cheap adds
    for (uint32_t i = 0; i < n; ++i)
        out.add (snapshot[i]);
    return out;
}

#if JUCE_IOS
//==========================================================================
// F-ios-perf-2 (iOS hunt 2026-08-19): read NSProcessInfo.thermalState from
// plain C++ (this TU is .cpp, and the project's .mm sources are owned by
// other lanes). Two objc_msgSend calls with an arm64-safe cast for an
// NSInteger return (no FP returns involved, so the plain objc_msgSend
// signature is the documented-safe one), class/SEL resolved once via
// function-local statics (thread-safe init). Allocation-free after the first
// call — safe inside the 60 Hz timer callback (sampled ~1 Hz anyway).
// NSProcessInfoThermalState: 0=Nominal 1=Fair 2=Serious 3=Critical, exactly
// ThermalLevel's raw values.
static HellcatAudioProcessor::ThermalLevel currentThermalLevel() noexcept
{
    using SendFn = long (*) (void*, void*);   // NSInteger (id, SEL)
    static void* const processInfoClass = (void*) objc_getClass ("NSProcessInfo");
    static void* const processInfoSel   = (void*) sel_registerName ("processInfo");
    static void* const thermalStateSel  = (void*) sel_registerName ("thermalState");
    static SendFn const  send           = (SendFn) objc_msgSend;
    if (processInfoClass == nullptr)
        return HellcatAudioProcessor::ThermalLevel::Nominal;
    void* const info = reinterpret_cast<void*> (send (processInfoClass, processInfoSel));   // +processInfo (long -> void*: same width on arm64)
    const long state = info != nullptr ? send (info, thermalStateSel) : 0;
    return state >= 3 ? HellcatAudioProcessor::ThermalLevel::Critical
         : state == 2 ? HellcatAudioProcessor::ThermalLevel::Serious
         : state == 1 ? HellcatAudioProcessor::ThermalLevel::Fair
                      : HellcatAudioProcessor::ThermalLevel::Nominal;
}
#endif

void HellcatAudioProcessor::DeferredParamTimer::timerCallback()
{
    // Message thread. Drain the deferred ring and dispatch each entry through
    // the exact GUI-path apply function for its class. These are idempotent
    // staging writes (engine setters -> atomics + dirty flags; onPartSelect ->
    // loadPartIntoApvts + syncAllParamsToEngine), the same work a GUI edit
    // does -- safe and correct here, and NOT safe on the audio thread
    // (which is why parameterChanged defers them).

    // Audit F1/F3 reaper: free the audio objects the audio thread parked when
    // installing staged swaps (FX processors on a type change, per-voice
    // Oversampling on a filter-quality change). One walk over 6 chains + 96
    // voices, almost always a no-op (a dirty flag gates the frees).
    owner.engine_.reapRetiredAudioObjects();

#if JUCE_IOS
    // F-ios-perf-2 thermal sampler (~1 Hz, every 60th 60 Hz tick):
    // allocation-free, advisory-only (thermalHint_ is read by the editor for a
    // transient status hint; NOTHING auto-changes sound). currentThermalLevel()
    // is two objc_msgSend calls after the once-only class/SEL resolution.
    if (++tick % 60 == 0)
    {
        const auto level = currentThermalLevel();
        const auto action = thermalActionForLevel (level);
        owner.thermalHint_.store (static_cast<int> (action), std::memory_order_relaxed);
    }
#endif

    const auto entries = owner.deferredParams_.drain();
    for (const auto& e : entries)
    {
        const auto& descs = getPatchParamDescriptors();
        if (e.index < 0 || static_cast<size_t> (e.index) >= descs.size())
            continue;
        const auto& d = descs[static_cast<size_t> (e.index)];
        if (d.isArp || d.isSequencer)
            owner.applyArpSeqParameter (d, e.value);
        else if (d.isFx)
            // FX params deferred from the audio thread (host automation):
            // applyFxParameter allocates (juce::String/substring/std::string
            // keys), so it must not run on the render thread. The engine stages
            // every FX value through the fxDirty_ atomics, so the <=16 ms
            // deferral is inaudible; latest-wins coalescing matches automation
            // burst semantics exactly.
            owner.applyFxParameter (d, e.value);
        else if (d.paramID == "part_select")
            owner.onPartSelect (static_cast<int> (e.value));
    }
}

HellcatAudioProcessor::HellcatAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
        .withOutput ("Main", juce::AudioChannelSet::stereo(), true)
        .withOutput ("VC1", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC2", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC3", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC4", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC5", juce::AudioChannelSet::mono(), false)
        .withOutput ("VC6", juce::AudioChannelSet::mono(), false)),
      apvts (*this, &undoManager_, "PARAMS", createHellcatParameterLayout())
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

    // Extract the embedded factory presets into the user app-data dirs on
    // first run (process-once) so the Patch combo is populated without user
    // setup: the GPL-3.0 Ambika banks (AFACTORY/A..S + AFACTORY_MULTI) plus the
    // ORIGINAL Hellcat patch bank (HFACTORY). This also makes sure the USER
    // save area exists. Non-fatal: a failure just leaves the combo empty.
    hellcat::ensureFactoryPresetsInstalled (getFactoryPatchDir(), getFactoryMultiDir(),
                                            getTemplatesDir(), getHellcatFactoryDir(),
                                            getUserPatchDir());

    // Start the deferred-parameter drain (60 Hz, message thread). Processor
    // construction happens on the message thread in every host (and in the
    // tests, where ScopedJuceInitialiser_GUI makes the main thread the message
    // thread), satisfying juce::Timer's threading requirement.
    deferredTimer_ = std::make_unique<DeferredParamTimer> (*this);
    deferredTimer_->startTimerHz (60);

    // Apply the DEFAULT filter-oversampling factor (2x since the 2026-08
    // default change) so a FRESH instance runs its filter at what the Settings
    // panel will show: the engine voices stage the factor (serviced on their
    // next prepare/audio block) and the latency probe reports the OS group
    // delay. A host-state restore re-applies the PERSISTED factor on top of
    // this (setStateData -> setOversamplingFactor), which is idempotent.
    setOversamplingFactor (getUiOversampling());

#if JUCE_IOS
    // Open-in loop (see ui/IosOpenIn.h): iOS offers "Open in Hellcat" for
    // .yml/.PRO/.MUL (document types grafted 2026-08-19), but
    // JUCE 9 drops application:openURL:options:. Install our handler and
    // route: presets import into the shared USER tree then LOAD through the
    // same main-thread paths the editor's FileChooser completions use (the
    // UIKit delegate delivers on the main thread). Standalone
    // ONLY — the AUv3 extension never receives openURL events, and the
    // Standalone processor is owned by StandalonePluginHolder for the app's
    // whole lifetime (the `this` capture below is safe by construction).
    if (wrapperType == wrapperType_Standalone)
        hellcat::installOpenInHandler (getUserPatchDir(), [this] (const juce::File& routed)
        {
            if (routed.hasFileExtension (".yml")) loadHellcatMultiFile (routed);
            else if (routed.hasFileExtension (".pro")) loadProgramFile (routed);
            else if (routed.hasFileExtension (".mul")) loadMultiFile (routed);
        });

    // F-ios-lc-4 (bug hunt 2026-08-19): publish the shared USER tree into the
    // CONTAINING APP's Documents at Standalone launch. Presets saved from
    // inside an AUv3 host (AUM/GarageBand) land in the App-Group USER tree,
    // but the per-save Documents mirror writes into the EXTENSION's private
    // container there — invisible to the Files app (it only browses the
    // containing app's Documents). This ADDITIVE, newest-wins sync (never
    // deletes; see PresetBrowser::syncTreeNewestWins) makes every accumulated
    // host save Files-visible the next time the Standalone app opens.
    // Standalone ONLY: in the AUv3 process userDocumentsDirectory is the
    // extension's private container (the invisible destination above).
    if (wrapperType == wrapperType_Standalone)
    {
        const auto docsUser = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                  .getChildFile ("Hellcat")
                                  .getChildFile ("USER");
        (void) PresetBrowser::syncTreeNewestWins (getUserPatchDir(), docsUser);
    }
#endif
}

HellcatAudioProcessor::~HellcatAudioProcessor()
{
    // Stop the deferred drain FIRST: the timer holds a back-reference to this
    // processor, and a callback firing mid-destruction would apply parameters
    // to a partially destroyed engine. stopTimer is synchronous (waits for any
    // in-flight callback on this -- the message -- thread).
    if (deferredTimer_ != nullptr)
        deferredTimer_->stopTimer();
    deferredTimer_.reset();
}

//==============================================================================
void HellcatAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate_ = sampleRate;   // cache for the audio-thread latency re-report
#if ! JUCE_IOS
    // Offline auto-max leak guard: a host that entered offline (8x boost) but
    // never called setNonRealtime(false) before re-preparing back in realtime
    // would carry the 8x into the new session. If we are back in realtime with
    // a boost still armed, restore the user's saved factor now (idempotent —
    // setNonRealtime(false)'s own restore then finds nothing left to do). A
    // re-prepare DURING the bounce (still non-realtime) correctly keeps 8x.
    if (offlineSavedOs_ >= 0 && ! isNonRealtime())
    {
        const int saved = offlineSavedOs_;
        offlineSavedOs_ = -1;
        applyOversamplingFactor (saved);
    }
#endif
    // Cache the prepared block size for processBlock's overflow clamp (FX
    // audit F3/F6): every engine-side scratch buffer is sized from this value,
    // so a host block larger than prepared must never reach the renderers
    // unchecked. Mirrors the engine's own jmax(1, ...) floor.
    preparedMaxBlock_.store (juce::jmax (1, samplesPerBlock), std::memory_order_relaxed);
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

void HellcatAudioProcessor::releaseResources()
{
    // F-ios-lc-2 (bug hunt 2026-08-19, iOS hunt lane "lifecycle"): hosts tear
    // down render resources on an audio-session interruption (phone call /
    // Siri / route change: the AUv3 wrapper's deallocateRenderResources calls
    // here) and re-allocate on resume (-> prepareToPlay). JUCE's
    // Synthesiser::prepareToPlay only clears all notes when the SAMPLE RATE
    // changes (juce_Synthesiser.cpp: approximatelyEqual guard), so voices
    // gated at the interruption resume gated and note-offs lost during the
    // window are never re-delivered — STUCK NOTES after an interruption.
    //
    // Split choice: engine_.resetAllVoices() DEFERS the kill to the audio
    // thread (stopNote(0,false) = Kill + clearCurrentNote, serviced at the top
    // of processTransport BEFORE any render) — the same split the patch/multi
    // loads use. It touches ONLY voice activity, never patch/part state (which
    // lives in the Parts' AtomicByteArrays and is untouched here; the next
    // prepare's syncAllParamsToEngine re-primes the voices from the APVTS
    // regardless). JUCE calls releaseResources with the callback stopped
    // (AudioProcessor contract), and even if a host ever raced it, the pending
    // flag is the same atomic path the loaders already use.
    engine_.resetAllVoices();
    // Drop any queued UI-keyboard MIDI so a stale note-on queued before the
    // interruption cannot fire after resume (prepareToPlay re-seeds the
    // collector's sample rate; this clears its queue + event counter).
    midiCollector_.reset (hostSampleRate_ > 0.0 ? hostSampleRate_ : 44100.0);
}

bool HellcatAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
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

void HellcatAudioProcessor::setNonRealtime (bool isNonRealtime) noexcept
{

    // Cache the host's offline-render state. The host wrapper calls this on the
    // message thread when entering/leaving a bounce (freeze/export).
    nonRealtime_.store (isNonRealtime, std::memory_order_relaxed);

#if ! JUCE_IOS
    // ---- Offline auto-max quality (desktop only) ----
    // Offline render (bounce/freeze/export) has no real-time budget: bump the
    // per-voice filter oversampling to 8x for the bounce, then restore the
    // user's factor on exit. iOS is EXCLUDED by measurement (PluginProcessor.cpp
    // state-restore rationale: 8x = 2.3-3.7x realtime on A12-class cores — even
    // offline that is a multi-minute bounce).
    //  - applyOversamplingFactor does NOT persist (host state + the Settings
    //    combo keep the user's choice);
    //  - the staged-install path pre-builds on THIS (message) thread and the
    //    audio thread swaps pointers only — no AT allocation, click-free;
    //  - double-entry guarded by offlineSavedOs_ (>= 0 = boost active);
    //  - a user factor change DURING a bounce updates the saved value and
    //    re-applies it (the user's explicit choice wins mid-bounce too).
    if (isNonRealtime)
    {
        if (offlineSavedOs_ < 0)
        {
            offlineSavedOs_ = getUiOversampling();
            if (offlineSavedOs_ != 8)
                applyOversamplingFactor (8);
        }
    }
    else if (offlineSavedOs_ >= 0)
    {
        const int saved = offlineSavedOs_;
        offlineSavedOs_ = -1;
        applyOversamplingFactor (saved);
    }
#endif

    // Defer to the base so its own bookkeeping (isNonRealtime()) stays in sync.
    juce::AudioProcessor::setNonRealtime (isNonRealtime);
}

//==========================================================================
void HellcatAudioProcessor::setManualTempoBpm (int bpm)
{
    // Clamp to the Settings slider's range (40..300): the value also arrives
    // from restored host state, which must never be trusted raw (the same
    // discipline as every other restored pref). The atomic carries the value
    // to the audio thread; uiPrefsLock_ keeps the setter vs getStateInformation
    // snapshot consistent (the state path reads/writes it under the same lock).
    const int clamped = juce::jlimit (40, 300, bpm);
    const std::lock_guard<std::mutex> l (uiPrefsLock_);
    manualTempoBpm_.store (clamped, std::memory_order_relaxed);
}

void HellcatAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // ---- Realtime overrun probe: capture the wall-clock cost of this block so
    // it can be compared to its real-time budget (numSamples/sampleRate). RT-
    // safe (steady_clock is lock/allocation-free; stores are relaxed atomics).
    // See getAudioLoad* / getAudioOverrunCount in the header.
    const auto probeStart = std::chrono::steady_clock::now();
    const int  probeNumSamples = buffer.getNumSamples();

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
    // No-host-tempo fallback (2026-08-19 AUv3 wave): hosts that expose no
    // musical context to the plugin (the GarageBand-class AUv3 hosts; also
    // the Standalone app, which has no transport) used to run the
    // arpeggiator clock at a hard-coded 120. Resolve the clock tempo as
    // HOST bpm when the playhead carries one, else the user's MANUAL bpm
    // (Settings > Arp Clock; persisted; default 120 = the old behaviour),
    // and publish the resolved source + value for the UI (status line +
    // the editor's transient hint). Relaxed stores — advisory display only.
    double bpm = manualTempoBpm_.load (std::memory_order_relaxed);
    bool isPlaying = true;  // default: run in standalone without transport
    bool hostBpm = false;
    if (auto* playHead = getPlayHead())
    {
        if (const auto pos = playHead->getPosition())
        {
            // Degenerate host values (0.0, negative, NaN) do NOT count as a
            // usable tempo: bpm<=0 leaves TransportClock at its previous tick
            // rate and NaN would reach static_cast<uint16_t> in the tempo-
            // synced voice-LFO path (dsp/voice.cpp) — formally UB. Treat such
            // hosts exactly like no-musical-context hosts: manual fallback.
            if (const auto b = pos->getBpm(); b && *b > 0.0 && std::isfinite (*b))
            {
                bpm = *b;
                hostBpm = true;
            }
            isPlaying = pos->getIsPlaying();
        }
    }
    hostTempoPresent_.store (hostBpm, std::memory_order_relaxed);
    lastClockBpm_.store (static_cast<float> (bpm), std::memory_order_relaxed);

    // Merge UI-injected MIDI (on-screen keyboard / wheels click-play) into the
    // buffer BEFORE processTransport so a clicked key is routed through the
    // arpeggiator / note-sequencer exactly like hardware MIDI: it must engage
    // the current Part's held-key stack + note generation, and — critically —
    // fire the key-release hook that calls Sequencer::allNotesOff(). The old order
    // (merge AFTER processTransport) bypassed the arp entirely: a clicked note
    // played directly AND its note-off never reached the key-release hook,
    // stranding the note-sequencer's sounding note on release. When the Part's
    // arp is OFF the note still passes straight through to the synth, so direct
    // click-play keeps its immediate sound. Additive: only adds messages queued
    // by addMidiEvent (empty when no UI).
    if (buffer.getNumSamples() > 0)
        midiCollector_.removeNextBlockOfMessages (midiMessages, buffer.getNumSamples());

    // Advance the arpeggiator transport (routes MIDI when arp is on, generates
    // arp notes before the block renders).
    engine_.processTransport (midiMessages, buffer.getNumSamples(), bpm, isPlaying);

    // The Synthesiser consumes the (possibly note-stripped) MIDI buffer and
    // renders the active voices additively. The engine's renderVoices override
    // routes each voice to its FIXED voicecard buffer (6 mono buffers) instead
    // of this master buffer, so renderNextBlock leaves `buffer` cleared here.
    // No post-processing stage: hardware Ambika has no master limiter — the
    // voicecard output feeds the analog VCA only, so the engine's per-voice
    // VCA is the final gain stage. The slices below (one per prepared-size
    // chunk) each run the FULL render pipeline for their range.

    // ---- Host block-size CHUNKED render (replaces the old silent-tail clamp) ----
    // Every engine-side buffer (voicecard buffers, FX-output buffers, each
    // FxChain's dry/wet scratch and every FX processor's internal scratch --
    // worst case FxWavefolder's 6x-oversampled osL_/osR_, sized maxBlock*6+8
    // at prepare) is sized from prepareToPlay's samplesPerBlock and is indexed
    // from 0. A host that renders a LARGER block than it prepared (buffer-size
    // transitions, offline/freeze renders, some AU/AUv3 hosts -- JUCE does not
    // universally guarantee numSamples <= samplesPerBlock) would overrun all
    // of them (a 6x-amplified heap OOB WRITE). The old remedy CLAMPED the
    // render count, silently dropping the tail of every oversized block (and
    // still firing out-of-window MIDI events with no audio for them).
    // Instead, TILE the host block with preparedMaxBlock_-sized slices: each
    // slice renders EXACTLY like one normal in-budget host block (engine
    // buffers always addressed from 0), then mixes into the host buffer at
    // [done, done+n). The FULL block renders -- no dropped tail.
    //
    // MIDI: when the block fits in ONE slice (an in-budget host block — the
    // overwhelmingly common case), the buffer is handed as-is with
    // startSample == 0: byte-identical to the old single-slice path. When the
    // block is TILED, EVERY slice (slice 0 included) receives only the events
    // inside its own window [done, done+n), rebased to [0,n) in a scratch
    // MidiBuffer. This is REQUIRED by juce::Synthesiser's processNextBlock
    // semantics: its closing std::for_each DRAINS every remaining event of
    // the handed buffer beyond numSamples (juce_Synthesiser.cpp:232-235), so
    // an unfiltered slice 0 would fire out-of-window events early (at its
    // own start, with no audio for them) AND they would re-fire in their home
    // slice — double-fire timing corruption in oversized/offline renders.
    // (The pre-tiling clamp path had the same early-fire, minus the re-fire.)
    // The scratch is cleared per slice (clear() keeps capacity: no
    // steady-state allocation). processTransport + the MIDI collector
    // already ran on the FULL count above (unchanged); the window filter sees
    // both direct-played notes (forwarded at their host positions) and
    // arp/sequencer-generated events (scheduled within [0, totalSamples)), so
    // both classes land in the correct temporal slice.
    //
    // preparedMaxBlock_ == 0 (not yet prepared) bypasses the tiling, keeping
    // the old behaviour for degenerate pre-prepare blocks.
    const int totalSamples = buffer.getNumSamples();
    const int prepared = preparedMaxBlock_.load (std::memory_order_relaxed);
    // True when the host block exceeds the prepared size and will be TILED:
    // every slice must then be handed a window-filtered MidiBuffer (see the
    // MIDI paragraph above). A single-slice render keeps the fast path (the
    // host buffer handed as-is, byte-identical to the pre-tiling behaviour).
    const bool tileMidi = (prepared > 0) && (totalSamples > prepared);
    const auto& vcBuffers = engine_.getVoiceCardBuffers();
    const auto& fxBuffers = engine_.getFxOutputBuffers();
    const int mainChans = getChannelCountOfBus (false, 0);

    int done = 0;
    while (done < totalSamples)
    {
        const int n = (prepared > 0) ? juce::jmin (prepared, totalSamples - done)
                                     : (totalSamples - done);

        // Slice MIDI: single-slice render = the block's buffer as-is (fires
        // every event at its position, exactly the old semantics); tiled
        // render = the events in [done, done+n), rebased by -done — for slice
        // 0 too (the Synthesiser drain would otherwise double-fire/early-fire
        // out-of-window events; see the MIDI paragraph above).
        const juce::MidiBuffer* sliceMidi = &midiMessages;
        if (tileMidi)
        {
            sliceMidiScratch_.clear();
            for (const auto m : midiMessages)
            {
                const int pos = m.samplePosition;
                if (pos >= done && pos < done + n)
                    sliceMidiScratch_.addEvent (m.getMessage(), pos - done);
            }
            sliceMidi = &sliceMidiScratch_;
        }

        // The Synthesiser consumes the slice's MIDI + renders the voices into
        // the per-voicecard buffers at [0..n) (an in-budget-sized render;
        // renderVoices clears exactly [0, n)).
        engine_.renderNextBlock (buffer, *sliceMidi, 0, n);

        // Render the per-part FX chains into their stereo FX-output buffers
        // (host rate, [0..n) of each chain buffer). Runs AFTER renderNextBlock
        // (so the voicecard buffers hold this slice) and BEFORE the main-bus
        // sum (which sources the main bus from the FX-output buffers). With all
        // fx*_enabled=0 the chains are dry copies, so the main bus is
        // audible-identical to the pre-FX mix. The aux buses remain raw
        // voicecard taps (dry).
        engine_.renderPartFx (n);

        // ---- Multi-output bus mixing (Ambika hardware: 6 individual voicecard
        // outputs + a global mix), PER SLICE at host-buffer [done, done+n) ----
        // Main bus: sum ALL six PER-PART FX-OUTPUT buffers into L and R. This
        // is the post-FX mix (each Part's stereo FX output); with FX disabled
        // it equals the pre-multi-out single-buffer mix (each voice's mono
        // signal, duplicated to L+R by the dry-copy chain). When the main bus
        // is mono, only L is written. Aux buses (VC1..VC6): each ENABLED aux
        // bus copies its DRY voicecard output.
        if (mainChans > 0)
        {
            auto mainBus = getBusBuffer (buffer, false, 0);
            for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            {
                mainBus.addFrom (0, done, fxBuffers[(size_t) p].getReadPointer (0), n, kMainMixHeadroomGain); // main L (-6 dB headroom)
                if (mainChans > 1)
                    mainBus.addFrom (1, done, fxBuffers[(size_t) p].getReadPointer (1), n, kMainMixHeadroomGain); // main R
            }

            // Master DC blocker (main bus only): the engine's filter+VCA are
            // DC-coupled, so any sub-audio/DC offset would otherwise leak as a
            // low-frequency rumble. The 15 Hz high-pass removes it without
            // affecting audible content. Raw aux voicecard buses are left
            // unfiltered. Applied in slice order, so the filter state stays
            // time-contiguous across the block.
            for (int ch = 0; ch < mainChans; ++ch)
            {
                auto* data = mainBus.getWritePointer (ch) + done;
                auto& f = dcBlocker_[(size_t) ch];
                for (int i = 0; i < n; ++i)
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
                auxBus.copyFrom (0, done, vcBuffers[(size_t) vc].getReadPointer (0), n);
        }

        done += n;
    }

    // ---- Realtime overrun probe: record this block's render/budget ratio. ----
    // 1.0 = the block used its entire real-time window (an xrun is imminent/has
    // occurred); >1.0 = it overran (the host missed the deadline -> glitch/
    // crackle). Peak + overrun-count are surfaced to the GUI so we can see
    // whether audible crackle correlates with the audio thread being starved
    // (e.g. by GUI render load sharing a core).
    {
        const auto probeEnd = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double> (probeEnd - probeStart).count();
        if (probeNumSamples > 0 && hostSampleRate_ > 0.0)
        {
            const double budgetSec = static_cast<double> (probeNumSamples) / hostSampleRate_;
            const double r = elapsedSec / budgetSec;
            audioLoadCurrent_.store (r, std::memory_order_relaxed);
            if (audioLoadResetReq_.exchange (false, std::memory_order_acq_rel))
            {
                audioLoadPeak_.store (r, std::memory_order_relaxed);
                audioOverrunCount_.store (r > 1.0 ? 1 : 0, std::memory_order_relaxed);
            }
            else
            {
                // lock-free peak update (CAS loop; no mutex on the audio thread).
                double peak = audioLoadPeak_.load (std::memory_order_relaxed);
                while (r > peak && ! audioLoadPeak_.compare_exchange_weak (
                           peak, r, std::memory_order_relaxed, std::memory_order_relaxed)) {}
                if (r > 1.0)
                    audioOverrunCount_.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }
}

//==============================================================================
void HellcatAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                   juce::MidiBuffer&)
{
    // A bypassed SYNTH outputs silence. Overridden because the inherited
    // juce::AudioProcessor::processBlockBypassed jasserts in debug builds when
    // getLatencySamples() > 0 (ours is nonzero whenever filter oversampling is
    // active, and during the offline auto-max 8x boost). Deliberately NO state
    // flushes: voices/FX keep running internally, so an un-bypass resumes where
    // it left off; the host plays its own dry signal for the bypassed period.
    // (Hosts that implement bypass by simply not calling processBlock never
    // reach this path; VST3 routes its auto-added Bypass parameter here.)
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::clear (buffer.getWritePointer (ch), buffer.getNumSamples());
}

//==============================================================================
void HellcatAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    // Suppress the engine re-apply while loadPartIntoApvts pushes engine->APVTS
    // (the feedback is redundant for byte/arp/seq params and corrupts the FX mod
    // matrix via applyFxParameter's stale sibling read -- see loadingPartIntoApvts_)
    // and while setStateInformation's replaceState rewrites the APVTS (see
    // restoringState_).
    if (loadingPartIntoApvts_ || restoringState_)
        return;

    const auto it = paramIndex_.find (parameterID.toStdString());
    if (it == paramIndex_.end())
        return;

    const auto& d = getPatchParamDescriptors()[static_cast<size_t> (it->second)];

    // Off-message-thread dispatch (host automation on the render thread, or the
    // CC/NRPN map inside processBlock): the arp/seq classes write the engine's
    // pendingConfig_ seqlock (single-writer invariant; a second writer tears
    // it), part_select runs loadPartIntoApvts' ~250 ValueTree+UndoManager
    // writes, and the FX params run applyFxParameter's juce::String/substring/
    // std::string key traffic (heap ops) -- none of it is audio-thread work.
    // Defer to the 60 Hz message-thread drain (DeferredParamTimer) instead: the
    // engine stages every one of these through atomics + dirty flags, so a
    // <=16 ms deferral is inaudible. On the message thread (GUI edits,
    // syncAllParamsToEngine) everything stays synchronous: zero added latency.
    if (! juce::MessageManager::existsAndIsCurrentThread()
        && (d.isArp || d.isSequencer || d.isFx || d.paramID == "part_select"))
    {
        deferredParams_.push (it->second, newValue);
        return;
    }

    // Arp/seq params route to the controller-side arpeggiator/sequencer,
    // not the patch-byte bridge (kArpSeqParamMap dispatch inside).
    if (d.isArp || d.isSequencer)
    {
        applyArpSeqParameter (d, newValue);
        return;
    }

    // Synth options (e.g. VCA curve) have no patch byte.
    if (d.isOption)
    {
        applyOptionParameter (d, newValue);
        return;
    }

    // Per-part FX params route to the engine's FX setters (no patch byte).
    if (d.isFx)
    {
        applyFxParameter (d, newValue);
        return;
    }

    const uint8_t byte = hellcatValueToPatchByte (d, newValue);
    if (d.isPart)
        engine_.applyPartByte (d.byteOffset, byte);
    else
        engine_.applyPatchByte (d.byteOffset, byte);

    // D4 legacy note: this used to ALSO clear the current part's custom-
    // tuning flag on a part_raga=0 write (custom tables shadowed byte 4 while
    // active). The custom-tuning subsystem was removed 2026-08-19 — the raga
    // byte is the whole tuning state now, so nothing to clear.
}

void HellcatAudioProcessor::applyParameterToEngine (const PatchParamDescriptor& d)
{
    if (d.isArp || d.isSequencer)
    {
        const float raw = apvts.getRawParameterValue (d.paramID)->load();
        applyArpSeqParameter (d, raw);
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
    if (d.isFx)
    {
        const float raw = apvts.getRawParameterValue (d.paramID)->load();
        applyFxParameter (d, raw);
        return;
    }

    const float raw = apvts.getRawParameterValue (d.paramID)->load();
    const uint8_t byte = hellcatValueToPatchByte (d, raw);
    if (d.isPart)
        engine_.applyPartByte (d.byteOffset, byte);
    else
        engine_.applyPatchByte (d.byteOffset, byte);
}

void HellcatAudioProcessor::applyArpSeqParameter (const PatchParamDescriptor& d, float rawValue)
{
    // Message-thread only: this writes the engine's pendingConfig_ seqlock
    // (single-writer). Audio-thread-origin arp/seq edits arrive via the deferred
    // ring instead (see parameterChanged / DeferredParamTimer).
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    const int v = static_cast<int> (rawValue);
    // Config fields (5 arp + 3 lengths) dispatch through the single
    // kArpSeqParamMap table; the seq STEP params ride their PartData
    // byteOffset (the Sequencer's sequence_data[] is offset by -16 within
    // PartData).
    if (const auto f = arpSeqFieldForID (d.paramID))
    {
        switch (*f)
        {
            case ambika::dsp::ArpSeqField::ArpMode:       engine_.setArpMode ((uint8_t) v); break;
            case ambika::dsp::ArpSeqField::ArpDirection:  engine_.setArpDirection ((uint8_t) v); break;
            case ambika::dsp::ArpSeqField::ArpOctave:     engine_.setArpOctave ((uint8_t) (juce::jlimit (1, 4, v))); break;
            case ambika::dsp::ArpSeqField::ArpPattern:    engine_.setArpPattern ((uint8_t) v); break;
            case ambika::dsp::ArpSeqField::ArpResolution: engine_.setArpResolution ((uint8_t) v); break;
            case ambika::dsp::ArpSeqField::SeqLength1:    engine_.setSequenceLength (0, (uint8_t) v); break;
            case ambika::dsp::ArpSeqField::SeqLength2:    engine_.setSequenceLength (1, (uint8_t) v); break;
            case ambika::dsp::ArpSeqField::SeqLength3:    engine_.setSequenceLength (2, (uint8_t) v); break;
        }
        return;
    }
    engine_.setSequenceDataByte (d.byteOffset - 16, static_cast<uint8_t> (v));
}

void HellcatAudioProcessor::applyOptionParameter (const PatchParamDescriptor& d, float rawValue)
{
    if (d.paramID == "vca_curve")
        engine_.setVcaExponential (static_cast<int> (rawValue) != 0);  // 0=Linearized, 1=Exponential
    else if (d.paramID == "part_select")
        onPartSelect (static_cast<int> (rawValue));
    else if (d.paramID == "filter_card")
    {
        using FT = ambika::dsp::FilterTopology;
        const int v = static_cast<int> (rawValue);
        // Stock Ambika boards first (SMR4 / 4P / SVF), then the character
        // cards (Ladder / Polivoks / IR3109). The choice order is frozen.
        const FT t = (v == 5) ? FT::FOUR_POLE_IR3109
                    : (v == 4) ? FT::TWO_POLE_POLIVOKS
                    : (v == 3) ? FT::FOUR_POLE_LADDER
                    : (v == 2) ? FT::TWO_POLE_SVF
                    : (v == 1) ? FT::FOUR_POLE_SSM2164
                               : FT::FOUR_POLE_OTA;
        engine_.setFilterTopology (t);   // global: every voice, every part
    }
    else if (d.paramID == "filter_drive")
    {
        // Choice index -> actual drive amount. Entry 1 ("1.2") is the
        // juce::dsp::LadderFilter ctor default, so the default reproduces the
        // pre-control sound. Ladder card only.
        static const float kDriveValues[] = { 1.0f, 1.2f, 1.5f, 2.0f, 3.0f, 5.0f, 8.0f, 12.0f };
        const int idx = juce::jlimit (0, 7, static_cast<int> (rawValue));
        engine_.setFilterDrive (kDriveValues[idx]);
    }
}

void HellcatAudioProcessor::applyFxParameter (const PatchParamDescriptor& d, float rawValue)
{
    // Decode the FX paramID with the ONE shared grammar decoder
    // (parseFxParamId) -> call the matching engine setter on the CURRENT part
    // (engine setters stage into fxState + set fxDirty_). Values are passed as
    // the raw controller-style int/choice index. Range clamps stay HERE
    // (per-setter policy; the decoder only splits the id).
    const int v = juce::roundToInt (rawValue);
    const FxParamId fx = parseFxParamId (juce::String (d.paramID));
    switch (fx.kind)
    {
        // Per-slot params: fx{1,2,3}_type/enabled/drywet/param{1..5}.
        case FxParamId::SlotType:    engine_.setFxSlotType    (fx.slot, static_cast<uint8_t> (v)); return;
        case FxParamId::SlotEnabled: engine_.setFxSlotEnabled (fx.slot, static_cast<uint8_t> (juce::jlimit (0, 1, v))); return;
        case FxParamId::SlotDryWet:  engine_.setFxSlotDryWet  (fx.slot, static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;
        case FxParamId::SlotParam:   engine_.setFxSlotParam   (fx.slot, fx.paramIdx, static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;

        case FxParamId::Topology:    engine_.setFxTopology (static_cast<uint8_t> (juce::jlimit (0, 2, v))); return;
        case FxParamId::Order:       engine_.setFxOrder    (static_cast<uint8_t> (juce::jlimit (0, 5, v))); return;
        // Master section (v3): global wet/dry + 3-band master EQ.
        case FxParamId::Mix:         engine_.setFxMix       (static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;
        case FxParamId::EqLow:       engine_.setFxEqLow     (static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;
        case FxParamId::EqMid:       engine_.setFxEqMid     (static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;
        case FxParamId::EqHigh:      engine_.setFxEqHigh    (static_cast<uint8_t> (juce::jlimit (0, 127, v))); return;

        // FX mod matrix: fxmod{1..16}_source/_dest/_amount. When ANY of the
        // three changes, re-read all three sibling APVTS values for this slot
        // and write them together via setFxModSlot so the engine never sees a
        // torn matrix slot (the MT writes all three atomics under one fxDirty_
        // publish).
        case FxParamId::ModSource:
        case FxParamId::ModDest:
        case FxParamId::ModAmount:
        {
            const juce::String base = "fxmod" + juce::String (fx.slot + 1);
            const auto readInt = [&] (const char* field) {
                const std::string pid = (base + field).toStdString();
                return juce::roundToInt (apvts.getRawParameterValue (pid)->load());
            };
            const uint8_t src = static_cast<uint8_t> (juce::jlimit (0, 255, readInt ("_source")));
            const uint8_t dst = static_cast<uint8_t> (juce::jlimit (0, 255, readInt ("_dest")));
            const int    amt = juce::jlimit (-63, 63, readInt ("_amount"));
            engine_.setFxModSlot (fx.slot, src, dst, static_cast<int8_t> (amt));
            return;
        }

        case FxParamId::None:
        default:
            return;
    }
}

void HellcatAudioProcessor::syncAllParamsToEngine()
{
    for (const auto& d : getPatchParamDescriptors())
        applyParameterToEngine (d);
}

//==========================================================================
// Multitimbral Part selection. Edits always route to the current Part's
// objects/voices (see SynthEngine::applyPatchByte), so each Part's storage is
// always up to date — switching Parts only needs to LOAD the new Part's stored
// values back into the APVTS (so the editor reflects it).
void HellcatAudioProcessor::onPartSelect (int newPart1Based)
{
    // Message-thread only: this runs loadPartIntoApvts (~250 ValueTree+
    // UndoManager writes). Audio-thread-origin part_select edits arrive via the
    // deferred ring (see parameterChanged / DeferredParamTimer).
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    const int newPart = juce::jlimit (0, SynthEngine::getNumParts() - 1, newPart1Based - 1);
    if (newPart == engine_.getCurrentPart())
        return;

    engine_.setCurrentPart (newPart);
    loadPartIntoApvts (newPart);
    syncAllParamsToEngine();   // make sure the new Part's voices match (idempotent)

    // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): re-point the
    // engine's telemetry frame at the newly-edited part and wipe its history
    // so the pill sparklines / stage markers / live filter curve never carry
    // the PREVIOUS part's values across the switch (this is the AUTHORITATIVE
    // point — every editor path (combo, Cmd+1..6, context menu) and every file
    // load routes through part_select -> here).
    engine_.resetUiTelemetry();
    engine_.setUiTelemetryPart (engine_.getCurrentPart());

    // DATA-INTEGRITY GUARD (undo cannot cross a part switch). Two hazards:
    //   (1) DUMP POLLUTION — the display dump above rewrites ~250 params;
    //       recorded as undo actions they would make the switch one giant
    //       undo step. Fixed at the source: loadPartIntoApvts writes the tree
    //       WITHOUT the UndoManager (display dumps are not user edits — the
    //       same doctrine as JUCE's replaceState clearing on state swap).
    //   (2) REPLAY MISROUTING — any PRE-switch edit (Part A's knob) replayed
    //       by undo()/redo() while Part B is current would push A's values
    //       through parameterChanged into B's engine storage — silent
    //       cross-part sound corruption. The clear below removes the stack,
    //       and undoSafe()/redoSafe() (the editor's undo entry points) sweep
    //       the late entries this clear cannot reach: JUCE appends the caller's
    //       part_select action AFTER its change listeners return, and the 10
    //       Hz APVTS tree-flush timer records host-automation writes late.
    // The isPerformingUndoRedo guard matters: an undo() REPLAY that includes
    // a part_select write re-enters here mid-iteration, and mutating the
    // transaction list underneath the iterating undo() crashes.
    undoInvalidatedByPartSwitch_ = true;
    if (! undoManager_.isPerformingUndoRedo())
    {
        undoManager_.beginNewTransaction();
        undoManager_.clearUndoHistory();
    }
}

//==========================================================================
// The editor's ONLY undo/redo entry points (header buttons + Cmd+Z). A part
// switch invalidates every recorded action's part context, and the JUCE
// append-after-listeners ordering + the 10 Hz tree-flush timer can leave
// late entries after onPartSelect's synchronous clear — sweep once more, then
// replay. Host automation never drives this UndoManager (it is a plugin-side
// UI concept), so these two entry points cover every real replay.
void HellcatAudioProcessor::undoSafe()
{
    if (undoInvalidatedByPartSwitch_)
    {
        undoInvalidatedByPartSwitch_ = false;
        undoManager_.beginNewTransaction();
        undoManager_.clearUndoHistory();
    }
    undoManager_.undo();
}

void HellcatAudioProcessor::redoSafe()
{
    if (undoInvalidatedByPartSwitch_)
    {
        undoInvalidatedByPartSwitch_ = false;
        undoManager_.beginNewTransaction();
        undoManager_.clearUndoHistory();
    }
    undoManager_.redo();
}

void HellcatAudioProcessor::loadPartIntoApvts (int part)
{
    // Suppress the parameterChanged feedback loop while we push engine->APVTS
    // (see loadingPartIntoApvts_). RAII: restored even if an early return is
    // added below.
    loadingPartIntoApvts_ = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard { loadingPartIntoApvts_ };

    auto& p = engine_.getPart (part);
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isOption)              // part_select / vca_curve are global
            continue;

        float value = 0.0f;
        if (d.isArp)
        {
            // Read the MT-authoritative arp config from pendingConfig_ (seqlock
            // snapshot; the live object lags it until the AT services configDirty_).
            const auto pc = p.readPendingConfig();
            if (const auto f = arpSeqFieldForID (d.paramID))
                value = (float) p.arpSeqPendingValue (pc, *f);
        }
        else if (d.isSequencer)
        {
            const auto pc = p.readPendingConfig();
            if (const auto f = arpSeqFieldForID (d.paramID))
                value = (float) p.arpSeqPendingValue (pc, *f);
            else
                value = (float) pc.seqData[(size_t) (d.byteOffset - 16)];
        }
        else if (d.isFx)
        {
            // FX is per-part: read the current Part's fxState atomics and map
            // them back to the APVTS (reverse of applyFxParameter). NOT skipped
            // like isOption (those are global); FX follows the part selector.
            // The ONE shared id decoder splits the id; this reader only maps.
            const auto& fx = p.fxState;
            const FxParamId fxid = parseFxParamId (juce::String (d.paramID));
            switch (fxid.kind)
            {
                case FxParamId::SlotType:    value = (float) fx.slotType    [(size_t) fxid.slot].load(); break;
                case FxParamId::SlotEnabled: value = (float) fx.slotEnabled [(size_t) fxid.slot].load(); break;
                case FxParamId::SlotDryWet:  value = (float) fx.slotDryWet  [(size_t) fxid.slot].load(); break;
                case FxParamId::SlotParam:   value = (float) fx.slotParam[(size_t) fxid.slot][(size_t) fxid.paramIdx].load(); break;
                case FxParamId::Topology:    value = (float) fx.topology.load(); break;
                case FxParamId::Order:       value = (float) fx.orderIdx.load(); break;
                case FxParamId::Mix:         value = (float) fx.mix.load(); break;
                case FxParamId::EqLow:       value = (float) fx.eqLow.load(); break;
                case FxParamId::EqMid:       value = (float) fx.eqMid.load(); break;
                case FxParamId::EqHigh:      value = (float) fx.eqHigh.load(); break;
                case FxParamId::ModSource:   value = (float) fx.modSource [(size_t) fxid.slot].load(); break;
                case FxParamId::ModDest:     value = (float) fx.modDest   [(size_t) fxid.slot].load(); break;
                case FxParamId::ModAmount:   value = (float) fx.modAmount [(size_t) fxid.slot].load(); break;
                case FxParamId::None:
                default: break;
            }
        }
        else
        {
            const uint8_t byte = d.isPart ? p.partBytes[(size_t) d.byteOffset]
                                           : p.patchBytes[(size_t) d.byteOffset];
            value = hellcatPatchByteToValue (d, byte);
        }

        // NON-UNDOABLE display write: this is an engine->APVTS reflection
        // (part switch / multi load), not a user edit — recording it would make
        // every part switch a ~250-action undo step that, replayed, writes the
        // previous part's values into the CURRENT part (cross-part
        // corruption; see onPartSelect). getParameterAsValue records via the
        // APVTS UndoManager; writing the SAME child-tree "value" property with
        // a null UndoManager fires the identical sync (tree -> parameter ->
        // attachments) minus the recording.
        if (auto child = apvts.state.getChildWithName (juce::Identifier (d.paramID));
            child.isValid())
            child.setProperty (juce::Identifier ("value"), value, nullptr);
        else
            apvts.getParameterAsValue (d.paramID) = value;   // defensive fallback
    }
}

//==========================================================================
// Ambika .PRO patch loading.
bool HellcatAudioProcessor::loadProgramFromBytes (const uint8_t* patch112, const uint8_t* part84)
{
    if (patch112 == nullptr || part84 == nullptr)
        return false;

    // Clean slate: kill every sounding voice so a patch switch during editing
    // never leaves stuck / orphaned voices carrying stale Part / patch state.
    engine_.resetAllVoices();

    // Push every patch/part byte back into the APVTS via the inverse mapping;
    // the GUI reads from the APVTS so it updates too.
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isArp || d.isOption || d.isFx)
            continue;  // not stored in the patch/part structs

        const uint8_t byte = (d.isPart || d.isSequencer ? part84 : patch112)[d.byteOffset];
        apvts.getParameterAsValue (d.paramID) = hellcatPatchByteToValue (d, byte);
    }

    // Apply the full patch atomically (the per-param listener writes may be
    // ValueTree/timer-deferred, so an explicit sync guarantees correctness).
    syncAllParamsToEngine();

    // An Ambika program carries NO FX information, so the previously-loaded
    // patch's FX would otherwise remain active. The reset runs AFTER
    // syncAllParamsToEngine() below (which re-applies EVERY param incl. fx from
    // the APVTS, so a pre-sync reset would be clobbered by the stale fx values):
    // reset the CURRENT part's FX directly in the engine, then refresh the fx*
    // APVTS params so the UI shows the clean state (all slots None / bypassed /
    // dry, Series topology, flat EQ, cleared mod matrix).
    engine_.resetPartFx (engine_.getCurrentPart());
    for (const auto& d : getPatchParamDescriptors())
        if (d.isFx)
            apvts.getParameterAsValue (d.paramID) = (float) d.defaultValue;

    // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the new patch's
    // histories/markers start clean — the pill sparklines + envelope stage
    // dots + live filter curve never carry the previous patch's motion across
    // the load boundary (epoch bump + engine-side wipe + re-point at this part).
    engine_.resetUiTelemetry();
    engine_.setUiTelemetryPart (engine_.getCurrentPart());
    return true;
}

bool HellcatAudioProcessor::loadProgramFile (const juce::File& file)
{
    AmbikaProgram prog;
    if (! parseAmbikaProgramFile (file, prog) || ! prog.hasPatch)
        return false;

    static constexpr std::array<uint8_t, 84> kSilentPart{};
    const uint8_t* partPtr = prog.hasPart ? prog.part.data() : kSilentPart.data();
    if (! loadProgramFromBytes (prog.patch.data(), partPtr))
        return false;

    setLoadedProgramName (prog.name.isNotEmpty() ? prog.name
                                                 : file.getFileNameWithoutExtension());
    return true;
}

void HellcatAudioProcessor::gatherCurrentPartBytes (std::array<uint8_t, 112>& patch,
                                                    std::array<uint8_t, 84>&  part) const
{
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.isArp || d.isOption || d.isFx)
            continue;  // no patch / part byte

        const float raw = apvts.getRawParameterValue (d.paramID)->load();

        // hellcatValueToPatchByte does not cover sequencer params (they live in
        // the controller PartData, not the Patch struct, and the shared helper
        // early-returns 0 for them), so convert their AudioParameterInt value to
        // the byte directly — otherwise the sequence would not round-trip.
        const uint8_t byte = d.isSequencer
            ? static_cast<uint8_t> (juce::jlimit (d.minValue, d.maxValue, static_cast<int> (raw)))
            : hellcatValueToPatchByte (d, raw);

        const int off = d.byteOffset;
        if (d.isPart || d.isSequencer)
        {
            if (off >= 0 && off < 84) part[(size_t) off] = byte;
        }
        else
        {
            if (off >= 0 && off < 112) patch[(size_t) off] = byte;
        }
    }
}

bool HellcatAudioProcessor::saveProgramFile (const juce::File& file)
{
    // The byte-exact inverse of loadProgramFromBytes: gatherCurrentPartBytes
    // iterates the same set of descriptors with the same skip rule (isArp /
    // isOption carry no patch or part byte) and the same patch/part routing,
    // and stores each one's current APVTS value as its faithful byte.
    AmbikaProgram prog;
    prog.name = getLoadedProgramName().isNotEmpty() ? getLoadedProgramName() : "Hellcat";
    // prog.patch / prog.part are zero-initialised (the bytes no parameter maps
    // to — e.g. PartData's MIDI channel / key zone / voice allocation, which live
    // in the .MUL MultiData, not the .PRO — stay 0, exactly as on load).
    gatherCurrentPartBytes (prog.patch, prog.part);
    prog.hasPatch = true;
    prog.hasPart  = true;

    return writeAmbikaProgramFile (file, prog);
}

juce::File HellcatAudioProcessor::getFactoryPatchDir()
{
    // Per-user app-data location (user-writable on macOS, unlike
    // ~/Library/Audio/Presets which is often root-owned). The Ambika factory
    // banks are extracted here as subfolders: <appdata>/Hellcat/AFACTORY/{A,B,F,S}/.
    // The "A" prefix holds the stock Ambika banks; the ORIGINAL Hellcat bank
    // installs under its own HFACTORY/ root (see getHellcatFactoryDir).
    return hellcat::getSharedContainerRoot()
        .getChildFile ("Hellcat/AFACTORY");
}

//==========================================================================
// Ambika .MUL (multi) loading — configures all 6 Parts at once.
bool HellcatAudioProcessor::loadMultiFile (const juce::File& file)
{
    AmbikaMulti multi;
    if (! parseAmbikaMultiFile (file, multi) || ! multi.ok)
        return false;

    // Validate-first (routing/sound-hybrid guard): the firmware writer always
    // emits ALL six Parts' Patch + PartData objects, so a .MUL whose MBKS
    // stream stops before the last part (a hand-trimmed or byte-truncated
    // file — the walker stops cleanly at the cut, so parse alone reports ok)
    // is corrupt. Accepting it would apply the NEW MultiData routing over the
    // PREVIOUS multi's patch/part bytes for the missing parts — a hybrid —
    // so reject the whole file instead.
    for (const auto& mp : multi.parts)
        if (! (mp.hasPatch && mp.hasPart))
            return false;

    // Clean slate: kill every sounding voice before the new multi configures
    // all 6 Parts (avoids stuck notes from the previous multi's routing)...
    engine_.resetAllVoices();
    // ...and reset the per-part VOICE SLOTS to the engine init allocation
    // (Part 0 = 6 voices, Parts 1..5 disabled) so the multi applies over a
    // defined state instead of the previous multi's leftover counts. The
    // .MUL's own MultiData part_mapping_ masks then overwrite every Part
    // below (parseAmbikaMultiFile only accepts files that carry MultiData).
    resetVoiceSlotsToInit();

    // Load every Part's patch + PartData bytes, plus its MIDI channel, key
    // zone, and voice allocation (from MultiData.part_mapping_[i]:
    // {midi_channel, low, high, alloc}). Also push each Part's arp/seq settings
    // from its PartData into its Arpeggiator + Sequencer (so ALL 6 Parts carry
    // their .MUL settings, not just the current Part).
    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        auto& part = engine_.getPart (i);
        // SANITIZE AT INGESTION (memory-safety wave 2026-08-22): .MUL bytes are
        // raw file data (no APVTS round-trip) — normalize to firmware domains
        // before they land in the engine, exactly like restoreState does for
        // host blobs. Identity for firmware-written files.
        if (multi.parts[(size_t) i].hasPatch)
        {
            ambika::dsp::Patch patchView;
            std::memcpy (&patchView, multi.parts[(size_t) i].patch.data(), sizeof (ambika::dsp::Patch));
            ambika::dsp::sanitizePatch (patchView);
            std::array<uint8_t, 112> sanitized {};
            std::memcpy (sanitized.data(), &patchView, sizeof (ambika::dsp::Patch));
            part.patchBytes = sanitized;   // AtomicByteArray::operator=(std::array)
        }
        if (multi.parts[(size_t) i].hasPart)
        {
            auto pb = multi.parts[(size_t) i].part;
            ambika::dsp::sanitizePartData (pb);
            part.partBytes = pb;
        }

        // An Ambika multi carries NO FX information -> reset every Part's FX to
        // a clean slate so the previously-loaded multi's FX does not survive the
        // load. (Part 0 is reflected into the APVTS by loadPartIntoApvts below.)
        engine_.resetPartFx (i);

        if (multi.hasMultiData)
        {
            const uint8_t* pm = multi.multiData.data() + static_cast<size_t> (i * 4);
            engine_.setPartChannel        (i, pm[0]);        // midi_channel (0 = Omni)
            engine_.setPartKeyrange       (i, pm[1], pm[2]); // keyrange_low, keyrange_high
            engine_.setPartVoiceAllocation (i, pm[3]);       // firmware 6-voicecard bitmask
        }

        if (multi.parts[(size_t) i].hasPart)
        {
            // Stage this Part's arp/seq config (PartData 7..14 + 16..79) through
            // pendingConfig_ + configDirty_ -- NOT the live objects -- so the
            // audio thread stays the sole writer of the Arpeggiator/Sequencer
            // objects (a direct write here raced the audio-thread clock loop),
            // and pendingConfig_ (the serialize source) stays in sync with the
            // loaded values. arp_sequencer_mode@7 drives arp + note-sequencer.
            engine_.stageArpSeqFromPartBytes (i);
        }
    }

    // Defer the patch/part-byte push + voice-allocation rebuild to the audio
    // thread. setPartVoiceAllocation (above) already marked the engine dirty; this
    // explicit mark also covers the polyphony / patch bytes that loadMultiFile
    // wrote directly into Part storage. The audio thread rebuilds + pushes next
    // block (one-block latency, invisible).
    engine_.markAllocationDirty();

    // Show Part 0 in the editor and re-apply its parameters.
    engine_.setCurrentPart (0);
    // Sync the part_select parameter to the engine's part-0 state (mirrors the
    // .yml multi path, HellcatPreset.cpp:794-795). Without this the
    // editor's part combo kept showing the PREVIOUSLY selected part while the
    // engine/edits had already moved to Part 0 -- the combo desynced from
    // engine state until the user re-picked a part. onPartSelect early-returns
    // (the engine is already on Part 0), so this is a parameter/combo update only.
    if (auto* ps = apvts.getParameter ("part_select"))
        ps->setValueNotifyingHost (ps->convertTo0to1 (1.0f));
    loadPartIntoApvts (0);   // engine→APVTS one-way display refresh (Part 0 authoritative)
    // NOTE: NO syncAllParamsToEngine() — engine storage is authoritative after
    // a multi-load; pushing the (Part-0-only) APVTS back would clobber the
    // just-loaded bytes with stale values.

    setLoadedProgramName (multi.name.isNotEmpty() ? multi.name
                                                 : file.getFileNameWithoutExtension());

    // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): a whole-multi
    // load swaps every part's patch — wipe the telemetry history and re-point
    // the frame at the freshly-shown Part 0 (onPartSelect early-returns here:
    // the engine was set to Part 0 directly, so its hook cannot run for this
    // boundary).
    engine_.resetUiTelemetry();
    engine_.setUiTelemetryPart (engine_.getCurrentPart());
    return true;
}

juce::File HellcatAudioProcessor::getFactoryMultiDir()
{
    return hellcat::getSharedContainerRoot()
        .getChildFile ("Hellcat/AFACTORY_MULTI");
}

juce::File HellcatAudioProcessor::getTemplatesDir()
{
    // Stock init templates (full-fidelity .yml multis): Mono / Poly /
    // Unison / Multitimbral. Created on first run by the factory installer.
    // <appdata>/Hellcat/TEMPLATES/.
    return hellcat::getSharedContainerRoot()
        .getChildFile ("Hellcat/TEMPLATES");
}

juce::File HellcatAudioProcessor::getHellcatFactoryDir()
{
    // The ORIGINAL Hellcat factory patches (64 single-part .yml patches,
    // bass/keys/leads/pads/fx — authored directly under presets/HFACTORY/.
    // Installed write-if-missing on first run, like the Ambika banks, but
    // under its own root: no GPL Ambika data lives here.
    // <appdata>/Hellcat/HFACTORY/.
    return hellcat::getSharedContainerRoot()
        .getChildFile ("Hellcat/HFACTORY");
}

juce::File HellcatAudioProcessor::getUserPatchDir()
{
    // User-writable area for the user's own saved patches/multis. Created on
    // first run by the factory installer. <appdata>/Hellcat/USER/.
    return hellcat::getSharedContainerRoot()
        .getChildFile ("Hellcat/USER");
}

bool HellcatAudioProcessor::saveMultiFile (const juce::File& file, int strategyInt)
{
    // The byte-exact inverse of loadMultiFile. Builds an AmbikaMulti from the
    // live engine + APVTS state: the CURRENT part's Patch/PartData bytes come
    // from the APVTS (captures uncommitted edits), the other 5 parts come from
    // engine storage, and MultiData.part_mapping_ is rebuilt from the engine's
    // per-part channel / keyrange / voice-allocation.
    AmbikaMulti multi;
    multi.name = getLoadedProgramName().isNotEmpty() ? getLoadedProgramName() : "Hellcat";

    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        auto& mp = multi.parts[(size_t) i];

        if (i == engine_.getCurrentPart())
        {
            // Same byte-bridge as saveProgramFile (captures live APVTS edits).
            gatherCurrentPartBytes (mp.patch, mp.part);
        }
        else
        {
            // Non-current parts: read straight from engine storage.
            engine_.getPart (i).patchBytes.copyTo (mp.patch);
            engine_.getPart (i).partBytes.copyTo (mp.part);
        }

        // Serialize the arp/seq config from pendingConfig_ (the message-thread-
        // authoritative source) rather than the live objects, which lag
        // pendingConfig_ until the audio thread services configDirty_. Both
        // pendingConfig_ and configDirty_ are written only on the message thread,
        // so this read is race-free. (PartData 7..14 + 16..79.)
        {
            const auto pc = engine_.getPart (i).readPendingConfig();
            mp.part[7]  = pc.arpMode;
            mp.part[8]  = pc.arpDirection;
            mp.part[9]  = pc.arpOctave;
            mp.part[10] = pc.arpPattern;
            mp.part[11] = pc.arpResolution;
            mp.part[12] = pc.seqLength[0];
            mp.part[13] = pc.seqLength[1];
            mp.part[14] = pc.seqLength[2];
            for (int o = 0; o < 64; ++o)
                mp.part[(size_t) (16 + o)] = pc.seqData[o];
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

    // Export fallback: when a Part requests more voices than its voicecards
    // (the voice-slot extension), the chosen strategy rewrites the bitmasks
    // (and optionally the polyphony modes) to map the requested voices onto
    // the 6 hardware cards. ChainSplit also writes sibling "-2.MUL"
    // unit files for physically chained Ambikas — AFTER the primary file, so
    // a mid-set failure can roll the whole generation back (see below).
    std::vector<AmbikaMulti> unitMultis;   // ChainSplit units 1..N (unit 0 rides the primary)
    {
        using namespace hellcat::mul_export;
        const Setup setup = getMulExportSetup();
        const auto strat = static_cast<Strategy> (juce::jlimit (0, 5, strategyInt));

        // One full multi per unit (same patches/routing; only masks + modes
        // differ). Unit 0 is `multi`; ChainSplit clones it per extra unit.
        const auto applySolution = [] (AmbikaMulti& m, const Solution& sol)
        {
            for (int i = 0; i < SynthEngine::getNumParts(); ++i)
            {
                m.multiData[(size_t) (i * 4 + 3)] = sol.masks[(size_t) i];
                if (sol.polyOverridden[(size_t) i])
                    m.parts[(size_t) i].part[15] = sol.polyMode[(size_t) i];
            }
        };

        if (strat == Strategy::ChainSplit)
        {
            const auto units = solveChain (setup);
            applySolution (multi, units.front());
            // Units 1..N are staged here and written AFTER the primary file
            // below: writing them first (the old order) left freshly-written
            // "-2.MUL" siblings on disk when the PRIMARY write failed, and a
            // mid-set unit failure left units from the NEW generation next to
            // the STALE primary — a chained-Ambika set that matches neither.
            for (size_t u = 1; u < units.size(); ++u)
            {
                AmbikaMulti unitMulti = multi;   // same patches + routing
                applySolution (unitMulti, units[u]);
                unitMultis.push_back (unitMulti);
            }
        }
        else if (strat != Strategy::AsIs)
        {
            applySolution (multi, solve (setup, strat));
        }
    }
    multi.hasMultiData = true;
    multi.ok = true;

    // PRIMARY first: its write is atomic (TemporaryFile), so a failure here
    // leaves the disk exactly as it was (nothing to roll back).
    if (! writeAmbikaMultiFile (file, multi))
        return false;

    // Then the ChainSplit unit siblings. On any failure, delete EVERY file
    // this save already wrote (units + the primary), so a failed save leaves
    // the disk as it was — the whole chained set loads as one consistent
    // generation or not at all.
    if (! unitMultis.empty())
    {
        juce::Array<juce::File> written;
        written.add (file);
        for (size_t u = 0; u < unitMultis.size(); ++u)
        {
            const juce::File unitFile = file.getParentDirectory().getChildFile (
                file.getFileNameWithoutExtension() + "-" + juce::String (u + 2) + ".MUL");
            if (! writeAmbikaMultiFile (unitFile, unitMultis[u]))
            {
                for (const auto& f : written)
                    f.deleteFile();
                return false;
            }
            written.add (unitFile);
        }
    }
    return true;
}

hellcat::mul_export::Setup HellcatAudioProcessor::getMulExportSetup() const
{
    // Voice-first model: the per-part slot count IS the requested voice count
    // (0 = a disabled part), and the card counts come from the DERIVED
    // voicecard masks (contiguous proportional share of the 6 cards — the
    // same rule the engine's rebuild tags voices with, so needsFallback
    // compares against what a .MUL can actually carry).
    hellcat::mul_export::Setup setup;
    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        const int slots = engine_.getPartVoiceSlots (i);
        const uint8_t mask = engine_.getPartVoiceAllocation (i);   // derived
        int cards = 0;
        for (int vc = 0; vc < 6; ++vc)
            if (mask & (1u << vc)) ++cards;
        setup.cards[(size_t) i] = cards;
        setup.active[(size_t) i] = slots > 0;
        setup.requested[(size_t) i] = slots;
        setup.polyMode[(size_t) i] = engine_.getPartPolyphony (i);
    }
    return setup;
}

//==========================================================================
// Hellcat-native preset format (.yml) — full-fidelity YAML.
bool HellcatAudioProcessor::saveHellcatPatchFile (const juce::File& file)
{
    const juce::String text = hellcat::preset::serializeHellcatPatch (*this);
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.writeText (text, false, false, "\n"))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}

bool HellcatAudioProcessor::loadHellcatPatchFile (const juce::File& file)
{
    juce::String text;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk()) return false;
        text = in.readEntireStreamAsString();
    }
    // VALIDATE FIRST, MUTATE LATER (the .MUL / .hellcat-multi doctrine): a
    // corrupt document must not reach resetAllVoices below — a failed load
    // previously cut every sounding voice and THEN failed, leaving silence
    // under a stale UI. applyHellcatMulti re-parses internally; this hoisted
    // check is the same cheap guard (an object with a `params:` object).
    {
        const juce::var tree = hellcat::preset::parseHellcatYaml (text);
        if (! tree.isObject() || ! tree["params"].isObject())
            return false;
    }
    // Clean slate before applying the new Hellcat-native patch.
    engine_.resetAllVoices();
    if (! hellcat::preset::applyHellcatPatch (*this, text))
        return false;
    // Derive a display name from the file (the in-document name is applied via
    // the loaded-program title separately by the editor).
    setLoadedProgramName (file.getFileNameWithoutExtension());

    // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): a patch-file load
    // swaps the whole edited patch — wipe the telemetry history + re-point at
    // the edited part so the pill sparklines / live markers start clean.
    engine_.resetUiTelemetry();
    engine_.setUiTelemetryPart (engine_.getCurrentPart());
    return true;
}

bool HellcatAudioProcessor::saveHellcatMultiFile (const juce::File& file)
{
    const juce::String text = hellcat::preset::serializeHellcatMulti (*this);
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.writeText (text, false, false, "\n"))
            return false;
        out.flush();
    }
    return temp.overwriteTargetFileWithTemporary();
}

bool HellcatAudioProcessor::loadHellcatMultiFile (const juce::File& file)
{
    juce::String text;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk()) return false;
        text = in.readEntireStreamAsString();
    }
    // VALIDATE FIRST, MUTATE LATER: parse the document and confirm it is a
    // real multi BEFORE any engine mutation runs. A malformed file (or a
    // non-multi document) previously left the engine already reset to the
    // init allocation — a failed load mutated the synth under a stale UI
    // (resetAllVoices + resetVoiceSlotsToInit had run, then applyHellcatMulti
    // returned false after them). applyHellcatMulti re-parses internally; this
    // pre-parse is the same cheap check hoisted ahead of the resets so a
    // failed load leaves the engine + UI exactly as they were. The parts
    // array must be NON-EMPTY and every entry an object: a degenerate
    // `parts: []` (or a list of scalars) would otherwise "load successfully"
    // over the PREVIOUS multi's leftover state (the reset only touches
    // slots/names, and applyHellcatMulti skips non-object entries entirely).
        // (b) A corrupt .yml multi (the multi path got validate-first; the
        // patch path used to resetAllVoices first and only then fail).
        // A parts entry must carry at least one RECOGNIZED part key — the
        // line-based parser wraps a BARE list item (`- 7`) as an object with
        // the key "0", so a plain isObject() check would pass garbage through.
        {
            const juce::var tree = hellcat::preset::parseHellcatYaml (text);
            if (! tree.isObject())
                return false;
            auto* partsArr = tree["parts"].getArray();
            if (partsArr == nullptr || partsArr->isEmpty())
                return false;
            static const char* kPartKeys[] = { "channel", "keyzone_low", "keyzone_high",
                                              "voice_allocation", "voice_slots", "name",
                                              "params", "tuning_mode", "tuning_offsets" };
            // NOTE: tuning_mode/tuning_offsets are LEGACY keys (the custom-
            // tuning subsystem was removed 2026-08-19). They stay in the
            // recognition list so old .yml files that carry them still
            // parse as valid parts (their values are ignored on apply — see
            // HellcatPreset.cpp); a bare/scalar parts entry must still fail.
            for (const auto& entry : *partsArr)
            {
                auto* entryObj = entry.getDynamicObject();
                if (entryObj == nullptr)
                    return false;
                bool hasRecognizedKey = false;
                for (const char* k : kPartKeys)
                    if (entryObj->hasProperty (juce::Identifier (k))) { hasRecognizedKey = true; break; }
                if (! hasRecognizedKey)
                    return false;   // bare/scalar item or an all-unknown-keys entry
            }
        }
    // Clean slate before applying the new Hellcat-native multi: kill sounding
    // voices AND reset the per-part voice slots to the engine init allocation
    // (Part 0 = 6 voices, Parts 1..5 disabled) — the format is human-editable,
    // so a parts list shorter than 6 (or a part node without voice_slots /
    // voice_allocation keys) would otherwise inherit the PREVIOUS multi's
    // leftover counts. applyHellcatMulti then restores the file's explicit
    // slots over the init state (a saved file always carries all 6).
    engine_.resetAllVoices();
    resetVoiceSlotsToInit();
    if (! hellcat::preset::applyHellcatMulti (*this, text))
        return false;
    setLoadedProgramName (file.getFileNameWithoutExtension());

    // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): whole-multi load —
    // wipe the telemetry history + re-point at the restored current part
    // (applyHellcatMulti routes its own part-0 select through the part_select
    // boundary, but this explicit reset also covers a same-part restore).
    engine_.resetUiTelemetry();
    engine_.setUiTelemetryPart (engine_.getCurrentPart());
    return true;
}

//==========================================================================
void HellcatAudioProcessor::resetVoiceSlotsToInit()
{
    // Mirror of the SynthEngine constructor's kInitVoiceAllocation
    // { 0x3f, 0, 0, 0, 0, 0 } (SynthEngine.cpp): Part 0 materializes 6 slots
    // (popcount of 0x3f), Parts 1..5 are disabled. Public setters only — the
    // public setPartVoiceSlots clamps 0 to 1, so disabling rides the legacy
    // setPartVoiceAllocation(part, 0) zero-mask path. Each setter defers the
    // pool re-partition to the audio thread (markAllocationDirty); the
    // multi loaders mark again after applying the file's own data.
    engine_.setPartVoiceSlots (0, 6);
    for (int p = 1; p < SynthEngine::getNumParts(); ++p)
        engine_.setPartVoiceAllocation (p, 0);

    // Whole-setup loads also reset the user part ALIASES ("Kick", "Lead",
    // ...): a stale name is a stale UI label for content the loaded file just
    // replaced, so resetVoiceSlotsToInit (the clean-slate both multi loaders
    // run before applying file data) clears them too. The .yml multi path
    // is safe against this reset by construction: its serializer ALWAYS emits
    // the per-part `name:` key (even empty), so applyHellcatMulti re-applies
    // the file's names right after. The .MUL format carries NO part names
    // (Ambika MultiData has no such field — names are a Hellcat extension),
    // so aliases correctly vanish on a .MUL load. Single-patch loads
    // (.PRO / .yml patch) deliberately do NOT pass through here: a part
    // alias is user metadata about the TRACK, not the patch — swapping the
    // sound keeps the label (same doctrine as a DAW track name surviving a
    // clip swap).
    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
        engine_.setPartName (p, {});
}

//==============================================================================
juce::AudioProcessorEditor* HellcatAudioProcessor::createEditor()
{
    return new HellcatEditor (*this);
}

//==========================================================================
void HellcatAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialize the full APVTS state plus the persisted UI preferences
    // (theme/zoom/tooltips) as root-level properties on the state tree. This is
    // backward compatible: old hosts that ignore unknown properties are
    // unaffected, and old saved states restore with UI defaults (Carbon/1.0/true).
    auto tree = apvts.copyState();
    // UI-preference snapshot under ONE lock acquisition (F-ios-lc-1): this
    // runs on whatever thread the host calls getStateInformation (AUv3
    // autosaves are NOT message-thread), while the setters run on the message
    // thread. juce::String is refcounted — a torn copy is a UAF class.
    juce::String theme, language, name;
    double zoom; bool tooltips, smoothing, modLampCat; int oversampling, refreshHz, fontMode, manualBpm;
    {
        const std::lock_guard<std::mutex> l (uiPrefsLock_);
        theme        = uiThemeName_;
        zoom         = uiZoom_;
        tooltips     = uiTooltips_;
        smoothing    = uiSmoothing_;
        modLampCat   = uiModLampCategory_;
        oversampling = uiOversampling_;
        refreshHz    = uiRefreshHz_;
        fontMode     = uiFontMode_;
        language     = uiLanguage_;
        manualBpm    = manualTempoBpm_.load (std::memory_order_relaxed);
        name         = loadedProgramName_;
    }
    tree.setProperty ("ui_theme", theme, nullptr);
    tree.setProperty ("ui_zoom", zoom, nullptr);
    tree.setProperty ("ui_tooltips", tooltips, nullptr);
    tree.setProperty ("ui_mod_lamp_category", modLampCat, nullptr);   // mod-matrix lamp colour policy
    tree.setProperty ("ui_smoothing", smoothing, nullptr);
    tree.setProperty ("ui_oversampling", oversampling, nullptr);
    tree.setProperty ("ui_refresh_hz", refreshHz, nullptr);   // live mod-feedback animation cadence (docs/LIVE_MOD_FEEDBACK_DESIGN.md)
    tree.setProperty ("ui_font_mode", fontMode, nullptr);
    tree.setProperty ("ui_language", language, nullptr);
    tree.setProperty ("manual_bpm", manualBpm, nullptr);   // arp-clock manual tempo (setManualTempoBpm clamps)
    tree.setProperty ("loaded_program_name", name, nullptr);   // last loaded preset (GUI title + browser label)
    // Full 6-Part multitimbral engine state (all parts' patch/part bytes, arp/seq
    // config, routing, voice allocation/mode). Base64 so it rides inside the XML
    // state tree; absent on pre-persistence states (backward compatible).
    juce::MemoryBlock engineBlob;
    engine_.captureState (engineBlob);
    tree.setProperty ("engine_state", engineBlob.toBase64Encoding(), nullptr);
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void HellcatAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
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
            // is untouched at this point. Parsed to locals first, then committed
            // under ONE lock acquisition (F-ios-lc-1): this runs on the host's
            // thread while the message-thread getters may be reading.
            juce::String rTheme   = tree.getProperty ("ui_theme", "Carbon").toString();
            const double rZoom    = static_cast<double> (tree.getProperty ("ui_zoom", 1.0));
            const bool rTooltips  = static_cast<bool> (tree.getProperty ("ui_tooltips", true));
            // Mod-lamp colour policy: absent in older states -> true (the
            // category colour, the richer default).
            const bool rModLampCat = static_cast<bool> (tree.getProperty ("ui_mod_lamp_category", true));
            const bool rSmoothing = static_cast<bool> (tree.getProperty ("ui_smoothing", false));
            // Fallback default 2: the 2026-08 default change (1x -> 2x). A
            // state that PERSISTED the property keeps its stored factor
            // (including 1x); only states from before the property existed
            // (or with it absent) get the new default.
            int rOversampling = static_cast<int> (tree.getProperty ("ui_oversampling", 2));
#if JUCE_IOS
            // F-ios-perf-1 (iOS hunt 2026-08-19): filter oversampling is
            // PER-VOICE (96 voices at max polyphony). Measured (repo harness,
            // M-series core): 8x = 0.93x realtime => 2.3-3.7x realtime on
            // A12-class iPad cores = guaranteed dropouts; 4x is 1.2-1.9x.
            // The Settings combo only offers 1x/2x on iOS; a state SAVED on
            // desktop (or an older iOS build) at 4x/8x silently clamps to 2x
            // here. Silent by design: audio continuity beats a modal warning,
            // and the combo already shows the effective value.
            if (rOversampling > 2)
                rOversampling = 2;
#endif
            const int rFontMode     = static_cast<int> (tree.getProperty ("ui_font_mode", 0));
            // Live mod-feedback animation rate (docs/LIVE_MOD_FEEDBACK_DESIGN.md):
            // absent in pre-2026-08 states -> the 30 default. Clamped to the
            // setting's 5..60 range inline (same lock discipline as the
            // siblings: restored state is never trusted raw — a hand-edited
            // XML with 0 or 999 must not drive an absurd poll timer).
            const int rRefreshHz     = juce::jlimit (5, 60, static_cast<int> (tree.getProperty ("ui_refresh_hz", 30)));
            juce::String rLanguage  = tree.getProperty ("ui_language", "auto").toString();
            // Arp-clock manual tempo: absent in pre-2026-08 states -> the 120
            // default (= the old hard-coded no-host-tempo behaviour).
            const int rManualBpm     = static_cast<int> (tree.getProperty ("manual_bpm", 120));
            // Last loaded preset name: absent in pre-persistence states -> KEEP
            // the current name (whatever the load path already set). A legacy
            // state must not reset the title to "Init".
            const bool hasLoadedName = tree.hasProperty ("loaded_program_name");
            juce::String rLoadedName = tree.getProperty ("loaded_program_name", juce::String()).toString();
            {
                const std::lock_guard<std::mutex> l (uiPrefsLock_);
                uiThemeName_   = std::move (rTheme);
                uiZoom_        = rZoom;
                uiTooltips_    = rTooltips;
                uiModLampCategory_ = rModLampCat;
                uiSmoothing_   = rSmoothing;
                uiOversampling_ = rOversampling;
                uiRefreshHz_    = rRefreshHz;
                uiFontMode_    = rFontMode;
                uiLanguage_    = std::move (rLanguage);
                // setManualTempoBpm's clamp range, applied inline (same lock):
                // restored state is never trusted raw.
                manualTempoBpm_.store (juce::jlimit (40, 300, rManualBpm), std::memory_order_relaxed);
                // Apply the preset name only when the state carried it (see
                // hasLoadedName above): a legacy state keeps the current name.
                if (hasLoadedName)
                    loadedProgramName_ = std::move (rLoadedName);
            }

            // JUCE 9 dispatch reality (verified against the vendored checkout):
            // replaceState fires valueTreeRedirected -> setDenormalisedValue ->
            // setValueNotifyingHost per CHANGED parameter, so parameterChanged
            // runs DURING replaceState on this thread. Guard with restoringState_
            // so those mid-restore callbacks are swallowed (they would otherwise
            // re-apply stale/partial values to the engine, racing the
            // authoritative engine-blob restore below; a part_select callback
            // would even drive the full part-load machinery re-entrantly).
            restoringState_ = true;
            struct RestoreGuard { bool& flag; ~RestoreGuard() { flag = false; } } restoreGuard { restoringState_ };
            apvts.replaceState (tree);

            // Restore the full 6-Part engine state if the blob is present; else
            // (legacy pre-persistence state) fall back to pushing the current-
            // part APVTS into the engine (Parts 1..5 revert to init, as before).
            const bool restored = [&]()
            {
                if (! tree.hasProperty ("engine_state"))
                    return false;
                juce::MemoryBlock blob;
                if (! blob.fromBase64Encoding (tree.getProperty ("engine_state").toString()))
                    return false;
                if (! engine_.restoreState (blob.getData(), blob.getSize()))
                    return false;
                // Engine is authoritative for all 6 parts; refresh the APVTS
                // display for the restored current part. restoreState itself
                // sets the engine's current part (it previously synced a mirror
                // only via the accidental re-entrant part_select callback, which
                // restoringState_ now suppresses).
                // Re-echo part_select to the restored current part (mirrors
                // loadMultiFile): loadPartIntoApvts SKIPS isOption descriptors
                // (incl. part_select), so if the tree's param and the blob's
                // savedCurrent ever diverge (a host-modified state, or a save
                // racing a deferred part_select drain), the header combo would
                // show the wrong part while edits land on the blob's part.
                // restoringState_ is still set, so parameterChanged skips the
                // engine re-apply — this is a parameter/combo update only.
                if (auto* ps = apvts.getParameter ("part_select"))
                    ps->setValueNotifyingHost (
                        ps->convertTo0to1 (static_cast<float> (engine_.getCurrentPart() + 1)));
                loadPartIntoApvts (engine_.getCurrentPart());
                // The three global OPTION params (vca_curve / filter_card /
                // filter_drive) are NOT in the engine blob (captureState carries
                // no option bytes) and loadPartIntoApvts SKIPS isOption
                // descriptors — so without this re-apply the restored session
                // renders with the ENGINE defaults while the UI combos show the
                // saved values (typical hosts call prepareToPlay BEFORE
                // setStateInformation, so the ctor/prepare sync never re-applies
                // them afterwards). Mirrors the legacy branch's
                // syncAllParamsToEngine option coverage with the exact option
                // apply tail. part_select is excluded: it is a selection, not an
                // engine option — onPartSelect would thrash the just-restored
                // part state (the engine already holds the restored part).
                for (const auto& d : getPatchParamDescriptors())
                    if (d.isOption && d.paramID != "part_select")
                        applyOptionParameter (d, apvts.getRawParameterValue (d.paramID)->load());
                // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the
                // restore swapped the whole 6-Part engine state — wipe the
                // telemetry history + re-point at the restored part so the pill
                // sparklines / live markers never carry the pre-restore session's
                // motion (replacing state mid-session leaves the editor open).
                engine_.resetUiTelemetry();
                engine_.setUiTelemetryPart (engine_.getCurrentPart());
                return true;
            }();
            if (! restored)
            {
                // Legacy state: the APVTS is authoritative. Select the SAVED
                // part BEFORE syncing (the original code got this ordering only
                // via replaceState's accidental re-entrant part_select callback;
                // the plan's literal sync-then-select order would write the
                // saved part's bytes into the previously-current part's
                // storage). syncAllParamsToEngine routes every byte edit through
                // the engine's current part, so the saved values must land on the
                // saved part.
                const int savedPart = juce::jlimit (0, SynthEngine::getNumParts() - 1,
                    juce::roundToInt (apvts.getRawParameterValue ("part_select")->load()) - 1);
                engine_.setCurrentPart (savedPart);
                syncAllParamsToEngine();
                loadPartIntoApvts (savedPart);   // display refresh (no-op values)
                // Live mod-feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): same
                // wipe + re-point for the legacy (APVTS-authoritative) restore
                // path above — a restored session starts with clean histories.
                engine_.resetUiTelemetry();
                engine_.setUiTelemetryPart (engine_.getCurrentPart());
            }
        }
        engine_.setParameterSmoothing (getUiSmoothing());
        // Restore + propagate the filter-oversampling factor (rebuilds the
        // latency probe + voices; the next prepareToPlay / processBlock reports
        // the matching latency).
        setOversamplingFactor (getUiOversampling());
    }
}

//==========================================================================
void HellcatAudioProcessor::setParameterSmoothing (bool smoothing)
{
    setUiSmoothing (smoothing);         // persist (locked accessor — F-ios-lc-1)
    engine_.setParameterSmoothing (smoothing);
}

//==========================================================================
void HellcatAudioProcessor::rebuildOsLatencyProbe (int osFactor)
{
    // The probe IS the per-voice Oversampling config (AmbikaVoice::
    // buildOversamplingFor builds both paths) so its getLatencyInSamples()
    // exactly matches what every voice adds. Never fed audio, so rebuilding on
    // the message thread is race-free. The caller passes the factor it is
    // applying — the persisted uiOversampling_ for user edits, 8x for the
    // offline auto-max boost (which must NOT touch the persisted pref).
    osLatencyProbe_ = AmbikaVoice::buildOversamplingFor (osFactor);   // null for factor 1
    // Stage the OS latency (input samples) for the audio thread to read --
    // it must not dereference osLatencyProbe_ (rebuilt here on the message
    // thread) from processBlock. acquire/release pair with computePluginLatency.
    stagedOsLatencyInputSamples_.store (
        osLatencyProbe_ != nullptr
            ? static_cast<int> (osLatencyProbe_->getLatencyInSamples())
            : 0,
        std::memory_order_release);
}

int HellcatAudioProcessor::computePluginLatency (double hostSampleRate) const
{
    // Lagrange resampler: 2 INPUT (internal) samples of latency
    // (LagrangeTraits::algorithmicLatency == 2.0). latency_host = 2 * hostRate/internalRate.
    int latencySamples = juce::roundToInt (2.0 * hostSampleRate
                                           / ambika::dsp::kInternalSampleRate);

    // Filter oversampling: the OS latency in INPUT samples is staged on the
    // message thread by rebuildOsLatencyProbe() (the probe's unique_ptr is
    // rebuilt there, so it must not be dereferenced from this audio-thread
    // call). Convert the staged value to host samples with the same ratio.
    const int osInputSamples = stagedOsLatencyInputSamples_.load (std::memory_order_acquire);
    latencySamples += juce::roundToInt (osInputSamples * hostSampleRate
                                        / ambika::dsp::kInternalSampleRate);

    return juce::jlimit (0, 4096, latencySamples);
}

void HellcatAudioProcessor::applyOversamplingFactor (int factor)
{
    // Supported factors: 1 / 2 / 4 / 8 (powers of two up to the 8x UI maximum).
    // Anything else snaps to the nearest supported factor.
    factor = AmbikaVoice::clampOversamplingFactor (factor);

    rebuildOsLatencyProbe (factor);                // message-thread safe (no audio)
    // Per-voice: PRE-BUILD + stage each voice's replacement Oversampling here
    // (message thread; audit F3) — the audio thread installs with pointer
    // moves only. Idle voices install on their next rendered note.
    engine_.setOversamplingFactor (factor);
    latencyDirty_.store (true, std::memory_order_release);   // report next block
}

void HellcatAudioProcessor::setOversamplingFactor (int factor)
{
    // The PUBLIC setter: persist the user's choice, then apply it. A factor
    // change made DURING an offline auto-max boost (editor open over a bounce)
    // re-targets the restore point and applies immediately — the user's
    // explicit choice wins mid-bounce too.
    factor = AmbikaVoice::clampOversamplingFactor (factor);

    setUiOversampling (factor);             // persist (locked accessor — F-ios-lc-1)
    if (offlineSavedOs_ >= 0)
        offlineSavedOs_ = factor;
    applyOversamplingFactor (factor);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HellcatAudioProcessor();
}
