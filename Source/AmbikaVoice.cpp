// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See AmbikaVoice.h.

#include "AmbikaVoice.h"

#include <array>
#include <cmath>

#include "AmbikaSound.h"
#include "dsp/patch.h"   // WAVEFORM_SAW

namespace
{
// Exponential VCA gain (60 dB OTA taper) with a makeup factor so its average
// over the 0..1 CV range ~matches the linear curve (linear avg = 0.5; raw exp
// avg ≈ 0.145 => makeup ≈ 3.5x). Clamped at 1.0 so a full-VCA note never clips
// the makeup. Keeps the exponential SHAPE while preventing the drastic level
// drop vs linear.
constexpr float kExpVcaMakeup = 3.5f;
inline float exponentialVcaGain (float n) noexcept
{
    return juce::jmin (1.0f, kExpVcaMakeup * std::pow (10.0f, (n - 1.0f) * 3.0f));
}
}  // namespace

bool AmbikaVoice::canPlaySound (juce::SynthesiserSound* sound)
{
    return dynamic_cast<AmbikaSound*> (sound) != nullptr;
}

void AmbikaVoice::ensureInitialized()
{
    if (initialized_)
        return;

    // Loads the faithful init patch (osc1/osc2 = WAVEFORM_NONE) and arms the
    // envelopes. The audible program is then written by the APVTS parameter
    // bridge (PluginProcessor::syncAllParamsToEngine), which sets every patch
    // byte -- so a fresh patch is audible (osc1 = saw) and every host/GUI/
    // automation edit drives the synth.
    voice_.Init();
    initialized_ = true;
}

void AmbikaVoice::prepare (double hostSampleRate, int /*blockSize*/)
{
    hostRate_   = hostSampleRate;
    speedRatio_ = ambika::dsp::kInternalSampleRate / hostSampleRate;

    // Pick up any pending oversampling factor change before arming the filter.
    // prepare() runs in host setup (non-concurrent with processBlock), so a
    // direct assignment of osFactor_ here is race-free. If the message thread
    // PRE-BUILT a staged object (audit F3), install it instead of rebuilding —
    // same configuration (buildOversamplingFor builds both paths) — so a
    // factor set before prepareToPlay is not built twice.
    osFactor_ = pendingOsFactor_.load (std::memory_order_relaxed);
    osFactorDirty_.store (false, std::memory_order_relaxed);
    {
        int expected = kOsStageStaged;
        if (osStageState_.compare_exchange_strong (expected, kOsStageEmpty,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
            filterOS_ = std::move (pendingOs_);   // install the pre-built object
        else
            filterOS_ = buildOversamplingFor (osFactor_);   // (null for factor 1)
    }
    // Retirement parking: claim + delete anything still parked (prepare runs
    // with the callback stopped, so a plain exchange is race-free here; this
    // mirrors reapRetired for the re-prepare path instead of overwriting the
    // slots, which would leak a parked object).
    for (auto& r : retiredOs_)
        delete r.exchange (nullptr, std::memory_order_acq_rel);
    retiredOsDirty_.store (false, std::memory_order_relaxed);

    // The analog filter operates on the internal-rate (or oversampled) signal
    // (matches hardware: the filter is post-DSP, pre-DAW). The filter runs at
    // osFactor_*kInternalSampleRate when OS is on (else at the engine rate).
    filter_.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_LADDER);
    filter_.setMode (0);   // LP
    prepareFilterAtOsRate();
    filter_.commit();

    interp_.reset();
    fifo_.clear();
    // Reserve the worst-case FIFO demand so the first block never reallocates
    // on the audio thread. At low host sample rates speedRatio_ (internal/host)
    // is large, so inputNeeded = ceil(kMaxChunk*speedRatio_) + kLookahead can
    // exceed 1024 (the old fixed bound) -> a push_back realloc on the audio
    // thread. Reserve from the actual ratio (+ one block + slack).
    fifo_.reserve (static_cast<size_t> (std::ceil (kMaxChunk * speedRatio_))
                   + static_cast<size_t> (kLookahead)
                   + static_cast<size_t> (ambika::dsp::kAudioBlockSize) + 16);

    // Optional parameter smoothing operates at the internal rate (the filter /
    // VCA run on the 39216 Hz engine signal), so the ramp length is sized from
    // kInternalSampleRate. 20 ms is a good zipper-noise / responsiveness trade.
    smoothedCutoffHz_.reset (ambika::dsp::kInternalSampleRate, 0.02);
    smoothedResonance_.reset (ambika::dsp::kInternalSampleRate, 0.02);
    smoothedVcaGain_.reset (ambika::dsp::kInternalSampleRate, 0.02);
    smoothingActive_ = false;   // re-arm the snap-on-first-block logic

    ensureInitialized();
}

void AmbikaVoice::setOversamplingFactor (int factor)
{
    // Clamp to the supported factors (1 / 2 / 4 / 8).
    factor = clampOversamplingFactor (factor);

    // No-op: the last staged factor matches AND nothing is queued. (This
    // implies the active osFactor_ already equals @p factor: state Empty with
    // pendingOsFactor_ == factor means the last consume/prepare installed
    // exactly that value — osFactor_ itself is AT-owned and must not be read
    // from this thread.) CONSUMING also satisfies the early-out: the AT is
    // installing exactly that staged factor, so re-staging is pointless. (The
    // audio-stopped-engine case never spins here either — the take-back CAS
    // in acquireOsStaging succeeds immediately.) Redundant calls (the
    // processor seeds every voice with the same factor) must not allocate 96
    // objects.
    if (pendingOsFactor_.load (std::memory_order_relaxed) == factor
        && osStageState_.load (std::memory_order_acquire) != kOsStageStaged)
        return;

    // Audit F3: PRE-BUILD the replacement Oversampling HERE on the calling
    // (message) thread and stage it. The audio thread installs it with
    // pointer moves only (consumeStagedOversampling) instead of running
    // make_unique + initProcessing for every voice inside one callback.
    // The internal block is the constant kAudioBlockSize (40), so the
    // pre-built object is valid at any host rate / block size.
    acquireOsStaging();
    pendingOsFactor_.store (factor, std::memory_order_relaxed);
    pendingOs_ = buildOversamplingFor (factor);   // null for factor 1
    osStageState_.store (kOsStageStaged, std::memory_order_release);

    // The dirty flag remains the trigger the audio thread polls (and the
    // extra fallback below); a silent voice installs on its next note,
    // exactly like the old rebuild.
    osFactorDirty_.store (true, std::memory_order_release);
}

AmbikaVoice::~AmbikaVoice()
{
    // Callback stopped at engine teardown: claim + delete parked objects.
    for (auto& r : retiredOs_)
        delete r.exchange (nullptr, std::memory_order_acq_rel);
}

std::unique_ptr<juce::dsp::Oversampling<float>> AmbikaVoice::buildOversamplingFor (int factor)
{
    if (factor <= 1)
        return nullptr;   // factor 1: no oversampling (bit-identical path)
    // juce::dsp::Oversampling takes a power-of-2 EXPONENT: 2^factorExp.
    // 2x -> 1, 4x -> 2, 8x -> 3. Min-phase IIR half-band = minimal latency;
    // max quality steepens the transition band. Integer latency is enabled
    // so getLatencyInSamples() is a whole number of INPUT samples (clean for
    // host PDC reporting).
    const size_t factorExp = (factor >= 8) ? 3 : (factor >= 4) ? 2 : 1;
    auto os = std::make_unique<juce::dsp::Oversampling<float>> (
        1u,
        factorExp,
        juce::dsp::Oversampling<float>::FilterType::filterHalfBandPolyphaseIIR,
        true,   // isMaxQuality
        true);  // useIntegerLatency
    // The internal block rendered to the filter is always kAudioBlockSize
    // (40) — processSamplesUp turns that into 40*factor.
    os->initProcessing (static_cast<size_t> (ambika::dsp::kAudioBlockSize));
    os->reset();
    return os;
}

//==========================================================================
// Staged OS swap internals (audit F3; mirrors FxChain's type staging).
bool AmbikaVoice::acquireOsStaging() noexcept
{
    for (;;)
    {
        int cur = kOsStageEmpty;
        if (osStageState_.compare_exchange_strong (
                cur, kOsStageFilling, std::memory_order_acquire, std::memory_order_acquire))
            return true;   // Empty -> Filling
        if (cur == kOsStageStaged
            && osStageState_.compare_exchange_strong (
                   cur, kOsStageFilling, std::memory_order_acquire, std::memory_order_acquire))
            return true;   // take back an unconsumed swap (coalesce: newest wins)
        juce::Thread::yield();   // only against an in-flight AT install (microseconds)
    }
}

bool AmbikaVoice::consumeStagedOversampling() noexcept
{
    int expected = kOsStageStaged;
    // Claim to CONSUMING, not Empty (bug hunt 2026-08-18 — mirrors the FxChain
    // kStageConsuming fix): pendingOs_ stays AT-owned until the final store
    // below, otherwise the MT's acquireOsStaging could start filling it while
    // this thread is still mid-move-out. The CAS's release ordering only
    // covers writes BEFORE it.
    if (! osStageState_.compare_exchange_strong (
            expected, kOsStageConsuming, std::memory_order_acq_rel, std::memory_order_relaxed))
        return false;

    // Pointer moves only: park the displaced object, install the staged one,
    // re-prepare the filter at the new rate. prepareFilterAtOsRate touches
    // only float filter state on this (owning) thread — the same work the old
    // AT rebuild did, minus the allocation.
    parkRetiredOversampling (filterOS_.release());
    filterOS_ = std::move (pendingOs_);
    osFactor_ = pendingOsFactor_.load (std::memory_order_relaxed);
    prepareFilterAtOsRate();
    filter_.commit();

    // pendingOs_ is free ONLY now (release-orders the move-out above before
    // this store, so an MT that observes Empty cannot be racing our reads).
    osStageState_.store (kOsStageEmpty, std::memory_order_release);
    return true;
}

void AmbikaVoice::parkRetiredOversampling (juce::dsp::Oversampling<float>* old) noexcept
{
    if (old == nullptr)
        return;
    for (auto& r : retiredOs_)
    {
        juce::dsp::Oversampling<float>* expected = nullptr;
        if (r.compare_exchange_strong (expected, old, std::memory_order_release,
                                       std::memory_order_relaxed))
        {
            retiredOsDirty_.store (true, std::memory_order_release);
            return;
        }
    }
    // Parking full (2 OS changes inside one 60 Hz reaper interval — needs a
    // double factor flip faster than the reaper). Free here as the documented
    // fallback.
    delete old;
}

void AmbikaVoice::reapRetired() noexcept
{
    if (! retiredOsDirty_.exchange (false, std::memory_order_acq_rel))
        return;
    for (auto& r : retiredOs_)
        delete r.exchange (nullptr, std::memory_order_acq_rel);
}

void AmbikaVoice::prepareFilterAtOsRate()
{
    // The cutoff is in Hz (rate-independent), so preparing the filter at
    // osFactor_*kInternalSampleRate with the SAME Hz cutoff is correct — it
    // simply gives the digital model more headroom before Nyquist.
    const double rate = (osFactor_ > 1)
        ? (static_cast<double> (osFactor_) * ambika::dsp::kInternalSampleRate)
        : ambika::dsp::kInternalSampleRate;
    const int blockSize = ambika::dsp::kAudioBlockSize * juce::jmax (1, osFactor_);

    filter_.prepare (rate, blockSize);
}

void AmbikaVoice::recreateOversampling()
{
    // Direct build on the calling thread: the AT belt-and-braces fallback and
    // prepare()'s non-concurrent rebuild. The message-thread staging uses
    // buildOversamplingFor() directly, so both paths are
    // configuration-identical (same exponent / filter / latency / block size).
    filterOS_ = buildOversamplingFor (osFactor_);
    prepareFilterAtOsRate();
}

void AmbikaVoice::startNote (int midiNoteNumber, float velocity,
                             juce::SynthesiserSound*, int /*currentPitchWheelPosition*/)
{
    ensureInitialized();

    // Capture the legato hint BEFORE it is consumed below: a legato retrigger
    // continues the sounding voice (firmware legato = no oscillator/envelope
    // restart), so it must NOT get the de-click ramp (that would punch a gap).
    // continuityNext_ extends the SAME audio-continuity treatment to a
    // retrigger of a MID-RELEASE voice (2026-08-22 mono fix): the voice is
    // still sounding, so its resampler FIFO and gain must continue too — only
    // the envelope re-attack (from the CURRENT value, firmware-faithful)
    // differs from the legato slide.
    const bool legato = legatoNext_;
    const bool continueAudio = legato || continuityNext_;

    // Drop any tail from a previous note so it does not bleed into this
    // attack — EXCEPT when continuing a sounding voice (legato overlap or a
    // mono retrigger of a release tail): those paths deliberately continue
    // the live audio. Clearing the resampler FIFO here would discard
    // ~0.4-1.3 ms of unconsumed internal samples and restart the Lagrange
    // interpolator cold — a time-skip discontinuity (click) at EVERY
    // retrigger. The fresh-note and hard-stop paths still clear (hard stop:
    // stopNote below).
    if (! continueAudio)
    {
        fifo_.clear();
        interp_.reset();
    }
    isReleasing_ = false;

    // De-click: arm the one-shot startup gain ramp ONLY on a fresh trigger.
    // (A continuing voice is already sounding — a gain ramp of 1→0→…→1
    // would CREATE the click the ramp exists to prevent.)
    if (continueAudio) { startupGain_ = 1.0f; startupRampRemaining_ = 0; }
    else               { startupGain_ = 0.0f; startupRampRemaining_ = kDeClickRamp; }

    // Part volume is NOT set here: it is applied once via the APVTS
    // `part_volume` parameter through SynthEngine::applyPartByte ->
    // AmbikaVoice::setPartByte -> voice_.set_part_data (matches firmware,
    // which pushes part volume in Part::Touch, never per note-on).

    // Pitch is 14-bit (7 bits note : 7 bits fine, units of 1/128 semitone).
    // Apply the controller-side part octave/tuning + the per-class tuning
    // table (firmware Part::TuneNote, part.cc:634): n = clamp(midi + octave*12,
    // 0, 127); note14 = n*128 + tuneOffsets_[note&11] + tuning. The table is
    // indexed by the RAW incoming note — firmware TuneNote does the SAME
    // (part.cc:640: `Lookup(..., midi_note % 12)` on the unclamped parameter,
    // while only the base pitch uses the clamped n); %12 is octave-shift-
    // invariant, so the two only differ when the 0..127 clamp BITES, and even
    // then Parvati stays byte-faithful to the firmware. Muted classes never
    // reach here (AcceptNote gate in SynthEngine::noteOn / triggerNoteInPart
    // refuses them upstream).
    const int baseNote = juce::jlimit (0, 127, midiNoteNumber + partOctave_ * 12);
    const int note14  = juce::jlimit (0, static_cast<int> (ambika::dsp::kHighestNote),
                                     baseNote * 128 + tuneOffsets_[midiNoteNumber % 12]
                                                   + partTuning_ + spreadDrift14_);
    lastNote14_.store (note14, std::memory_order_relaxed);   // test-only readout
    const int velInt = juce::jlimit (0, 255, static_cast<int> (velocity * 255.0f));
    voice_.Trigger (static_cast<uint16_t> (note14), static_cast<uint8_t> (velInt & 0xFF), legato ? 1 : 0);
    legatoNext_ = false;   // one-shot hint, consumed
    continuityNext_ = false;   // one-shot hint, consumed
    spreadDrift14_ = 0;    // one-shot hint, consumed (default trigger => no drift)

    // SF-1: stage the active note for the lock-free message-thread snapshot
    // (mirrors Synthesiser setting currentlyPlayingNote just before startNote).
    displayedNote_.store (midiNoteNumber, std::memory_order_relaxed);
    displayedActive_.store (true, std::memory_order_relaxed);
}


void AmbikaVoice::stopNote (float /*velocity*/, bool allowTailOff)
{
    if (allowTailOff)
    {
        voice_.Release();
        isReleasing_ = true;
    }
    else
    {
        voice_.Kill();
        isReleasing_ = false;
        clearCurrentNote();
        // Hard stop: the next voice starts from silence — snap the VCA glide
        // to 0 so it does not ramp from a stale loud gain into the new note.
        vcaGlideGain_ = 0.0f;
        // SF-1: voice is fully freed — clear the staged snapshot (mirrors the
        // base currentlyPlayingNote = -1 set by clearCurrentNote).
        displayedNote_.store (-1, std::memory_order_relaxed);
        displayedActive_.store (false, std::memory_order_relaxed);
        fifo_.clear();
        interp_.reset();
    }
}

void AmbikaVoice::applyMpeToVoice()
{
    // DIRECT oscillator pitch bend (always audible, independent of the
    // mod-matrix routing). The 14-bit note pitch is in 1/128-semitone units,
    // so convert the semitone offset with the same scale.
    voice_.set_pitch_bend_offset (static_cast<int16_t> (std::lround (mpePitchBendSemitones_ * 128.0f)));

    // Mod-matrix feed. MOD_SRC_PITCH_BEND is AC-coupled (128 = neutral): map
    // the configured per-voice bend range -range..+range (semitones) onto
    // 1..255 about the 128 centre so full deflection reaches +/-127.
    const float range = mpeBendRangeSemitones_ > 0.0f ? mpeBendRangeSemitones_ : 1.0f;
    const float norm  = juce::jlimit (-1.0f, 1.0f, mpePitchBendSemitones_ / range);
    const int bendSrc = juce::jlimit (0, 255, juce::roundToInt (128.0f + norm * 127.0f));
    voice_.set_modulation_source (ambika::dsp::MOD_SRC_PITCH_BEND, static_cast<uint8_t> (bendSrc));

    // DC-coupled sources (0..255): channel pressure -> MOD_SRC_AFTERTOUCH,
    // CC74 (MPE "slide") -> MOD_SRC_EXPRESSION.
    voice_.set_modulation_source (ambika::dsp::MOD_SRC_AFTERTOUCH,
        static_cast<uint8_t> (juce::jlimit (0, 255, juce::roundToInt (mpePressure_ * 255.0f))));
    voice_.set_modulation_source (ambika::dsp::MOD_SRC_EXPRESSION,
        static_cast<uint8_t> (juce::jlimit (0, 255, juce::roundToInt (mpeSlide_ * 255.0f))));
}

void AmbikaVoice::fillInternalBlock()
{
    // Service a staged oversampling-factor change ON THE AUDIO THREAD (audit
    // F3). The message-thread setter PRE-BUILT the replacement Oversampling
    // object, so the fast path is POINTER MOVES ONLY: install the staged object,
    // park the displaced one for the reaper, re-prepare the filter at the new
    // rate (float state only — the same work the old rebuild did minus the
    // allocation). AnalogFilter::prepare resets the filter state, so the
    // switch is click-free (just a rare quality toggle).
    // exchange (acq_rel) check-and-clear: a plain load()+store(false) would
    // drop a change staged by the message thread between the two ops (a lost
    // update on a rapid double-toggle). exchange makes the clear atomic with
    // the check, matching the frame/options/config dirty-flag pattern.
    if (! consumeStagedOversampling()
        && osFactorDirty_.exchange (false, std::memory_order_acq_rel))
    {
        // Extra fallback (legacy staged-flag path, no pre-built
        // object): no current caller reaches this (the message-thread setter
        // always stages an object before publishing the flag), but a future
        // caller that only sets the flag stays CORRECT — the old AT-side
        // rebuild (allocation on the AT, identical configuration via
        // buildOversamplingFor).
        if (osFactor_ != pendingOsFactor_.load (std::memory_order_relaxed))
        {
            osFactor_ = pendingOsFactor_.load (std::memory_order_relaxed);
            recreateOversampling();
        }
    }

    // Service a pending filter-card topology change ON THE AUDIO THREAD (mirrors
    // the osFactor_ staging above). filter_.setTopology + prepareFilterAtOsRate
    // mutate float filter state; applying them here keeps them off the concurrent
    // processSample reader. prepareFilterAtOsRate() -> filter_.prepare() resets
    // the filter state, so the switch is click-free.
    if (topologyDirty_.exchange (false, std::memory_order_acq_rel))
    {
        filter_.setTopology (pendingTopology_.load (std::memory_order_relaxed));
        prepareFilterAtOsRate();
        filter_.commit();
    }

    voice_.ProcessBlock();   // renders exactly kAudioBlockSize (40) uint8 samples

    // ---- FX mod matrix @ 980 Hz: capture this voice's mod sources into the ----
    // ---- per-internal-block ring (PURE OBSERVATION — no audio mutation).  ----
    // SynthEngine::renderPartFx reads the first-active voice's ring at the
    // internal-block cadence so the FX params modulate at ~980 Hz, not host-
    // block rate. Byte-identical to the audio path without this block.
    if (modRingCount_ < kModRingCap)
    {
        auto& entry = modRing_[(size_t) modRingCount_];
        for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
            entry[(size_t) src] = voice_.modulation_source (static_cast<uint8_t> (src));
        ++modRingCount_;
    }

    // Sized-array pointer: either the voice's rendered block or the local
    // crush-held copy — the kAudioBlockSize contract survives the shadow
    // reassignment (a mis-sized buffer cannot enter this path).
    const std::array<uint8_t, ambika::dsp::kAudioBlockSize>* out = &voice_.output();

    // Crush (sample-and-hold decimator): the firmware voicecard DAC updates its
    // output only every voice_.crush() internal samples (voicecard.cc:74). Apply
    // the same zero-order-hold here, at the 8-bit internal rate, BEFORE the
    // filter+VCA (the hardware crush is pre-analog-VCF). crush()==1 => no hold.
    const uint8_t crush = voice_.crush();
    // Declared at FUNCTION scope (not inside the `if` below) so the `out = crushed`
    // shadow stays valid for every downstream read (1x default + smoothing paths,
    // and the oversampled raw fill). Previously block-scoped, `out` dangled after
    // the `if` closed -> stack-use-after-scope (ASAN abort at the read below).
    std::array<uint8_t, ambika::dsp::kAudioBlockSize> crushed {};
    if (crush > 1)
    {
        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
        {
            if (++crushSampleCounter_ >= static_cast<int> (crush))
            {
                crushSampleCounter_ = 0;
                crushHeldSample_ = (*out)[i];
            }
            crushed[i] = crushHeldSample_;
        }
        out = &crushed;   // shadow: all downstream paths read the held samples
    }

    // ---- 1x path (default): bit-identical to the un-oversampled engine ----
    if (osFactor_ == 1)
    {
    if (! smoothingEnabled_)
    {
        smoothingActive_ = false;

        // --- Default path: control-rate CV applied ONCE per 40-sample block ---
        // (matches the hardware's CV-update cadence). Bit-identical to the
        // un-smoothed engine.
        filter_.setCutoffHz (ambika::dsp::AnalogFilter::cutoffByteToHz (voice_.cutoff()));
        filter_.setResonance (static_cast<float> (voice_.resonance()) / 255.0f);
        applyFilterModeFromVoice();
        filter_.commit();

        // VCA: the firmware VCA is analog, post-DSP; apply its gain here (the
        // response-curve notes live in vcaGainTargetFromVoice).
        const float vcaGainTarget = vcaGainTargetFromVoice();

        // VCA glide (2026-08-22 release-noise fix): linearly ramp the gain
        // from the LAST APPLIED gain to this block's target across the 40
        // internal samples. The analog VCA smooths the block-rate CV steps in
        // hardware; a per-block ZOH staircase AM-modulates the signal at the
        // ~980 Hz control rate — audible as aliasing-like noise while the
        // envelope moves (worst in the release tail). Static CV → zero diff →
        // bit-identical to the staircase.
        const float g0 = vcaGlideGain_;
        const float gStep = (vcaGainTarget - g0) / static_cast<float> (ambika::dsp::kAudioBlockSize);

        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
        {
            // 8-bit (centred 128) -> float. Keep the (128 vs 127) asymmetry exact.
            float s = static_cast<float> (static_cast<int> ((*out)[i]) - 128) / 128.0f;
            s = filter_.processSample (s);
            s *= g0 + gStep * static_cast<float> (i);
            fifo_.push_back (s);
        }
        vcaGlideGain_ = vcaGainTarget;
        return;
    }

    // --- Smoothing path: ramp cutoff / resonance / VCA gain per sample (20 ms
    // linear). The voice's cutoff/resonance/vca fold in keytracking + env + lfo;
    // we smooth the three dominant zipper sources (AnalogFilter exposes cheap
    // per-sample setCutoffHz / setResonance setters, so all three are smoothed
    // — resonance shares the same commit() as cutoff at no extra cost).
    const float targetCutoff = ambika::dsp::AnalogFilter::cutoffByteToHz (voice_.cutoff());
    const float targetReso   = static_cast<float> (voice_.resonance()) / 255.0f;
    const float targetGain   = vcaGainTargetFromVoice();

    applyFilterModeFromVoice();

    // Snap to the current targets on the first block after enabling, so the
    // smoother never ramps up from a stale/zero value (re-enable is click-free).
    if (! smoothingActive_)
    {
        smoothedCutoffHz_.setCurrentAndTargetValue (targetCutoff);
        smoothedResonance_.setCurrentAndTargetValue (targetReso);
        smoothedVcaGain_.setCurrentAndTargetValue (targetGain);
        smoothingActive_ = true;
    }
    else
    {
        smoothedCutoffHz_.setTargetValue (targetCutoff);
        smoothedResonance_.setTargetValue (targetReso);
        smoothedVcaGain_.setTargetValue (targetGain);
    }

    for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
    {
        // 8-bit (centred 128) -> float. Keep the (128 vs 127) asymmetry exact.
        float s = static_cast<float> (static_cast<int> ((*out)[i]) - 128) / 128.0f;
        filter_.setCutoffHz (smoothedCutoffHz_.getNextValue());
        filter_.setResonance (smoothedResonance_.getNextValue());
        filter_.commit();
        s = filter_.processSample (s);
        s *= smoothedVcaGain_.getNextValue();
        fifo_.push_back (s);
    }
    }   // end 1x path
    else
    {
        // ---- Oversampling path (osFactor_ > 1) ----
        // The oscillator stays at the FIXED internal rate (39216 Hz); only the
        // analog filter MODEL runs at osFactor_*that rate. See
        // fillOversampledBlock (pure code motion from here).
        fillOversampledBlock (*out);
    }
}

void AmbikaVoice::applyFilterModeFromVoice()
{
    // 4-pole cards are lowpass-only (hardware); the SVF honours
    // LP/BP/HP/NOTCH.
    const auto topo = filter_.getTopology();
    const bool fourPole = (topo == ambika::dsp::FilterTopology::FOUR_POLE_LADDER
                        || topo == ambika::dsp::FilterTopology::FOUR_POLE_SSM2164);
    filter_.setMode (fourPole ? 0 : static_cast<int> (voice_.mode()));
}

float AmbikaVoice::vcaGainTargetFromVoice() const
{
    // The firmware VCA is analog, post-DSP. Two response curves (mirrors the
    // firmware log/lin jumper):
    //  - Linearized: gain = vca/255 (the lut_res_vca_linearization mode, where
    //    the table pre-warps the OTA so the output is linear).
    //  - Exponential: ~60 dB OTA taper (the raw VCA response, linear-CV mode).
    const float n = static_cast<float> (voice_.vca()) / 255.0f;
    return vcaExponential_ ? exponentialVcaGain (n) : n;
}

void AmbikaVoice::fillOversampledBlock (const std::array<uint8_t, ambika::dsp::kAudioBlockSize>& out)
{
    // Flow: render 40 raw floats -> upsample N× -> filter at N×39216 ->
    // downsample to 40 -> VCA (internal rate, linear so no aliasing) -> FIFO.
    float raw[ambika::dsp::kAudioBlockSize];
    for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
        raw[i] = static_cast<float> (static_cast<int> (out[i]) - 128) / 128.0f;

    // Upsample 40 -> 40*N into the Oversampling internal buffer.
    const float* inChannels[1] = { raw };
    juce::dsp::AudioBlock<const float> inBlock (inChannels, 1u,
        static_cast<size_t> (ambika::dsp::kAudioBlockSize));
    auto osBlock = filterOS_->processSamplesUp (inBlock);

    applyFilterModeFromVoice();

    float* d = osBlock.getChannelPointer (0);
    const size_t osN = osBlock.getNumSamples();   // == 40 * osFactor_

    if (! smoothingEnabled_)
    {
        smoothingActive_ = false;
        // Control-rate CV applied once per block (matches the 1x path).
        filter_.setCutoffHz (ambika::dsp::AnalogFilter::cutoffByteToHz (voice_.cutoff()));
        filter_.setResonance (static_cast<float> (voice_.resonance()) / 255.0f);
        filter_.commit();
        for (size_t i = 0; i < osN; ++i)
            d[i] = filter_.processSample (d[i]);
    }
    else
    {
        // Smooth cutoff / resonance / VCA per INTERNAL sample (20 ms). The OS
        // buffer holds osFactor_ sub-samples per internal sample, so the
        // smoothers advance at each internal-sample boundary -> 40 steps per
        // block, matching the 1x path (keeps smoother state consistent when
        // OS is toggled mid-note).
        const float targetCutoff = ambika::dsp::AnalogFilter::cutoffByteToHz (voice_.cutoff());
        const float targetReso   = static_cast<float> (voice_.resonance()) / 255.0f;
        const float targetGain   = vcaGainTargetFromVoice();

        if (! smoothingActive_)
        {
            smoothedCutoffHz_.setCurrentAndTargetValue (targetCutoff);
            smoothedResonance_.setCurrentAndTargetValue (targetReso);
            smoothedVcaGain_.setCurrentAndTargetValue (targetGain);
            smoothingActive_ = true;
        }
        else
        {
            smoothedCutoffHz_.setTargetValue (targetCutoff);
            smoothedResonance_.setTargetValue (targetReso);
            smoothedVcaGain_.setTargetValue (targetGain);
        }

        const size_t step = static_cast<size_t> (osFactor_);
        for (size_t i = 0; i < osN; ++i)
        {
            if ((i % step) == 0u)
            {
                filter_.setCutoffHz (smoothedCutoffHz_.getNextValue());
                filter_.setResonance (smoothedResonance_.getNextValue());
                filter_.commit();
            }
            d[i] = filter_.processSample (d[i]);
        }
    }

    // Downsample 40*N -> 40 (back into raw).
    float* outChannels[1] = { raw };
    juce::dsp::AudioBlock<float> downBlock (outChannels, 1u,
        static_cast<size_t> (ambika::dsp::kAudioBlockSize));
    filterOS_->processSamplesDown (downBlock);

    // VCA at the internal rate (linear gain -> no aliasing) + push to FIFO.
    // Same VCA glide as the 1x default path: ramp from the last applied
    // gain to the target across the block (see the note there).
    if (! smoothingEnabled_)
    {
        const float vcaGainTarget = vcaGainTargetFromVoice();
        const float g0 = vcaGlideGain_;
        const float gStep = (vcaGainTarget - g0) / static_cast<float> (ambika::dsp::kAudioBlockSize);
        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
            fifo_.push_back (raw[i] * (g0 + gStep * static_cast<float> (i)));
        vcaGlideGain_ = vcaGainTarget;
    }
    else
    {
        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
            fifo_.push_back (raw[i] * smoothedVcaGain_.getNextValue());
    }
}

void AmbikaVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                                   int startSample, int numSamples)
{
    if (hostRate_ <= 0.0 || numSamples <= 0)
        return;

    // Idle-voice self-gate. JUCE's Synthesiser calls renderNextBlock for EVERY
    // voice every block; an idle voice (no current note, not in a release tail)
    // must emit silence. Without this, an idle voice runs the full DSP. The
    // multiplicative ENV->VCA modulation only fully closes the VCA when the
    // amount is exactly 63, so any patch with amount < 63 leaves the VCA
    // open (~126) and the oscillator renders at pitch_value_ == 0 (sub-audio)
    // => a startup low-frequency rumble. isVoiceActive() is true while a voice
    // holds a note, INCLUDING during release tail-off until this same function
    // calls clearCurrentNote(), so release tails are unaffected. This is the
    // faithful equivalent of the firmware idling an inactive voicecard.
    // (Voice::Init() primes the envelope increments so a gated idle voice is
    // trigger-ready — see voice.cpp.)
    if (! isVoiceActive())
        return;

    int written = 0;
    while (written < numSamples)
    {
        const int chunk = juce::jmin (kMaxChunk, numSamples - written);

        // Refill the internal-rate FIFO until there is enough input (samples
        // consumed by the resampler for `chunk` outputs) + lookahead margin.
        const int inputNeeded = static_cast<int> (std::ceil (chunk * speedRatio_)) + kLookahead;
        while (static_cast<int> (fifo_.size()) < inputNeeded)
            fillInternalBlock();

        float tmpOut[kMaxChunk];
        // Safe overload: produces exactly `chunk` outputs, does not over-read
        // the FIFO, returns the number of internal samples consumed.
        const int consumed = interp_.process (speedRatio_,
                                              fifo_.data(),
                                              tmpOut,
                                              chunk,
                                              static_cast<int> (fifo_.size()),
                                              0 /*no wrap*/);

        // De-click: scale this chunk by the one-shot startup gain ramp (active
        // only for ~1 ms after a fresh, non-legato note start). Legato and
        // sustained notes are unchanged (gain == 1.0). State persists across
        // chunks, so a ramp may straddle the kMaxChunk boundary.
        if (startupRampRemaining_ > 0)
        {
            for (int i = 0; i < chunk; ++i)
            {
                tmpOut[i] *= startupGain_;
                if (--startupRampRemaining_ == 0) { startupGain_ = 1.0f; break; }
                startupGain_ += kDeClickInc;
            }
        }

        // Add the per-voice mono signal to channel 0 of the TARGET buffer. The
        // engine's renderVoices override passes this voice's FIXED voicecard
        // buffer (mono); the processor then sums all voicecard buffers into the
        // main stereo mix and copies each to its optional aux bus. Writing only
        // channel 0 keeps the per-voice signal a single mono contribution (the
        // L/R duplication now happens once, in the processor's main-bus mix).
        outputBuffer.addFrom (0, startSample + written, tmpOut, chunk);

        written += chunk;

        const int toRemove = juce::jmin (consumed, static_cast<int> (fifo_.size()));
        fifo_.erase (fifo_.begin(), fifo_.begin() + toRemove);
    }

    // Release tail-off: free the voice once the engine's VCA has converged
    // (envelope DEAD => essentially silent) OR once ALL three envelopes have
    // reached DEAD (Voice::envelopesDead). The second condition is robust to a
    // patch with no ENV->VCA modulation routing. In such a patch the
    // multiplicative VCA never collapses below 2 and the voice would
    // otherwise linger active (stuck meter / reduced polyphony) until JUCE
    // steals it. (The few ms of FIFO tail below the noise floor are
    // inaudible.)
    if (isReleasing_ && (voice_.vca() < 2 || voice_.envelopesDead()))
    {
        clearCurrentNote();
        // SF-1: release tail finished — clear the staged snapshot.
        displayedNote_.store (-1, std::memory_order_relaxed);
        displayedActive_.store (false, std::memory_order_relaxed);
        isReleasing_ = false;
        fifo_.clear();
        interp_.reset();
    }
}
