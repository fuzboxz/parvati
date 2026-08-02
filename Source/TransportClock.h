// Copyright (c) 2024 805LABS / Parvati.
//
// TransportClock — converts the host transport tempo (BPM from
// juce::AudioPlayHead) into a stream of 24-PPQN clock ticks, sample-accurate.
// The ticks drive the Arpeggiator (and, in a later task, tempo-synced LFOs).

#ifndef PARVATI_TRANSPORT_CLOCK_H_
#define PARVATI_TRANSPORT_CLOCK_H_

namespace parvati
{

class TransportClock
{
public:
    void prepare (double sampleRate)
    {
        sampleRate_ = sampleRate;
        recompute();
    }

    void setTempo (double bpm)
    {
        bpm_ = bpm;
        recompute();
    }

    // Advance by numSamples; return the number of whole 24-PPQN ticks elapsed.
    int advance (int numSamples)
    {
        fractionalTick_ += static_cast<double> (numSamples);
        int ticks = 0;
        while (fractionalTick_ >= samplesPerTick_)
        {
            fractionalTick_ -= samplesPerTick_;
            ++ticks;
        }
        return ticks;
    }

    void reset() { fractionalTick_ = 0.0; }

    double getBpm() const { return bpm_; }
    double getSamplesPerTick() const { return samplesPerTick_; }

private:
    void recompute()
    {
        // 24 PPQN: samplesPerTick = sampleRate * 60 / (bpm * 24)
        if (bpm_ > 0.0)
            samplesPerTick_ = sampleRate_ * 60.0 / (bpm_ * 24.0);
    }

    double sampleRate_     = 48000.0;
    double bpm_            = 120.0;
    double samplesPerTick_ = 1000.0;
    double fractionalTick_ = 0.0;
};

}  // namespace parvati

#endif  // PARVATI_TRANSPORT_CLOCK_H_
