// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See VoiceMeter.h.

#include "VoiceMeter.h"

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"

namespace
{
// Standard music convention: MIDI 60 == C4.
const char* const kNoteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
}

VoiceMeter::VoiceMeter()
{
    state_.fill (CellState {});
    startTimerHz (30);

    // Accessibility name/description (read by the default handler).
    setTitle ("Voice activity meter");
    setDescription ("Voice meter");
}

VoiceMeter::~VoiceMeter()
{
    stopTimer();
}

void VoiceMeter::setStateProvider (std::function<std::vector<VoiceActivity>()> provider)
{
    provider_ = std::move (provider);
}

int VoiceMeter::getActiveVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& c : state_)
        if (c.active)
            ++n;
    return n;
}

//==========================================================================
// Accessibility: expose the live active-voice count as a read-only text value
// ("N of 6") so screen readers announce the meter state.
struct VoiceMeter::VoiceCountInterface : public juce::AccessibilityTextValueInterface
{
    explicit VoiceCountInterface (VoiceMeter& o) : owner (o) {}

    bool isReadOnly() const override { return true; }

    juce::String getCurrentValueAsString() const override
    {
        return juce::String (owner.getActiveVoiceCount()) + " of " + juce::String (kNumVoicecards);
    }

    void setValueAsString (const juce::String&) override {}   // read-only

    VoiceMeter& owner;
};

std::unique_ptr<juce::AccessibilityHandler> VoiceMeter::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (
        *this,
        juce::AccessibilityRole::group,
        juce::AccessibilityActions{},
        juce::AccessibilityHandler::Interfaces { std::make_unique<VoiceCountInterface> (*this) });
}

const ParvatiTheme* VoiceMeter::currentTheme() const noexcept
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    return lnf ? lnf->getTheme() : nullptr;
}

juce::String VoiceMeter::midiNoteName (int note)
{
    if (note < 0 || note > 127)
        return {};
    const int octave = note / 12 - 1;   // 60 -> 5 - 1 == 4 => "C4"
    return juce::String (kNoteNames[note % 12]) + juce::String (octave);
}

void VoiceMeter::timerCallback()
{
    if (! provider_)
        return;

    const auto next = provider_();
    if (next.size() < static_cast<size_t> (kNumVoicecards))
        return;   // malformed frame; keep the last good state

    bool changed = false;
    for (int c = 0; c < kNumVoicecards; ++c)
    {
        // Voice i == voicecard i (one voice per card), so cell c is fed directly
        // by provider slot c.
        const auto& slot = next[static_cast<size_t> (c)];
        if (state_[(size_t) c].active != slot.active || state_[(size_t) c].note != slot.note)
        {
            state_[(size_t) c] = { slot.active, slot.note };
            changed = true;
        }
    }

    if (changed)
    {
        repaint();

        // Announce the active-voice count to accessibility clients when it
        // changes (screen readers read the value interface, e.g. "5 of 6").
        const int count = getActiveVoiceCount();
        if (count != lastAnnouncedCount_)
        {
            lastAnnouncedCount_ = count;
            if (auto* handler = getAccessibilityHandler())
                handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
        }
    }
}

void VoiceMeter::paint (juce::Graphics& g)
{
    // Compact single-row strip of 6 voice indicators (one per firmware
    // voicecard): a small square (filled accent when the voice is active, an
    // outline when free) followed by "V#:<note>" / "V#:--". The enclosing Global
    // group panel already supplies the frame, so only a subtle strip fill is
    // painted here. Text follows the active font mode (Console/Serif/Sans).
    const ParvatiTheme* t = currentTheme();
    const juce::Colour panel     = t ? t->panelBackground : juce::Colour (0xff24242e);
    const juce::Colour outlineC  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const juce::Colour accent    = t ? t->accent          : juce::Colour (0xffe8b84b);
    const juce::Colour textValue = t ? t->textValue       : juce::Colour (0xffe8e8ee);

    g.setColour (panel);
    g.fillRect (getLocalBounds());

    const juce::Font font = [this]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            return lnf->appFont (14.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (14.0f));
    }();

    constexpr float sq = 8.0f;   // square indicator edge
    for (int c = 0; c < kNumVoicecards; ++c)
    {
        const auto r = cellRects_[(size_t) c].toFloat();
        const bool active = state_[(size_t) c].active;
        const juce::String noteText =
            active ? midiNoteName (state_[(size_t) c].note) : "--";
        const juce::String label = "V" + juce::String (c + 1) + ":" + noteText;
        const juce::Colour col = active ? textValue : accent;

        // Square indicator: filled accent (active) / outline (idle).
        const juce::Rectangle<float> sqRect (r.getX() + 2.0f,
                                             r.getCentreY() - sq * 0.5f, sq, sq);
        if (active)
        {
            g.setColour (accent);
            g.fillRect (sqRect);
        }
        else
        {
            g.setColour (outlineC);
            g.drawRect (sqRect, 1.0f);
        }

        g.setColour (col);
        g.setFont (font);
        g.drawText (label,
                    r.withTrimmedLeft (sq + 5.0f),
                    juce::Justification::centredLeft, false);
    }
}

void VoiceMeter::resized()
{
    // Compact single row: 6 even voice indicators across the strip. No separate
    // label column (the active count is shown in the editor's bottom status
    // strip; accessibility exposes it via getActiveVoiceCount()).
    auto area = getLocalBounds().reduced (4);

    const int gap = 6;
    const int totalGap = (kNumVoicecards - 1) * gap;
    const int cellW = juce::jmax (8, (area.getWidth() - totalGap) / kNumVoicecards);
    const int cellH = area.getHeight();
    int x = area.getX();
    const int y = area.getY();
    for (int c = 0; c < kNumVoicecards; ++c)
    {
        cellRects_[(size_t) c] = juce::Rectangle<int> (x, y, cellW, cellH);
        x += cellW + gap;
    }
}
