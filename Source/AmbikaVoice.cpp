// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See AmbikaVoice.h.

#include "AmbikaVoice.h"

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
    // direct assignment of osFactor_ here is race-free.
    osFactor_ = pendingOsFactor_.load (std::memory_order_relaxed);
    osFactorDirty_.store (false, std::memory_order_relaxed);

    // The analog filter operates on the internal-rate (or oversampled) signal
    // (matches hardware: the filter is post-DSP, pre-DAW). recreateOversampling()
    // prepares the filter at osFactor_*kInternalSampleRate when OS is on (else at
    // the engine rate) and builds/destroys the per-voice Oversampling object.
    filter_.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_LADDER);
    filter_.setMode (0);   // LP
    recreateOversampling();
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
    // Clamp to the supported factors (1 / 2 / 4).
    if (factor != 1 && factor != 2 && factor != 4)
        factor = (factor <= 1) ? 1 : (factor <= 2 ? 2 : 4);

    // Stage the change for the audio thread. Recreating the per-voice
    // Oversampling object here (message thread) would race fillInternalBlock()
    // (audio thread), so we defer: osFactorDirty_ is serviced at the top of the
    // next fillInternalBlock(), which rebuilds the filter + Oversampling on the
    // owning thread. A silent voice rebuilds on its next note (no stale audio).
    pendingOsFactor_.store (factor, std::memory_order_relaxed);
    osFactorDirty_.store (true, std::memory_order_release);
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
    if (osFactor_ > 1)
    {
        // juce::dsp::Oversampling takes a power-of-2 EXPONENT: 2^factorExp.
        // 2x -> 1, 4x -> 2. Min-phase IIR half-band = minimal latency; max
        // quality steepens the transition band. Integer latency is enabled so
        // getLatencyInSamples() is a whole number of INPUT samples (clean for
        // host PDC reporting).
        const size_t factorExp = (osFactor_ >= 4) ? 2 : 1;
        filterOS_ = std::make_unique<juce::dsp::Oversampling<float>> (
            1u,
            factorExp,
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandPolyphaseIIR,
            true,   // isMaxQuality
            true);  // useIntegerLatency
        // The internal block rendered to the filter is always kAudioBlockSize
        // (40) — processSamplesUp turns that into 40*osFactor_.
        filterOS_->initProcessing (static_cast<size_t> (ambika::dsp::kAudioBlockSize));
        filterOS_->reset();
    }
    else
    {
        filterOS_.reset();
    }

    prepareFilterAtOsRate();
}

void AmbikaVoice::startNote (int midiNoteNumber, float velocity,
                             juce::SynthesiserSound*, int /*currentPitchWheelPosition*/)
{
    ensureInitialized();

    // Capture the legato hint BEFORE it is consumed below: a legato retrigger
    // continues the sounding voice (firmware legato = no oscillator/envelope
    // restart), so it must NOT get the de-click ramp (that would punch a gap).
    const bool legato = legatoNext_;

    // Drop any tail from a previous note so it doesn't bleed into this attack.
    fifo_.clear();
    interp_.reset();
    isReleasing_ = false;

    // De-click: arm the one-shot startup gain ramp ONLY on a fresh trigger.
    if (legato) { startupGain_ = 1.0f; startupRampRemaining_ = 0; }
    else        { startupGain_ = 0.0f; startupRampRemaining_ = kDeClickRamp; }

    // Part volume is NOT set here: it is applied once via the APVTS
    // `part_volume` parameter through SynthEngine::applyPartByte ->
    // AmbikaVoice::setPartByte -> voice_.set_part_data (matches firmware,
    // which pushes part volume in Part::Touch, never per note-on).

    // Pitch is 14-bit (7 bits note : 7 bits fine, units of 1/128 semitone).
    // Apply the controller-side part octave/tuning (firmware Part::TuneNote,
    // part.cc:634): n = clamp(midi + octave*12, 0, 127); note14 = n*128 + tuning.
    const int baseNote = juce::jlimit (0, 127, midiNoteNumber + partOctave_ * 12);
    const int note14  = juce::jlimit (0, static_cast<int> (ambika::dsp::kHighestNote),
                                     baseNote * 128 + partTuning_ + spreadDrift14_);
    const int velInt = juce::jlimit (0, 255, static_cast<int> (velocity * 255.0f));
    voice_.Trigger (static_cast<uint16_t> (note14), static_cast<uint8_t> (velInt & 0xFF), legato ? 1 : 0);
    legatoNext_ = false;   // one-shot hint, consumed
    spreadDrift14_ = 0;    // one-shot hint, consumed (default trigger => no drift)

    // SF-1: stage the active note for the lock-free message-thread snapshot
    // (mirrors Synthesiser setting currentlyPlayingNote just before startNote).
    displayedNote_.store (midiNoteNumber, std::memory_order_relaxed);
    displayedActive_.store (true, std::memory_order_relaxed);
}

void AmbikaVoice::retriggerNote (juce::SynthesiserSound* sound, int midiNoteNumber, float velocity)
{
    // Already-active voice: just update the dsp pitch via startNote -> Trigger.
    // No kill (unlike juce::Synthesiser::startVoice's stopNote(0,false) -> Kill),
    // so the envelope keeps sustaining and the firmware Voice::Trigger(legato)
    // (which skips the re-attack) slides the pitch instead of going silent.
    setKeyDown (true);
    startNote (midiNoteNumber, velocity, sound, /*currentPitchWheelPosition*/ 0);
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
    // Service a pending oversampling-factor change ON THE AUDIO THREAD. The
    // message-thread setter (setOversamplingFactor) only stages pending + flag;
    // the actual rebuild (filter re-prepare + Oversampling recreate) happens
    // here so the per-voice Oversampling object is never freed under a
    // concurrent processSamplesUp/Down. AnalogFilter::prepare resets the filter
    // state, so the switch is click-free (just a rare quality toggle).
    // exchange (acq_rel) check-and-clear: a plain load()+store(false) would
    // drop a change staged by the message thread between the two ops (a lost
    // update on a rapid double-toggle). exchange makes the clear atomic with the
    // check, matching the frame/options/config dirty-flag pattern.
    if (osFactorDirty_.exchange (false, std::memory_order_acq_rel))
    {
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

    const uint8_t* out = voice_.output();

    // Crush (sample-and-hold decimator): the firmware voicecard DAC updates its
    // output only every voice_.crush() internal samples (voicecard.cc:74). Apply
    // the same zero-order-hold here, at the 8-bit internal rate, BEFORE the
    // filter+VCA (the hardware crush is pre-analog-VCF). crush()==1 => no hold.
    const uint8_t crush = voice_.crush();
    // Declared at FUNCTION scope (not inside the `if` below) so the `out = crushed`
    // shadow stays valid for every downstream read (1x default + smoothing paths,
    // and the oversampled raw fill). Previously block-scoped, `out` dangled after
    // the `if` closed -> stack-use-after-scope (ASAN abort at the read below).
    uint8_t crushed[ambika::dsp::kAudioBlockSize];
    if (crush > 1)
    {
        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
        {
            if (++crushSampleCounter_ >= static_cast<int> (crush))
            {
                crushSampleCounter_ = 0;
                crushHeldSample_ = out[i];
            }
            crushed[i] = crushHeldSample_;
        }
        out = crushed;   // shadow: all downstream paths read the held samples
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
        // 4-pole cards are lowpass-only (hardware); the SVF honours LP/BP/HP/NOTCH.
        {
            const auto topo = filter_.getTopology();
            const bool fourPole = (topo == ambika::dsp::FilterTopology::FOUR_POLE_LADDER
                                || topo == ambika::dsp::FilterTopology::FOUR_POLE_SSM2164);
            filter_.setMode (fourPole ? 0 : static_cast<int> (voice_.mode()));
        }
        filter_.commit();

        // VCA: the firmware VCA is analog, post-DSP; apply its gain here. Two
        // response curves (mirrors the firmware log/lin jumper):
        //  - Linearized: gain = vca/255 (the lut_res_vca_linearization mode, where
        //    the table pre-warps the OTA so the output is linear).
        //  - Exponential: ~60 dB OTA taper (the raw VCA response, linear-CV mode).
        float vcaGain;
        if (vcaExponential_)
        {
            const float n = static_cast<float> (voice_.vca()) / 255.0f;
            vcaGain = exponentialVcaGain (n);  // 60 dB OTA taper, makeup-compensated
        }
        else
        {
            vcaGain = static_cast<float> (voice_.vca()) / 255.0f;
        }

        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
        {
            // 8-bit (centred 128) -> float. Keep the (128 vs 127) asymmetry exact.
            float s = static_cast<float> (static_cast<int> (out[i]) - 128) / 128.0f;
            s = filter_.processSample (s);
            s *= vcaGain;
            fifo_.push_back (s);
        }
        return;
    }

    // --- Smoothing path: ramp cutoff / resonance / VCA gain per sample (20 ms
    // linear). The voice's cutoff/resonance/vca fold in keytracking + env + lfo;
    // we smooth the three dominant zipper sources (AnalogFilter exposes cheap
    // per-sample setCutoffHz / setResonance setters, so all three are smoothed
    // — resonance rides the same commit() as cutoff at no extra cost).
    const float targetCutoff = ambika::dsp::AnalogFilter::cutoffByteToHz (voice_.cutoff());
    const float targetReso   = static_cast<float> (voice_.resonance()) / 255.0f;
    const float vcaNorm      = static_cast<float> (voice_.vca()) / 255.0f;
    const float targetGain   = vcaExponential_
        ? exponentialVcaGain (vcaNorm)   // ~60 dB OTA taper, makeup-compensated
        : vcaNorm;

    // 4-pole cards are lowpass-only (hardware); the SVF honours LP/BP/HP/NOTCH.
    {
        const auto topo = filter_.getTopology();
        const bool fourPole = (topo == ambika::dsp::FilterTopology::FOUR_POLE_LADDER
                            || topo == ambika::dsp::FilterTopology::FOUR_POLE_SSM2164);
        filter_.setMode (fourPole ? 0 : static_cast<int> (voice_.mode()));
    }

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
        float s = static_cast<float> (static_cast<int> (out[i]) - 128) / 128.0f;
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
        // analog filter MODEL runs at osFactor_*that rate to reduce its digital
        // aliasing for higher fidelity. Flow: render 40 raw floats -> upsample
        // N× -> filter at N×39216 -> downsample to 40 -> VCA (internal rate,
        // linear so no aliasing) -> FIFO.
        float raw[ambika::dsp::kAudioBlockSize];
        for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
            raw[i] = static_cast<float> (static_cast<int> (out[i]) - 128) / 128.0f;

        // Upsample 40 -> 40*N into the Oversampling internal buffer.
        const float* inChannels[1] = { raw };
        juce::dsp::AudioBlock<const float> inBlock (inChannels, 1u,
            static_cast<size_t> (ambika::dsp::kAudioBlockSize));
        auto osBlock = filterOS_->processSamplesUp (inBlock);

        // 4-pole cards are lowpass-only (hardware); the SVF honours LP/BP/HP/NOTCH.
        {
            const auto topo = filter_.getTopology();
            const bool fourPole = (topo == ambika::dsp::FilterTopology::FOUR_POLE_LADDER
                                || topo == ambika::dsp::FilterTopology::FOUR_POLE_SSM2164);
            filter_.setMode (fourPole ? 0 : static_cast<int> (voice_.mode()));
        }

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
            const float vcaNorm      = static_cast<float> (voice_.vca()) / 255.0f;
            const float targetGain   = vcaExponential_
                ? exponentialVcaGain (vcaNorm)
                : vcaNorm;

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
        if (! smoothingEnabled_)
        {
            const float vcaNorm = static_cast<float> (voice_.vca()) / 255.0f;
            const float vcaGain = vcaExponential_
                ? exponentialVcaGain (vcaNorm)
                : vcaNorm;
            for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
                fifo_.push_back (raw[i] * vcaGain);
        }
        else
        {
            for (int i = 0; i < ambika::dsp::kAudioBlockSize; ++i)
                fifo_.push_back (raw[i] * smoothedVcaGain_.getNextValue());
        }
    }
}

void AmbikaVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                                   int startSample, int numSamples)
{
    if (hostRate_ <= 0.0 || numSamples <= 0)
        return;

    // Idle-voice self-gate. JUCE's Synthesiser calls renderNextBlock for EVERY
    // voice every block; an idle voice (no current note, not in a release tail)
    // must emit silence. Without this, an idle voice runs the full DSP and,
    // because the multiplicative ENV->VCA modulation only fully closes the VCA
    // when the amount is exactly 63, any patch with amount < 63 leaves the VCA
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
        // Safe overload: produces exactly `chunk` outputs, won't over-read the
        // FIFO, returns the number of internal samples consumed.
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
    // patch with no ENV->VCA modulation routing, where the multiplicative VCA
    // never collapses below 2 and the voice would otherwise linger active
    // (stuck meter / reduced polyphony) until JUCE steals it. (The few ms of
    // FIFO tail below the noise floor are inaudible.)
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
