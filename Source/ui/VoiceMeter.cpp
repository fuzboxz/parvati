// Copyright (c) 2024 805LABS / Parvati.  See VoiceMeter.h.

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
    state_.assign (static_cast<size_t> (kNumVoices), VoiceActivity {});
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
    for (const auto& v : state_)
        if (v.active)
            ++n;
    return n;
}

//==========================================================================
// Accessibility: expose the live active-voice count as a read-only text value
// ("N of 16") so screen readers announce the meter state.
struct VoiceMeter::VoiceCountInterface : public juce::AccessibilityTextValueInterface
{
    explicit VoiceCountInterface (VoiceMeter& o) : owner (o) {}

    bool isReadOnly() const override { return true; }

    juce::String getCurrentValueAsString() const override
    {
        return juce::String (owner.getActiveVoiceCount()) + " of " + juce::String (kNumVoices);
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

bool VoiceMeter::isVoicecardBoundary (int voiceIndex)
{
    switch (voiceIndex)
    {
        // Last voice of each firmware voicecard block:
        // vc0={0,1,2} vc1={3,4,5} vc2={6,7,8} vc3={9,10,11} vc4={12,13} vc5={14,15}
        case 2: case 5: case 8: case 11: case 13:
            return true;
        default:
            return false;
    }
}

void VoiceMeter::timerCallback()
{
    if (! provider_)
        return;

    const auto next = provider_();
    const size_t n = static_cast<size_t> (kNumVoices);
    if (next.size() < n)
        return;   // malformed frame; keep the last good state

    bool changed = false;
    for (size_t i = 0; i < n; ++i)
    {
        if (state_[i].active != next[i].active || state_[i].note != next[i].note)
        {
            state_[i] = next[i];
            changed = true;
        }
    }
    if (changed)
    {
        repaint();

        // Announce the active-voice count to accessibility clients when it
        // changes (screen readers read the value interface, e.g. "5 of 16").
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
    const ParvatiTheme* t = currentTheme();
    const juce::Colour panel     = t ? t->panelBackground : juce::Colour (0xff24242e);
    const juce::Colour outlineC  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const juce::Colour accent    = t ? t->accent          : juce::Colour (0xffe8b84b);
    const juce::Colour textDim   = t ? t->textDim         : juce::Colour (0xff9a9aa8);
    const juce::Colour textValue = t ? t->textValue       : juce::Colour (0xffe8e8ee);

    // Bordered panel.
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const float corner = 4.0f;
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, corner);
    g.setColour (outlineC);
    g.drawRoundedRectangle (bounds, corner, 1.0f);

    // ---- Left header: "Voices" + active count ----
    int active = 0;
    for (const auto& v : state_)
        if (v.active)
            ++active;

    if (! labelArea_.isEmpty())
    {
        auto label = labelArea_;   // local copy (removeFromTop mutates)
        g.setColour (textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Voices",
                    label.removeFromTop (label.getHeight() / 2),
                    juce::Justification::centred, false);
        g.setColour (accent);
        g.drawText (juce::String (active) + "/" + juce::String (kNumVoices),
                    label, juce::Justification::centred, false);
    }

    // ---- 16 voice cells ----
    const size_t n = static_cast<size_t> (kNumVoices);
    const float cellCorner = 2.5f;
    for (size_t i = 0; i < n; ++i)
    {
        const auto r = cellRects_[i].toFloat();

        if (state_[i].active)
        {
            g.setColour (accent.withAlpha (0.85f));
            g.fillRoundedRectangle (r, cellCorner);
            g.setColour (accent);
            g.drawRoundedRectangle (r, cellCorner, 1.0f);

            // Note name only if the cell is wide enough to be legible.
            if (r.getWidth() >= 16.0f)
            {
                g.setColour (textValue);
                g.setFont (juce::FontOptions (9.0f));
                g.drawText (midiNoteName (state_[i].note), r,
                            juce::Justification::centred, false);
            }
        }
        else
        {
            // Free voice: a faint outline with a dim centre dot.
            g.setColour (outlineC);
            g.drawRoundedRectangle (r, cellCorner, 1.0f);
            const float dotR = juce::jmin (2.5f, r.getWidth() * 0.18f, r.getHeight() * 0.18f);
            const auto centre = r.getCentre();
            g.setColour (textDim.withAlpha (0.5f));
            g.fillEllipse (centre.x - dotR, centre.y - dotR, dotR * 2.0f, dotR * 2.0f);
        }
    }
}

void VoiceMeter::resized()
{
    auto area = getLocalBounds().reduced (5, 4);

    // Compact header strip on the left ("Voices" + count).
    labelArea_ = area.removeFromLeft (juce::jmin (44, area.getWidth() / 7));
    area.removeFromLeft (6);   // gutter between header and cells

    // Distribute the 16 cells across the remaining width with voicecard-group
    // gaps. Boundary voices (2,5,8,11,13) get the wider group gap.
    const int innerGap = 2;
    const int groupGap = 6;
    const int totalGap = 10 * innerGap + 5 * groupGap;   // 10 inner + 5 group
    const int avail    = area.getWidth() - totalGap;
    const int cellW    = juce::jmax (4, avail / kNumVoices);
    const int cellH    = area.getHeight();
    int x = area.getX();
    const int y = area.getY();

    for (int i = 0; i < kNumVoices; ++i)
    {
        cellRects_[static_cast<size_t> (i)] =
            juce::Rectangle<int> (x, y, cellW, cellH);
        x += cellW + (isVoicecardBoundary (i) ? groupGap : innerGap);
    }
}
