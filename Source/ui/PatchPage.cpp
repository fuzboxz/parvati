// Phase 2 of the "Patch page" feature: the Patch juce::Component implementation.
// See PatchPage.h for the design summary and /tmp/parvati_patch_design.md
// ("Phase 2") for the full spec. Phase 1 (PatchArrangement.{h,cpp}) supplies
// applyArrangement / inferArrangement; this component drives the engine purely
// through its EXISTING public setters.

#include "PatchPage.h"

#include "PatchArrangement.h"

#include "PluginProcessor.h"   // ParvatiAudioProcessor::getEngine()
#include "ParamPage.h"        // ParamPage complete type (reflowToWidth/getContentHeight)
#include "ParvatiLookAndFeel.h" // ParvatiLookAndFeel::getTheme (hosted-page colours)
#include "ThemeManager.h"       // ThemeManager (complete type)
#include "TuningTables.h"      // tuningPresetName (Tune combo items)
#include "ui/NoteName.h"       // midiNoteName (key-zone knob readouts)
#include "ui/ParamHelp.h"      // getParamHelp (table-cell tooltips)

#include <array>
#include <cstdint>
#include <vector>

//==============================================================================
// SHARED COLUMN GEOMETRY — the single source of truth for the part-table
// columns. Both the header strip and every PartRow consume partColumnRects(),
// so captions and cells can never drift apart, and BOTH distribute the FULL
// band width across the visible columns (the fixed 942pt arithmetic pre-
// 2026-08-20 left the panel's right ~40% empty on wide editors).
namespace
{
struct PartTableColumns
{
    enum Index
    {
        kName = 0, kVoices, kCh, kZoneLo, kZoneHi,
        kOct, kPorta, kLgo, kVol, kFine, kSpr, kTune, kPoly,
        kCount
    };
};

// The content inset shared by BOTH the header strip and every row: the
// header paints its captions from the SAME inset band the rows lay their
// cells out from, so a caption's x always equals its column's cell x (the
// pre-2026-08-20 header painted from the full band while rows used
// reduced(4) — every caption sat 4px left of its column; "headers don't
// line up with the controls").
constexpr int kTableContentInset = 4;

// Which columns each table tab shows (regrouped 2026-08-20 per the Ambika
// note-path split: MIDI carries the NOTE-ROUTING controls — channel, key
// zone, the voice ALLOCATOR (Mono/Poly/Unison/Cyclic/Chain, which selects
// how incoming notes map to voices) and Octave (transpose acts on the note
// stream); Voice carries the SOUND-shaping controls — voice count, glide,
// legato feel, output level/tuning/detune, scale. Portamento/Legato
// straddle the line (they shape note TRANSITIONS); they stay on Voice
// where the sound character lives.
//   order: kName, kVoices, kCh, kZoneLo, kZoneHi, kOct, kPorta, kLgo,
//          kVol, kFine, kSpr, kTune, kPoly
constexpr bool kVoiceTabMask[PartTableColumns::kCount] = {
    true,  true,  false, false, false,
    false, true,  true,  true,  true,  true,  true,  true };
constexpr bool kMidiTabMask[PartTableColumns::kCount] = {
    true,  false, true,  true,  true,
    true,  false, false, false, false, false, false, false };

// Per-column layout: minimum width, flex weight (share of the slack), and a
// maximum width for the KNOB columns (a round dial gains nothing beyond ~64pt
// — its slack flows to the text columns instead). Minimums are the measured
// 1024x500-floor budget (Voice tab: 728 + 11 gaps = 772 <= 944).
struct PartColumnSpec { int minW; int weight; int maxW; };
constexpr PartColumnSpec kColumnSpecs[PartTableColumns::kCount] = {
    /* kName   */ {  96, 1,        160 },   // capped: the name field must fit its 16-char content, not fill the row
    /* kVoices */ {  56, 2,         90 },   // measured fit: numbers 0..16 + combo chrome
    /* kCh     */ {  56, 3,        110 },   // "Omni" + 1..16 + chrome
    /* kZoneLo */ {  44, 1,        72 },
    /* kZoneHi */ {  44, 1,        72 },
    /* kOct    */ {  48, 1,         90 },
    /* kPorta  */ {  44, 1,        88 },   // "Portamento" caption needs ~64pt+
    /* kLgo    */ {  48, 1,         90 },
    /* kVol    */ {  44, 1,        72 },
    /* kFine   */ {  44, 1,        80 },   // "Fine Tune"
    /* kSpr    */ {  44, 1,        72 },
    /* kTune   */ {  96, 2,        170 },   // "Parameshwari" + chrome
    /* kPoly   */ {  96, 2,        150 }
};
constexpr int kPartColGap = 4;   // between consecutive VISIBLE columns

// The table's CONTENT width for a mask: every visible column at its MAXIMUM
// plus the gaps (2026-08-20: fixed reasonable column widths — the combos no
// longer stretch to fill a wide editor; when the band is wider than this the
// whole table body centres instead of growing).
int tableContentWidth (const bool* visibleMask)
{
    int n = 0, w = 0;
    for (int i = 0; i < PartTableColumns::kCount; ++i)
        if (visibleMask[i])
        {
            ++n;
            w += kColumnSpecs[i].maxW;
        }
    return w + juce::jmax (0, n - 1) * kPartColGap;
}

// The table's BAND width is TAB-INDEPENDENT (2026-08-23 user request): the
// frame — the arrangement summary row (combo + tab strip + the export
// buttons), the header strip and the row band — keeps the WIDEST tab's
// content width on BOTH tabs. Before this, switching to MIDI clamped the
// whole body to the narrower column set and the panel visibly COLLAPSED
// (the export buttons + arrangement combo jumped left, the rows shrank);
// now only the active tab's column group changes inside the unchanged band
// (a narrower group CENTRES — see partColumnRects).
int tableBandContentWidth()
{
    return juce::jmax (tableContentWidth (kVoiceTabMask),
                       tableContentWidth (kMidiTabMask));
}

// The centred content band for a full-width @p band: min(band, band-content)
// wide, horizontally centred — the clamp is the TAB-INDEPENDENT union width
// (see tableBandContentWidth), so the band geometry is identical on both
// tabs. Both the rows' resized() and the header's paint()/columnXForTest()
// consume THIS, so captions and cells stay aligned inside the shared band.
juce::Rectangle<int> centredTableBand (juce::Rectangle<int> band)
{
    const int contentW = juce::jmin (band.getWidth(), tableBandContentWidth());
    return band.withSizeKeepingCentre (contentW, band.getHeight());
}

// Column rects (x/width) for a row/content band @p b (already inset), laying
// out ONLY the columns whose mask entry is true (hidden columns get an empty
// rect). The band's full width is distributed: every visible column starts at
// its minimum, then the slack (band - minimums - gaps) is split across the
// flex weights, respecting each column's max (knob caps push their share back
// into the pool). A band narrower than the minimums scales everything down
// proportionally (cannot happen at the 1024 floor; defensive).
std::array<juce::Rectangle<int>, PartTableColumns::kCount> partColumnRects (
    juce::Rectangle<int> b, const bool* visibleMask)
{
    std::array<juce::Rectangle<int>, PartTableColumns::kCount> r {};

    int nVisible = 0, minTotal = 0;
    for (int i = 0; i < PartTableColumns::kCount; ++i)
        if (visibleMask[i])
        {
            ++nVisible;
            minTotal += kColumnSpecs[i].minW;
        }
    const int gaps  = juce::jmax (0, nVisible - 1) * kPartColGap;
    const int avail = juce::jmax (0, b.getWidth() - gaps);
    int slack = juce::jmax (0, avail - minTotal);

    // Iterative capped weighted split: assign each unresolved column its
    // weighted share of the slack; a column whose share would exceed its max
    // is FIXED at the max, its min->max excess is consumed from the slack,
    // and the remaining columns re-split what is left. Converges in a few
    // passes (each pass fixes at least one column or assigns everyone).
    int widths[PartTableColumns::kCount] {};
    bool resolved[PartTableColumns::kCount] {};
    for (;;)
    {
        int remWeight = 0;
        for (int i = 0; i < PartTableColumns::kCount; ++i)
            if (visibleMask[i] && ! resolved[i])
                remWeight += kColumnSpecs[i].weight;
        if (remWeight <= 0 || slack <= 0)
            break;

        bool anyFixed = false;
        for (int i = 0; i < PartTableColumns::kCount; ++i)
            if (visibleMask[i] && ! resolved[i])
            {
                const int share  = slack * kColumnSpecs[i].weight / remWeight;
                const int wanted = kColumnSpecs[i].minW + share;
                if (wanted >= kColumnSpecs[i].maxW)
                {
                    widths[i] = kColumnSpecs[i].maxW;
                    slack -= juce::jmax (0, kColumnSpecs[i].maxW - kColumnSpecs[i].minW);
                    resolved[i] = true;
                    anyFixed = true;
                }
            }
        if (! anyFixed)
        {
            for (int i = 0; i < PartTableColumns::kCount; ++i)
                if (visibleMask[i] && ! resolved[i])
                {
                    widths[i] = kColumnSpecs[i].minW
                              + slack * kColumnSpecs[i].weight / remWeight;
                    resolved[i] = true;
                }
            break;
        }
    }
    // Safety: any visible column still unresolved takes its minimum.
    for (int i = 0; i < PartTableColumns::kCount; ++i)
        if (visibleMask[i] && ! resolved[i])
            widths[i] = kColumnSpecs[i].minW;
    // Narrow-band fallback: shrink proportionally so the last column stays in
    // the band (cannot occur at the supported floor; keeps geometry sane if a
    // future narrower host pane appears).
    int total = gaps;
    for (int i = 0; i < PartTableColumns::kCount; ++i)
        if (visibleMask[i]) total += widths[i];
    if (total > b.getWidth() && total > 0)
    {
        const double s = static_cast<double> (b.getWidth()) / static_cast<double> (total);
        for (int i = 0; i < PartTableColumns::kCount; ++i)
            if (visibleMask[i])
                widths[i] = juce::jmax (24, juce::roundToInt (widths[i] * s));
    }
    // Cumulative-rounding clamp: after any scaling, the placed columns (sum
    // + gaps) must end exactly at the band's right edge — shave any excess
    // off the LAST visible column so the row never overflows its bounds
    // (the overlap test pins this at every tested editor width).
    {
        int lastVis = -1, placed = 0, count = 0;
        for (int i = 0; i < PartTableColumns::kCount; ++i)
            if (visibleMask[i])
            {
                lastVis = i;
                placed += widths[i];
                ++count;
            }
        if (lastVis >= 0)
        {
            const int gapsNow = juce::jmax (0, count - 1) * kPartColGap;
            const int excess = b.getX() + placed + gapsNow - b.getRight();
            if (excess > 0)
                widths[lastVis] = juce::jmax (24, widths[lastVis] - excess);
        }
    }

    // Place left-to-right with uniform gaps between visible columns. When
    // the ACTIVE tab's columns (at their maxima, after the flex split) are
    // NARROWER than the tab-independent band — the MIDI group inside the
    // Voice-width band, 2026-08-23 — the group packs against the band's LEFT
    // edge (user follow-up: left-aligned reads as a steadier grid than the
    // centred group; the Part-name column keeps a constant x across both
    // tabs), while the frame (summary row + export buttons + header) keeps
    // spanning the full fixed band.
    int x = b.getX();
    for (int i = 0; i < PartTableColumns::kCount; ++i)
        if (visibleMask[i])
        {
            r[(size_t) i].setBounds (x, b.getY(), widths[i], b.getHeight());
            x += widths[i] + kPartColGap;
        }
    return r;
}
}  // namespace

//==============================================================================
namespace
{
// Alpha applied to an inactive Part row (0 voices) so the split is legible while
// the row stays visible AND interactive (the user can still raise its voice
// count to activate it).
constexpr float kInactiveRowAlpha = 0.4f;
}  // namespace

//==============================================================================
// One Part row: "Part N" + voice-count combo + MIDI-channel combo + key-zone
// knobs (Lo/Hi) + polyphony combo. Every control binds DIRECTLY to the engine's
// existing per-part setters (no APVTS). Inactive parts (0 voices) are dimmed but
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
        // Editable part name/alias (Parvati extension): click (desktop) or tap
        // (touch) to rename -- "Kick", "Lead", ... Empty reverts to "Part N".
        partLabel_.setEditable (true, true, false);
        partLabel_.setColour (juce::Label::outlineWhenEditingColourId,
                              juce::Colours::transparentBlack);
        partLabel_.onEditorShow = [this]
        {
            // Edit the RAW name (clear the "Part N" placeholder first).
            if (auto* ed = partLabel_.getCurrentTextEditor())
            {
                const auto raw = engine_.getPartName (partIndex_);
                ed->setText (raw, juce::dontSendNotification);
                ed->setInputRestrictions (16);
            }
        };
        partLabel_.onEditorHide = [this]
        {
            if (refreshing_) return;
            auto text = partLabel_.getText (true).trim();
            engine_.setPartName (partIndex_, text);   // empty -> placeholder on refresh
            refreshNameDisplay();
            owner_.partNamesChanged();
        };
        addAndMakeVisible (partLabel_);

        // Viewport safety net (T4): a TOUCH drag that starts anywhere on this
        // row must not ALSO scroll the enclosing Viewport — the ignore-drag
        // flag covers the row's combos/knobs/labels too, exactly like the
        // ParamControl cells. Mouse drags never scroll-on-drag anyway (the
        // viewport's default nonHover mode is touch-only).
        setViewportIgnoreDragFlag (true);

        // ---- Tooltips: every interactive cell carries help text — the
        // ParamHelp entries for the part_* params, inline TRANS text for the
        // table-only controls (re-translated by buildInlineTips on language
        // switches). Applied through applyTooltipState(), which honours the
        // editor-wide tooltips toggle (the ParamControl contract, mirrored so
        // the Settings switch covers the table too). ----
        buildInlineTips();
        applyTooltipState();
        // HIG touch target: the DRAWN dropdown stays a compact 24pt strip
        // while each combo's BOUNDS — its tap band — fill the column height
        // (44pt after the 12pt caption band; see resized). The L&F reads this
        // "parvatiComboVisualH" property (drawComboBox /
        // positionComboBoxText), so the rows keep their exact look.
        for (auto* c : { &voicesCombo_, &channelCombo_, &polyCombo_, &tuneCombo_,
                        &octaveCombo_, &legatoCombo_ })
            c->getProperties().set ("parvatiComboVisualH", 24);

        // ---- Voices: this Part's voice count drawn from the shared 96-voice
        // pool (0..16). 0 DISABLES the Part (no voice for it in the pool; the
        // row dims); any combination of counts is legal (pool = 6x16, so every
        // Part can be maxed simultaneously); the 6 hardware voicecards are
        // DERIVED from these counts for the individual outputs + the .MUL
        // export. Combo id = count + 1 (JUCE ids must be non-zero). ----
        for (int n = 0; n <= kMaxVoicesPerPart; ++n)
            voicesCombo_.addItem (juce::String (n), n + 1);
        voicesCombo_.onChange = [this] { onVoicesChanged(); };
        // Localised (TRANS per sentence — the codebase idiom for multi-line
        // tooltips is concatenation of TRANS fragments, since TRANS must wrap
        // each COMPLETE source string to hit the translation table).
        voicesTip_ =
            TRANS ("How many voices this part plays at once from the shared ")
            + TRANS ("96-voice pool (0-16; 0 disables the part; the pool holds 6 x 16 so ")
            + TRANS ("all parts can be maxed at the same time; the hardware voicecards ")
            + TRANS ("are shared out automatically for the individual outputs and the ")
            + TRANS (".MUL export).");
        addAndMakeVisible (voicesCombo_);

        // ---- Ch: Omni (0) + 1..16 (id = channel + 1). ----
        channelCombo_.addItem (TRANS ("Omni"), 1);
        for (int c = 1; c <= 16; ++c)
            channelCombo_.addItem (juce::String (c), c + 1);
        channelCombo_.onChange = [this] { onChannelChanged(); };
        addAndMakeVisible (channelCombo_);

        // ---- Zone: two compact knobs (Lo/Hi, 0..127). NoTextBox — the L&F
        // draws the readout in the centre of the arc-ring (no value box). ----
        auto setupKnob = [this] (juce::Slider& s, double mn, double mx) {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (mn, mx, 1.0);
            // The knob is drag-only: a wheel over it must never tweak the
            // value (same idiom as the ParamControl cells / the FX knobs) —
            // an unhandled wheel bubbles up so it scrolls the page instead.
            s.setScrollWheelEnabled (false);
            addAndMakeVisible (s);
        };
        setupKnob (loSlider_, 0.0, 127.0);
        setupKnob (hiSlider_, 0.0, 127.0);
        // Show the MIDI note name ("C4") instead of the raw 0..127 number.
        loSlider_.textFromValueFunction = [] (double v) {
            return midiNoteName (juce::roundToInt (v));
        };
        hiSlider_.textFromValueFunction = [] (double v) {
            return midiNoteName (juce::roundToInt (v));
        };
        const auto onZone = [this] { onZoneChanged(); };
        loSlider_.onValueChange = onZone;
        hiSlider_.onValueChange = onZone;

        // ---- Oct: per-part transpose (PartData byte 1, signed int8 -2..+2).
        // A compact 5-item combo — the table's per-part pitch setting (moved
        // here from the old "Part / Play" page knob; the part_octave APVTS
        // parameter stays valid for host automation). Combo id = value + 3
        // (JUCE ids must be non-zero). ----
        for (int o = -2; o <= 2; ++o)
            octaveCombo_.addItem ((o > 0 ? "+" : "") + juce::String (o), o + 3);
        octaveCombo_.onChange = [this] { onOctaveChanged(); };
        addAndMakeVisible (octaveCombo_);

        // ---- Porta: glide time (PartData byte 6, 0..63). A compact 44pt
        // NoTextBox knob copying the Zone-knob idiom — the L&F draws the
        // percentage readout in the arc centre. ----
        setupKnob (portaSlider_, 0.0, 63.0);
        portaSlider_.textFromValueFunction = [] (double v) {
            return juce::String (juce::roundToInt (v * 100.0 / 63.0)) + "%";
        };
        portaSlider_.onValueChange = [this] { onPortamentoChanged(); };

        // ---- Output columns (the completing absorption): Vol = part volume
        // (PartData byte 0, 0..127), Fine = fine tuning (SIGNED byte 2,
        // -127..127 in 1/128-semitone units), Spr = per-voice detune spread
        // (byte 3, 0..40). Compact 36pt NoTextBox knobs with centre-arc
        // readouts (the Porta/Zone idiom; the readout formats match
        // SynthParamLabels exactly: % of 127 / +-ct via x*100/128 / % of 40).
        // The part_* APVTS parameters stay valid for host automation. ----
        setupKnob (volSlider_, 0.0, 127.0);
        volSlider_.textFromValueFunction = [] (double v) {
            return juce::String (juce::roundToInt (v * 100.0 / 127.0)) + "%";
        };
        volSlider_.onValueChange = [this] { onVolumeChanged(); };

        setupKnob (fineSlider_, -127.0, 127.0);
        fineSlider_.textFromValueFunction = [] (double v) {
            const int ct = juce::roundToInt (v * 100.0 / 128.0);
            return (ct > 0 ? "+" : juce::String()) + juce::String (ct) + "ct";
        };
        fineSlider_.onValueChange = [this] { onFineTuneChanged(); };

        setupKnob (sprSlider_, 0.0, 40.0);
        sprSlider_.textFromValueFunction = [] (double v) {
            return juce::String (juce::roundToInt (v * 100.0 / 40.0)) + "%";
        };
        sprSlider_.onValueChange = [this] { onSpreadChanged(); };

        // ---- Lgo: legato (PartData byte 5, 0/1). Compact On/Off combo. ----
        buildLegatoItems();
        legatoCombo_.onChange = [this] { onLegatoChanged(); };
        addAndMakeVisible (legatoCombo_);

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

        // ---- Tune: per-part microtonal tuning (firmware raga presets).
        // "12-EDO" (id 1) + the 32 firmware presets (ids 2..33 = raga byte
        // 1..32). Preset writes go through the SAME byte-4 path as the
        // part_raga APVTS param (so saves/exports carry them); the resolved
        // mode IS the raga byte (0 = 12-EDO, 1..32 = preset). The former
        // "Custom…" entry (id 34, TuningEditor popover) was removed with the
        // custom-tuning subsystem (2026-08-19). ----
        buildTuneItems();
        tuneCombo_.onChange = [this] { onTuningChanged(); };
        addAndMakeVisible (tuneCombo_);

        refreshLanguage();
    }

    //----------------------------------------------------------------------
    // Layout: a horizontal strip of labelled columns. The name column absorbed
    // the removed Cards column's width (128 -> 156); the Tune column is
    // budgeted for the longest preset name ("Parameshwari" ~81px + 24px combo
    // chrome). WIDTH ARITHMETIC (measured by probe at the 1024x500 floor,
    // 2026-08-20): editor content 992 -> vertical scrollbar 8 -> hosted page
    // 984 -> Global group panel x=16 w=952 -> table panel 952 -> row (4px
    // insets) w=944. Row content: 156+6+76+68+4+48+48+8+48+4+48+4+48 (through
    // Lgo) = 566 + 4+110+4+140 (Tune/Poly, gaps tightened 8->4 to fund the
    // new cells — the same 4pt gap the Oct/Porta/Lgo trio already uses; no
    // CELL was shrunk) + 2+36+2+36+2+36 (Vol/Fine/Spr) = 942 <= 944 (2px
    // breathing); row + insets = 946 <= the 948 budget. The briefed
    // "~130pt slack" measured 108pt — without the 8pt gap reclaim even 33pt
    // cells would not fit. The knobs are 36pt dials in 36x44 bands (the L&F
    // squares via jmin(w,h)): full 44pt-tall tap band, 36pt wide — three
    // 44pt-wide cells (132pt + gaps) are arithmetically impossible at the
    // floor, and overlapping bounds would steal neighbour hits.
    // Width arithmetic (measured 2026-08-20, see partColumnRects above):
    // 942pt of content at the 1024x500 floor vs the 944pt row budget. The
    // knobs are dials in 44pt-tall tap bands centred on the full row height
    // (36pt wide for the Vol/Fine/Spr trio — the L&F squares via jmin(w,h);
    // three 44pt-wide cells would not fit; see the width comment above).
    void resized() override
    {
        // The ACTIVE table tab drives the mask: only its columns are laid out
        // (and visible); the rest keep their last bounds but are hidden by
        // applyTableTab, so every accessor/choose* seam still works on the
        // hidden cells (they read state, not visibility).
        const bool* mask = midiTab_ ? kMidiTabMask : kVoiceTabMask;
        const auto c = partColumnRects (
            centredTableBand (getLocalBounds().reduced (kTableContentInset)), mask);
        lastColumnRects_ = c;   // test hook: the row's ACTUAL column geometry

        partLabel_.setBounds (c[PartTableColumns::kName]);

        // Combos fill their column width in a 44pt band; knobs are 44pt
        // squares (the L&F squares via jmin(w,h)). All centre on the full row
        // height — the per-row caption band is gone (replaced by the single
        // header strip), so the HIG tap band is unchanged at 44pt.
        auto combo = [] (juce::Rectangle<int> col, int rowH)
        { return col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, rowH)); };
        auto knob = [] (juce::Rectangle<int> col, int rowH)
        { return col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, rowH)); };
        const int h = getHeight();

        voicesCombo_.setBounds  (combo (c[PartTableColumns::kVoices], h));
        channelCombo_.setBounds (combo (c[PartTableColumns::kCh],     h));
        loSlider_.setBounds     (knob   (c[PartTableColumns::kZoneLo], h));
        hiSlider_.setBounds     (knob   (c[PartTableColumns::kZoneHi], h));
        octaveCombo_.setBounds  (combo (c[PartTableColumns::kOct],    h));
        portaSlider_.setBounds  (knob   (c[PartTableColumns::kPorta], h));
        legatoCombo_.setBounds  (combo (c[PartTableColumns::kLgo],    h));
        volSlider_.setBounds    (knob   (c[PartTableColumns::kVol],   h));
        fineSlider_.setBounds   (knob   (c[PartTableColumns::kFine],  h));
        sprSlider_.setBounds    (knob   (c[PartTableColumns::kSpr],   h));
        tuneCombo_.setBounds    (combo (c[PartTableColumns::kTune],   h));
        polyCombo_.setBounds    (combo (c[PartTableColumns::kPoly],   h));
    }

    // Switch the row's visible column set (PartTablePanel drives this on a
    // tab change; PartRow::resized consumes the same mask). All cells stay
    // CONSTRUCTED and state-readable — only visibility changes. Regrouped
    // 2026-08-20: Ch/Zone/Oct/Poly live on MIDI; Voices/Porta/Lgo/Vol/Fine/
    // Spr/Tune on Voice; Name on both.
    void applyTableTab (bool midiTab)
    {
        midiTab_ = midiTab;
        channelCombo_.setVisible (midiTab);
        voicesCombo_.setVisible (! midiTab);
        loSlider_.setVisible (midiTab);
        hiSlider_.setVisible (midiTab);
        octaveCombo_.setVisible (midiTab);
        portaSlider_.setVisible (! midiTab);
        legatoCombo_.setVisible (! midiTab);
        volSlider_.setVisible (! midiTab);
        fineSlider_.setVisible (! midiTab);
        sprSlider_.setVisible (! midiTab);
        tuneCombo_.setVisible (! midiTab);
        polyCombo_.setVisible (! midiTab);
        partLabel_.setVisible (true);
        resized();
    }

    // Test hook: the mask of columns this row now renders (mirrors the
    // panel's active tab; PartRow::resized consumes the same kVoice/kMidi
    // masks, so this is the row-visible truth the test asserts against).
    const bool* visibleColumnsForTest() const { return midiTab_ ? kMidiTabMask : kVoiceTabMask; }

    //----------------------------------------------------------------------
    // Tooltip gate (the ParamControl contract, mirrored for table cells):
    // stash each cell's help text, apply or blank per the editor-wide toggle.
    // Called at construction, on language changes (translated inline texts)
    // and from PatchPage::setTableTooltipsEnabled when the Settings toggle
    // flips.
    void applyTooltipState()
    {
        const bool on = ParamControl::tooltipsEnabled();
        auto apply = [on] (juce::SettableTooltipClient& c, const juce::String& text) {
            c.setTooltip (on ? text : juce::String());
        };
        apply (partLabel_,    partLabelTip_);
        apply (voicesCombo_,  voicesTip_);
        apply (channelCombo_, channelTip_);
        apply (loSlider_,     zoneLoTip_);
        apply (hiSlider_,     zoneHiTip_);
        apply (octaveCombo_,  getParamHelp ("part_octave"));
        apply (portaSlider_,  getParamHelp ("part_portamento"));
        apply (legatoCombo_,  getParamHelp ("part_legato"));
        apply (volSlider_,    getParamHelp ("part_volume"));
        apply (fineSlider_,   getParamHelp ("part_tuning"));
        apply (sprSlider_,    getParamHelp ("part_spread"));
        apply (polyCombo_,    getParamHelp ("part_polyphony"));
        apply (tuneCombo_,    getParamHelp ("part_raga"));
    }

    // Re-translate the inline (table-only) tooltip texts. Called at
    // construction and from refreshLanguage so a live language switch
    // re-renders them (ParamHelp strings are English-only by design).
    void buildInlineTips()
    {
        partLabelTip_ =
            TRANS ("Click (or tap) to rename this part — an empty name reverts to the ")
            + TRANS ("default 'Part N' label.");
        channelTip_ =
            TRANS ("MIDI channel this part listens on (Omni responds on every ")
            + TRANS ("channel; multitimbral stacks usually need distinct channels).");
        zoneLoTip_ =
            TRANS ("Key zone: the lowest MIDI note this part responds to (notes ")
            + TRANS ("below stay silent so another part can use them).");
        zoneHiTip_ =
            TRANS ("Key zone: the highest MIDI note this part responds to (notes ")
            + TRANS ("above stay silent so another part can use them).");
    }

    // Test hook: every interactive cell exposes a tooltip when the global
    // toggle is ON (pins the "tooltips seem empty" regression class).
    bool allCellsHaveTooltipsForTest()
    {
        juce::SettableTooltipClient* cells[] = {
            &partLabel_, &voicesCombo_, &channelCombo_, &loSlider_, &hiSlider_,
            &octaveCombo_, &portaSlider_, &legatoCombo_, &volSlider_,
            &fineSlider_, &sprSlider_, &polyCombo_, &tuneCombo_ };
        return std::all_of (std::begin (cells), std::end (cells),
            [] (juce::SettableTooltipClient* c) { return c->getTooltip().isNotEmpty(); });
    }

    // The DISPLAYED name (placeholder "Part N" applied when empty).
    juce::String displayedNameForTest() const { return partLabel_.getText (true); }

    // Test hook: the x-position of column @p i from this row's LAST layout
    // (-1 for a hidden/empty column rect). Used with the header's accessor
    // to pin caption/cell alignment.
    int columnXForTest (int i) const
    {
        if (i < 0 || i >= PartTableColumns::kCount)
            return -1;
        return lastColumnRects_[static_cast<size_t> (i)].getX();
    }

    //----------------------------------------------------------------------
    // Re-read this Part's engine state into the controls WITHOUT firing onChange.
    void refresh()
    {
        refreshing_ = true;
        // Voice count 0..16: combo id = count + 1 (0 = a DISABLED part, a real
        // selectable item — never a ghosted placeholder).
        const int slots = juce::jlimit (0, kMaxVoicesPerPart, engine_.getPartVoiceSlots (partIndex_));
        voicesCombo_.setSelectedId (slots + 1, juce::dontSendNotification);
        refreshNameDisplay();

        channelCombo_.setSelectedId (static_cast<int> (engine_.getPartChannel (partIndex_)) + 1,
                                     juce::dontSendNotification);

        loSlider_.setValue (static_cast<double> (engine_.getPartKeyrangeLow (partIndex_)),
                            juce::dontSendNotification);
        hiSlider_.setValue (static_cast<double> (engine_.getPartKeyrangeHigh (partIndex_)),
                            juce::dontSendNotification);

        const uint8_t poly = engine_.getPart (partIndex_).partBytes[15];   // PartData byte 15 = polyphony
        polyCombo_.setSelectedId (static_cast<int> (poly) + 1, juce::dontSendNotification);

        // Part-character columns (absorbed knobs): byte 1 is SIGNED int8
        // (-2..+2); bytes 5/6 are 0/1 and 0..63.
        const auto& part = engine_.getPart (partIndex_);
        const int oct = juce::jlimit (-2, 2, static_cast<int> (
            static_cast<int8_t> (part.partBytes[1])));
        octaveCombo_.setSelectedId (oct + 3, juce::dontSendNotification);
        legatoCombo_.setSelectedId ((part.partBytes[5] != 0 ? 2 : 1), juce::dontSendNotification);
        portaSlider_.setValue (static_cast<double> (juce::jlimit (0, 63,
                       static_cast<int> (part.partBytes[6]))), juce::dontSendNotification);

        // Output columns (completing absorption): byte 0 = volume (0..127),
        // byte 2 = fine tuning (SIGNED int8 -127..127), byte 3 = spread
        // (0..40).
        volSlider_.setValue (static_cast<double> (juce::jlimit (0, 127,
                       static_cast<int> (part.partBytes[0]))), juce::dontSendNotification);
        fineSlider_.setValue (static_cast<double> (juce::jlimit (-127, 127,
                       static_cast<int> (static_cast<int8_t> (part.partBytes[2])))),
                       juce::dontSendNotification);
        sprSlider_.setValue (static_cast<double> (juce::jlimit (0, 40,
                       static_cast<int> (part.partBytes[3]))), juce::dontSendNotification);

        syncTuningDisplay();

        refreshing_ = false;
        updateDimState();
    }

    // Re-apply every chrome string through TRANS() (called by the editor after a
    // live language switch) and rebuild the channel/poly combo items (the Omni
    // + mode names are translated), preserving each selection.
    void refreshLanguage()
    {
        // (Column captions moved to the single header strip —
        // PartTablePanel::ColumnHeader::refreshLanguage.)
        buildInlineTips();
        applyTooltipState();

        {
            // Legato items are translated; the Octave items are bare signed
            // numbers (language-independent, like the Voices numbers).
            const int prev = legatoCombo_.getSelectedId();
            buildLegatoItems();
            legatoCombo_.setSelectedId (prev, juce::dontSendNotification);
        }

        {
            const int prev = channelCombo_.getSelectedId();
            // dontSendNotification: the DEFAULT clear() posts an async change
            // message that fires onChange AFTER the rebuild has restored the
            // selection below — onChannelChanged would then re-run with a
            // possibly-stale combo and overwrite engine state (same fix as the
            // voices/tune combos: a programmatic rebuild must never drive the
            // write path).
            channelCombo_.clear (juce::dontSendNotification);
            channelCombo_.addItem (TRANS ("Omni"), 1);
            for (int c = 1; c <= 16; ++c)
                channelCombo_.addItem (juce::String (c), c + 1);
            channelCombo_.setSelectedId (prev, juce::dontSendNotification);
        }
        {
            const int prev = polyCombo_.getSelectedId();
            // Same dontSendNotification fix as the channel combo above.
            polyCombo_.clear (juce::dontSendNotification);
            polyCombo_.addItem (TRANS ("Mono"), 1);
            polyCombo_.addItem (TRANS ("Poly"), 2);
            polyCombo_.addItem (TRANS ("Unison 2x"), 3);
            polyCombo_.addItem (TRANS ("Cyclic"), 4);
            polyCombo_.addItem (TRANS ("Chain"), 5);
            polyCombo_.setSelectedId (prev, juce::dontSendNotification);
        }
        // The Voices items are bare numbers (language-independent); only the
        // selection needs re-applying after the clear (combo id = count + 1).
        {
            voicesCombo_.clear (juce::dontSendNotification);
            for (int n = 0; n <= kMaxVoicesPerPart; ++n)
                voicesCombo_.addItem (juce::String (n), n + 1);
            voicesCombo_.setSelectedId (
                juce::jlimit (0, kMaxVoicesPerPart, engine_.getPartVoiceSlots (partIndex_)) + 1,
                juce::dontSendNotification);
        }
        {
            // Preset names are proper nouns (firmware scale names) — kept
            // untranslated; only the fixed chrome items are.
            const int prev = tuneCombo_.getSelectedId();
            buildTuneItems();
            tuneCombo_.setSelectedId (prev, juce::dontSendNotification);
        }
        refreshNameDisplay();
        repaint();
    }

    // Colours come from the inherited L&F (read at paint time) — just repaint.
    void applyThemeColors() { repaint(); }

    // Show the user name if set, else the "Part N" placeholder (translated).
    void refreshNameDisplay()
    {
        const auto n = engine_.getPartName (partIndex_);
        partLabel_.setText (n.isNotEmpty() ? n
                                           : TRANS ("Part") + " " + juce::String (partIndex_ + 1),
                            juce::dontSendNotification);
    }

    // The Poly combo's displayed mode (0..4 = the combo id - 1).
    int displayedPolyphony() const
    {
        const int id = polyCombo_.getSelectedId();
        return juce::jlimit (0, 4, id - 1);
    }

    // ---- Part-character display/drive hooks (Oct / Porta / Lgo columns) ----
    int displayedOctave() const
    {
        return juce::jlimit (-2, 2, octaveCombo_.getSelectedId() - 3);
    }
    void chooseOctave (int octaves)
    {
        refreshing_ = true;
        octaveCombo_.setSelectedId (juce::jlimit (-2, 2, octaves) + 3, juce::dontSendNotification);
        refreshing_ = false;
        onOctaveChanged();
    }
    int displayedLegato() const
    {
        return legatoCombo_.getSelectedId() == 2 ? 1 : 0;
    }
    void chooseLegato (int on)
    {
        refreshing_ = true;
        legatoCombo_.setSelectedId (on != 0 ? 2 : 1, juce::dontSendNotification);
        refreshing_ = false;
        onLegatoChanged();
    }
    int displayedPortamento() const
    {
        return juce::jlimit (0, 63, juce::roundToInt (portaSlider_.getValue()));
    }
    void choosePortamento (int value)
    {
        refreshing_ = true;
        portaSlider_.setValue (static_cast<double> (juce::jlimit (0, 63, value)),
                               juce::dontSendNotification);
        refreshing_ = false;
        onPortamentoChanged();
    }

    // ---- Output-column display/drive hooks (Vol / Fine / Spr) ----
    int displayedVolume() const
    {
        return juce::jlimit (0, 127, juce::roundToInt (volSlider_.getValue()));
    }
    void chooseVolume (int value)
    {
        refreshing_ = true;
        volSlider_.setValue (static_cast<double> (juce::jlimit (0, 127, value)),
                             juce::dontSendNotification);
        refreshing_ = false;
        onVolumeChanged();
    }
    int displayedFineTune() const
    {
        return juce::jlimit (-127, 127, juce::roundToInt (fineSlider_.getValue()));
    }
    void chooseFineTune (int value)
    {
        refreshing_ = true;
        fineSlider_.setValue (static_cast<double> (juce::jlimit (-127, 127, value)),
                              juce::dontSendNotification);
        refreshing_ = false;
        onFineTuneChanged();
    }
    int displayedSpread() const
    {
        return juce::jlimit (0, 40, juce::roundToInt (sprSlider_.getValue()));
    }
    void chooseSpread (int value)
    {
        refreshing_ = true;
        sprSlider_.setValue (static_cast<double> (juce::jlimit (0, 40, value)),
                             juce::dontSendNotification);
        refreshing_ = false;
        onSpreadChanged();
    }

    // The Voices combo's displayed voice count (0..16; 0 = the
    // part is disabled — a real selected item, not a placeholder).
    int displayedVoiceSlots() const
    {
        const int id = voicesCombo_.getSelectedId();
        return juce::jlimit (0, kMaxVoicesPerPart, id - 1);   // combo id = count + 1
    }

    // Test/automation hook: set the Voices combo as if the user chose it, then
    // run the normal engine write path (onVoicesChanged -> disable or
    // setPartVoiceSlots). JUCE does not fire a combo's onChange for a
    // programmatic setSelectedId. 0 DISABLES the part (the combo's "0" item).
    void chooseVoiceSlots (int slots)
    {
        refreshing_ = true;
        voicesCombo_.setSelectedId (juce::jlimit (0, kMaxVoicesPerPart, slots) + 1,
                                    juce::dontSendNotification);
        refreshing_ = false;
        onVoicesChanged();
    }

    // The Tune combo's displayed mode (0..32).
    int displayedTuningMode() const
    {
        return tuneCombo_.getSelectedId() - 1;
    }

    // Test/automation hook: set the Tune combo as if the user chose it, then
    // run the normal byte-4 write path (JUCE does not fire a combo's onChange
    // for a programmatic setSelectedId).
    void chooseTuningMode (int mode)
    {
        refreshing_ = true;
        tuneCombo_.setSelectedId (tuningModeToComboId (mode), juce::dontSendNotification);
        refreshing_ = false;
        onTuningChanged();
    }

    // Re-select the Tune combo from the engine's resolved mode (called by
    // refresh()). No onChange fired.
    void syncTuningDisplay()
    {
        refreshing_ = true;
        tuneCombo_.setSelectedId (tuningModeToComboId (engine_.resolvedTuningMode (partIndex_)),
                                  juce::dontSendNotification);
        refreshing_ = false;
    }

    // Dim the row when its Part has 0 voices (inactive). setAlpha keeps the row
    // visible AND interactive so the user can still raise its voice count.
    void updateDimState()
    {
        setAlpha (engine_.getPartVoiceSlots (partIndex_) == 0 ? kInactiveRowAlpha : 1.0f);
    }

private:
    PatchPage& owner_;
    const int partIndex_;
    SynthEngine& engine_;
    bool refreshing_ = false;

    // (The former per-row caption labels were replaced by the single header
    // strip — PartTablePanel::ColumnHeader — sharing partColumnRects().)
    juce::Label partLabel_;
    // Inline (table-only) tooltip texts, re-translated by buildInlineTips()
    // on language switches. The part_* cells read ParamHelp directly.
    juce::String partLabelTip_, voicesTip_, channelTip_, zoneLoTip_, zoneHiTip_;
    // Active table tab (false = Voice, true = MIDI) — driven by
    // PartTablePanel::setActiveTab via applyTableTab.
    bool midiTab_ = false;

    // The row's last-computed column rects (test-hook source; refreshed in
    // resized from the SAME partColumnRects() call that positions the cells).
    std::array<juce::Rectangle<int>, PartTableColumns::kCount> lastColumnRects_ {};
    juce::ComboBox voicesCombo_, channelCombo_, polyCombo_, tuneCombo_, octaveCombo_, legatoCombo_;
    juce::Slider loSlider_, hiSlider_, portaSlider_;
    juce::Slider volSlider_, fineSlider_, sprSlider_;     // output columns (bytes 0/2/3)

    // (Re)build the Tune combo items: "12-EDO" (id 1) + the 32 firmware
    // presets (ids 2..33, untranslated proper nouns). Id mapping:
    // combo id = raga byte + 1 (mode 0..32 -> id 1..33).
    void buildTuneItems()
    {
        tuneCombo_.clear (juce::dontSendNotification);
        tuneCombo_.addItem (TRANS ("12-EDO"), 1);
        for (int id = 1; id <= parvati::kNumTuningPresets; ++id)
            tuneCombo_.addItem (juce::String (parvati::tuningPresetName (id)), id + 1);
    }

    static int tuningModeToComboId (int mode)
    {
        return juce::jlimit (1, 33, mode + 1);   // mode 0..32 -> id 1..33
    }

    void onVoicesChanged()
    {
        if (refreshing_) return;
        // The combo's id is the voice count + 1 (0..16). A 0 count DISABLES
        // the Part via the legacy materialization path (a zero mask
        // materializes 0 slots — the engine's only disable entry point; the
        // public setPartVoiceSlots clamps 0 to 1 by design).
        const int slots = juce::jlimit (0, kMaxVoicesPerPart, voicesCombo_.getSelectedId() - 1);
        if (slots == 0)
            engine_.setPartVoiceAllocation (partIndex_, 0);
        else
            engine_.setPartVoiceSlots (partIndex_, slots);
        owner_.postPartEdit();
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

    void onTuningChanged()
    {
        if (refreshing_) return;
        const int id = tuneCombo_.getSelectedId();
        if (id < 1 || id > 33)
            return;
        const int mode = id - 1;   // 0 = 12-EDO, 1..32 = raga preset byte
        // Byte-4 write through the poly idiom (setCurrentPart +
        // applyPartByte(4, mode) + restore): the same PartData byte the
        // part_raga APVTS param drives, so saves/exports carry the preset.
        const int saved = engine_.getCurrentPart();
        engine_.setCurrentPart (partIndex_);
        engine_.applyPartByte (4, static_cast<uint8_t> (mode));
        engine_.setCurrentPart (saved);
        owner_.postPartEdit();
        // Same APVTS staleness reason as onPolyChanged: the hosted param
        // grid's part_raga combo reads the CURRENT part's value.
        owner_.proc_.loadPartIntoApvts (engine_.getCurrentPart());
    }

    // The three part-character write paths share the Poly/Tune idiom:
    // setCurrentPart + applyPartByte(offset, value) + restore (the byte has no
    // dedicated setter), then postPartEdit + a CURRENT-part APVTS re-sync so
    // the hosted knobs / a save stay correct (engine storage is authoritative;
    // the next part switch re-reads it).
    void writeCharacterByte (int offset, uint8_t value)
    {
        const int saved = engine_.getCurrentPart();
        engine_.setCurrentPart (partIndex_);
        engine_.applyPartByte (offset, value);
        engine_.setCurrentPart (saved);
        owner_.postPartEdit();
        owner_.proc_.loadPartIntoApvts (engine_.getCurrentPart());
    }

    void onOctaveChanged()
    {
        if (refreshing_) return;
        // Combo id = value + 3; the byte is signed int8 (-2..+2).
        const int oct = juce::jlimit (-2, 2, octaveCombo_.getSelectedId() - 3);
        writeCharacterByte (1, static_cast<uint8_t> (static_cast<int8_t> (oct)));
    }

    void onPortamentoChanged()
    {
        if (refreshing_) return;
        writeCharacterByte (6, static_cast<uint8_t> (juce::jlimit (0, 63,
                          juce::roundToInt (portaSlider_.getValue()))));
    }

    void onLegatoChanged()
    {
        if (refreshing_) return;
        writeCharacterByte (5, legatoCombo_.getSelectedId() == 2 ? 1 : 0);
    }

    // Output-column write paths (completing absorption): byte 0 = volume,
    // byte 2 = fine tuning (SIGNED), byte 3 = spread. Same writeCharacterByte
    // idiom (setCurrentPart + applyPartByte + restore + postPartEdit +
    // current-part APVTS re-sync).
    void onVolumeChanged()
    {
        if (refreshing_) return;
        writeCharacterByte (0, static_cast<uint8_t> (juce::jlimit (0, 127,
                          juce::roundToInt (volSlider_.getValue()))));
    }

    void onFineTuneChanged()
    {
        if (refreshing_) return;
        writeCharacterByte (2, static_cast<uint8_t> (static_cast<int8_t> (juce::jlimit (-127, 127,
                          juce::roundToInt (fineSlider_.getValue())))));
    }

    void onSpreadChanged()
    {
        if (refreshing_) return;
        writeCharacterByte (3, static_cast<uint8_t> (juce::jlimit (0, 40,
                          juce::roundToInt (sprSlider_.getValue()))));
    }

    // Rebuild the Lgo items ("Off"/"On", translated), preserving selection.
    void buildLegatoItems()
    {
        legatoCombo_.clear (juce::dontSendNotification);
        legatoCombo_.addItem (TRANS ("Off"), 1);
        legatoCombo_.addItem (TRANS ("On"), 2);
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
// The 6-part voice-allocation table: a plain container that PARENTS the six
// PartRows (PatchPage keeps owning them via rows_) and lays them out exactly
// as the old layoutScrollBody did — 6 rows of height 56 with 4px gaps, a 4px
// inset so the rows sit inside the hosting Global panel's border. ABOVE the
// rows it carries the patch ARRANGEMENT summary row (the arrangement combo +
// the "Voices Y/96" readout — PatchPage-owned, parented here), so the Global
// panel reads top-down: arrangement, then the per-part rows it configures.
// The panel itself is attached into the HOSTED ParamPage's "Global" group as
// an EXTERNAL decoration (hostParamPage), so the table renders inside that
// bordered panel, below the global knobs. It paints nothing (the owning group
// panel's theme background shows through).
class PatchPage::PartTablePanel : public juce::Component,
                                     private juce::ChangeListener
{
public:
    explicit PartTablePanel (PatchPage& owner) : owner_ (owner)
    {
        addAndMakeVisible (header_);
        // Voice / MIDI segmented toggle — the GroupPager idiom (a bare
        // TabbedButtonBar rendered by the editor-wide ParvatiLookAndFeel's
        // drawTabButton), INLINE at the right of the arrangement summary row
        // (no extra vertical budget). Switching tabs swaps the column mask:
        // rows hide/re-layout their cells, the header relabels, and both
        // REDISTRIBUTE the full width across the visible columns.
        tabStrip_.addTab (TRANS ("Voice"), juce::Colours::transparentBlack, -1);
        tabStrip_.addTab (TRANS ("MIDI"),  juce::Colours::transparentBlack, -1);
        tabStrip_.setCurrentTabIndex (0, juce::dontSendNotification);
        tabStrip_.addChangeListener (this);
        addAndMakeVisible (tabStrip_);
    }

    // Switch the active table tab (false = Voice, true = MIDI): every row's
    // visible column set + the header captions follow; the shared column
    // geometry re-distributes the full band across the ACTIVE tab's columns.
    void setActiveTab (bool midi)
    {
        header_.refreshLabels (midi);
        for (int i = 0; i < kNumParts; ++i)
            owner_.rows_[ (size_t) i]->applyTableTab (midi);
        resized();
    }

    // Summary row height + gap above the part rows.
    static constexpr int kSummaryH = 44;
    static constexpr int kSummaryGap = 8;
    // Header strip: the single column-caption row (replaces the former
    // per-row caption bands — ONE header, localized, sharing the exact
    // column geometry the rows lay out from).
    static constexpr int kHeaderH = 18;
    static constexpr int kHeaderGap = 4;
    // Natural panel height: 4px top inset + the arrangement summary row
    // (44px + 8px gap) + the header strip (18px + 4px gap) + 6 rows x 56
    // + 5 gaps x 4 + 4px bottom inset. The reserved external-decoration
    // height the hosted page uses for the group's layout (see hostParamPage).
    static constexpr int kTableH = 4 + kSummaryH + kSummaryGap
                                 + kHeaderH + kHeaderGap + 6 * 56 + 5 * 4 + 4;

    void resized() override
    {
        constexpr int rowH = 56;
        constexpr int rowGap = 4;
        constexpr int inset = 4;
        auto b = getLocalBounds().reduced (inset, inset);

        // ---- Fixed-width centred table body (2026-08-20): the combos no
        // longer stretch to fill a wide editor — every column has a measured
        // MAXIMUM and the whole table (summary row + header + part rows)
        // lives in a band of min(full, sum-of-maxes + gaps), horizontally
        // CENTRED. At/below the 1024 floor the band IS the full width (the
        // flex minimums still fill it), so nothing shrinks. Rows/header also
        // centre internally from their own bounds (idempotent) so a direct
        // bounds set cannot de-centre them. ----
        const int bandW  = b.getWidth();
        // TAB-INDEPENDENT band (2026-08-23): the clamp is the UNION of the
        // two tabs' content widths (== the Voice tab's — see
        // tableBandContentWidth), so the frame keeps the same width on BOTH
        // tabs and switching to MIDI no longer collapses the panel.
        const int contW  = juce::jmin (bandW, tableBandContentWidth());
        lastBandW_  = bandW;
        lastContW_  = contW;
        b = b.withX (b.getX() + (bandW - contW) / 2).withWidth (contW);

        // ---- Arrangement summary row: the arrangement combo (220pt wide, a
        // 44pt HIG tap band with a 26pt visual box — same idiom as the part
        // rows) + the "Voices Y/96" pool-budget readout to its right.
        // PatchPage-owned members parented into this panel (a nested class
        // has access). ----
        {
            auto summary = b.removeFromTop (kSummaryH);
            // Arrangement combo FIRST (leftmost — the user's 2026-08-20
            // follow-up: the Custom/Mono/etc selector leads the row), then
            // the [Voice|MIDI] tab strip. The Ambika EXPORT buttons sit at
            // the RIGHT edge (right-aligned cluster, 8pt gap between) — the
            // middle stays calm. The buttons fill the full 44pt band (the
            // same HIG idiom as the arrangement combo).
            {
                const int mulW = 108, proW = 104, gap = 8;
                auto mulBtn = summary.removeFromRight (mulW);
                summary.removeFromRight (gap);
                auto proBtn = summary.removeFromRight (proW);
                // 26pt VISUAL box centred in the 44pt tap band (the same HIG
                // idiom as the arrangement combo — 2026-08-23: filling the
                // full 44pt made the outlined action buttons read as fat
                // slabs; the combo-matching visual height lines the row up).
                owner_.exportMulButton_.setBounds (mulBtn.withSizeKeepingCentre (mulW, 26));
                owner_.exportProButton_.setBounds (proBtn.withSizeKeepingCentre (proW, 26));
            }
            owner_.arrangementCombo_.setBounds (summary.removeFromLeft (220));
            summary.removeFromLeft (12);
            const int stripW = juce::jmin (150, juce::jmax (0, summary.getWidth() - 12));
            tabStrip_.setBounds (summary.removeFromLeft (stripW)
                                        .withSizeKeepingCentre (stripW, kTabBarH));
        }
        b.removeFromTop (kSummaryGap);

        // ---- Single column-header strip (localized captions, painted at
        // the shared column x-positions so it can never drift from the
        // cells; non-interactive so no HIG target is required). ----
        header_.setBounds (b.removeFromTop (kHeaderH));
        b.removeFromTop (kHeaderGap);

        for (int i = 0; i < kNumParts; ++i)
        {
            // rows_ is PatchPage-private; a nested class has access.
            owner_.rows_[ (size_t) i]->setBounds (b.removeFromTop (rowH));
            b.removeFromTop (rowGap);
        }
    }

    // Localize the header captions (called from PatchPage::refreshLanguage).
    void refreshLanguage() { header_.refreshLanguage(); }

    // The header strip. Paints the ACTIVE tab's captions at the shared
    // column rects (masked: hidden columns draw nothing) — themed via the
    // L&F's theme secondary-text colour with a sane fallback, 11pt bold,
    // centred-left like the old per-row captions.
    class ColumnHeader : public juce::Component
    {
    public:
        void paint (juce::Graphics& g) override
        {
            const ParvatiTheme* t = parvati::themeFor (*this);
            g.setColour (t != nullptr ? t->textSecondary
                                       : juce::Colours::lightgrey.withAlpha (0.85f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            const bool* mask = midi_ ? kMidiTabMask : kVoiceTabMask;
            // SAME inset band the rows use (kTableContentInset): the caption
            // x-positions are then exactly the cell x-positions (pinned by
            // the header-vs-row alignment test).
            const auto c = partColumnRects (
                centredTableBand (getLocalBounds().reduced (kTableContentInset)), mask);
            // labels_ is the FILTERED caption list (hidden columns absent),
            // so the binding column→caption must resolve the filtered
            // position — indexing labels_ by the raw column index painted
            // the wrong caption over every column after the first hidden
            // one (2026-08-20: "legato has tune as the column header").
            // captionForColumn() is the single mapping both paint and the
            // test hook use.
            for (int i = 0; i < PartTableColumns::kCount; ++i)
                if (mask[i])
                    g.drawText (captionForColumn (i), c[(size_t) i],
                                juce::Justification::centredLeft, true);
        }

        // The caption paint() draws over column @p col (filtered-list
        // position resolved against the ACTIVE mask). Test hook: pins the
        // label↔column binding, not just the x-alignment.
        juce::String captionForColumn (int col) const
        {
            const bool* mask = midi_ ? kMidiTabMask : kVoiceTabMask;
            int vi = 0;
            for (int i = 0; i < PartTableColumns::kCount; ++i)
                if (mask[i])
                {
                    if (i == col)
                        return labels_[vi];
                    ++vi;
                }
            return {};
        }

        // Swap captions for @p midi's column set (also the language-refresh
        // path: refreshLanguage() re-translates the ACTIVE set).
        void refreshLabels (bool midi)
        {
            midi_ = midi;
            labels_ = captions (midi_);
            repaint();
        }

        void refreshLanguage() { labels_ = captions (midi_); repaint(); }

        juce::StringArray labels() const { return labels_; }

        // Test hook: the x-position this header paints column @p i at
        // (-1 when hidden). Computed from the EXACT paint geometry
        // (getLocalBounds() == (0,0,getWidth(),getHeight()) at paint time).
        int columnXForTest (int i) const
        {
            const bool* mask = midi_ ? kMidiTabMask : kVoiceTabMask;
            if (i < 0 || i >= PartTableColumns::kCount || ! mask[i])
                return -1;
            return partColumnRects (centredTableBand (getLocalBounds().reduced (kTableContentInset)),
                                    mask)[static_cast<size_t> (i)].getX();
        }

    private:
        // Caption per column, in PartTableColumns order; the INACTIVE tab's
        // columns are simply not listed (captions(midi) returns the ACTIVE
        // tab's visible columns in column order). Regrouped 2026-08-20:
        // MIDI = Part/Ch/Zone Lo/Hi/Oct/Polyphony; Voice = Part/Voices/
        // Porta/Lgo/Vol/Fine/Spr/Tune (masks above carry the reasoning).
        static juce::StringArray captions (bool midi)
        {
            juce::StringArray out;
            auto addIf = [&out] (bool visible, const juce::String& s) {
                if (visible) out.add (s);
            };
            addIf (true,                       TRANS ("Part"));
            addIf (! midi,                     TRANS ("Voices"));
            addIf (midi,                       TRANS ("Channel"));
            addIf (midi,                       TRANS ("Zone Low"));
            addIf (midi,                       TRANS ("Zone High"));
            addIf (midi,                       TRANS ("Octave"));
            addIf (! midi,                     TRANS ("Portamento"));
            addIf (! midi,                     TRANS ("Legato"));
            addIf (! midi,                     TRANS ("Volume"));
            addIf (! midi,                     TRANS ("Fine Tune"));
            addIf (! midi,                     TRANS ("Spread"));
            addIf (! midi,                     TRANS ("Tune"));
            addIf (! midi,                     TRANS ("Polyphony"));
            return out;
        }

        bool midi_ = false;   // default tab: Voice
        juce::StringArray labels_ = captions (false);
    };

    // ChangeListener (TabbedButtonBar is a ChangeBroadcaster): tab switch
    // -> swap the column mask + relabel the header.
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        setActiveTab (tabStrip_.getCurrentTabIndex() == 1);
    }

private:
    PatchPage& owner_;
    ColumnHeader header_;

    // Last resized() geometry (test pins: content <= sum-of-maxes + gaps and
    // the centring offset >= 0 at wide editors).
    int lastBandW_ = 0, lastContW_ = 0;
    juce::TabbedButtonBar tabStrip_ { juce::TabbedButtonBar::TabsAtTop };

public:
    static constexpr int kTabBarH = 24;   // compact segmented toggle (GroupPager uses 28 for page pills)

    // Test hooks: the ACTIVE tab (0 = Voice, 1 = MIDI), its header captions,
    // and the ACTIVE column mask (the rows' visible set).
    int activeTabForTest() const { return tabStrip_.getCurrentTabIndex(); }

    // Test pins for the fixed-width centred policy: the band width and the
    // actual table content width from the last resized().
    int bandWidthForTest() const { return lastBandW_; }
    int contentWidthForTest() const { return lastContW_; }
    juce::StringArray headerLabelsForTest() const { return header_.labels(); }
    const bool* visibleMaskForTest() const
    { return tabStrip_.getCurrentTabIndex() == 1 ? kMidiTabMask : kVoiceTabMask; }

    // Test hooks: the header's painted column x and ROW 0's cell column x
    // for @p i — the alignment pin compares them per visible column.
    int headerColumnXForTest (int i) const { return header_.columnXForTest (i); }
    juce::String headerCaptionForTest (int col) const { return header_.captionForColumn (col); }

    int rowColumnXForTest (int i) const
    { return owner_.rows_.empty() ? -1 : owner_.rows_[0]->columnXForTest (i); }

    // Test hook: the tab strip's x (the LEFTMOST-control pin; the strip is a
    // direct child of this panel, in panel-local coords).
    int tabStripXForTest() const { return tabStrip_.getX(); }

    // Drive the segmented toggle exactly as a click does (fires the change
    // callback through the bar, so rows + header follow the real path).
    // sendChangeMessage is ASYNC (triggerAsyncUpdate), so the hook ALSO
    // applies synchronously — the async callback is idempotent (same index
    // -> same mask) so test timing cannot flake.
    void setCurrentTabIndexForTest (int tabIndex)
    {
        const int t = juce::jlimit (0, 1, tabIndex);
        setActiveTab (t == 1);
        tabStrip_.setCurrentTabIndex (t, juce::dontSendNotification);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PartTablePanel)
};

//==============================================================================
PatchPage::PatchPage (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : proc_ (processor), themeManager_ (themeManager),
      scrollBody_ (std::make_unique<ScrollBody> (*this)),
      tablePanel_ (std::make_unique<PartTablePanel> (*this))
{
    buildArrangementCombo();
    arrangementCombo_.onChange = [this] { onArrangementChanged(); };
    // HIG touch target: 26pt visual box inside a 44pt tap band (the summary
    // row's full height — see PartTablePanel::resized).
    arrangementCombo_.getProperties().set ("parvatiComboVisualH", 26);
    // The arrangement selector + the pool-budget readout live INSIDE the
    // Global panel's table (the summary row above the 6 part rows), not on
    // the page chrome — the panel reads top-down: arrangement, then the rows.
    tablePanel_->addAndMakeVisible (arrangementCombo_);

    // ---- Ambika export buttons (summary row, right edge). Export ONLY: the
    // top-bar Load/Save are .parvati (2026-08-20); these are the explicit
    // hardware-shareable paths (.PRO = current part, byte-faithful;
    // .MUL = whole 6-Part setup, incl. the voice-slot fallback dialog when a
    // Part requests more voices than its voicecards). Desktop gating lives in
    // the EDITOR's wiring (the file pickers need a window server); the
    // callbacks are null-safe for headless tests.
    exportProButton_.onClick = [this] { clickExportProForTest(); };
    exportMulButton_.onClick = [this] { clickExportMulForTest(); };
    // PROPER BUTTON CHROME (2026-08-23 user request): the default flat tonal
    // block read as floating text on the table panel. Both buttons carry the
    // "parvatiButtonOutlined" property (ParvatiLookAndFeel::drawButtonBackground
    // strokes a 1px rounded outline derived from the text colour) plus an
    // accent-tinted fill/text pair re-resolved on every theme change via
    // applyExportButtonChrome().
    // Text + tooltips at construction (refreshLanguage re-translates them on
    // a live language switch — the editor calls it after building).
    exportProButton_.setTooltip (
        TRANS ("Export this part as an Ambika .PRO patch ")
        + TRANS ("(byte-faithful, hardware-shareable; Parvati-only options ")
        + TRANS ("— VCA curve, filter card, arp — are not carried; use Save ")
        + TRANS ("(.parvati) for the full patch)."));
    exportMulButton_.setTooltip (
        TRANS ("Export the whole 6-part setup as an Ambika .MUL multi ")
        + TRANS ("(hardware-shareable; if a part needs more voices than its ")
        + TRANS ("voicecards, the export-fallback dialog maps them onto the ")
        + TRANS ("6 cards)."));
    tablePanel_->addAndMakeVisible (exportProButton_);
    tablePanel_->addAndMakeVisible (exportMulButton_);
    exportProButton_.getProperties().set ("parvatiButtonOutlined", true);
    exportMulButton_.getProperties().set ("parvatiButtonOutlined", true);
    applyExportButtonChrome();

    // Pool-budget readout: how many of the 96 pool voices are allocated
    // across all parts (sum of the per-part voiceCount_ snapshots) — the only
    // budget label in the voice-first model (any combination of per-part
    // counts is legal, so there is nothing to cap).

    for (int i = 0; i < kNumParts; ++i)
    {
        rows_[ (size_t) i] = std::make_unique<PartRow> (*this, i);
        // The rows live inside the table panel (their layout lives in its
        // resized()); the panel is attached into the hosted page's Global
        // group in hostParamPage.
        tablePanel_->addAndMakeVisible (*rows_[ (size_t) i]);
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
    // Export buttons re-skin with the (possibly new) accent.
    applyExportButtonChrome();
    repaint();
}

void PatchPage::applyExportButtonChrome()
{
    // Accent-tinted ACTION look for the two Ambika export buttons: a subtle
    // accent wash fill + accent text (the L&F adds the matching outline via
    // the parvatiButtonOutlined property; hover brightens both). WithAlpha
    // colours must be RE-SET per theme (they are baked at call time), which
    // is why this lives here and is called from applyThemeColors.
    const auto accent = themeManager_.getCurrentTheme().accentPrimary;
    for (auto* b : { &exportProButton_, &exportMulButton_ })
    {
        b->setColour (juce::TextButton::buttonColourId, accent.withAlpha (0.16f));
        b->setColour (juce::TextButton::textColourOffId, accent);
        b->setColour (juce::TextButton::textColourOnId,
                      themeManager_.getCurrentTheme().backgroundBase);
    }
}

void PatchPage::refreshLanguage()
{
    buildArrangementCombo();
    for (auto& r : rows_)
        r->refreshLanguage();
    if (tablePanel_ != nullptr)
        tablePanel_->refreshLanguage();   // the column-header strip
    // Export buttons (TRANS text + tooltips re-applied on a live switch).
    exportProButton_.setButtonText (TRANS ("Export .PRO"));
    exportMulButton_.setButtonText (TRANS ("Export .MUL"));
    exportProButton_.setTooltip (
        TRANS ("Export this part as an Ambika .PRO patch ")
        + TRANS ("(byte-faithful, hardware-shareable; Parvati-only options ")
        + TRANS ("— VCA curve, filter card, arp — are not carried; use Save ")
        + TRANS ("(.parvati) for the full patch)."));
    exportMulButton_.setTooltip (
        TRANS ("Export the whole 6-part setup as an Ambika .MUL multi ")
        + TRANS ("(hardware-shareable; if a part needs more voices than its ")
        + TRANS ("voicecards, the export-fallback dialog maps them onto the ")
        + TRANS ("6 cards)."));
    repaint();
}

void PatchPage::clickExportProForTest()
{
    // The REAL button path (both the button's onClick and tests land here):
    // fire the editor seam; null-safe for headless construction.
    if (onExportPro)
        onExportPro();
}

void PatchPage::clickExportMulForTest()
{
    if (onExportMul)
        onExportMul();
}

juce::String PatchPage::exportProTooltipForTest() { return exportProButton_.getTooltip(); }
juce::String PatchPage::exportMulTooltipForTest() { return exportMulButton_.getTooltip(); }

void PatchPage::setTableTooltipsEnabled (bool)
{
    // The gate is ParamControl::tooltipsEnabled() (the editor flips it via
    // ParamControl::setTooltipsEnabled before calling this); rows re-read it
    // and blank/restore their cell tooltips accordingly.
    for (auto& r : rows_)
        r->applyTooltipState();
}

juce::StringArray PatchPage::headerLabelsForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->headerLabelsForTest()
                                  : juce::StringArray();
}

// Test hooks: the six part rows' DISPLAYED name labels, in part order
juce::StringArray PatchPage::displayedPartNamesForTest() const
{
    juce::StringArray out;
    for (const auto& r : rows_)
        out.add (r->displayedNameForTest());
    return out;
}

bool PatchPage::tableTooltipsCompleteForTest()
{
    return std::all_of (rows_.begin(), rows_.end(),
        [] (const std::unique_ptr<PartRow>& r) { return r->allCellsHaveTooltipsForTest(); });
}

int PatchPage::activeTableTabForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->activeTabForTest() : 0;
}

const bool* PatchPage::tableVisibleMaskForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->visibleMaskForTest() : kVoiceTabMask;
}

// Fixed-width centred table policy pins (band vs content from last resized).
int PatchPage::tableBandWidthForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->bandWidthForTest() : -1;
}

int PatchPage::tableContentWidthForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->contentWidthForTest() : -1;
}

void PatchPage::chooseTableTabForTest (int tabIndex)
{
    if (tablePanel_ != nullptr)
        tablePanel_->setCurrentTabIndexForTest (tabIndex);
}

// Alignment pins: the header's painted column x vs row 0's cell x for the
// same column (equal per visible column), and the tab strip's x position.
int PatchPage::headerColumnXForTest (int column) const
{
    return tablePanel_ != nullptr ? tablePanel_->headerColumnXForTest (column) : -1;
}

// The caption paint() draws over @p column (the label↔column binding).
juce::String PatchPage::headerCaptionForTest (int column) const
{
    return tablePanel_ != nullptr ? tablePanel_->headerCaptionForTest (column)
                                  : juce::String();
}

int PatchPage::rowColumnXForTest (int column) const
{
    return tablePanel_ != nullptr ? tablePanel_->rowColumnXForTest (column) : -1;
}

int PatchPage::tabStripXForTest() const
{
    return tablePanel_ != nullptr ? tablePanel_->tabStripXForTest() : -1;
}

void PatchPage::buildArrangementCombo()
{
    // The 5 selectable templates (ids 1..5, matching Arrangement order) + a
    // separator + "Custom" as a REAL but DISABLED item (id 6): Custom is
    // infer-only state, never user-selectable — but a selected-but-disabled
    // item still displays at full strength, so a loaded patch that matches no
    // preset never renders as a ghosted textWhenNothingSelected placeholder
    // (juce draws that at 50% alpha — the wrong-style "Custom" text bug).
    const int prev = arrangementCombo_.getSelectedId();
    // dontSendNotification: the DEFAULT clear() posts an async change message
    // that fires onChange AFTER the rebuild has restored the selection below —
    // onArrangementChanged would then re-apply a possibly-stale template over
    // engine state (e.g. right after a host recall the page has not re-read
    // yet). A programmatic rebuild must never drive the write path.
    arrangementCombo_.clear (juce::dontSendNotification);
    for (int i = 0; i < arrangementCount(); ++i)
        arrangementCombo_.addItem (TRANS (arrangementLabel (static_cast<Arrangement> (i))), i + 1);
    arrangementCombo_.addSeparator();
    arrangementCombo_.addItem (TRANS ("Custom"), 6);
    arrangementCombo_.setItemEnabled (6, false);
    arrangementCombo_.setSelectedId (prev, juce::dontSendNotification);
}

void PatchPage::setArrangementFromEngine()
{
    const Arrangement a = inferArrangement (proc_.getEngine());
    refreshing_ = true;
    // Custom is the combo's REAL (disabled, infer-only) id-6 item — a full-
    // strength label, never the ghosted nothing-selected placeholder.
    if (a == Arrangement::Custom)
        arrangementCombo_.setSelectedId (6, juce::dontSendNotification);
    else
        arrangementCombo_.setSelectedId (static_cast<int> (a) + 1, juce::dontSendNotification);
    refreshing_ = false;
}

void PatchPage::onArrangementChanged()
{
    if (refreshing_) return;
    const int id = arrangementCombo_.getSelectedId();
    if (id < 1 || id > arrangementCount()) return;   // id 6 (Custom) can never fire: it is disabled
    applyArrangement (proc_.getEngine(), static_cast<Arrangement> (id - 1));
    refresh();
    // applyArrangement writes each part's polyphony ENGINE-DIRECT (setCurrentPart
    // + applyPartByte(15,...)). The hosted globalPage_ knob for `part_polyphony`
    // reads from the APVTS, so the current part's APVTS value goes stale after an
    // apply. Re-sync the current part's engine state into the APVTS (existing
    // public machinery) so the knob + an APVTS-based save reflect the new mode.
    proc_.loadPartIntoApvts (proc_.getEngine().getCurrentPart());
}

Arrangement PatchPage::getDisplayedArrangement() const
{
    const int id = arrangementCombo_.getSelectedId();
    if (id == 6)
        return Arrangement::Custom;   // the infer-only tail item
    return (id >= 1 && id <= arrangementCount())
        ? static_cast<Arrangement> (id - 1) : Arrangement::Custom;
}

int PatchPage::getDisplayedTuningMode (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedTuningMode();
}

void PatchPage::chooseTuningMode (int part, int mode)
{
    if (part < 0 || part >= kNumParts || mode < 0 || mode > 33) return;
    rows_[(size_t) part]->chooseTuningMode (mode);
}

int PatchPage::getDisplayedVoiceSlots (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedVoiceSlots();
}

int PatchPage::getDisplayedPolyphony (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedPolyphony();
}

int PatchPage::getDisplayedOctave (int part) const
{
    if (part < 0 || part >= kNumParts) return 0;
    return rows_[(size_t) part]->displayedOctave();
}

void PatchPage::chooseOctave (int part, int octaves)
{
    if (part < 0 || part >= kNumParts) return;   // clamped inside the row
    rows_[(size_t) part]->chooseOctave (octaves);
}

int PatchPage::getDisplayedLegato (int part) const
{
    if (part < 0 || part >= kNumParts) return 0;
    return rows_[(size_t) part]->displayedLegato();
}

void PatchPage::chooseLegato (int part, int on)
{
    if (part < 0 || part >= kNumParts) return;
    rows_[(size_t) part]->chooseLegato (on);
}

int PatchPage::getDisplayedPortamento (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedPortamento();
}

void PatchPage::choosePortamento (int part, int value)
{
    if (part < 0 || part >= kNumParts) return;   // clamped inside the row
    rows_[(size_t) part]->choosePortamento (value);
}

int PatchPage::getDisplayedVolume (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedVolume();
}

void PatchPage::chooseVolume (int part, int value)
{
    if (part < 0 || part >= kNumParts) return;   // clamped inside the row
    rows_[(size_t) part]->chooseVolume (value);
}

int PatchPage::getDisplayedFineTune (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedFineTune();
}

void PatchPage::chooseFineTune (int part, int value)
{
    if (part < 0 || part >= kNumParts) return;   // clamped inside the row
    rows_[(size_t) part]->chooseFineTune (value);
}

int PatchPage::getDisplayedSpread (int part) const
{
    if (part < 0 || part >= kNumParts) return -1;
    return rows_[(size_t) part]->displayedSpread();
}

void PatchPage::chooseSpread (int part, int value)
{
    if (part < 0 || part >= kNumParts) return;   // clamped inside the row
    rows_[(size_t) part]->chooseSpread (value);
}

void PatchPage::chooseVoiceSlots (int part, int slots)
{
    if (part < 0 || part >= kNumParts) return;   // slots clamped inside the row (0 disables the part)
    rows_[(size_t) part]->chooseVoiceSlots (slots);
}

void PatchPage::refresh()
{
    refreshing_ = true;
    for (auto& r : rows_)
        r->refresh();
    refreshing_ = false;
    setArrangementFromEngine();
}

void PatchPage::postPartEdit()
{
    for (auto& r : rows_)
        r->updateDimState();
    setArrangementFromEngine();
}

// A part name was edited: forward to the editor (Part-selector relabel) if a
// callback is installed.
void PatchPage::partNamesChanged()
{
    if (onPartNamesChanged)
        onPartNamesChanged();
}

void PatchPage::resized()
{
    // No page-level heading chrome: the scrolled body starts directly at the
    // hosted Global panel, whose table's first row IS the arrangement summary
    // (the combo + the "Voices Y/96" readout) — the arrangement lives with
    // the per-part rows it configures, not above them.
    auto area = getLocalBounds().reduced (16);

    // The whole body (hosted Global panel with its part rows) scrolls
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
    // (just the hosted patch-wide ParamPage — whose Global panel CONTAINS the
    // 6-part voice-allocation table as an external decoration — expressed in
    // the body's local coordinates).
    auto layoutAtWidth = [this] (int cw)
    {
        int y = 0;
        // Hosted patch-wide ParamPage (Part/Play + Global panels with the
        // merged allocation table) — the whole body. The part rows are laid
        // out by tablePanel_'s resized() (the table is INSIDE the hosted
        // page), so nothing but the page bounds happens here.
        if (hostedParamPage_ != nullptr)
        {
            // -1 = NATURAL-HEIGHT reflow: the hosted page is sized to its
            // wrapped content only. With 0 the reflow falls back to THIS
            // page's scroll Viewport height and stretches the hosted page to
            // fill it — a big void below the Global panel.
            hostedParamPage_->reflowToWidth (juce::jmax (200, cw), -1);
            hostedParamPage_->setBounds (0, y, cw,
                                         juce::jmax (200, hostedParamPage_->getContentHeight()));
            y += hostedParamPage_->getHeight();
            y += 10;   // breathing gap below the hosted page at the body tail
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
    // with the part rows (T4). Editor retains ownership; reparent only.
    if (hostedParamPage_ != nullptr)
    {
        scrollBody_->addAndMakeVisible (hostedParamPage_);
        // END STATE (completing absorption, 2026-08-20): the hosted page
        // renders ONLY [Global panel: the 3 global-option knobs + the 6-part
        // table] — every per-part setting (incl. Vol/Fine/Spr) is a table
        // column. The table
        // rides the page's EXTERNAL decoration slot (non-owning):
        // PatchPage keeps owning tablePanel_ (which parents the rows); the
        // page only parents + positions it. Contract: tablePanel_ must
        // outlive the hosted page, or no relayout may run after this page
        // dies (JUCE removes the child cleanly on destruction, so teardown
        // itself is safe in any order).
        hostedParamPage_->setGroupExternalDecoration ("Global", tablePanel_.get (),
                                                      PartTablePanel::kTableH);
    }
    resized();
}
