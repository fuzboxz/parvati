// Copyright (c) 2026 Jozsef OttICSAK / Parvati.
//
// LiveFeedbackHub — the editor-owned pump of the LIVE modulation feedback
// system (docs/LIVE_MOD_FEEDBACK_DESIGN.md). One juce::Timer (message thread)
// reads ONE consistent engine telemetry frame per tick (via the fetcher the
// editor binds to SynthEngine::readUiTelemetry) and caches it; the UI
// components (CentralModBar pills, EnvelopeDisplay stage markers,
// FilterResponseDisplay live curve) then read the CACHE through their own
// poll timers via std::function providers — so the engine's seqlock is read
// exactly once per tick no matter how many components consume it.
//
// Decoupled contract: the hub knows NOTHING about SynthEngine (it takes a
// plain fetcher returning bool-valid), and the components know nothing about
// the hub (they take LiveEnvStage / LiveFilterValues providers from
// ModTelemetryTypes.h). The editor glues the two halves.
//
// Thread discipline: juce::Timer callbacks + all methods run on the message
// thread, so the cached snapshot needs no synchronization of its own.

#pragma once

#include <functional>

#include <juce_events/juce_events.h>

#include "ModTelemetryTypes.h"

namespace parvati
{

class LiveFeedbackHub : private juce::Timer
{
public:
    /** @param fetch fills the snapshot from the engine; returns false when the
                   engine frame is torn or its epoch is stale (the hub then
                   keeps the previous cache but flags it invalid). */
    explicit LiveFeedbackHub (std::function<bool (ModTelemetrySnapshot&)> fetch);
    ~LiveFeedbackHub() override;

    /** Animation cadence for the whole live-feedback system (Hz). Clamped to
        5..60; the persisted user setting (ui_refresh_hz, default 30) drives
        this. Takes effect immediately. */
    void setRateHz (int hz);
    int  rateHz() const noexcept { return rateHz_; }

    /** The cached frame (message thread only). @p out receives the last good
        frame; the RETURN value is its validity (false = stale after a reset or
        a torn read — consumers should hide their live overlays). */
    bool snapshot (ModTelemetrySnapshot& out) const;

    /** Envelope 1..3 live stage of the cached frame (index clamped; an
        inactive/stale cache returns an inactive LiveEnvStage). */
    LiveEnvStage envStage (int envIndex) const;

    /** Live effective filter values of the cached frame (inactive when the
        cache is stale or no voice is active). */
    LiveFilterValues liveFilter() const;

    /** Cached frame's validity (false after an engine reset until the next
        good tick — the pill histories hide during that window). */
    bool valid() const noexcept { return valid_; }

    /** Force-invalidate the cache (e.g. when the editor learns of a patch /
        part change before the next tick observes the epoch bump). */
    void invalidate() noexcept { valid_ = false; }

private:
    void timerCallback() override;

    std::function<bool (ModTelemetrySnapshot&)> fetch_;
    ModTelemetrySnapshot cached_ {};
    bool valid_ = false;
    int  rateHz_ = 30;
};

}  // namespace parvati
