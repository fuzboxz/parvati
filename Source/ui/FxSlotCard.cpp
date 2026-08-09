// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotCard.h.

#include "FxSlotCard.h"

#include "FxSlotVisualizer.h"
#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"
#include "PluginEditor.h"          // ParamControl complete type
#include "PluginProcessor.h"       // ParvatiAudioProcessor::getApvts()
#include "ParameterLayout.h"       // PatchParamDescriptor
#include "dsp/fx/FxTypes.h"        // FxType

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
//==============================================================================
// PowerToggle — the Enable/Bypass button. Draws the IEC 5009 power glyph (a
// vertical bar rising into an open-topped arc) with juce::Path, so there is NO
// unicode/font dependency (mirrors IconButton's glyph recipe). The glyph reads
// accentSecondary (the orange FX/bypass accent) when the slot is ENABLED and
// reads dimmed (textDisabled) when bypassed. Its toggle STATE is driven from the
// fx{N}_enable APVTS Value by FxSlotCard; clicking flips that Value (handled by
// the card's onClick), so this button does NOT toggle its own state on click.
class PowerToggle : public juce::Button
{
public:
    PowerToggle() : juce::Button ({}) { setClickingTogglesState (false); }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        const ParvatiTheme* t = nullptr;
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            t = lnf->getTheme();

        const juce::Colour accent = t ? t->accentSecondary : juce::Colour (0xffe8b84b);
        const juce::Colour text   = t ? t->textPrimary     : juce::Colour (0xffe8e8ee);
        const juce::Colour dim    = t ? t->textDisabled    : text.withAlpha (0.35f);

        juce::Colour c = (getToggleState() || isButtonDown) ? accent : dim;
        if (! getToggleState() && isMouseOverButton)
            c = text.brighter (0.20f);
        if (! isEnabled())
            c = text.withAlpha (0.25f);

        g.setColour (c);

        const auto r = getLocalBounds().toFloat().reduced (3.0f);
        const auto c2 = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;

        // Open-topped arc (~270 deg, gap at the top): two quadratic beziers that
        // leave a vertical slit through which the central bar rises.
        const float leftX  = c2.x - rad * 0.92f;
        const float rightX = c2.x + rad * 0.92f;
        const float topY   = c2.y - rad * 1.15f;
        const float botY   = c2.y + rad * 1.05f;

        juce::Path arc;
        arc.startNewSubPath (leftX, botY);
        arc.quadraticTo (c2.x - rad * 1.30f, c2.y, leftX, topY);          // lower-left -> upper-left
        arc.startNewSubPath (rightX, botY);
        arc.quadraticTo (c2.x + rad * 1.30f, c2.y, rightX, topY);         // lower-right -> upper-right
        g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));

        // Central vertical bar from the centre up to the top of the arc slit.
        g.drawLine (juce::Line<float> (c2.x, c2.y + rad * 0.35f, c2.x, topY), 2.2f);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerToggle)
};

//==============================================================================
// Per-type active-parameter count + semantic short labels (the labels that
// replace the static "FX{N} Param K" descriptor text on the live knobs). idx is
// 0..3 (param1..4). Inactive params (idx >= activeParamCount) are hidden.
int activeParamCount (FxType t) noexcept
{
    switch (t)
    {
        case FxType::GainPan: return 2;
        case FxType::Delay:   return 3;
        case FxType::Reverb:  return 4;
        case FxType::Chorus:  return 2;
        case FxType::None:
        case FxType::Count:   return 0;
    }
    return 0;   // unreachable; keeps -Wreturn-type calm
}

const char* paramLabel (FxType t, int idx) noexcept
{
    switch (t)
    {
        case FxType::GainPan:
            if (idx == 0) return "Gain";
            if (idx == 1) return "Pan";
            break;
        case FxType::Delay:
            if (idx == 0) return "Time";
            if (idx == 1) return "Feedback";
            if (idx == 2) return "Spread";
            break;
        case FxType::Reverb:
            if (idx == 0) return "Size";
            if (idx == 1) return "Damp";
            if (idx == 2) return "Level";
            if (idx == 3) return "Width";
            break;
        case FxType::Chorus:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            break;
        case FxType::None:
        case FxType::Count:
            break;
    }
    return "-";
}

// Per-type ENGAGEMENT defaults applied the moment a user selects an effect
// type. The generic slot params all default to 0 — silent (Delay time=0,
// drywet=0 = fully dry, enabled=0 = bypassed) — so picking a type otherwise
// sounds like "nothing happened". These seed each effect with an audible,
// characteristic starting point. Applied from parameterChanged() ONLY on a real
// fx{N}_type change (never at construction). Because APVTS listener dispatch is
// synchronous on the message thread AND the descriptor order is type, enabled,
// drywet, param1..4, a preset/part load that sets type THEN params overrides
// these — so saved patches keep their own values.
struct FxTypeDefaults { uint8_t enabled; uint8_t drywet; uint8_t p[4]; };
FxTypeDefaults fxTypeDefaults (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Delay:   return { 1,  80, { 50, 50, 40,  0 } }; // Time / Feedback / Spread
        case FxType::Reverb:  return { 1,  80, { 64, 50, 64, 64 } }; // Size / Damp / Level / Width
        case FxType::Chorus:  return { 1,  80, { 50, 64,  0,  0 } }; // Rate / Depth
        case FxType::GainPan: return { 1, 127, { 64, 64,  0,  0 } }; // Gain(0 dB) / Pan(centre)
        case FxType::None:
        case FxType::Count:   break;
    }
    return { 0, 0, { 0, 0, 0, 0 } };
}

// Layout constants (px). The card sits in the FX top row (~261..271px tall at
// the 600..620px editor height). The layout mirrors the synth OSC/Mixer/Filter
// sections: a header band, a STYLED algorithm dropdown (Shape/Mode parity), a
// COMPACT visualizer band (synth decoration parity, <= kVisMax), and a
// Mixer-style 3-column knob GRID (kCellH = the synth cell height). Fixed
// header / dropdown / visualizer heights keep the three cards' baselines
// aligned; the grid centres vertically in its remainder so a short row-set
// reads balanced + spacious.
constexpr int kPad         = 6;      // card edge inset
constexpr int kHeaderH     = 16;     // header row (title + power toggle; synth kGroupTitleH parity)
constexpr int kHalfGap     = 2;      // gap below the header + between bands
constexpr int kTypeRowH    = 28;     // algorithm dropdown row (styled combo height, Shape/Mode parity)
constexpr int kComboH      = 28;     // dropdown height (ParamControl combo parity)
constexpr int kComboChrome = 26;     // fit-to-text chrome: pad + amber chevron + slack
constexpr int kComboMinW   = 80;     // dropdown floor width
constexpr int kGridCols    = 3;      // knob grid column count (Mixer parity)
constexpr int kCellH       = 64;     // knob cell height (synth cellH parity: label band + 52px dial)
constexpr int kVisMin      = 40;     // visualizer band floor (legible at the min size)
constexpr int kVisMax      = 80;     // visualizer band cap (synth kDecorationH parity)
// Bypass affordance: a bypassed slot's live controls (knobs + visualizer + type
// combo) are recessed to this alpha so the slot reads as inactive at a glance
// (0.5 matches the synth GroupComponent / knob disabled alpha).
constexpr float kBypassedAlpha = 0.5f;
constexpr float kCorner        = 7.0f; // card panel corner radius (synth GroupComponent parity)

// Fit-to-text width of a ComboBox's longest item, measured in the active
// LookAndFeel combo font (mirrors ParamControl::maxChoiceTextWidth) so the FX
// type dropdown sizes itself exactly like the Osc "Shape" / Filter "Mode"
// selectors (widest choice + kComboChrome, centred in its row).
int maxComboItemWidth (const juce::ComboBox& combo)
{
    const auto f = [&]() -> juce::Font
    {
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&combo.getLookAndFeel()))
            return lnf->appFont (14.0f, juce::Font::plain);
        return juce::Font (juce::FontOptions (14.0f));
    }();

    int widest = 0;
    for (int i = 0; i < combo.getNumItems(); ++i)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo.getItemText (i)));
    widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo.getText()));
    return widest;
}

// Knob-grid column count for a given visible-knob count. Delay (4) / Reverb (5)
// use the full 3 columns (the approved 2-row look); a 3-knob type (Chorus /
// Gain-Pan) drops to 2 columns so it forms a 2-row grid instead of a single
// sparse row; a lone knob (None => Mix only) gets 1 column. Kept in one place so
// resized()'s row/height budget and layoutParamGrid()'s placement stay in sync.
int knobGridCols (int count) noexcept
{
    if (count <= 1) return 1;
    if (count <= 3) return 2;
    return kGridCols;
}
} // namespace

//==============================================================================
FxSlotCard::FxSlotCard (ParvatiAudioProcessor& processor, int slot,
                        const PatchParamDescriptor* p1Desc, const PatchParamDescriptor* p2Desc,
                        const PatchParamDescriptor* p3Desc, const PatchParamDescriptor* p4Desc,
                        const PatchParamDescriptor* drywetDesc)
    : processor_ (processor),
      slot_ (juce::jlimit (0, 2, slot)),
      prefix_ ("fx" + juce::String (slot_ + 1) + "_")
{
    // ---- Five full ParamControl knobs (modulation-destination parity) ----
    if (p1Desc     != nullptr) p1_     = std::make_unique<ParamControl> (processor_, *p1Desc);
    if (p2Desc     != nullptr) p2_     = std::make_unique<ParamControl> (processor_, *p2Desc);
    if (p3Desc     != nullptr) p3_     = std::make_unique<ParamControl> (processor_, *p3Desc);
    if (p4Desc     != nullptr) p4_     = std::make_unique<ParamControl> (processor_, *p4Desc);
    if (drywetDesc != nullptr) drywet_ = std::make_unique<ParamControl> (processor_, *drywetDesc);

    for (auto* pc : { p1_.get(), p2_.get(), p3_.get(), p4_.get(), drywet_.get() })
        if (pc != nullptr)
            addAndMakeVisible (*pc);

    // ---- Type combo (ComboBoxAttachment auto-uses the param's choices) ----
    typeCombo_ = std::make_unique<juce::ComboBox> ();
    typeCombo_->setTooltip ("FX " + juce::String (slot_ + 1) + " algorithm");
    // Populate from the param's OWN choice list BEFORE the attachment:
    // AudioProcessorValueTreeState::ComboBoxAttachment does NOT add items itself,
    // so without this the dropdown is empty and nothing is selectable.
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (processor_.getApvts ().getParameter (prefix_ + "type")))
        typeCombo_->addItemList (p->choices, 1);
    addAndMakeVisible (*typeCombo_);
    typeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor_.getApvts (), prefix_ + "type", *typeCombo_);

    // ---- Power/bypass toggle (bound to the 0..1 enable Int via a Value) ----
    powerToggle_ = std::make_unique<PowerToggle> ();
    powerToggle_->setTooltip ("FX " + juce::String (slot_ + 1) + " enable / bypass");
    addAndMakeVisible (*powerToggle_);
    powerToggle_->onClick = [this]
    {
        // Flip the enable param: 0 <-> 1. The Value listener writes the new
        // toggle state back onto the button for display.
        const bool on = ! powerToggle_->getToggleState();
        enabledValue_ = on ? 1.0f : 0.0f;
    };

    // ---- APVTS parameter listeners (type + enable) ----
    // addParameterListener fires on ANY change (combo edit, host automation,
    // preset load) — a Value::Listener on a separate getParameterAsValue instance
    // does NOT, so without this the knob visible-set + power state would freeze
    // at construction. type is read LIVE in currentTypeIndex() via getParameter.
    enabledValue_ = processor.getApvts ().getParameterAsValue (prefix_ + "enabled");
    processor.getApvts ().addParameterListener (prefix_ + "type", this);
    processor.getApvts ().addParameterListener (prefix_ + "enabled", this);

    // ---- Visualizer (normalized APVTS getters) ----
    const auto prefixStr = prefix_;
    auto norm = [&processor, prefixStr] (const char* tail) -> float
    {
        auto* p = processor.getApvts ().getParameter (prefixStr + tail);
        return p != nullptr ? p->getValue () : 0.0f;
    };
    visualizer_ = std::make_unique<FxSlotVisualizer> (
        [norm] { return norm ("type"); },
        [norm] { return norm ("param1"); },
        [norm] { return norm ("param2"); },
        [norm] { return norm ("param3"); },
        [norm] { return norm ("param4"); },
        [norm] { return norm ("drywet"); });
    addAndMakeVisible (*visualizer_);

    // Initial knob visible set + semantic labels + power state.
    refreshFromType();
    refreshEnabled();
}

FxSlotCard::~FxSlotCard()
{
    // Detach BEFORE the AsyncUpdater base cancels its pending update, so no
    // in-flight parameterChanged can reach a half-destroyed card.
    processor_.getApvts ().removeParameterListener (prefix_ + "type", this);
    processor_.getApvts ().removeParameterListener (prefix_ + "enabled", this);
}

//==============================================================================
int FxSlotCard::currentTypeIndex() const
{
    // Read the LIVE parameter value (a cached Value would not track external
    // changes such as preset load / host automation).
    auto* p = processor_.getApvts ().getParameter (prefix_ + "type");
    const float v = p != nullptr ? p->getValue () : 0.0f;
    // fx{N}_type is an AudioParameterChoice: getValue() is NORMALIZED 0..1, so
    // scale it back to the choice index (0..Count-1). Without this, e.g. Reverb
    // (idx 3, normalized 0.75) would read as idx 1 (GainPan) -> wrong knobs.
    // Mirrors FxSlotVisualizer::typeIndex().
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    return juce::jlimit (0, kLast, juce::roundToInt (v * static_cast<float> (kLast)));
}

void FxSlotCard::refreshFromType()
{
    const auto t = static_cast<FxType> (currentTypeIndex());
    const int active = activeParamCount (t);

    ParamControl* params[4] = { p1_.get(), p2_.get(), p3_.get(), p4_.get() };
    for (int i = 0; i < 4; ++i)
    {
        auto* pc = params[i];
        if (pc == nullptr)
            continue;
        // Relabel to the active algorithm's semantic name (or revert to the
        // descriptor label for an inactive param so a future show is correct).
        pc->setDisplayLabel (i < active ? juce::String (paramLabel (t, i)) : juce::String());
    }
    if (drywet_ != nullptr)
        drywet_->setDisplayLabel ("Mix");

    resized();   // reflow the visible-set + anchored dry/wet
}

void FxSlotCard::refreshEnabled()
{
    // Read the LIVE enable value (0..1 Int param).
    auto* p = processor_.getApvts ().getParameter (prefix_ + "enabled");
    const bool on = (p != nullptr ? juce::roundToInt (p->getValue ()) : 0) != 0;
    if (powerToggle_ != nullptr)
        powerToggle_->setToggleState (on, juce::dontSendNotification);

    // Bypass affordance: recess the LIVE controls (knobs + visualizer + type
    // combo) when the slot is bypassed, so a disabled slot reads as inactive at a
    // glance — without it a bypassed slot's knobs stay full-brightness and look
    // live. NON-colour (alpha only): the panel / title / power glyph keep full
    // alpha so the bypass state + slot identity stay legible. setAlpha is
    // compositing-only (it does NOT disable interaction), so the values remain
    // editable even while bypassed.
    const float contentAlpha = on ? 1.0f : kBypassedAlpha;
    juce::Component* content[] = { p1_.get(), p2_.get(), p3_.get(), p4_.get(),
                                   drywet_.get(), visualizer_.get(), typeCombo_.get() };
    for (auto* c : content)
        if (c != nullptr)
            c->setAlpha (contentAlpha);
}

void FxSlotCard::parameterChanged (const juce::String& id, float /*newValue*/)
{
    if (id != prefix_ + "type" && id != prefix_ + "enabled")
        return;
    // On the message thread refresh immediately (so a synchronous render / preset
    // load is reflected at once); otherwise defer to the message thread.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || ! mm->isThisTheMessageThread())
    {
        triggerAsyncUpdate();
        return;
    }

    // Selecting an effect TYPE (None -> effect, or effect -> effect): seed the
    // slot with that effect's audible per-type defaults so it is immediately
    // engaging (enabled + a characteristic wet level + sensible params). Writing
    // these re-fires parameterChanged(enabled) synchronously (harmless: just
    // refreshes the toggle); param writes fire no card listener. A preset/part
    // load sets type BEFORE these params (descriptor order), so loaded values
    // override the defaults — saved patches are preserved.
    if (id == prefix_ + "type")
    {
        const auto t = static_cast<FxType> (currentTypeIndex());
        if (t != FxType::None)
        {
            const auto d = fxTypeDefaults (t);
            auto& apvts  = processor_.getApvts();
            apvts.getParameterAsValue (prefix_ + "enabled") = (float) d.enabled;
            apvts.getParameterAsValue (prefix_ + "drywet")  = (float) d.drywet;
            for (int k = 0; k < 4; ++k)
                apvts.getParameterAsValue (prefix_ + "param" + juce::String (k + 1)) = (float) d.p[k];
        }
    }

    refreshFromType();
    refreshEnabled();
}

void FxSlotCard::handleAsyncUpdate()
{
    refreshFromType();
    refreshEnabled();
}

//==============================================================================
void FxSlotCard::layoutParamGrid (const juce::Rectangle<int>& gridArea)
{
    if (gridArea.isEmpty())
        return;

    const auto t = static_cast<FxType> (currentTypeIndex());
    const int active = activeParamCount (t);

    ParamControl* params[4] = { p1_.get(), p2_.get(), p3_.get(), p4_.get() };

    // The knob sequence: ACTIVE algorithm params (param1..N) in row-major order,
    // then the Mix (dry/wet) knob as the LAST cell (bottom-right for Reverb /
    // Delay). Count varies by type: None=1 (Mix only), GainPan/Chorus=3,
    // Delay=4, Reverb=5.
    juce::Array<ParamControl*> knobs;
    for (int i = 0; i < active; ++i)
        if (params[i] != nullptr)
            knobs.add (params[i]);
    if (drywet_ != nullptr)
        knobs.add (drywet_.get());

    // Hide every owned knob NOT in the grid (inactive algorithm params).
    for (auto* pc : params)
        if (pc != nullptr && knobs.indexOf (pc) < 0)
            pc->setVisible (false);

    const int count = knobs.size();
    if (count <= 0)
        return;

    // Mixer-style grid. Columns adapt to the knob count (3 for Delay/Reverb,
    //    2 for Chorus/Gain-Pan so they read as a 2-row grid, not a single row):
    //    every multi-knob type lands on ~2 rows. Equal-width cells; the rightmost
    // the rightmost column absorbs the integer remainder so the grid fills the
    // full width. Cell height is the synth kCellH parity (label band + 52px
    // dial, centred by ParamControl), shrunk only if the grid region is shorter
    // than rows * kCellH. The knob block is centred VERTICALLY in the region so
    // a short row-set (None / GainPan / Chorus = 1 row) reads balanced.
    const int cols   = knobGridCols (count);
    const int rows   = (count + cols - 1) / cols;
    const int cellH  = juce::jmin (kCellH, gridArea.getHeight() / juce::jmax (1, rows));
    const int cellW  = gridArea.getWidth() / cols;
    const int blockH = rows * cellH;
    const int y0     = gridArea.getY() + (gridArea.getHeight() - blockH) / 2;

    for (int i = 0; i < count; ++i)
    {
        const int col = i % cols;
        const int row = i / cols;
        auto* pc = knobs[i];
        const int x = gridArea.getX() + col * cellW;
        // Rightmost column absorbs the integer width remainder (consistent
        // across every row).
        const int w = (col == cols - 1) ? (gridArea.getRight() - x) : cellW;
        const int y = y0 + row * cellH;
        pc->setVisible (true);
        pc->setBounds (juce::Rectangle<int> (x, y, w, cellH));
    }
}

//==============================================================================
void FxSlotCard::resized()
{
    auto area = getLocalBounds ().reduced (kPad);
    if (area.isEmpty())
        return;

    // ---- Header row: title (upper-left, painted) + power toggle (top-right) ----
    auto header = area.removeFromTop (kHeaderH);
    if (powerToggle_ != nullptr)
        powerToggle_->setBounds (header.removeFromRight (kHeaderH - 2).reduced (2));
    // The remaining header band is the title (drawn in paint(), no child).

    if (area.isEmpty())
        return;
    area.removeFromTop (kHalfGap);   // gap below the header divider

    // ---- Type row: the algorithm dropdown as a STYLED combo (Osc "Shape" /
    //      Filter "Mode" parity) — 28px tall, fit-to-text width, centred. The
    //      combo already inherits the editor-wide ComboBox theme colours
    //      (backgroundInput fill, amber accentPrimary chevron, borderless) via
    //      the LookAndFeel — identical to the synth selectors — so only the SIZE
    //      is set here (was previously full-width / 20px). ----
    if (typeCombo_ != nullptr && area.getHeight() > kTypeRowH)
    {
        auto typeRow = area.removeFromTop (kTypeRowH);
        const int textW  = maxComboItemWidth (*typeCombo_) + kComboChrome;
        const int comboW = juce::jlimit (kComboMinW, juce::jmax (kComboMinW, typeRow.getWidth()), textW);
        const int comboX = typeRow.getX() + (typeRow.getWidth() - comboW) / 2;
        typeCombo_->setBounds (comboX, typeRow.getY(), comboW, kComboH);
        area.removeFromTop (kHalfGap);
    }

    // ---- Body: a COMPACT visualizer band (<= kVisMax, synth decoration parity)
    //      on top + a Mixer-style 3-column knob GRID below. The grid is the
    //      primary control surface, so it claims its ideal height (rows *
    //      kCellH) first; the band fills the rest down to kVisMin. If even the
    //      band floor cannot fit alongside the ideal grid, the band holds at
    //      kVisMin and the grid shrinks. (Was: a large ~2/3-body band + a single
    //      knob row.) ----
    const auto t     = static_cast<FxType> (currentTypeIndex());
    const int active = activeParamCount (t);
    int count = active;
    if (drywet_ != nullptr) ++count;              // the Mix knob is always present
    count = juce::jmax (1, count);
    const int cols = knobGridCols (count);
    const int rows = (count + cols - 1) / cols;
    const int gridIdealH = rows * kCellH;

    const int bodyH = area.getHeight();
    int visH  = kVisMax;
    int gridH = gridIdealH;
    if (visH + gridH + kHalfGap > bodyH)          // ideal grid + max band does not fit
    {
        if (gridIdealH + kVisMin + kHalfGap <= bodyH)   // band can shrink to its floor
        {
            visH  = juce::jmax (0, bodyH - gridIdealH - kHalfGap);
            gridH = gridIdealH;
        }
        else                                            // grid must shrink; band holds kVisMin
        {
            visH  = juce::jmin (kVisMin, juce::jmax (0, bodyH - kHalfGap));
            gridH = juce::jmax (0, bodyH - visH - kHalfGap);
        }
    }

    if (visualizer_ != nullptr && visH > 0)
    {
        visualizer_->setBounds (area.removeFromTop (visH));
        if (! area.isEmpty())
            area.removeFromTop (kHalfGap);
    }

    layoutParamGrid (area);
}

//==============================================================================
void FxSlotCard::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel ());
    const ParvatiTheme* t = lnf != nullptr ? lnf->getTheme () : nullptr;

    const juce::Colour panel   = t ? t->containerFill   : juce::Colour (0xff202028);
    const juce::Colour title   = t ? t->textSecondary   : juce::Colour (0xffb0b0bc);
    const juce::Colour accent  = t ? t->accentSecondary : juce::Colour (0xffe8b84b);

    const auto r = getLocalBounds ().toFloat ();

    // ---- Card panel: BORDERLESS (synth GroupComponent parity). Depth comes from
    //      the tonal lift of containerFill over the page backgroundBase — no
    //      outline, no under-header divider (the title band alone separates). ----
    g.setColour (panel);
    g.fillRoundedRectangle (r, kCorner);

    // ---- Title "FX N" upper-left: BOLD + UPPERCASE (14px), mirroring the synth
    //      card GroupComponent header. An accent tick sits just left of the text
    //      (the FX-slot accent marker); the power toggle lives top-right. ----
    juce::Font font = lnf != nullptr ? lnf->appFont (14.0f, juce::Font::bold)
                                    : juce::Font (juce::FontOptions (14.0f, juce::Font::bold));
    const juce::String name = "FX" + juce::String (slot_ + 1);   // "FX1" (uppercase)
    const int tickX  = kPad + 2;
    const int titleX = tickX + 6;
    const int titleW = juce::jmax (0, getWidth() - titleX - kPad);
    const juce::Rectangle<int> titleRect (titleX, kPad, titleW, kHeaderH);

    g.setColour (accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (static_cast<float> (tickX),
                                                    static_cast<float> (kPad + 6),
                                                    2.5f, static_cast<float> (kHeaderH - 12)),
                            1.2f);
    g.setColour (title);
    g.setFont (font);
    g.drawText (name, titleRect, juce::Justification::centredLeft, true);
}

//==============================================================================
void FxSlotCard::applyThemeColors()
{
    // Push the live theme token onto the visualizer trace so a theme switch
    // re-tints it immediately (the visualizer otherwise reads accentSecondary
    // live each paint).
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel ()))
    {
        if (const auto* th = lnf->getTheme ())
        {
            if (visualizer_ != nullptr)
                visualizer_->setCategoryColour (th->accentSecondary);
        }
    }

    repaint();
}
