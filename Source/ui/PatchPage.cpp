// Phase 2 of the "Patch page" feature: the Patch juce::Component implementation.
// See PatchPage.h for the design summary and /tmp/parvati_patch_design.md
// ("Phase 2") for the full spec. Phase 1 (PatchArrangement.{h,cpp}) supplies
// applyArrangement / inferArrangement; this component drives the engine purely
// through its EXISTING public setters.

#include "PatchPage.h"

#include "PatchArrangement.h"

#include "PluginProcessor.h"   // ParvatiAudioProcessor::getEngine()
#include "PluginEditor.h"      // ParamPage complete type (reflowToWidth/getContentHeight)
#include "TuningTables.h"      // tuningPresetName (Tune combo items)
#include "ui/NoteName.h"       // midiNoteName (key-zone knob readouts)

#include <cstdint>

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

        auto setupCaption = [this] (juce::Label& l) {
            l.setJustificationType (juce::Justification::centredLeft);
            l.setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (l);
        };
        setupCaption (voicesCaption_);
        setupCaption (chCaption_);
        setupCaption (zoneLoCaption_);
        setupCaption (zoneHiCaption_);
        setupCaption (octCaption_);
        setupCaption (portaCaption_);
        setupCaption (lgoCaption_);
        setupCaption (polyCaption_);
        setupCaption (tuneCaption_);

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
        voicesCombo_.setTooltip (
            TRANS ("How many voices this part plays at once, drawn from the shared ")
            + TRANS ("96-voice pool (0-16). 0 disables the part entirely — it gets no ")
            + TRANS ("voice in the pool and stops sounding. Every part can be maxed out ")
            + TRANS ("at the same time — the pool holds 6 x 16. The hardware voicecards ")
            + TRANS ("are shared out automatically for the individual outputs and the ")
            + TRANS (".MUL hardware export."));
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
    void resized() override
    {
        auto b = getLocalBounds().reduced (4);

        partLabel_.setBounds (b.removeFromLeft (156));
        b.removeFromLeft (6);

        // Voices (the part's pool voice count; 44pt HIG tap band)
        {
            auto col = b.removeFromLeft (76);
            voicesCaption_.setBounds (col.removeFromTop (12));
            voicesCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        // Ch (68pt: the widest item "Omni" + the combo's dropdown-arrow
        // reserve at 13pt text needs ~64pt — 56 truncated it; the row still
        // consumes ~712pt, well inside the 1024 floor)
        {
            auto col = b.removeFromLeft (68);
            chCaption_.setBounds (col.removeFromTop (12));
            channelCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        // Zone Low knob. Caption band 14->12 (the same trick T7 applied to the
        // combo captions) so the 56pt row yields a full 44pt band for the
        // dial — the HIG touch minimum (was a 40px dial in a 42px band).
        {
            auto col = b.removeFromLeft (48);
            zoneLoCaption_.setBounds (col.removeFromTop (12));
            loSlider_.setBounds (col.withSizeKeepingCentre (44, juce::jmin (44, col.getHeight())));
        }
        // Zone High knob (same 44pt band as Zone Low).
        {
            auto col = b.removeFromLeft (48);
            zoneHiCaption_.setBounds (col.removeFromTop (12));
            hiSlider_.setBounds (col.withSizeKeepingCentre (44, juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (8);
        // ---- Part-character columns absorbed from the old "Part / Play"
        // page knobs: Oct (transpose), Porta (glide), Lgo (legato). 48pt each
        // + 4pt gaps = 152pt; the row totals ~824pt against the ~952pt
        // working width at the 1024 floor (see the layout comment above). ----
        {
            auto col = b.removeFromLeft (48);
            octCaption_.setBounds (col.removeFromTop (12));
            octaveCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        {
            auto col = b.removeFromLeft (48);
            portaCaption_.setBounds (col.removeFromTop (12));
            portaSlider_.setBounds (col.withSizeKeepingCentre (44, juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        {
            auto col = b.removeFromLeft (48);
            lgoCaption_.setBounds (col.removeFromTop (12));
            legatoCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        // ---- Output columns absorbed from the old "Part / Play" page knobs:
        // Vol (volume, byte 0), Fine (fine tuning, SIGNED byte 2), Spr (detune
        // spread, byte 3). 36pt cells with 2pt gaps = 114pt (see the width
        // arithmetic in the layout comment above). ----
        {
            auto col = b.removeFromLeft (36);
            volCaption_.setBounds (col.removeFromTop (12));
            volSlider_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (2);
        {
            auto col = b.removeFromLeft (36);
            fineCaption_.setBounds (col.removeFromTop (12));
            fineSlider_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (2);
        {
            auto col = b.removeFromLeft (36);
            sprCaption_.setBounds (col.removeFromTop (12));
            sprSlider_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        // Tune (microtonal scale preset / Custom… popover)
        {
            auto col = b.removeFromLeft (110);
            tuneCaption_.setBounds (col.removeFromTop (12));
            tuneCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
        b.removeFromLeft (4);
        // Poly (sized to the dropdown width - no longer the row tail)
        {
            auto col = b.removeFromLeft (juce::jmin (140, b.getWidth()));
            polyCaption_.setBounds (col.removeFromTop (12));
            polyCombo_.setBounds (col.withSizeKeepingCentre (col.getWidth(), juce::jmin (44, col.getHeight())));
        }
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
        voicesCaption_.setText (TRANS ("Voices"), juce::dontSendNotification);
        chCaption_.setText (TRANS ("Ch"), juce::dontSendNotification);
        zoneLoCaption_.setText (TRANS ("Zone Low"), juce::dontSendNotification);
        zoneHiCaption_.setText (TRANS ("Zone High"), juce::dontSendNotification);
        octCaption_.setText (TRANS ("Oct"), juce::dontSendNotification);
        portaCaption_.setText (TRANS ("Porta"), juce::dontSendNotification);
        lgoCaption_.setText (TRANS ("Lgo"), juce::dontSendNotification);
        volCaption_.setText (TRANS ("Vol"), juce::dontSendNotification);
        fineCaption_.setText (TRANS ("Fine"), juce::dontSendNotification);
        sprCaption_.setText (TRANS ("Spr"), juce::dontSendNotification);
        polyCaption_.setText (TRANS ("Polyphony"), juce::dontSendNotification);
        tuneCaption_.setText (TRANS ("Tune"), juce::dontSendNotification);

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

    // The Poly combo's currently-displayed mode (0..4 = the combo id - 1).
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

    // The Voices combo's currently-displayed voice count (0..16; 0 = the
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

    // The Tune combo's currently-displayed mode (0..32).
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

    juce::Label partLabel_, voicesCaption_, chCaption_, zoneLoCaption_, zoneHiCaption_,
                 octCaption_, portaCaption_, lgoCaption_, polyCaption_, tuneCaption_;
    juce::Label volCaption_, fineCaption_, sprCaption_;   // output columns (Vol/Fine/Spr)
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
class PatchPage::PartTablePanel : public juce::Component
{
public:
    explicit PartTablePanel (PatchPage& owner) : owner_ (owner) {}

    // Summary row height + gap above the part rows.
    static constexpr int kSummaryH = 44;
    static constexpr int kSummaryGap = 8;
    // Natural panel height: 4px top inset + the arrangement summary row
    // (44px + 8px gap) + 6 rows x 56 + 5 gaps x 4 + 4px bottom inset. The
    // reserved external-decoration height the hosted page uses for the
    // group's layout (see hostParamPage).
    static constexpr int kTableH = 4 + kSummaryH + kSummaryGap + 6 * 56 + 5 * 4 + 4;

    void resized() override
    {
        constexpr int rowH = 56;
        constexpr int rowGap = 4;
        constexpr int inset = 4;
        auto b = getLocalBounds().reduced (inset, inset);

        // ---- Arrangement summary row: the arrangement combo (220pt wide, a
        // 44pt HIG tap band with a 26pt visual box — same idiom as the part
        // rows) + the "Voices Y/96" pool-budget readout to its right.
        // PatchPage-owned members parented into this panel (a nested class
        // has access). ----
        {
            auto summary = b.removeFromTop (kSummaryH);
            owner_.arrangementCombo_.setBounds (summary.removeFromLeft (220));
            summary.removeFromLeft (12);
            owner_.voicesTotalLabel_.setBounds (summary);
        }
        b.removeFromTop (kSummaryGap);

        for (int i = 0; i < kNumParts; ++i)
        {
            // rows_ is PatchPage-private; a nested class has access.
            owner_.rows_[ (size_t) i]->setBounds (b.removeFromTop (rowH));
            b.removeFromTop (rowGap);
        }
    }

private:
    PatchPage& owner_;

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

    // Pool-budget readout: how many of the 96 pool voices are allocated
    // across all parts (sum of the per-part voiceCount_ snapshots) — the only
    // budget label in the voice-first model (any combination of per-part
    // counts is legal, so there is nothing to cap).
    voicesTotalLabel_.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    voicesTotalLabel_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accentPrimary);
    voicesTotalLabel_.setJustificationType (juce::Justification::centredLeft);
    tablePanel_->addAndMakeVisible (voicesTotalLabel_);

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
    const auto theme = themeManager_.getCurrentTheme();
    voicesTotalLabel_.setColour (juce::Label::textColourId, theme.accentPrimary);
    repaint();
}

void PatchPage::refreshLanguage()
{
    buildArrangementCombo();
    for (auto& r : rows_)
        r->refreshLanguage();
    updateVoicesTotal();   // "Voices Y/96" is TRANS-built chrome
    repaint();
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

void PatchPage::updateVoicesTotal()
{
    // Pool budget: the sum of the per-part ASSIGNED slots (getPartVoiceSlots)
    // — the user-configured counts the rows below this label edit, read
    // immediately on the message thread. The old basis (the
    // audio-thread-published voiceCount_ snapshot) was wrong for this readout:
    // it lagged a fresh slots edit until the next process block (so a part
    // lowered to 1 kept reading 16) and reflected CHAIN's doubled voice sets,
    // so the label disagreed with the rows. Assigned slots are the honest
    // "how big is my patch" number: one active part with 1 voice reads
    // "Voices 1/96".
    int voices = 0;
    for (int p = 0; p < kNumParts; ++p)
        voices += proc_.getEngine().getPartVoiceSlots (p);
    voicesTotalLabel_.setText (TRANS ("Voices") + " " + juce::String (voices) + "/"
                                   + juce::String (kNumVoices),
                               juce::dontSendNotification);
}

void PatchPage::refresh()
{
    refreshing_ = true;
    for (auto& r : rows_)
        r->refresh();
    refreshing_ = false;
    updateVoicesTotal();
    setArrangementFromEngine();
}

void PatchPage::postPartEdit()
{
    for (auto& r : rows_)
        r->updateDimState();
    updateVoicesTotal();
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
