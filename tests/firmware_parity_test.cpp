// Firmware-parity oracle (deterministic tooling, "tool 9"): drives the
// VENDORED Ambika controller (ambika_reference/controller/{multi,part,
// resources,voice_allocator}.cc + avrlib/random.cc, compiled on the host via
// the recording shims in tests/firmware_shim/) and Parvati's own engine
// through IDENTICAL scripted event sequences, then diffs per-event
// observables:
//
//   - which parts accept a note (per-channel/per-zone routing)
//   - the tuned 14-bit note a part triggers (12-EDO, spread 0)
//   - sustain-pedal (CC64) swallow / pedal-up drain decisions
//   - all-notes-off (CC123) bookkeeping clearing (direct + arp mode)
//   - which parts' voices receive CC1 modulation writes (per channel)
//   - the arpeggiator's generated-note position under clocked playback
//   - note-sequence step decode (velocity-0 handling)
//
// The firmware side runs SYNCHRONOUSLY (Multi::NoteOn etc. + explicit
// Multi::Clock() calls kept in lockstep with Parvati's elapsed ticks); the
// Parvati side runs through the REAL audio path (ParvatiAudioProcessor::
// processBlock at 48 kHz / 120 BPM, where 24-PPQN ticks are exactly 1000
// samples). Both sides are configured part-for-part through their public
// configuration APIs — never by poking private engine state.
//
// KNOWN DIVERGENCES: tests/firmware_parity_known_divergences.txt lists the
// documented, deliberate (or deferred) behavioural differences. A check
// tagged with a divergence id ASSERTS THE DIVERGENCE STILL EXISTS — if the
// engine ever converges with the firmware there, the harness fails with a
// stale-allowlist message (remove the entry and promote the check to an
// equality check). Any UNTAGGED check that fails is a NEW divergence ->
// hard failure. The allowlist file and the harness validate each other's
// completeness in both directions.
//
// Self-test (--self-test): inverts the expectation of one allowlisted check
// (treats the divergence as expected-EQUAL) and verifies the harness DOES
// catch it — proving the allowlist lock bites. Used as the tool's canary
// during authoring; also safe to run headless in CI.
//
// Build/run (from the repo root):
//   cmake --build build --target parvati_firmware_parity_test &&
//   ./build/parvati_firmware_parity_test

#include <algorithm>
#include "unified_test_runner.h"
#include <cstddef>   // offsetof (voicecard audio oracle patch-byte addressing)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// FIRMWARE SIDE (namespace ambika; the vendored controller compiled on the
// host through the tests/firmware_shim/ include path — see those headers).
// ---------------------------------------------------------------------------
#include "controller/multi.h"
#include "controller/midi_dispatcher.h"
#include "controller/voicecard_tx.h"
// Voicecard audio oracle facade (mix-gain-glide scenario): the REAL firmware
// Voice::ProcessBlock, driven through a narrow firmware-only TU — the
// voicecard headers themselves collide with both the controller headers and
// the port's DSP headers, so they never appear in this TU.
#include "firmware_shim/voicecard_oracle.h"

// ---------------------------------------------------------------------------
// PARVATI SIDE.
// ---------------------------------------------------------------------------
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/voice.h"    // bare dsp::Voice for the byte-level audio oracle
#include "dsp/random.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace
{

int g_failures = 0;
bool g_selfTest = false;   // --self-test: prove the allowlist lock bites

void check (bool cond, const std::string& msg)
{
    if (cond)
        std::printf ("  ok  : %s\n", msg.c_str());
    else
    {
        ++g_failures;
        std::printf ("  FAIL: %s\n", msg.c_str());
    }
}

//===========================================================================
// The known-divergence allowlist (tests/firmware_parity_known_divergences.txt
// is the source of truth; see that file for the entries and rationale).
std::set<std::string> g_divergences;

// Divergence-assert: passes IFF the two sides DIFFER (and the id is
// allowlisted). If they are EQUAL the divergence has been closed — the entry
// is stale and must be removed (fail loudly so no phantom entry survives).
void checkDiverges (const std::string& id, const std::string& fw,
                    const std::string& pv, const std::string& what)
{
    if (g_divergences.count (id) == 0)
    {
        check (false, "check tagged '" + id + "' has no allowlist entry (add it to "
                      "tests/firmware_parity_known_divergences.txt) [" + what + "]");
        return;
    }
    if (g_selfTest && id == "unicast-vs-multicast")
    {
        // Canary: pretend this divergence was fixed (expect EQUAL). The check
        // must FAIL — proving an equality expectation would have caught a
        // silent convergence.
        check (fw == pv, "[self-test canary] allowlist lock bites: '"
                         + id + "' as an EQUALITY would fail [fw=" + fw
                         + " pv=" + pv + "]");
        return;
    }
    check (fw != pv, "[" + id + "] divergence still documented: " + what
                    + " [fw=" + fw + " pv=" + pv + "]");
}

// Plain parity check: the two sides must agree.
void checkEquals (const std::string& fw, const std::string& pv, const std::string& what)
{
    check (fw == pv, what + " [fw=" + fw + " pv=" + pv + "]");
}

//===========================================================================
// Shared per-part scenario configuration, applied identically through each
// side's PUBLIC configuration API (field -1 = leave at that side's default).
struct PartCfg
{
    int channel   = -1;   // 0 = Omni, else 1..16
    int lo        = -1;
    int hi        = -1;
    int allocMask = -1;   // 6-voicecard bitmask; 0 = disabled part
    int poly      = -1;   // PartData byte 15 (0 MONO / 1 POLY)
    int arpMode   = -1;   // byte 7  (0 step / 1 arp / 2 note-seq)
    int arpDir    = -1;   // byte 8
    int arpOct    = -1;   // byte 9
    int arpPat    = -1;   // byte 10
    int arpDiv    = -1;   // byte 11 (ticks-per-step divider index)
};

constexpr int kRate   = 48000;
constexpr int kBlock  = 250;   // 4 blocks == 1000 samples == exactly 1 tick @ 120 BPM
constexpr int kTicksFromSamples = 1000;

//===========================================================================
// Firmware oracle.
class FwOracle
{
public:
    void reset()
    {
        ambika::Multi::Init (true);   // factory defaults (parts on ch 1..6)
        ambika::midi_dispatcher.clearLog();
        ambika::voicecard_tx.clearLog();
        fwClocks_ = 0;
    }

    void configure (const std::vector<PartCfg>& cfg)
    {
        for (int p = 0; p < (int) cfg.size(); ++p)
        {
            const PartCfg& c = cfg[(size_t) p];
            if (c.channel >= 0)
                ambika::Multi::mutable_data()->part_mapping_[(size_t) p].midi_channel = (uint8_t) c.channel;
            if (c.lo >= 0)
                ambika::Multi::mutable_data()->part_mapping_[(size_t) p].keyrange_low = (uint8_t) c.lo;
            if (c.hi >= 0)
                ambika::Multi::mutable_data()->part_mapping_[(size_t) p].keyrange_high = (uint8_t) c.hi;
            if (c.allocMask >= 0)
            {
                ambika::Multi::mutable_data()->part_mapping_[(size_t) p].voice_allocation = (uint8_t) c.allocMask;
                ambika::Multi::mutable_part ((uint8_t) p)->AssignVoices ((uint8_t) c.allocMask);
            }
            auto* d = ambika::Multi::mutable_part ((uint8_t) p)->mutable_data();
            if (c.poly    >= 0) d->polyphony_mode     = (uint8_t) c.poly;
            if (c.arpMode >= 0) d->arp_sequencer_mode = (uint8_t) c.arpMode;
            if (c.arpDir  >= 0) d->arp_direction      = (uint8_t) c.arpDir;
            if (c.arpOct  >= 0) d->arp_octave         = (uint8_t) c.arpOct;
            if (c.arpPat  >= 0) d->arp_pattern        = (uint8_t) c.arpPat;
            if (c.arpDiv  >= 0) d->arp_divider        = (uint8_t) c.arpDiv;
            ambika::Multi::mutable_part ((uint8_t) p)->Touch();   // re-sync prescalers
        }
        // Configuration writes (AssignVoices / Touch / patch uploads) emit
        // Kill + WriteBlock + WriteData events into the voicecard log that
        // must NOT pollute the scenario observables below (releaseEvents /
        // lastModWrite count "since the last clear") — clear both logs AFTER
        // the configuration settles so counters start at scenario t0.
        ambika::midi_dispatcher.clearLog();
        ambika::voicecard_tx.clearLog();
    }

    // ---- events ----
    // CHANNEL CONVENTION: the harness speaks 1-based MIDI channels everywhere
    // (the JUCE side is 1-based); the firmware's Multi:: events take 0-based
    // channels, so every event converts here. Configuration values
    // (PartCfg.channel / midi_channel) stay in the firmware's own 1-based
    // part-mapping convention (0 = Omni).
    void noteOn (int ch, int note, int vel) { ambika::Multi::NoteOn ((uint8_t) (ch - 1), (uint8_t) note, (uint8_t) vel); }
    void noteOff (int ch, int note)         { ambika::Multi::NoteOff ((uint8_t) (ch - 1), (uint8_t) note, 100); }
    void cc (int ch, int num, int val)      { ambika::Multi::ControlChange ((uint8_t) (ch - 1), (uint8_t) num, (uint8_t) val); }
    void allNotesOff (int ch)               { ambika::Multi::AllNotesOff ((uint8_t) (ch - 1)); }
    void polyAftertouch (int ch, int note, int vel)
    {
        ambika::Multi::Aftertouch ((uint8_t) (ch - 1), (uint8_t) note, (uint8_t) vel);
    }
    void clockTo (int targetTicks)          // explicit MIDI-clock ticks
    {
        while (fwClocks_ < targetTicks) { ambika::Multi::Clock(); ++fwClocks_; }
    }
    void transportStop()                    { ambika::Multi::Stop(); }
    void transportStart()                   { ambika::Multi::Start(); }

    // ---- observables ----
    int pressedKeys (int p) const { return ambika::Multi::part ((uint8_t) p).num_pressed_keys(); }

    // Set of parts that PLAYED a note: a kTrigger reached one of part p's
    // allocated voicecards. (The dispatcher's OnNote fires for every ACCEPTING
    // part — including a mask-0 part with no voices, which accepts+
    // dispatches but never plays. The Parvati oracle below derives the same
    // set from actually-sounding voices, so the firmware side must be
    // play-based too or the two oracles disagree on unallocated parts.)
    std::set<int> partsThatPlayed() const
    {
        std::set<int> s;
        for (const auto& e : ambika::voicecard_tx.log())
            if (e.kind == ambika::FwVoicecardEvent::kTrigger)
                for (int p = 0; p < 6; ++p)
                {
                    const uint8_t mask = ambika::Multi::data().part_mapping_[(size_t) p].voice_allocation;
                    if (mask & (1u << e.voice))
                        s.insert (p);
                }
        return s;
    }

    // The last note part p generated/played (-1 when it has not played).
    int lastPlayedNote (int p) const
    {
        int last = -1;
        for (const auto& e : ambika::midi_dispatcher.log())
            if (e.kind == ambika::FwMidiEvent::kOnNote
                && e.partPtr == ambika::Multi::mutable_part ((uint8_t) p))
                last = e.note;
        return last;
    }

    // Tuned 14-bit note of part p's last Trigger (12-EDO, spread 0 ->
    // midi_note * 128 on both implementations).
    int lastTriggerNote14 (int p) const
    {
        const auto mask = ambika::Multi::data().part_mapping_[(size_t) p].voice_allocation;
        int last = -1;
        for (const auto& e : ambika::voicecard_tx.log())
            if (e.kind == ambika::FwVoicecardEvent::kTrigger && (mask & (1u << e.voice)))
                last = (int) e.note;
        return last;
    }

    // Release/Kill commands issued to part p's voices since the last log
    // clear (a held sustain pedal must SUPPRESS these).
    int releaseEvents (int p) const
    {
        const auto mask = ambika::Multi::data().part_mapping_[(size_t) p].voice_allocation;
        int n = 0;
        for (const auto& e : ambika::voicecard_tx.log())
            if ((e.kind == ambika::FwVoicecardEvent::kRelease
                 || e.kind == ambika::FwVoicecardEvent::kKill)
                && (mask & (1u << e.voice)))
                ++n;
        return n;
    }

    // Last modulation value written to any of part p's voices for modSrc
    // (-1 when the part's voices received no such write).
    int lastModWrite (int p, int modSrc) const
    {
        const auto mask = ambika::Multi::data().part_mapping_[(size_t) p].voice_allocation;
        int last = -1;
        for (const auto& e : ambika::voicecard_tx.log())
            if (e.kind == ambika::FwVoicecardEvent::kWriteData
                && e.dataType == ambika::VOICECARD_DATA_MODULATION
                && e.address == (uint8_t) modSrc
                && (mask & (1u << e.voice)))
                last = e.value;
        return last;
    }

    // Note-sequence step decode at unit level (the PartData sequence bytes —
    // byte layout per firmware part.h: gate bit 7 of the note byte, legato
    // bit 7 of the velocity byte; sequence-3 note pairs live at offset 32
    // inside the 64-byte sequence_data area).
    struct Step { int note; int velocity; int gate; int legato; };
    static Step seqStep (int p, int step)
    {
        const uint8_t* seq = ambika::Multi::part ((uint8_t) p).raw_sequence_data();
        const uint8_t off = (uint8_t) ((32 + (step << 1)) & 0x3f);
        Step s;
        s.note     = seq[off] & 0x7f;
        s.gate     = (seq[off] & 0x80) ? 1 : 0;
        s.velocity = seq[off + 1] & 0x7f;
        s.legato   = (seq[off + 1] & 0x80) ? 1 : 0;
        return s;
    }
    static void setSeqStep (int p, int step, int note, int gate, int velocity, int legato)
    {
        uint8_t* seq = ambika::Multi::mutable_part ((uint8_t) p)->mutable_raw_sequence_data();
        const uint8_t off = (uint8_t) ((32 + (step << 1)) & 0x3f);
        seq[off]     = (uint8_t) ((note & 0x7f) | (gate ? 0x80 : 0));
        seq[off + 1] = (uint8_t) ((velocity & 0x7f) | (legato ? 0x80 : 0));
    }

private:
    int fwClocks_ = 0;
};

//===========================================================================
// Host-playhead mock: the processor reads transport state (isPlaying) from
// AudioPlayHead, defaulting to TRUE headlessly (the standalone default) — so
// a "transport stopped" scenario needs a playhead that reports it. This is
// what a DAW looks like to the plugin with playback stopped (W8 item 2: the
// phrase restart only engages while the host transport is stopped).
struct MockPlayHead : public juce::AudioPlayHead
{
    std::atomic<bool> playing { true };
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing.load());
        info.setBpm (120.0);
        return info;
    }
};

//===========================================================================
// Parvati oracle — the REAL audio path: processBlock drives routing, the
// arpeggiator and the sustain bookkeeping exactly as in the plugin.
class PvOracle
{
public:
    PvOracle() : proc_ (std::make_unique<ParvatiAudioProcessor>())
    {
        proc_->setPlayHead (&playhead_);
        proc_->prepareToPlay (kRate, kBlock);
    }
    ~PvOracle() { if (proc_ != nullptr) proc_->setPlayHead (nullptr); }

    SynthEngine& engine() { return proc_->getEngine(); }
    // (observers are non-const: they read through the processor's engine ref)

    // Ticks elapsed by the rendered audio (24 PPQN @ 120 BPM == 1000 samples).
    int ticks() const { return samplesRendered_ / kTicksFromSamples; }

    void configure (const std::vector<PartCfg>& cfg)
    {
        SynthEngine& e = proc_->getEngine();
        const int saved = e.getCurrentPart();
        for (int p = 0; p < (int) cfg.size(); ++p)
        {
            const PartCfg& c = cfg[(size_t) p];
            if (c.channel >= 0)          e.setPartMidiChannel (p, (uint8_t) c.channel);
            if (c.lo >= 0 && c.hi >= 0)  e.setPartKeyZone (p, c.lo, c.hi);
            if (c.allocMask >= 0)        e.setPartVoiceAllocation (p, (uint8_t) c.allocMask);
            e.setCurrentPart (p);
            if (c.poly    >= 0) e.applyPartByte (15, (uint8_t) c.poly);
            // Arp/seq config MUST ride the STAGED setters (writePendingConfig
            // + configDirty_, what the APVTS path uses): applyPartByte(7..11)
            // only writes the bytes + frameDirty_ and the LIVE arp/seq objects
            // never see the mode — the sequencer would not run at all and the
            // arp would not hold keys (scenarios [5]/[9] silently weakened).
            if (c.arpMode >= 0) e.setArpMode ((uint8_t) c.arpMode);
            if (c.arpDir  >= 0) e.setArpDirection ((uint8_t) c.arpDir);
            if (c.arpOct  >= 0) e.setArpOctave ((uint8_t) c.arpOct);
            if (c.arpPat  >= 0) e.setArpPattern ((uint8_t) c.arpPat);
            if (c.arpDiv  >= 0) e.setArpResolution ((uint8_t) c.arpDiv);
        }
        e.setCurrentPart (saved);
        renderSamples (kBlock);   // flush the deferred allocation rebuild
    }

    // ---- events (through the real MIDI seam) ----
    // Render ONE block carrying the given events verbatim (phrase-restart
    // scenario: a whole chord in one block so the transport cannot tick
    // between its note-ons).
    void injectMidi (const juce::MidiBuffer& m) { render (m, kBlock); }

    void noteOn (int ch, int note, int vel)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (ch, (uint8_t) note, (uint8_t) vel), 0);
        render (m, kBlock);
    }
    void noteOff (int ch, int note)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOff (ch, (uint8_t) note), 0);
        render (m, kBlock);
    }
    void cc (int ch, int num, int val)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::controllerEvent (ch, num, val), 0);
        render (m, kBlock);
    }
    void allNotesOff (int ch)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        render (m, kBlock);
    }
    void polyAftertouch (int ch, int note, int vel)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::aftertouchChange (ch, note, vel), 0);
        render (m, kBlock);
    }
    void clockTo (int targetTicks) { renderSamples (targetTicks * kTicksFromSamples - samplesRendered_); }
    void transportStop()           { playhead_.playing.store (false); }
    void transportStart()          { playhead_.playing.store (true); }

    // ---- observables ----
    // Parts with at least one KEYED (held) sounding voice — the parts that
    // accepted the last note events.
    std::set<int> partsThatPlayed()
    {
        std::set<int> s;
        SynthEngine& e = engine();
        for (int v = 0; v < kNumVoices; ++v)
            if (auto* av = e.getAmbikaVoice (v);
                av != nullptr && av->isVoiceActive() && av->isKeyDown())
            {
                const int p = av->getPartIndex();
                if (p >= 0 && p < 6) s.insert (p);
            }
        return s;
    }

    int lastTriggerNote14 (int p)
    {
        SynthEngine& e = engine();
        int last = -1;
        for (int vi : e.getPart (p).voiceIndices)
            if (auto* av = e.getAmbikaVoice (vi); av != nullptr)
            {
                const int n = av->getLastNote14();
                if (n >= 0) last = n;
            }
        return last;
    }

    // Sustain observables, robust against release TAILS: a tail-off release
    // keeps a voice "active" until its envelope finishes, so the drain checks
    // render extra audio first (see partSilentAfter).
    // - heldNoteStr: the note of part p's STILL-SOUNDING voice ("none" when
    //   silent) — while the pedal swallows a release this stays at the note.
    std::string heldNoteStr (int p)
    {
        SynthEngine& e = engine();
        for (int vi : e.getPart (p).voiceIndices)
            if (auto* av = e.getAmbikaVoice (vi); av != nullptr && av->isDisplayedActive())
                return std::to_string (av->getCurrentlyPlayingNote());
        return "none";
    }
    // - partSilentAfter: render `ms` of audio, then report whether every voice
    //   of part p has gone silent (the init patch's VCA release 60 finishes
    //   well inside 3 s — verified empirically; generous on purpose).
    bool partSilentAfter (int p, int ms)
    {
        renderSamples (ms * kRate / 1000);
        SynthEngine& e = engine();
        for (int vi : e.getPart (p).voiceIndices)
            if (auto* av = e.getAmbikaVoice (vi); av != nullptr && av->isDisplayedActive())
                return false;
        return true;
    }

    // Direct-mode held-key count (the MONO note stack; POLY notes live in
    // the voice pool itself and are observed via anyKeyDownStr).
    int heldKeys (int p) { return static_cast<int> (engine().getPart (p).monoStack.size()); }

    bool arpHoldsKeys (int p) { return engine().getPart (p).arp.hasHeldKeys(); }
    int  arpLastNote (int p)  { return (int) engine().getPart (p).arp.lastNote(); }

    // Last modulation value on part p's voices for modSrc (-1 none).
    int lastModWrite (int p, int modSrc)
    {
        // MAX over the part's voices, not last-writer-wins: the firmware's
        // per-note writes (poly-AT) hit ONE voice of the part while the idle
        // trailing voices keep 0 — a plain "last =" loop clobbered the written
        // voice with the idle one (W8; the channel-wide CC family masked it by
        // writing every voice).
        SynthEngine& e = engine();
        int best = 0;
        for (int vi : e.getPart (p).voiceIndices)
            if (auto* av = e.getAmbikaVoice (vi); av != nullptr)
                best = std::max (best, (int) av->getModulationSource ((uint8_t) modSrc));
        return best;
    }

private:
    void render (const juce::MidiBuffer& midi, int n)
    {
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        proc_->processBlock (buf, const_cast<juce::MidiBuffer&> (midi));
        samplesRendered_ += n;
    }
    void renderSamples (int n)
    {
        int done = 0;
        while (done < n)
        {
            const int chunk = std::min (kBlock, n - done);
            render (juce::MidiBuffer(), chunk);
            done += chunk;
        }
    }
    MockPlayHead playhead_;   // declared BEFORE proc_ (destructs after it): the playhead pointer handed to the processor must never dangle
    std::unique_ptr<ParvatiAudioProcessor> proc_;
    int samplesRendered_ = 0;
};

std::string setStr (const std::set<int>& s)
{
    std::string out;
    for (int v : s) { if (! out.empty()) out += ","; out += std::to_string (v); }
    return out.empty() ? "none" : out;
}

// Keep the firmware's explicit clock in lockstep with Parvati's elapsed
// ticks (the Parvati side renders real audio; the firmware ticks explicitly).
void syncClock (FwOracle& fw, PvOracle& pv)
{
    fw.clockTo (pv.ticks());
}

// ---------------------------------------------------------------------------
// Scenarios.
// ---------------------------------------------------------------------------

// Common two-part layout: part 0 on ch 1 (voices 0..2), part 1 on ch 2
// (voices 3..5) — the same 3+3 split the engine's multitimbral test uses.
// Parts 2..5 are EXPLICITLY disabled (mask 0) on BOTH sides: the firmware
// factory default puts them on ch 3..6 with live allocations, so an "unrouted"
// ch3 note would otherwise be accepted by firmware part 2 while Parvati's
// (0-slot, disabled) part 2 rejects it — a setup asymmetry, not a parity
// divergence. Mask 0 + no voices: neither side can play those parts.
std::vector<PartCfg> twoPartSplit()
{
    std::vector<PartCfg> cfg (6);
    cfg[0] = { 1, 0, 127, 0x07, -1, -1, -1, -1, -1, -1 };
    cfg[1] = { 2, 0, 127, 0x38, -1, -1, -1, -1, -1, -1 };
    cfg[2] = { 3, 0, 127, 0x00, -1, -1, -1, -1, -1, -1 };
    cfg[3] = { 4, 0, 127, 0x00, -1, -1, -1, -1, -1, -1 };
    cfg[4] = { 5, 0, 127, 0x00, -1, -1, -1, -1, -1, -1 };
    cfg[5] = { 6, 0, 127, 0x00, -1, -1, -1, -1, -1, -1 };
    return cfg;
}

// [1] Per-channel acceptance with NON-overlapping channels: unicast routing
// is equivalent to the firmware's multicast when only one part can accept.
void scenarioChannelAccept()
{
    std::printf ("\n[1] channel acceptance (non-overlapping 3+3 split)\n");
    FwOracle fw; fw.reset(); fw.configure (twoPartSplit());
    PvOracle pv; pv.configure (twoPartSplit());

    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "ch1 note accepted by the same part(s)");
    fw.noteOn (2, 64, 100); pv.noteOn (2, 64, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "ch1+ch2 notes accepted by the same part(s)");
    fw.noteOn (3, 67, 100); pv.noteOn (3, 67, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "ch3 (unrouted) note accepted by nobody new");

    // Tuned 14-bit note parity (12-EDO, spread 0): midi_note * 128.
    checkEquals (std::to_string (fw.lastTriggerNote14 (0)), std::to_string (pv.lastTriggerNote14 (0)),
                 "part 0 tuned 14-bit note (60 -> 7680)");
    checkEquals (std::to_string (fw.lastTriggerNote14 (1)), std::to_string (pv.lastTriggerNote14 (1)),
                 "part 1 tuned 14-bit note (64 -> 8192)");
}

// [2] Overlapping routing: an Omni part PLUS a specific-channel part —
// firmware parity (W8 item 4): EVERY accepting part plays the note.
void scenarioMulticastRouting()
{
    std::printf ("\n[2] overlapping routing: Omni part + ch-2 part (multicast)\n");
    auto cfg = twoPartSplit();
    cfg[0].channel = 0;    // part 0 Omni (accepts everything)

    FwOracle fw; fw.reset(); fw.configure (cfg);
    PvOracle pv; pv.configure (cfg);

    fw.noteOn (2, 60, 100); pv.noteOn (2, 60, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "ch2 note while part 0 is Omni: EVERY accepting part plays (multicast)");

    // Symmetric release: both parts' notes release on the note-off (the
    // multicast predicate is the same for on and off).
    fw.noteOff (2, 60); pv.noteOff (2, 60); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "multicast note-off releases every accepting part");
}

// [3] Wrap-around key zones (lo > hi): the firmware accepts the complement
// set (the classic hardware split trick); Parvati ports it (W8 item 1).
void scenarioWrapZone()
{
    std::printf ("\n[3] wrap-around key zone (lo=100 hi=20)\n");
    auto cfg = twoPartSplit();
    cfg[1].lo = 100; cfg[1].hi = 20;   // wrap: accepts <=20 OR >=100

    FwOracle fw; fw.reset(); fw.configure (cfg);
    PvOracle pv; pv.configure (cfg);

    fw.noteOn (2, 5, 100); pv.noteOn (2, 5, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "note 5 inside wrap zone [100..20]: accepted by both");

    // A note in the (excluded) middle must NOT reach the wrap part.
    fw.noteOn (2, 60, 100); pv.noteOn (2, 60, 100); syncClock (fw, pv);
    checkEquals (setStr (fw.partsThatPlayed()), setStr (pv.partsThatPlayed()),
                 "note 60 outside wrap zone: rejected by both");
}

// [4] Sustain pedal (CC64): swallow the key release, drain on pedal-up.
// MUST MATCH (the wave-7 firmware port).
void scenarioSustain()
{
    std::printf ("\n[4] sustain pedal (CC64) swallow + drain\n");
    FwOracle fw; fw.reset(); fw.configure (twoPartSplit());
    PvOracle pv; pv.configure (twoPartSplit());

    fw.cc (1, 64, 127); pv.cc (1, 64, 127);   // pedal down (ch1)
    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100); syncClock (fw, pv);
    fw.noteOff (1, 60); pv.noteOff (1, 60); syncClock (fw, pv);

    checkEquals (std::to_string (fw.pressedKeys (0)), "1",
                 "firmware retains the released key (flagged for pedal-up drain)");
    checkEquals (std::to_string (fw.releaseEvents (0)), "0",
                 "firmware issued no Release/Kill while the pedal held the note");
    checkEquals (pv.heldNoteStr (0), "60",
                 "Parvati swallowed the release (note 60 still sounding)");

    fw.cc (1, 64, 0); pv.cc (1, 64, 0); syncClock (fw, pv);   // pedal up
    checkEquals (std::to_string (fw.pressedKeys (0)), "0",
                 "firmware drained the sustained note on pedal-up");
    checkEquals (std::to_string (fw.releaseEvents (0)), "1",
                 "firmware released the voice on pedal-up");
    check (pv.partSilentAfter (0, 3000),
           "Parvati released the sustained voice on pedal-up (silent after the release tail)");
}

// [5] All-notes-off (CC123): clears held-key bookkeeping per receiving part
// (direct mode AND arp mode). MUST MATCH (wave-7).
void scenarioAllNotesOff()
{
    std::printf ("\n[5] all-notes-off (CC123) bookkeeping\n");
    {
        FwOracle fw; fw.reset(); fw.configure (twoPartSplit());
        PvOracle pv; pv.configure (twoPartSplit());
        fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100);
        fw.noteOn (1, 64, 100); pv.noteOn (1, 64, 100);
        fw.allNotesOff (1); pv.allNotesOff (1); syncClock (fw, pv);
        checkEquals (std::to_string (fw.pressedKeys (0)), std::to_string (pv.heldKeys (0)),
                     "direct-mode held keys cleared on CC123");
        check (pv.partSilentAfter (0, 3000),
               "Parvati released every part-0 voice on CC123 (silent after the tail)");
        check (fw.releaseEvents (0) >= 1, "firmware released the part-0 voices on CC123");
    }
    {
        auto cfgA = twoPartSplit();
        cfgA[0].arpMode = 1; cfgA[0].arpDir = 0; cfgA[0].arpOct = 1;
        cfgA[0].arpPat = 0; cfgA[0].arpDiv = 10;
        FwOracle fw2; fw2.reset(); fw2.configure (cfgA);
        PvOracle pv2; pv2.configure (cfgA);
        fw2.noteOn (1, 60, 100); pv2.noteOn (1, 60, 100);
        fw2.noteOn (1, 67, 100); pv2.noteOn (1, 67, 100);
        fw2.allNotesOff (1); pv2.allNotesOff (1); syncClock (fw2, pv2);
        checkEquals (std::to_string (fw2.pressedKeys (0)),
                     pv2.arpHoldsKeys (0) ? "1" : "0",
                     "arp-mode held keys cleared on CC123 [fw pressed / pv arp stack]");
    }
}

// [6] CC1 (mod wheel) routing: only the receiving channel's parts' voices get
// the modulation write (value << 1). MUST MATCH (wave-7).
void scenarioModWheelRouting()
{
    std::printf ("\n[6] CC1 mod-wheel routing (per channel)\n");
    FwOracle fw; fw.reset(); fw.configure (twoPartSplit());
    PvOracle pv; pv.configure (twoPartSplit());

    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100);
    fw.noteOn (2, 64, 100); pv.noteOn (2, 64, 100);
    fw.cc (2, 1, 40); pv.cc (2, 1, 40); syncClock (fw, pv);

    // "Untouched" is encoded differently by the two oracles: the firmware
    // log reports -1 (no WriteData ever reached part 0's voices), while the
    // live engine reads its mod-source INITIAL value (0). Both mean the ch2
    // CC1 did not reach part 0 — assert the pair, not a raw equality.
    {
        const int fwv = fw.lastModWrite (0, ambika::MOD_SRC_WHEEL);
        const int pvv = pv.lastModWrite (0, ambika::dsp::MOD_SRC_WHEEL);
        check ((fwv == -1 && pvv == 0) || fwv == pvv,
               "part 0 (ch1) untouched by a ch2 CC1 [fw=" + std::to_string (fwv)
                   + " pv=" + std::to_string (pvv) + "]");
    }
    checkEquals (std::to_string (fw.lastModWrite (1, ambika::MOD_SRC_WHEEL)),
                 std::to_string (pv.lastModWrite (1, ambika::dsp::MOD_SRC_WHEEL)),
                 "part 1 (ch2) received CC1 (40 << 1 = 80)");
}

// [7] Polyphonic aftertouch: firmware parity (W8 item 3) — the value is
// written to the accepting part's voices through accept_channel_note.
void scenarioPolyAftertouch()
{
    std::printf ("\n[7] polyphonic aftertouch\n");
    FwOracle fw; fw.reset(); fw.configure (twoPartSplit());
    PvOracle pv; pv.configure (twoPartSplit());

    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100);
    fw.polyAftertouch (1, 60, 90); pv.polyAftertouch (1, 60, 90); syncClock (fw, pv);

    checkEquals (std::to_string (fw.lastModWrite (0, ambika::MOD_SRC_AFTERTOUCH)),
                 std::to_string (pv.lastModWrite (0, ambika::dsp::MOD_SRC_AFTERTOUCH)),
                 "poly-AT writes MOD_SRC_AFTERTOUCH to the accepting part's voices");

    // A note on the OTHER channel's part must not receive part 0's AT.
    fw.polyAftertouch (2, 64, 30); pv.polyAftertouch (2, 64, 30); syncClock (fw, pv);
    checkEquals (std::to_string (fw.lastModWrite (0, ambika::MOD_SRC_AFTERTOUCH)),
                 std::to_string (pv.lastModWrite (0, ambika::dsp::MOD_SRC_AFTERTOUCH)),
                 "poly-AT on ch2 leaves part 0 (ch1) untouched");
}

// [8] Arp phrase restart: firmware parity (W8 item 2) — a NEW phrase while
// the transport is stopped restarts the arp at pattern step 0.
void scenarioArpPhraseRestart()
{
    std::printf ("\n[8] arp phrase restart (transport stopped)\n");
    auto cfg = twoPartSplit();
    cfg[0].arpMode = 1; cfg[0].arpDir = 0; cfg[0].arpOct = 1;
    cfg[0].arpPat = 0; cfg[0].arpDiv = 10;   // 6 ticks per arp step

    FwOracle fw; fw.reset(); fw.configure (cfg);
    PvOracle pv; pv.configure (cfg);

    // OBSERVABLE ASYMMETRY (why a recorder, not arp.lastNote()): the firmware
    // oracle reads the DISPATCHER LOG's last generated note, which persists
    // across later skipped steps; the arp's live previousNote_ resets to 0xff
    // whenever the pattern bit is clear (pattern 0 = 0x5555 has only odd
    // bits, so alternate steps skip). Record the generated notes through the
    // note-on seam (the scenario-9 idiom) so the two observables match.
    std::vector<int> pvGenerated;
    pv.engine().getPart (0).arp.setNoteOnCallback (
        [&pvGenerated] (int, int note, uint8_t) { pvGenerated.push_back (note); });

    // Phrase 1: 3 held notes; clock past >= 2 arp steps.
    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100);
    fw.noteOn (1, 64, 100); pv.noteOn (1, 64, 100);
    fw.noteOn (1, 67, 100); pv.noteOn (1, 67, 100);
    pv.clockTo (13); syncClock (fw, pv);
    // Phrase ends: release everything; the firmware transport stops.
    fw.noteOff (1, 60); pv.noteOff (1, 60);
    fw.noteOff (1, 64); pv.noteOff (1, 64);
    fw.noteOff (1, 67); pv.noteOff (1, 67);
    fw.transportStop(); pv.transportStop();
    pvGenerated.clear();   // phrase 2's first note is the FIRST after the restart

    // Phrase 2 (same notes) + two arp steps. All three note-ons in ONE
    // block: the pv transport ticks continuously per rendered block, so
    // separate note-on blocks let the forced first arp step fire mid-phrase
    // with only one key held (a block-boundary asymmetry vs the explicit-
    // clock firmware oracle, whose first Clock sees all three keys — not a
    // semantic difference).
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 60, (uint8_t) 100), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 64, (uint8_t) 100), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 67, (uint8_t) 100), 0);
        pv.injectMidi (m);
    }
    fw.noteOn (1, 60, 100);
    fw.noteOn (1, 64, 100);
    fw.noteOn (1, 67, 100);
    pv.clockTo (pv.ticks() + 12); syncClock (fw, pv);   // 2 steps: restart step + one more

    const int fwNote = fw.lastPlayedNote (0);
    const int pvNote = pvGenerated.empty() ? -1 : pvGenerated.back();
    std::printf ("     first generated note of phrase 2: fw=%d pv=%d\n", fwNote, pvNote);
    checkEquals (std::to_string (fwNote), std::to_string (pvNote),
                 "new phrase restarts the arp at pattern step 0 (both sides)");
    check (pvGenerated.size() >= 2,
           "phrase 2 generated at least two arp steps (restart step + one more)");

    // ---- Phase B: while the transport PLAYS, a new phrase does NOT restart
    // (firmware's running_ gate: NoteOn only calls Start() when !running_;
    // the engine's !isPlaying condition is the port). Both sides keep their
    // pattern position, so the first note of the next phrase is wherever the
    // pattern left off — assert PARITY of that note (not a fixed value).
    pvGenerated.clear();
    pv.transportStart();   // playhead playing=true (the next render starts arps)
    fw.transportStart();
    pv.clockTo (pv.ticks() + 2); syncClock (fw, pv);   // settle the start step
    pvGenerated.clear();
    {
        juce::MidiBuffer m;   // phrase 3 in one block, transport running
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 60, (uint8_t) 100), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 62, (uint8_t) 100), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) 65, (uint8_t) 100), 0);
        pv.injectMidi (m);
    }
    fw.noteOn (1, 60, 100);
    fw.noteOn (1, 62, 100);
    fw.noteOn (1, 65, 100);
    pv.clockTo (pv.ticks() + 12); syncClock (fw, pv);
    const int fwNote3 = fw.lastPlayedNote (0);
    const int pvNote3 = pvGenerated.empty() ? -1 : pvGenerated.front();
    std::printf ("     first generated note of phrase 3 (transport running): fw=%d pv=%d\n", fwNote3, pvNote3);
    checkEquals (std::to_string (fwNote3), std::to_string (pvNote3),
                 "while the transport plays, a new phrase does NOT restart (parity of position)");
}

// [9] Note-sequence velocity-0 decode: the firmware returns velocity 0 for
// a gated step programmed to 0; Parvati substitutes 100. Observable at the
// note-on seams: the firmware's midi dispatcher records the generated note's
// velocity; Parvati's Sequencer note-on callback (which the engine normally
// wires to triggerNoteInPart) is replaced by a recorder for this scenario on
// a DEDICATED oracle — the callback receives exactly the decoded velocity.
void scenarioVelocityZeroDivergence()
{
    std::printf ("\n[9] sequencer velocity-0 decode\n");
    std::vector<std::pair<int,int>> recorded;   // (note, velocity) from the seq note-on seam
    auto cfg = twoPartSplit();
    cfg[0].arpMode = 2;   // note-sequencer mode
    cfg[0].arpDiv  = 10;  // 6 ticks per step

    // Programme step 0 of sequence 3 (the note-data area): gate on, vel 0.
    // Firmware bytes: sequence_data offset (32 + 0*2) & 0x3f == 32; Parvati:
    // PartData offset 16 + 32 (same layout). Sequence-3 length -> byte 14.
    FwOracle fw; fw.reset(); fw.configure (cfg);
    {
        auto* d = ambika::Multi::mutable_part (0)->mutable_data();
        d->sequence_length[2] = 16;
        FwOracle::setSeqStep (0, 0, /*note*/ 60, /*gate*/ 1, /*velocity*/ 0, /*legato*/ 0);
        ambika::Multi::mutable_part (0)->Touch();
    }

    PvOracle pv; pv.configure (cfg);
    {
        SynthEngine& e = pv.engine();
        const int saved = e.getCurrentPart();
        e.setCurrentPart (0);
        // The LIVE-edit seams (what NoteStepControl uses): setSequenceLength /
        // setSequenceDataByte stage through pendingConfig_ — applyPartByte
        // alone only pushes bytes to the voicecards, not the engine-side
        // sequencer.
        e.setSequenceLength (2, 16);                            // sequence_length[2]
        e.setSequenceDataByte (32,     (uint8_t) (60 | 0x80));  // note + gate
        e.setSequenceDataByte (32 + 1, (uint8_t) 0);            // velocity 0
        e.setCurrentPart (saved);
        // Record the seq's generated notes (replaces the engine's trigger
        // wiring for this scenario — we only need the decoded velocity).
        e.getPart (0).seq.setNoteOnCallback (
            [&recorded] (int, int note, uint8_t velocity)
            { recorded.emplace_back (note, (int) velocity); });
    }

    // Hold a key (the arp stack gates the clock) + two seq steps on both
    // sides (arpDiv 10 == 6 ticks/step; two steps give the staged seq config
    // a full service/render cycle after the pendingConfig_ flush and leave
    // margin at the tick boundary — one step sat exactly ON the boundary).
    fw.noteOn (1, 60, 100); pv.noteOn (1, 60, 100);
    pv.clockTo (pv.ticks() + 12); syncClock (fw, pv);

    int fwVel = -1, pvVel = -1, fwNote = -1;
    for (const auto& e : ambika::midi_dispatcher.log())
        if (e.kind == ambika::FwMidiEvent::kOnNote
            && e.partPtr == ambika::Multi::mutable_part (0)
            && e.note == 60)
        { fwVel = e.velocity; fwNote = e.note; }
    for (const auto& r : recorded) { pvVel = r.second; }
    (void) fwNote;
    checkEquals ("60", std::to_string (recorded.empty() ? -1 : recorded.back().first),
                 "sequencer generated the programmed note (60)");
    checkDiverges ("velocity-zero-substitution",
                   std::to_string (fwVel), std::to_string (pvVel),
                   "gated step with velocity byte 0: firmware generates velocity 0, Parvati substitutes 100");
}

//===========================================================================
// [10] VOICECARD AUDIO ORACLE — "mix-gain-glide" (sanctioned audio-path
// divergence). Drives the REAL firmware Voice::ProcessBlock (static class;
// output drained from the ambika::audio_buffer ring) block-by-block against
// the port's ambika::dsp::Voice with identical patch bytes, note and RNG
// state:
//   * static mix CVs → every rendered block BYTE-EQUAL (the glide is
//     transparent when nothing changes — this pins the whole line-for-line
//     port fidelity story at the byte level, silence blocks included),
//   * a mid-render mix_balance tick → the tick block DIFFERS (firmware
//     latches the crossfade gains once per 40-sample block, voice.cc:441-
//     442; Parvati glides them across the block — the zipper fix measured
//     at 0.0597-vs-0.05 by parvati_synth_drag_probe),
//   * after settling (the sub-LSB snap closes the ramp within one extra
//     block) → BYTE-EQUAL again: the glide converges to the firmware
//     targets exactly, it does not permanently offset the mix.
void scenarioMixGainGlideAudio()
{
    std::printf ("\n--- [10] voicecard audio oracle: mix-gain-glide ---\n");
    constexpr int kBlockBytes = fw_voicecard::kAudioBlockSize;
    static_assert (ambika::dsp::kAudioBlockSize == kBlockBytes, "port block size");
    static_assert ((int) ambika::dsp::WAVEFORM_SAW == 1, "port saw id (NONE=0, SAW=1)");

    // RNG lockstep: both sides run the same 16-bit LFSR (boot seed 0x21,
    // advanced exactly once per block by the MOD_SRC_NOISE source). The
    // Parvati-side scenarios above rendered 6-voice audio and advanced only
    // the port's global — re-seed both so the voices tick in lockstep.
    fw_voicecard::SeedRandom (0x21);
    ambika::dsp::random().Seed (0x21);

    // Patch-byte addresses (identical layouts: the port's Patch is a copy;
    // sizeof==112 is pinned by static_assert in dsp/constants.h, and any
    // address mismatch would trip the byte-equality checks below loudly).
    const int offOsc1Shape = fw_voicecard::Osc1ShapeOffset();
    const int offBalance   = fw_voicecard::MixBalanceOffset();
    const uint8_t sawId    = fw_voicecard::WaveformSaw();

    // --- firmware side (statics; Init() loads the same all-zeros-osc init
    //     patch as the port's kInitPatch — set a saw on osc 1 so the mix
    //     crossfade actually carries signal) ---
    fw_voicecard::Init();
    fw_voicecard::SetPatchByte (offOsc1Shape, sawId);
    fw_voicecard::SetPatchByte (offBalance, 24);   // gain 96 (bal domain 0..63)
    fw_voicecard::Trigger (60 << 7, 100, 0);

    // --- Parvati side: a bare dsp::Voice — no engine, no JUCE in the path ---
    ambika::dsp::Voice pv;
    pv.Init();
    pv.set_patch_data (static_cast<uint8_t> (offOsc1Shape), sawId);
    pv.set_patch_data (static_cast<uint8_t> (offBalance), 24);
    pv.Trigger (60 << 7, 100, 0);

    const auto hex = [kBlockBytes] (const uint8_t* p) {
        std::string s;
        s.reserve (static_cast<size_t> (kBlockBytes) * 3);
        char b[4];
        for (int i = 0; i < kBlockBytes; ++i)
        {
            std::snprintf (b, sizeof (b), "%02X ", p[i]);
            s += b;
        }
        return s;
    };

    // One block per side (the firmware facade drains its 40 bytes from the
    // audio ring — the vca()<2 silence path writes 40 too, so the drains
    // always match the writes).
    bool anySignal = false;
    uint8_t fwBlock[kBlockBytes];
    const auto fwRender = [&fwBlock, &hex]() {
        fw_voicecard::ProcessBlock (fwBlock);
        return hex (fwBlock);
    };
    const auto pvRender = [&pv, &hex, &anySignal, kBlockBytes]() {
        pv.ProcessBlock();
        const auto& out = pv.output();   // raw pointer (HEAD) or std::array —
        for (int i = 0; i < kBlockBytes; ++i)   // indexable either way
            if (out[i] != 128) { anySignal = true; break; }
        return hex (&out[0]);
    };

    // Phase 1 — static CV: 12 blocks, every one byte-equal (env attack is 0,
    // so the VCA is open from the first blocks; the equality is over real
    // signal, pinned by the audibility tripwire below).
    for (int b = 0; b < 12; ++b)
        checkEquals (fwRender(), pvRender(),
                     "static mix_balance=24, osc1 saw: block " + std::to_string (b) + " byte-equal");
    check (anySignal, "voice audible (equality compared real signal, not silence)");

    // Phase 2 — balance tick 24 -> 32 (gain 96 -> 128: a 32/255 gain step,
    // twice the drag probe's 16/255 UI tick): firmware steps, Parvati glides.
    fw_voicecard::SetPatchByte (offBalance, 32);
    pv.set_patch_data (static_cast<uint8_t> (offBalance), 32);
    const std::string fwTick = fwRender();
    const std::string pvTick = pvRender();
    checkDiverges ("mix-gain-glide", fwTick, pvTick,
                   "balance tick 24->32: firmware latches gains per block, Parvati glides across it");

    // Phase 3 — settle: the sub-LSB snap closes the ramp tail within one
    // block, after which the renders are byte-equal again.
    (void) fwRender();
    (void) pvRender();   // settle block (ramp tail; not compared)
    checkEquals (fwRender(), pvRender(), "post-glide settled block byte-equal (converged exactly)");
}

//===========================================================================
// Allowlist loading / validation.
bool loadAllowlist (const std::string& path, std::set<std::string>& out)
{
    std::ifstream in (path);
    if (! in.is_open())
        return false;
    std::string line;
    while (std::getline (in, line))
    {
        const auto hash = line.find ('#');
        if (hash != std::string::npos) line = line.substr (0, hash);
        const auto first = line.find_first_not_of (" \t\r");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of (" \t\r");
        out.insert (line.substr (first, last - first + 1));
    }
    return true;
}

}  // namespace

//===========================================================================
TEST(firmware_parity_test)
{
    // (--self-test was argv[1] in the standalone binary; the unified runner
    // has no argv, so keep the default full run.)
    std::printf ("=== Parvati firmware parity oracle ===\n");

    const char* allowlistPath = "tests/firmware_parity_known_divergences.txt";
    if (! loadAllowlist (allowlistPath, g_divergences))
    {
        std::printf ("FATAL: cannot open %s (run from the repo root)\n", allowlistPath);
        return false;
    }
    std::printf ("allowlist: %zu divergence(s)\n", g_divergences.size());

    juce::ScopedJuceInitialiser_GUI juceInit;

    scenarioChannelAccept();          // [1]
    scenarioMulticastRouting();       // [2]
    scenarioWrapZone();               // [3]
    scenarioSustain();                // [4]
    scenarioAllNotesOff();            // [5]
    scenarioModWheelRouting();        // [6]
    scenarioPolyAftertouch();         // [7]
    scenarioArpPhraseRestart();       // [8]
    scenarioVelocityZeroDivergence(); // [9]
    scenarioMixGainGlideAudio();      // [10] voicecard audio oracle

    // Allowlist <-> harness completeness (both directions).
    const std::set<std::string> exercised = {
        "velocity-zero-substitution",
        "mix-gain-glide",
    };
    for (const auto& id : g_divergences)
        if (exercised.count (id) == 0)
            check (false, "allowlist entry '" + id + "' is not exercised by the harness (remove it or add a check)");
    for (const auto& id : exercised)
        if (g_divergences.count (id) == 0)
            check (false, "harness divergence '" + id + "' has no allowlist entry");

    if (g_failures == 0)
        std::printf ("\nFIRMWARE PARITY TEST: ALL CHECKS PASSED (0 failures)\n");
    else
        std::printf ("\nFIRMWARE PARITY TEST: FAILURES (%d)\n", g_failures);
    return g_failures == 0;
}
