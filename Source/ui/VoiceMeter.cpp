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

void VoiceMeter::visibilityChanged()
{
    // Hidden (another top page active) -> stop polling; shown -> resume.
    if (isVisible())
        startTimerHz (30);
    else
        stopTimer();
}

void VoiceMeter::setStateProvider (std::function<std::vector<VoiceActivity>()> provider)
{
    provider_ = std::move (provider);
}

int VoiceMeter::getActiveVoiceCount() const noexcept
{
    int n = 0;
    for (int c = 0; c < cellCount_; ++c)
        if (state_[(size_t) c].active)
            ++n;
    return n;
}

//==========================================================================
// Accessibility: expose the live active-voice count as a read-only text value
// ("N of M", M == the current part's allocated voice count) so screen readers
// announce the meter state.
struct VoiceMeter::VoiceCountInterface : public juce::AccessibilityTextValueInterface
{
    explicit VoiceCountInterface (VoiceMeter& o) : owner (o) {}

    bool isReadOnly() const override { return true; }

    juce::String getCurrentValueAsString() const override
    {
        return juce::String (owner.getActiveVoiceCount()) + " of " + juce::String (owner.cellCount_);
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

    // The frame SIZE is the current part's allocated voice count (see the
    // part-relative contract in setStateProvider): 0..kMaxCells entries, an
    // empty frame being a valid disabled part. Truncate anything larger
    // (defensive — a CHAIN part tops out at 2 x 16 voices == kMaxCells).
    const int n = juce::jmin (static_cast<int> (next.size()), kMaxCells);

    // A size change is a REALLOCATION: relayout the strip and drop the state of
    // cells that vanished, and repaint even if no individual cell flipped.
    bool changed = false;
    if (n != cellCount_)
    {
        cellCount_ = n;
        layoutCells();
        for (int c = cellCount_; c < kMaxCells; ++c)
            state_[(size_t) c] = CellState {};
        changed = true;
    }

    for (int c = 0; c < cellCount_; ++c)
    {
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
        // changes (screen readers read the value interface, e.g. "5 of 8").
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
    // Compact single-row strip of voice indicators for the CURRENT part (one
    // per allocated voice): a small square (filled accent when the voice is
    // active, an outline when free) followed by "V#:<note>" / "V#:--" while the
    // strip is wide enough (a maxed-out 16-cell strip degrades to squares-only
    // rather than truncating the labels into noise). The enclosing Global group
    // panel already supplies the frame, so only a subtle strip fill is painted
    // here. Text follows the active font mode (Console/Serif/Sans). An empty
    // part (no cards / no slots) paints the background only.
    const ParvatiTheme* t = currentTheme();
    const juce::Colour panel     = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const juce::Colour outlineC  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const juce::Colour accent    = t ? t->accentPrimary          : parvati::parvatiFallbackAccent;
    const juce::Colour textValue = t ? t->textPrimary       : juce::Colour (0xffe8e8ee);

    g.setColour (panel);
    g.fillRect (getLocalBounds());

    if (cellCount_ == 0)
        return;

    const juce::Font font = [this]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            return lnf->appFont (14.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (14.0f));
    }();

    constexpr float sq = 8.0f;   // square indicator edge
    constexpr float kMinTextWidth = 44.0f;   // "V#:<note>" legibility floor
    for (int c = 0; c < cellCount_; ++c)
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

        if (r.getWidth() >= kMinTextWidth)
        {
            g.setColour (col);
            g.setFont (font);
            g.drawText (label,
                        r.withTrimmedLeft (sq + 5.0f),
                        juce::Justification::centredLeft, false);
        }
    }
}

void VoiceMeter::layoutCells()
{
    // Even single row across the strip; no separate label column (the active
    // count is shown in the editor's bottom status strip; accessibility exposes
    // it via getActiveVoiceCount()).
    auto area = getLocalBounds().reduced (4);

    const int gap = 6;
    const int totalGap = (cellCount_ - 1) * gap;
    const int cellW = cellCount_ > 0
        ? juce::jmax (8, (area.getWidth() - totalGap) / cellCount_)
        : 0;
    const int cellH = area.getHeight();
    int x = area.getX();
    const int y = area.getY();
    for (int c = 0; c < kMaxCells; ++c)
    {
        if (c < cellCount_)
        {
            cellRects_[(size_t) c] = juce::Rectangle<int> (x, y, cellW, cellH);
            x += cellW + gap;
        }
        else
        {
            cellRects_[(size_t) c] = {};   // beyond the current frame: no cell
        }
    }
}

void VoiceMeter::resized()
{
    layoutCells();
}
