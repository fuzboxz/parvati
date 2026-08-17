// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See VoicePoolView.h.

#include "VoicePoolView.h"

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"

VoicePoolView::VoicePoolView()
{
    startTimerHz (30);

    // Accessibility name/description (read by the default handler).
    setTitle ("Voice pool");
    setDescription ("Voice pool view");
}

VoicePoolView::~VoicePoolView()
{
    stopTimer();
}

void VoicePoolView::visibilityChanged()
{
    // OWN-visibility flips only (this component being shown/hidden directly).
    // They are harmless either way: the EFFECTIVE poll gate is the
    // isShowing() check at the top of timerCallback(). Component::setVisible
    // notifies only the component itself, so an ANCESTOR toggle (the editor
    // hiding the whole Patch page when another top page is selected) never
    // reaches this nested view — visibilityChanged alone cannot gate the
    // poll. Kept so a direct show/hide of the view itself also manages the
    // timer.
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}

void VoicePoolView::setStateProvider (std::function<VoicePoolFrame()> provider)
{
    provider_ = std::move (provider);
}

int VoicePoolView::getActiveVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& r : rows_)
        n += r.activeCount;
    return n;
}

int VoicePoolView::getAllocatedVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& r : rows_)
        n += r.allocated;   // the FULL allocation (CHAIN rows can exceed 16)
    return n;
}

//==========================================================================
// Accessibility: expose the live total active-voice count as a read-only text
// value ("N of 96") so screen readers announce the pool state.
struct VoicePoolView::PoolCountInterface : public juce::AccessibilityTextValueInterface
{
    explicit PoolCountInterface (VoicePoolView& o) : owner (o) {}

    bool isReadOnly() const override { return true; }

    juce::String getCurrentValueAsString() const override
    {
        return juce::String (owner.getActiveVoiceCount()) + " of " + juce::String (kPoolSize);
    }

    void setValueAsString (const juce::String&) override {}   // read-only

    VoicePoolView& owner;
};

std::unique_ptr<juce::AccessibilityHandler> VoicePoolView::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (
        *this,
        juce::AccessibilityRole::group,
        juce::AccessibilityActions{},
        juce::AccessibilityHandler::Interfaces { std::make_unique<PoolCountInterface> (*this) });
}

const ParvatiTheme* VoicePoolView::currentTheme() const noexcept
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    return lnf ? lnf->getTheme() : nullptr;
}

void VoicePoolView::timerCallback()
{
    // The effective poll gate: poll ONLY while actually on-screen. The view
    // lives INSIDE the Patch page's scrolled body, so it is off-screen
    // whenever another top page is active — but that hide toggles an
    // ANCESTOR's visibility, which never fires this view's
    // visibilityChanged() (and isVisible() here is the always-true own flag).
    // isShowing() walks the ancestor chain (and the desktop), so the 30 Hz
    // provider pass — 6 label builds + a 96-voice walk each tick — stops the
    // moment the Patch page is hidden, and resumes when it is shown again.
    // (pollNow(), the test hook, bypasses this gate: a headless test instance
    // has no desktop ancestor, so isShowing() would be permanently false.)
    if (! isShowing())
        return;

    applyFrame();
}

void VoicePoolView::applyFrame()
{
    if (! provider_)
        return;

    const auto frame = provider_();

    // Diff the frame into the rows: a LABEL or SIZE change re-layouts (a
    // card/slot edit or a rename), a count change just repaints. `allocated`
    // keeps the FULL frame size (a CHAIN part's doubled set counts too) so
    // the counts never under-report — see the header contract.
    bool changed   = false;
    bool relayout  = false;
    int  totalActive = 0;

    for (int p = 0; p < kNumParts; ++p)
    {
        auto& row = rows_[(size_t) p];
        const auto& pf = frame.parts[(size_t) p];

        if (row.label != pf.label)
        {
            row.label = pf.label;
            changed = true;
        }

        const int allocated = static_cast<int> (pf.voices.size());
        if (allocated != row.allocated)
        {
            row.allocated = allocated;
            relayout = true;
        }

        // Counts always cover the FULL allocation (a CHAIN row's voices
        // 17..32 count too).
        int act = 0;
        for (const auto& v : pf.voices)
            if (v.active)
                ++act;
        totalActive += act;
        if (row.activeCount != act)
        {
            row.activeCount = act;
            changed = true;
        }
    }

    if (relayout)
    {
        layoutRows();
        changed = true;
    }

    if (changed)
    {
        repaint();

        // Announce the total active count to accessibility clients when it
        // changes (screen readers read the value interface, e.g. "5 of 96").
        if (totalActive != lastAnnouncedCount_)
        {
            lastAnnouncedCount_ = totalActive;
            if (auto* handler = getAccessibilityHandler())
                handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
        }
    }
}

void VoicePoolView::paint (juce::Graphics& g)
{
    // Compact 6-row grid (one row per Part): truncated label (left) | a tiny
    // "active/allocated" count (right). The top band carries the total
    // allocation "X/96". A card-less (0-voice) part stays listed but dimmed,
    // mirroring the Patch page's inactive rows. Text follows the active font
    // mode.
    const ParvatiTheme* t = currentTheme();
    const juce::Colour panel    = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const juce::Colour text     = t ? t->textPrimary     : juce::Colour (0xffe8e8ee);
    const juce::Colour textDim  = t ? t->textSecondary   : juce::Colour (0xff9a9aa8);

    g.setColour (panel);
    g.fillRect (getLocalBounds());

    const juce::Font font = [this]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            return lnf->appFont (11.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (11.0f));
    }();

    // Top band: the pool total, right-aligned (pairs with the Patch page's
    // "Voice pool" caption on the left).
    g.setColour (textDim);
    g.setFont (font);
    g.drawText (juce::String (getAllocatedVoiceCount()) + "/" + juce::String (kPoolSize),
                getLocalBounds().reduced (3).removeFromTop (15),
                juce::Justification::centredRight, false);

    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& row = rows_[(size_t) p];
        // Inactive part: dim the whole row like the Patch page does (still
        // visible so the 6-part structure reads at a glance).
        const float alpha = row.allocated == 0 ? 0.4f : 1.0f;

        juce::String label = row.label;
        // Truncate to ~6 chars + ellipsis: the rows are narrow by design and
        // a name like "Snare" or "Part 3" must stay unambiguous. Done here
        // (paint) — NOT in layout — so timerCallback's raw-label diff stays
        // stable and an unchanged name never repaints the view.
        if (label.length() > 7)
            label = label.substring (0, 6) + juce::String (juce::CharPointer_UTF8 ("\xE2\x80\xA6"));

        g.setColour (textDim.withMultipliedAlpha (alpha));
        g.drawText (label, row.labelRect, juce::Justification::centredLeft, true);

        // A card-less (disabled) part reads "—" rather than a confusing "0/0".
        const juce::String countText = row.allocated == 0
            ? juce::String (juce::CharPointer_UTF8 ("\xE2\x80\x94"))
            : juce::String (row.activeCount) + "/" + juce::String (row.allocated);
        g.setColour ((row.activeCount > 0 ? text : textDim).withMultipliedAlpha (alpha));
        g.drawText (countText, row.countRect, juce::Justification::centredRight, false);
    }
}

void VoicePoolView::layoutRows()
{
    // Fixed geometry (compact by design — the view sits below the hosted
    // Global page inside the Patch page's scrolled body): 3pt inset, a 15pt
    // total band, then six 13pt rows with 1pt gaps (kHeight is the
    // authoritative height budget).
    auto area = getLocalBounds().reduced (3);
    area.removeFromTop (15);
    area.removeFromTop (3);

    constexpr int labelW = 72;   // "~6 chars" of 11pt text + the ellipsis
    constexpr int countW = 44;

    for (int p = 0; p < kNumParts; ++p)
    {
        if (p > 0)
            area.removeFromTop (1);   // 1pt inter-row gap (kHeight budget)
        auto& row = rows_[(size_t) p];
        auto r = area.removeFromTop (13);
        row.rowRect = r;
        row.labelRect = r.removeFromLeft (labelW);
        r.removeFromLeft (6);
        row.countRect = r.removeFromRight (countW);
        r.removeFromRight (6);
    }
}

void VoicePoolView::resized()
{
    layoutRows();
}
