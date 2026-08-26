// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.  See LiveFeedbackHub.h.

#include "LiveFeedbackHub.h"

namespace hellcat
{

LiveFeedbackHub::LiveFeedbackHub (std::function<bool (ModTelemetrySnapshot&)> fetch)
    : fetch_ (std::move (fetch))
{
    jassert (fetch_ != nullptr);   // the editor always binds readUiTelemetry
    // Runs UNCONDITIONALLY at the configured rate (2026-08-21 reliability
    // fix): earlier versions gated the timer on the editor's isShowing(),
    // which is peer-derived and unreliable as a start signal (a starved hook left
    // the pump stopped forever -> every strip cleared -> the shipped
    // invisible-indicator bug). The per-tick cost while nothing consumes the
    // cache is ONE bounded seqlock read (~µs, no repaints — the consumers
    // gate themselves), which is the correct price for a pump that must
    // simply work whenever the editor might be on screen.
    startTimerHz (rateHz_);
}

LiveFeedbackHub::~LiveFeedbackHub()
{
    stopTimer();
}

void LiveFeedbackHub::setRateHz (int hz)
{
    // <= 0 stops (see header); otherwise clamp to the 5..60 window.
    const int clamped = juce::jlimit (5, 60, hz);
    if (hz <= 0)
    {
        stopTimer();
        rateHz_ = 0;
        return;
    }
    if (clamped == rateHz_)
        return;
    rateHz_ = clamped;
    // Restart the timer at the new cadence (a no-op repaint-wise: the next tick
    // simply re-reads the engine frame).
    startTimerHz (rateHz_);
}

void LiveFeedbackHub::setRunning (bool running)
{
    if (running)
    {
        if (! isTimerRunning() && rateHz_ > 0)
            startTimerHz (rateHz_);
    }
    else
    {
        stopTimer();
    }
}

bool LiveFeedbackHub::snapshot (ModTelemetrySnapshot& out) const
{
    out = cached_;
    return valid_;
}

LiveEnvStage LiveFeedbackHub::envStage (int envIndex) const
{
    LiveEnvStage s;
    if (! valid_ || envIndex < 0 || envIndex > 2)
        return s;
    s.active   = cached_.voiceActive;
    s.stage    = static_cast<int> (cached_.envStage[(size_t) envIndex]);
    s.progress = cached_.envProgress[(size_t) envIndex];
    return s;
}

LiveFilterValues LiveFeedbackHub::liveFilter() const
{
    LiveFilterValues f;
    if (! valid_ || ! cached_.voiceActive)
        return f;
    f.active   = true;
    f.cutoff01 = static_cast<float> (cached_.effCutoff)    / 255.0f;
    f.reso01   = static_cast<float> (cached_.effResonance) / 255.0f;
    return f;
}

LiveOscValues LiveFeedbackHub::liveOsc (int oscIndex) const
{
    LiveOscValues v;
    if (! valid_ || ! cached_.voiceActive)
        return v;
    v.active  = true;
    // The effective OSC byte domain is 0..127 (dst_[MOD_DST_PARAMETER_*] >> 7),
    // NOT the filter's 0..255 — normalize over 127 so the display's byte
    // quantization round-trips exactly.
    v.param01 = static_cast<float> (cached_.effOscParam[static_cast<size_t> (
                    juce::jlimit (0, 1, oscIndex))]) / 127.0f;
    return v;
}

void LiveFeedbackHub::timerCallback()
{
    if (! fetch_)
        return;
    // One bounded seqlock read per tick. A torn/stale frame keeps the previous
    // cache but flags it invalid (consumers hide their live overlays for one
    // tick — exactly the desired behaviour across a patch reset).
    ModTelemetrySnapshot frame;
    valid_ = fetch_ (frame);
    if (valid_)
        cached_ = frame;
}

}  // namespace hellcat
