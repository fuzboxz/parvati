// Phase 2 of the "Patch page" feature: the Patch juce::Component implementation.
// See PatchPage.h for the design summary and /tmp/parvati_patch_design.md
// ("Phase 2") for the full spec. Phase 1 (PatchArrangement.{h,cpp}) supplies
// applyArrangement / inferArrangement; this component drives the engine purely
// through its EXISTING public setters.

#include "PatchPage.h"

#include "PatchArrangement.h"

#include "PluginProcessor.h"   // ParvatiAudioProcessor::getEngine()
#include "PluginEditor.h"      // ParamPage complete type (reflowToWidth/getContentHeight)
#include "ui/NoteName.h"       // midiNoteName (key-zone knob readouts)

#include <cstdint>

//==============================================================================
namespace
{
// popcount over the 6-bit voicecard bitmask (a Part's voiceAllocation).
int cardPopcount (uint8_t mask)
{
    int n = 0;
    for (; mask; mask >>= 1)
        n += mask & 1;
    return n;
}

// Alpha applied to an inactive Part row (0 cards) so the split is legible while
// the row stays visible AND interactive (the user can still raise its card count
// to activate it).
constexpr float kInactiveRowAlpha = 0.4f;

// Number of selectable cards per Part / total cards (authentic hardware = 6).
constexpr int kMaxCards = 6;
}  // namespace

//==============================================================================
// One Part row: "Part N" + card-count combo + MIDI-channel combo + key-zone
// knobs (Lo/Hi) + polyphony combo. Every control binds DIRECTLY to the engine's
// existing per-part setters (no APVTS). Inactive parts (0 cards) are dimmed but
// remain visible + interactive.
class PatchPage::PartRow : public juce::Component
{
public:
    PartRow (PatchPage& owner, int partIndex)
        : owner_ (owner), partIndex_ (partIndex), engine_ (owner.proc_.getEngine())
    {
        // ---- chrome ----
        partLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        partLabel_.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (partLabel_);

        // Viewport safety net (T4): a TOUCH drag that starts anywhere on this
        // row must not ALSO scroll the enclosing Viewport — the ignore-drag
        // flag covers the row's combos/knobs/labels too, exactly like the
        // ParamControl cells. Mouse drags never scroll-on-drag anyway (the
        // viewport's default nonHover mode is touch-only).
        setViewportIgnoreDragFlag (true);

        auto setupCaption = [this] (juce::Label& l) {
            l.setJustificationType (juce::Justification::centredLeft);
            l.setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (l);
        };
        setupCaption (cardsCaption_);
        setupCaption (chCaption_);
        setupCaption (zoneLoCaption_);
        setupCaption (zoneHiCaption_);
        setupCaption (polyCaption_);

        // HIG touch target: the DRAWN dropdown stays a compact 24pt strip
        // while each combo's BOUNDS — its tap band — fill the column height
        // (44pt after the 12pt caption band; see resized). The L&F reads this
        // "parvatiComboVisualH" property (drawComboBox /
        // positionComboBoxText), so the rows keep their exact look.
        for (auto* c : { &cardsCombo_, &channelCombo_, &polyCombo_ })
            c->getProperties().set ("parvatiComboVisualH", 24);

        // ---- Cards: count 0..6 (id = count + 1). Sum across rows capped at 6
        // (enforced by PatchPage::recomputeCardAllocation). ----
        for (int n = 0; n <= kMaxCards; ++n)
            cardsCombo_.addItem (juce::String (n), n + 1);
        cardsCombo_.onChange = [this] { onCardsChanged(); };
        addAndMakeVisible (cardsCombo_);

        // ---- Ch: Omni (0) + 1..16 (id = channel + 1). ----
        channelCombo_.addItem (TRANS ("Omni"), 1);
        for (int c = 1; c <= 16; ++c)
            channelCombo_.addItem (juce::String (c), c + 1);
        channelCombo_.onChange = [this] { onChannelChanged(); };
        addAndMakeVisible (channelCombo_);

        // ---- Zone: two compact knobs (Lo/Hi, 0..127). NoTextBox — the L&F
        // draws the readout in the centre of the arc-ring (no value box). ----
        auto setupKnob = [this] (juce::Slider& s) {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (0.0, 127.0, 1.0);
            // Show the MIDI note name ("C4") instead of the raw 0..127 number.
            s.textFromValueFunction = [] (double v) {
                return midiNoteName (juce::roundToInt (v));
            };
            addAndMakeVisible (s);
        };
        setupKnob (loSlider_);
        setupKnob (hiSlider_);
        const auto onZone = [this] { onZoneChanged(); };
        loSlider_.onValueChange = onZone;
        hiSlider_.onValueChange = onZone;

        // ---- Poly: Mono/Poly/Unison 2x/Cyclic/Chain (id = mode + 1).
        // Written via the same idiom PatchArrangement uses (setCurrentPart +
        // applyPartByte(15,mode) + restore), because PartData byte 15 has no
        // dedicated setter. ----
        polyCombo_.addItem (TRANS ("Mono"), 1);
        polyCombo_.addItem (TRANS ("Poly"), 2);
        polyCombo_.addItem (TRANS ("Unison 2x"), 3);
        polyCombo_.addItem (TRANS ("Cyclic"), 4);
        polyCombo_.addItem (TRANS ("Chain"), 5);
        polyCombo_.onChange = [this] { onPolyChanged(); };
        addAndMakeVisible (polyCombo_);

        refreshLanguage();
    }

    //----------------------------------------------------------------------
    // Layout: a horizontal strip of labelled columns.
    void resized() override
    {
        auto b = getLocalBounds().reduced (4);

        partLabel_.setBounds (b.removeFromLeft (62));
        b.removeFromLeft (6);

        // Cards
        {
            // 12pt caption band + the remaining 44pt of the 56pt row = a
            // full-height HIG tap band around the compact 24pt visual box.
            auto col = b.removeFromLeft (92);
            cardsCaption_.setBounds (col.removeFromTop (12));
            cardsCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        // Ch
        {
            auto col = b.removeFromLeft (92);
            chCaption_.setBounds (col.removeFromTop (12));
            channelCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        // Zone Low knob. Caption band 14->12 (the same trick T7 applied to the
        // combo captions) so the 56pt row yields a full 44pt band for the
        // dial — the HIG touch minimum (was a 40px dial in a 42px band).
        {
            auto col = b.removeFromLeft (64);
            zoneLoCaption_.setBounds (col.removeFromTop (12));
            loSlider_.setBounds (col.withSizeKeepingCentre (44, juce::jmin (44, col.getHeight())));
        }
        // Zone High knob (same 44pt band as Zone Low).
        {
            auto col = b.removeFromLeft (64);
            zoneHiCaption_.setBounds (col.removeFromTop (12));
            hiSlider_.setBounds (col.withSizeKeepingCentre (44, juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (8);
        // Poly (remaining width)
        {
            auto col = b;
            polyCaption_.setBounds (col.removeFromTop (12));
            polyCombo_.setBounds (col.withSizeKeepingCentre (juce::jmin (140, col.getWidth()), juce::jmin (44, col.getHeight())));
        }
    }

    //----------------------------------------------------------------------
    // Re-read this Part's engine state into the controls WITHOUT firing onChange.
    void refresh()
    {
        refreshing_ = true;
        const int cardN = cardPopcount (engine_.getPartVoiceAllocation (partIndex_));
        cardsCombo_.setSelectedId (cardN + 1, juce::dontSendNotification);

        channelCombo_.setSelectedId (static_cast<int> (engine_.getPartChannel (partIndex_)) + 1,
                                     juce::dontSendNotification);

        loSlider_.setValue (static_cast<double> (engine_.getPartKeyrangeLow (partIndex_)),
                            juce::dontSendNotification);
        hiSlider_.setValue (static_cast<double> (engine_.getPartKeyrangeHigh (partIndex_)),
                            juce::dontSendNotification);

        const uint8_t poly = engine_.getPart (partIndex_).partBytes[15];   // PartData byte 15 = polyphony
        polyCombo_.setSelectedId (static_cast<int> (poly) + 1, juce::dontSendNotification);

        refreshing_ = false;
        updateDimState();
    }

    // Re-apply every chrome string through TRANS() (called by the editor after a
    // live language switch) and rebuild the channel/poly combo items (the Omni
    // + mode names are translated), preserving each selection.
    void refreshLanguage()
    {
        partLabel_.setText (TRANS ("Part") + " " + juce::String (partIndex_ + 1), juce::dontSendNotification);
        cardsCaption_.setText (TRANS ("Cards"), juce::dontSendNotification);
        chCaption_.setText (TRANS ("Ch"), juce::dontSendNotification);
        zoneLoCaption_.setText (TRANS ("Zone Low"), juce::dontSendNotification);
        zoneHiCaption_.setText (TRANS ("Zone High"), juce::dontSendNotification);
        polyCaption_.setText (TRANS ("Polyphony"), juce::dontSendNotification);

        {
            const int prev = channelCombo_.getSelectedId();
            channelCombo_.clear();
            channelCombo_.addItem (TRANS ("Omni"), 1);
            for (int c = 1; c <= 16; ++c)
                channelCombo_.addItem (juce::String (c), c + 1);
            channelCombo_.setSelectedId (prev, juce::dontSendNotification);
        }
        {
            const int prev = polyCombo_.getSelectedId();
            polyCombo_.clear();
            polyCombo_.addItem (TRANS ("Mono"), 1);
            polyCombo_.addItem (TRANS ("Poly"), 2);
            polyCombo_.addItem (TRANS ("Unison 2x"), 3);
            polyCombo_.addItem (TRANS ("Cyclic"), 4);
            polyCombo_.addItem (TRANS ("Chain"), 5);
            polyCombo_.setSelectedId (prev, juce::dontSendNotification);
        }
        repaint();
    }

    // Colours come from the inherited L&F (read at paint time) — just repaint.
    void applyThemeColors() { repaint(); }

    // The currently-displayed card count (0..6) from the combo.
    int cardCount() const
    {
        const int id = cardsCombo_.getSelectedId();
        return id > 0 ? id - 1 : 0;
    }

    // Highest card count this row's combo currently offers (0..6). With the
    // dynamic per-row cap this is 6 minus the cards used by the OTHER rows, so
    // the GUI never offers a count that would exceed the 6-card total.
    int cardCountMax() const
    {
        const int n = cardsCombo_.getNumItems();
        return n > 0 ? cardsCombo_.getItemId (n - 1) - 1 : 0;
    }

    // Rebuild the card-count combo to offer 0..@p maxCount, then select the
    // engine's actual count for this Part (@p displayCount, clamped into range).
    // Sourcing the selection from the engine (not the combo's stale cardCount())
    // is what keeps the displayed count correct after an arrangement change
    // widens this Part's allocation beyond what its (previously-narrowed) combo
    // still offered. Called by PatchPage::rebuildCardCombos so each row only
    // offers budget-legal counts AND mirrors the engine.
    void rebuildCardItems (int maxCount, int displayCount)
    {
        maxCount = juce::jlimit (0, 6, maxCount);
        refreshing_ = true;
        // dontSendNotification: clear()'s default is sendNotificationAsync,
        // which (with a non-zero selection) arms a DEFERRED onChange that fires
        // after refreshing_ has already been reset to false — re-entering
        // onCardsChanged -> recompute -> rebuild -> clear() ... an infinite
        // async ping-pong that permanently saturated one core at idle. The
        // synchronous refresh below sets the selection itself, so no change
        // signal is needed here.
        cardsCombo_.clear (juce::dontSendNotification);
        for (int n = 0; n <= maxCount; ++n)
            cardsCombo_.addItem (juce::String (n), n + 1);
        cardsCombo_.setSelectedId (juce::jlimit (0, maxCount, displayCount) + 1,
                                   juce::dontSendNotification);
        refreshing_ = false;
    }

    // Test/automation hook: set the card-count combo to @p n WITHOUT firing
    // onChange, clamped to what the combo currently offers (a row whose budget
    // is spent offers only 0, so asking for more yields 0 — exactly as a user
    // who can only pick from the offered items). PatchPage::chooseCardCount
    // then drives the cap-check + write path explicitly (JUCE does not fire a
    // combo's onChange for a programmatic setSelectedId in a headless test).
    void setCardCountDisplay (int n)
    {
        n = juce::jlimit (0, cardCountMax(), n);
        cardsCombo_.setSelectedId (n + 1, juce::dontSendNotification);
    }

    // Revert the card combo to the engine's actual count (used when an edit
    // would push the total over 6). No onChange fired.
    void revertCardDisplay()
    {
        const int n = cardPopcount (engine_.getPartVoiceAllocation (partIndex_));
        refreshing_ = true;
        cardsCombo_.setSelectedId (n + 1, juce::dontSendNotification);
        refreshing_ = false;
    }

    // Dim the row when its Part has 0 cards (inactive). setAlpha keeps the row
    // visible AND interactive so the user can still raise its card count.
    void updateDimState()
    {
        const int n = cardPopcount (engine_.getPartVoiceAllocation (partIndex_));
        setAlpha (n == 0 ? kInactiveRowAlpha : 1.0f);
    }

private:
    PatchPage& owner_;
    const int partIndex_;
    SynthEngine& engine_;
    bool refreshing_ = false;

    juce::Label partLabel_, cardsCaption_, chCaption_, zoneLoCaption_, zoneHiCaption_, polyCaption_;
    juce::ComboBox cardsCombo_, channelCombo_, polyCombo_;
    juce::Slider loSlider_, hiSlider_;

    void onCardsChanged()
    {
        if (refreshing_) return;
        owner_.recomputeCardAllocation (partIndex_);
    }

    void onChannelChanged()
    {
        if (refreshing_) return;
        engine_.setPartMidiChannel (partIndex_, channelCombo_.getSelectedId() - 1);
        owner_.postPartEdit();
    }

    void onZoneChanged()
    {
        if (refreshing_) return;
        engine_.setPartKeyZone (partIndex_,
                                static_cast<int> (loSlider_.getValue()),
                                static_cast<int> (hiSlider_.getValue()));
        owner_.postPartEdit();
    }

    void onPolyChanged()
    {
        if (refreshing_) return;
        // Same idiom PatchArrangement uses: PartData byte 15 has no dedicated
        // setter, so switch currentPart, write byte 15, then restore.
        const int saved = engine_.getCurrentPart();
        engine_.setCurrentPart (partIndex_);
        engine_.applyPartByte (15, static_cast<uint8_t> (polyCombo_.getSelectedId() - 1));
        engine_.setCurrentPart (saved);
        owner_.postPartEdit();
        // The polyphony knob in the hosted globalPage_ reads the CURRENT part's
        // value from the APVTS, which goes stale after this engine-direct write.
        // Re-sync the current part (engine->APVTS) so the knob + a save stay
        // correct (same staleness reason as the arrangement-combo path above).
        owner_.proc_.loadPartIntoApvts (engine_.getCurrentPart());
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PartRow)
};

//==============================================================================
// The vertically-scrolled body of the Patch page: the 6 Part rows + the hosted
// patch-wide ParamPage. A plain Component that paints the page background — a
// juce::Viewport has no background of its own, and the body is grown to at
// least the view height in layoutScrollBody() so a fitting body leaves no
// unpainted tail below the rows.
class PatchPage::ScrollBody : public juce::Component
{
public:
    explicit ScrollBody (PatchPage& owner) : owner_ (owner) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (owner_.themeManager_.getCurrentTheme().backgroundBase);
    }

private:
    PatchPage& owner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrollBody)
};

//==============================================================================
PatchPage::PatchPage (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : proc_ (processor), themeManager_ (themeManager),
      scrollBody_ (std::make_unique<ScrollBody> (*this))
{
    heading_.setText (TRANS ("Patch"), juce::dontSendNotification);
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    addAndMakeVisible (heading_);

    buildArrangementCombo();
    arrangementCombo_.onChange = [this] { onArrangementChanged(); };
    // HIG touch target: 26pt visual box inside a 44pt tap band (see resized).
    arrangementCombo_.getProperties().set ("parvatiComboVisualH", 26);
    addAndMakeVisible (arrangementCombo_);

    cardsTotalLabel_.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    cardsTotalLabel_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accentPrimary);
    cardsTotalLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (cardsTotalLabel_);

    for (int i = 0; i < kMaxCards; ++i)
    {
        rows_[ (size_t) i] = std::make_unique<PartRow> (*this, i);
        scrollBody_->addAndMakeVisible (*rows_[ (size_t) i]);
    }

    // T4 scroll safety net: the rows + the hosted global page scroll vertically
    // inside a Viewport. At the tuned design size the body fits (it is grown to
    // the view height — no scrollbar, no layout change vs the old direct
    // layout); only in a short host frame does the vertical scrollbar appear,
    // turning previously unreachable clipped rows/page into reachable scrolled
    // content. Mouse drags never scroll-on-drag (default nonHover mode is
    // touch-only); the mouse WHEEL scrolls (knob wheels are disabled and juce
    // bubbles an unhandled wheel up to the Viewport).
    viewport_.setScrollBarsShown (true, false, false, false);   // vertical-only, shown only when the body overflows
    viewport_.setViewedComponent (scrollBody_.get(), false);    // body is member-owned, not view-owned
    addAndMakeVisible (viewport_);

    setSize (640, 540);
    refresh();
}

PatchPage::~PatchPage() = default;

void PatchPage::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

void PatchPage::applyThemeColors()
{
    const auto accent = themeManager_.getCurrentTheme().accentPrimary;
    heading_.setColour (juce::Label::textColourId, accent);
    cardsTotalLabel_.setColour (juce::Label::textColourId, accent);
    repaint();
}

void PatchPage::refreshLanguage()
{
    heading_.setText (TRANS ("Patch"), juce::dontSendNotification);
    buildArrangementCombo();
    for (auto& r : rows_)
        r->refreshLanguage();
    repaint();
}

void PatchPage::buildArrangementCombo()
{
    const int prev = arrangementCombo_.getSelectedId();
    arrangementCombo_.clear();
    arrangementCombo_.setTextWhenNothingSelected (TRANS ("Custom"));
    arrangementCombo_.addItem (TRANS ("Single"), 1);
    arrangementCombo_.addItem (TRANS ("Stack"), 2);
    arrangementCombo_.addItem (TRANS ("Split 2"), 3);
    arrangementCombo_.addItem (TRANS ("Layer 2"), 4);
    arrangementCombo_.addItem (TRANS ("Multi 6"), 5);
    arrangementCombo_.setSelectedId (prev, juce::dontSendNotification);
}

void PatchPage::setArrangementFromEngine()
{
    const Arrangement a = inferArrangement (proc_.getEngine());
    refreshing_ = true;
    if (a == Arrangement::Custom)
        arrangementCombo_.setSelectedId (0, juce::dontSendNotification);   // shows "Custom"
    else
        arrangementCombo_.setSelectedId (static_cast<int> (a) + 1, juce::dontSendNotification);
    refreshing_ = false;
}

void PatchPage::onArrangementChanged()
{
    if (refreshing_) return;
    const int id = arrangementCombo_.getSelectedId();
    if (id < 1 || id > 5) return;
    applyArrangement (proc_.getEngine(), static_cast<Arrangement> (id - 1));
    refresh();
    // applyArrangement writes each part's polyphony ENGINE-DIRECT (setCurrentPart
    // + applyPartByte(15,...)). The hosted globalPage_ knob for `part_polyphony`
    // reads from the APVTS, so the current part's APVTS value goes stale after an
    // apply. Re-sync the current part's engine state into the APVTS (existing
    // public machinery) so the knob + an APVTS-based save reflect the new mode.
    proc_.loadPartIntoApvts (proc_.getEngine().getCurrentPart());
}

int PatchPage::getDisplayedCardCount (int part) const
{
    if (part < 0 || part >= kMaxCards) return -1;
    return rows_[(size_t) part]->cardCount();
}

Arrangement PatchPage::getDisplayedArrangement() const
{
    const int id = arrangementCombo_.getSelectedId();
    return (id >= 1 && id <= 5) ? static_cast<Arrangement> (id - 1) : Arrangement::Custom;
}

void PatchPage::chooseCardCount (int part, int count)
{
    if (part < 0 || part >= kMaxCards || count < 0 || count > kMaxCards) return;
    // Mirror a user edit: set the combo to the requested count, then run the
    // exact cap-check + contiguous-bitmask write path (recomputeCardAllocation).
    rows_[(size_t) part]->setCardCountDisplay (count);
    recomputeCardAllocation (part);
}

int PatchPage::getCardCountMax (int part) const
{
    if (part < 0 || part >= kMaxCards) return -1;
    return rows_[(size_t) part]->cardCountMax();
}

void PatchPage::rebuildCardCombos()
{
    // Source of truth = the ENGINE, not the combos: a row's combo may still hold
    // a stale/narrowed selection from a previous arrangement (e.g. only "0"), so
    // reading cardCount() here would compute the per-row caps against the wrong
    // total and leave the displayed count stuck. Reading the engine popcounts
    // makes both the caps and the displayed counts always track the engine.
    int counts[kMaxCards] {};
    int total = 0;
    for (int p = 0; p < kMaxCards; ++p)
    {
        counts[p] = cardPopcount (proc_.getEngine().getPartVoiceAllocation (p));
        total += counts[p];
    }

    // Each row may offer 0..(6 - cards used by the OTHER rows), so the GUI never
    // offers a count that would exceed the 6-card total, and each row's selection
    // mirrors its engine allocation.
    for (int p = 0; p < kMaxCards; ++p)
    {
        const int usedByOthers = total - counts[p];
        const int maxForThis = juce::jlimit (0, kMaxCards, kMaxCards - usedByOthers);
        rows_[(size_t) p]->rebuildCardItems (maxForThis, counts[p]);
    }
}

void PatchPage::updateCardsTotal()
{
    int used = 0;
    for (int p = 0; p < kMaxCards; ++p)
        used += cardPopcount (proc_.getEngine().getPartVoiceAllocation (p));
    cardsTotalLabel_.setText (TRANS ("Cards") + " " + juce::String (used) + "/"
                                  + juce::String (kMaxCards),
                              juce::dontSendNotification);
}

void PatchPage::refresh()
{
    refreshing_ = true;
    for (auto& r : rows_)
        r->refresh();
    refreshing_ = false;
    rebuildCardCombos();
    updateCardsTotal();
    setArrangementFromEngine();
}

void PatchPage::postPartEdit()
{
    for (auto& r : rows_)
        r->updateDimState();
    rebuildCardCombos();
    updateCardsTotal();
    setArrangementFromEngine();
}

void PatchPage::recomputeCardAllocation (int changedPart)
{
    if (refreshing_) return;

    // Read all 6 counts from the combos (the just-edited one already holds its
    // new value).
    int counts[kMaxCards] {};
    int sum = 0;
    for (int p = 0; p < kMaxCards; ++p)
    {
        counts[p] = rows_[ (size_t) p]->cardCount();
        sum += counts[p];
    }

    if (sum > kMaxCards)
    {
        // Reject: the edit would exceed the 6-card total. Revert the changed
        // row's display to the engine's actual (pre-edit) count; the engine is
        // untouched. The displayed counts therefore always satisfy sum <= 6.
        rows_[ (size_t) changedPart]->revertCardDisplay();
        return;
    }

    // Recompute the 6 contiguous bitmasks in part order (part 0 takes the first
    // count0 cards, part 1 the next count1, ...) and write ALL of them.
    //
    // Deviation from the spec's "for each changed part" hint: contiguous
    // reassignment shifts the card cursor for EVERY part from the first changed
    // one onward, so a part whose own count is unchanged can still need a
    // different bitmask position. Comparing new vs old masks is therefore unsafe
    // under the engine's exclusive-ownership steal. Writing all 6 disjoint
    // contiguous masks in order is the simplest correct contract (each write is
    // idempotent for a disjoint mask), and is exactly what applyArrangement does.
    auto& engine = proc_.getEngine();
    const int saved = engine.getCurrentPart();
    int cursor = 0;
    for (int p = 0; p < kMaxCards; ++p)
    {
        uint8_t mask = 0;
        for (int c = 0; c < counts[p]; ++c)
            mask |= static_cast<uint8_t> (1u << (cursor + c));
        cursor += counts[p];
        engine.setPartVoiceAllocation (p, mask);
    }
    engine.setCurrentPart (saved);

    postPartEdit();
}

void PatchPage::resized()
{
    auto area = getLocalBounds().reduced (16);

    heading_.setBounds (area.removeFromTop (30));
    // 10pt (not 6): the arrangement combo's 44pt tap band is centred on the
    // 26pt top row and reaches 9pt above it — a 6pt gap made it clip 3pt into
    // the heading (R3 sibling-overlap).
    area.removeFromTop (10);
    {
        // The top row is a 26pt band; the arrangement combo's TAP band is
        // grown to 44pt centred on it (transparent padding into the gaps
        // above/below — the 26pt visual box via "parvatiComboVisualH" — so no
        // sibling moves and the page height is unchanged).
        auto topRow = area.removeFromTop (26);
        arrangementCombo_.setBounds (topRow.removeFromLeft (220)
                                         .withSizeKeepingCentre (220, 44));
        topRow.removeFromLeft (12);
        cardsTotalLabel_.setBounds (topRow.removeFromLeft (170));
    }
    area.removeFromTop (10);

    // Everything below the fixed header chrome (rows + hosted page) scrolls
    // vertically inside the Viewport when it overflows (T4 safety net).
    viewport_.setBounds (area);
    layoutScrollBody();
}

void PatchPage::layoutScrollBody()
{
    const int vw = viewport_.getWidth();
    const int vh = viewport_.getHeight();
    if (vw <= 0 || vh <= 0)
        return;

    // Lay the body out at the given width; returns the natural height used
    // (rows first, then the hosted page reflowed to the same width — exactly
    // the old direct layout, just expressed in the body's local coordinates).
    auto layoutAtWidth = [this] (int cw)
    {
        constexpr int rowH = 56;
        constexpr int rowGap = 4;
        int y = 0;
        for (int i = 0; i < kMaxCards; ++i)
        {
            rows_[ (size_t) i]->setBounds (0, y, cw, rowH);
            y += rowH + rowGap;
        }
        if (hostedParamPage_ != nullptr)
        {
            y += 8;   // breathing room below the last row
            hostedParamPage_->reflowToWidth (juce::jmax (200, cw), 0);
            hostedParamPage_->setBounds (0, y, cw,
                                         juce::jmax (200, hostedParamPage_->getContentHeight()));
            y += hostedParamPage_->getHeight();
        }
        return y;
    };

    // Full view width first: a body that FITS keeps the old direct layout (no
    // scrollbar, no width change). Only an overflowing body is re-laid one
    // scrollbar-thickness narrower so the vertical scrollbar never covers the
    // right edge (the same pattern as the workspace active-editor hosts /
    // FxMatrixView).
    int cw = vw;
    int totalH = layoutAtWidth (cw);
    if (totalH > vh)
    {
        cw = juce::jmax (150, vw - viewport_.getScrollBarThickness());
        totalH = layoutAtWidth (cw);
    }

    // Grow to at least the view height so a fitting body still paints the page
    // background over the whole viewport area (no foreign colour below).
    scrollBody_->setSize (cw, juce::jmax (totalH, vh));
}

void PatchPage::hostParamPage (juce::Component* paramPage)
{
    hostedParamPage_ = dynamic_cast<ParamPage*> (paramPage);
    // The hosted page lives INSIDE the scrolled body so it scrolls together
    // with the rows (T4). Editor retains ownership; reparent only.
    if (hostedParamPage_ != nullptr)
        scrollBody_->addAndMakeVisible (hostedParamPage_);
    resized();
}
