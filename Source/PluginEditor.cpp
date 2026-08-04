// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See PluginEditor.h.

#include "PluginEditor.h"
#include "PatchFile.h"
#include "ParvatiPreset.h"
#include "ui/EnvelopeDisplay.h"
#include "ui/ParamHelp.h"
#include "ui/WheelsComponent.h"
#include "ui/Translations.h"

#include <algorithm>   // std::remove for the ParamControl instance registry

namespace
{
// ---- Map a parameter ID to one of the GUI sections --------------------------
// (Derived from the well-defined paramID prefixes in ParameterLayout.cpp, so the
//  verified APVTS byte-bridge stays untouched.)
enum class Section { Oscillators, Mixer, Filter, Envelopes, Lfos, ModMatrix, Modifiers, Arp, Sequencer, Global, Multi };

Section sectionForId (const juce::String& id)
{
    // Global synth options (no Patch/Part byte) live on the dedicated Global
    // tab. Check these BEFORE the prefix rules so e.g. "filter_card" (a global
    // option) is not swept into the Filter page by the "filter" prefix.
    if (id == "filter_card")  return Section::Global;
    if (id == "vca_curve")    return Section::Global;
    if (id == "filter_drive") return Section::Global;   // Ladder drive: a global option, like filter_card
    // Order matters: "modif" before "mod", "arp" before others.
    if (id.startsWith ("arp"))       return Section::Arp;
    if (id.startsWith ("seq"))       return Section::Sequencer;
    if (id.startsWith ("osc"))       return Section::Oscillators;
    if (id.startsWith ("mix"))       return Section::Mixer;
    if (id.startsWith ("filter"))    return Section::Filter;
    if (id.startsWith ("modif"))     return Section::Modifiers;
    if (id.startsWith ("mod"))       return Section::ModMatrix;
    // Each firmware env_lfo unit is BOTH an envelope (A/D/S/R) and an LFO
    // (shape/rate); they are independent modulation sources, so the two halves
    // route to separate Envelopes / LFOs tabs (the struct sharing is a firmware
    // memory-layout detail only). voice_lfo_* is the per-voice LFO (MOD_SRC_LFO_4).
    if (id.startsWith ("voice_lfo")) return Section::Lfos;
    if (id.startsWith ("env"))       return id.contains ("_lfo_") ? Section::Lfos : Section::Envelopes;
    if (id.startsWith ("part"))      return Section::Global;   // part volume/legato/portamento
    return Section::Global;
}
}  // namespace

//==============================================================================
//==============================================================================
bool ParamControl::tooltipsEnabled_ = true;

namespace
{
// Live ParamControl instances (message-thread only: built/destroyed by the GUI
// component tree, toggled from the Settings panel). Function-local static avoids
// static-initialization-order issues across translation units.
std::vector<ParamControl*>& paramControlRegistry()
{
    static std::vector<ParamControl*> r;
    return r;
}
}  // namespace

//==============================================================================
ParamControl::ParamControl (ParvatiAudioProcessor& processor, const PatchParamDescriptor& d)
    : desc_ (d), processor_ (processor), paramIDStr_ (d.paramID)
{
    label_ = std::make_unique<juce::Label> (d.paramID + "_lbl", d.label);
    label_->setJustificationType (juce::Justification::centred);
    label_->setFont (juce::FontOptions (12.0f));
    // Label / combo / slider colours all come from the editor-wide
    // ParvatiLookAndFeel (inherited through the component tree).
    addAndMakeVisible (*label_);

    if (d.choices != nullptr)
    {
        comboBox_ = std::make_unique<juce::ComboBox> (d.paramID);
        comboBox_->addItemList (*d.choices, 1);
        addAndMakeVisible (*comboBox_);
        comboAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.getApvts(), d.paramID, *comboBox_);
        // Catch right-clicks on the combo (it would otherwise swallow the popup
        // click before this component sees it). `false` => events for the combo
        // only (no recursion into its popup children).
        comboBox_->addMouseListener (this, false);
    }
    else
    {
        slider_ = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag,
                                                   juce::Slider::TextBoxBelow);
        slider_->setTextBoxIsEditable (true);
        if (d.isSequencer)
        {
            // The length control is marked ("Length" label + accent arc) so it
            // reads as the sequence length, not just another step pot. The step
            // cells stay compact (label hidden; the group header "Sequencer n"
            // identifies them) and are dimmed when past the active length
            // (see refreshStepEnabled, wired below).
            if (paramIDStr_.startsWith ("seq_length_"))
            {
                label_->setText (TRANS ("Length"), juce::dontSendNotification);
                label_->setFont (juce::FontOptions (12.0f, juce::Font::bold));
                slider_->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 16);
                if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
                    if (auto* theme = lnf->getTheme())
                        slider_->setColour (juce::Slider::rotarySliderFillColourId, theme->accent);
            }
            else
            {
                label_->setVisible (false);
                slider_->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 36, 14);
            }
        }
        addAndMakeVisible (*slider_);
        sliderAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.getApvts(), d.paramID, *slider_);
        // Catch right-clicks on the knob/text-box (same reason as the combo).
        slider_->addMouseListener (this, false);
    }

    // Cache the per-parameter help text and push it onto the interactive
    // children. The editor-wide TooltipWindow only queries the LEAF component
    // under the cursor, so the cell's own TooltipClient is insufficient — the
    // mouse actually hovers the Slider/ComboBox/Label child. Register the
    // instance so the global on/off toggle (setTooltipsEnabled) can re-apply
    // the text later without rebuilding the controls.
    helpText_ = getParamHelp (d.paramID);
    paramControlRegistry().push_back (this);
    applyTooltipState();

    // Sequencer steps subscribe to their sibling length param so they dim/enable
    // live as the length changes. Seeded once here from the current value.
    lengthParamID_ = siblingLengthParamFor (paramIDStr_);
    if (lengthParamID_.isNotEmpty())
    {
        processor_.getApvts().addParameterListener (lengthParamID_, this);
        refreshStepEnabled();
    }
}

ParamControl::~ParamControl()
{
    if (lengthParamID_.isNotEmpty())
        processor_.getApvts().removeParameterListener (lengthParamID_, this);
    auto& r = paramControlRegistry();
    r.erase (std::remove (r.begin(), r.end(), this), r.end());
}

//==========================================================================
// Sequencer step dimming: map a step paramID to its sibling sequence length
// param, parse the step index, and enable/disable the step's slider so steps
// past the active length read as inactive (the LookAndFeel omits the knob's
// fill arc when a slider is disabled).
juce::String ParamControl::siblingLengthParamFor (const juce::String& id)
{
    if (id.startsWith ("seq1_step")) return "seq_length_1";
    if (id.startsWith ("seq2_step")) return "seq_length_2";
    if (id.startsWith ("seqnote_step") || id.startsWith ("seqnote_vel")) return "seq_length_3";
    return {};
}

int ParamControl::parseStepIndex (const juce::String& id)
{
    // Extract the trailing digit run (e.g. "seq1_step7" -> 7,
    // "seqnote_vel11" -> 11). Steps are the only params with a non-empty
    // lengthParamID_, so a stray number elsewhere is harmless.
    int lastDigit = id.length() - 1;
    while (lastDigit >= 0 && id[lastDigit] >= '0' && id[lastDigit] <= '9')
        --lastDigit;
    if (lastDigit + 1 < id.length())
        return id.substring (lastDigit + 1).getIntValue();
    return -1;
}

void ParamControl::parameterChanged (const juce::String&, float)
{
    refreshStepEnabled();
}

void ParamControl::refreshStepEnabled()
{
    if (lengthParamID_.isEmpty() || slider_ == nullptr)
        return;
    if (auto* raw = processor_.getApvts().getRawParameterValue (lengthParamID_))
    {
        // The length param's verified range is 1..32 (ParameterLayout.cpp), but
        // only 16 step cells exist, so cap the comparison at 16.
        const int len = juce::jlimit (1, 16, juce::roundToInt (raw->load()));
        const int idx = parseStepIndex (paramIDStr_);
        const bool on = (idx < 0) || (idx < len);
        slider_->setEnabled (on);
        repaint();
    }
}

void ParamControl::applyTooltipState()
{
    const juce::String tip = tooltipsEnabled_ ? helpText_ : juce::String();
    if (label_    != nullptr) label_->setTooltip (tip);
    if (slider_   != nullptr) slider_->setTooltip (tip);
    if (comboBox_ != nullptr) comboBox_->setTooltip (tip);
}

void ParamControl::setTooltipsEnabled (bool enabled)
{
    tooltipsEnabled_ = enabled;
    for (auto* c : paramControlRegistry())
        c->applyTooltipState();
}

juce::String ParamControl::getTooltip()
{
    // When tooltips are disabled (Settings panel toggle), return an empty String
    // so the editor's TooltipWindow shows nothing. Cleaner than recreating the
    // window or toggling its visibility.
    return tooltipsEnabled_ ? getParamHelp (desc_.paramID) : juce::String();
}

void ParamControl::resized()
{
    auto b = getLocalBounds().reduced (2);
    // Reserve the label band for non-sequencer controls AND for the marked
    // length control (which shows "Length"); plain step cells hide the label.
    if (! desc_.isSequencer || paramIDStr_.startsWith ("seq_length_"))
    {
        label_->setBounds (b.removeFromTop (15));
        b.removeFromTop (3);
    }

    if (slider_)
    {
        slider_->setBounds (b);
    }
    else if (comboBox_)
    {
        comboBox_->setBounds (b.withSizeKeepingCentre (b.getWidth(),
                                                       juce::jmin (26, b.getHeight())));
    }
}

//==========================================================================
// Phase 4b: right-click context menu (Reset to default / Randomize).
void ParamControl::mouseDown (const juce::MouseEvent& e)
{
    // Only popup (right-click / Ctrl-click) triggers the menu; every other
    // click falls through to normal slider/combo interaction.
    if (e.mods.isPopupMenu())
        showContextMenu();
}

void ParamControl::showContextMenu()
{
    juce::PopupMenu menu;
    // SafePointer guards against the control being deleted while the async
    // menu is still open (e.g. editor closed mid-menu).
    juce::Component::SafePointer<ParamControl> safe (this);
    menu.addItem ("Reset to default", [safe] { if (safe != nullptr) safe->resetToDefault(); });
    menu.addItem ("Randomize",        [safe] { if (safe != nullptr) safe->randomize(); });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void ParamControl::resetToDefault()
{
    // getParameterAsValue returns a Value bound to the APVTS parameter; assigning
    // the denormalized value drives the attachment (control moves) AND the
    // processor's APVTS::Listener (engine byte-bridge) — the same path patch
    // loading uses. defaultValue is the denormalized value (Int value or Choice
    // index), matching how the APVTS stores each type. beginNewTransaction()
    // first so this reset is its own discrete undo step (Phase 4c).
    processor_.getUndoManager().beginNewTransaction();
    processor_.getApvts().getParameterAsValue (desc_.paramID) =
        static_cast<float> (desc_.defaultValue);
}

void ParamControl::randomize()
{
    float value = 0.0f;
    if (comboBox_ != nullptr && desc_.choices != nullptr)
    {
        const int n = desc_.choices->size();
        if (n <= 0)
            return;
        // Random choice index.
        value = static_cast<float> (juce::Random::getSystemRandom().nextInt (n));
    }
    else
    {
        const int lo = desc_.minValue;
        const int hi = desc_.maxValue;
        if (hi < lo)
            return;
        // Random value in the inclusive [minValue, maxValue] range.
        value = static_cast<float> (lo + juce::Random::getSystemRandom().nextInt (hi - lo + 1));
    }

    // beginNewTransaction() so each Randomize is a discrete undo step (Phase 4c).
    processor_.getUndoManager().beginNewTransaction();
    processor_.getApvts().getParameterAsValue (desc_.paramID) = value;
}

//==============================================================================
juce::String ParamPage::groupForId (const juce::String& id)
{
    // ---- Mixer splits into three sub-groups (exact ids) ----
    if (id == "mix_balance" || id == "mix_op" || id == "mix_param")
        return "Mixer";
    if (id == "mix_sub_shape" || id == "mix_sub")
        return "Sub Oscillator";
    if (id == "mix_noise" || id == "mix_fuzz" || id == "mix_crush")
        return "Noise / Waveshaper";

    // ---- The two filter modulation amounts share one panel ----
    if (id == "filter_env" || id == "filter_lfo")
        return "Filter Mod";

    // ---- Sequencer length slots (exact ids) belong to their step group ----
    if (id == "seq_length_1") return "Sequencer 1";
    if (id == "seq_length_2") return "Sequencer 2";
    if (id == "seq_length_3") return "Note Sequencer";

    // ---- Synth options with no patch byte ----
    if (id == "vca_curve" || id == "filter_card" || id == "filter_drive")
        return "Global";

    // ---- Sequencer step grids (prefixes) ----
    if (id.startsWith ("seq1_step")) return "Sequencer 1";
    if (id.startsWith ("seq2_step")) return "Sequencer 2";
    if (id.startsWith ("seqnote_"))  return "Note Sequencer";

    // ---- Oscillators / filters ----
    if (id.startsWith ("osc1_"))    return "Osc 1";
    if (id.startsWith ("osc2_"))    return "Osc 2";
    if (id.startsWith ("filter1_")) return "Filter 1";
    if (id.startsWith ("filter2_")) return "Filter 2";

    // ---- Envelopes / LFOs (now on separate tabs) ----
    // env{N}_attack/decay/sustain/release -> "Env N (role)"; env{N}_lfo_* -> "LFO N".
    // Role verified from the dsp routing: ENV3 -> VCA (mod-matrix default,
    // amount 63) = Amp; ENV2 -> filter cutoff (hardcoded filter_env) = Filter;
    // ENV1 -> free mod parameters = Mod.
    if (id.startsWith ("env") && id.length() > 3 && id[3] >= '1' && id[3] <= '3')
    {
        const juce::String n = juce::String::charToString (id[3]);
        if (id.contains ("_lfo_"))
            return "LFO " + n;
        if (n == "1") return "Env 1 (Mod)";
        if (n == "2") return "Env 2 (Filter)";
        return "Env 3 (Amp)";
    }
    if (id.startsWith ("voice_lfo_")) return "Voice LFO";

    // ---- Modifiers (modifN_*) — checked before the "mod" rule ----
    if (id.startsWith ("modif") && id.length() > 5 && id[5] >= '0' && id[5] <= '9')
    {
        juce::String num;
        for (int i = 5; i < id.length() && id[i] >= '0' && id[i] <= '9'; ++i) num += id[i];
        return "Modifier " + num;
    }

    // ---- Mod matrix (modN_*) ----
    if (id.startsWith ("mod") && id.length() > 3 && id[3] >= '0' && id[3] <= '9')
    {
        juce::String num;
        for (int i = 3; i < id.length() && id[i] >= '0' && id[i] <= '9'; ++i) num += id[i];
        return "Mod " + num;
    }

    // ---- Part / Play + Arp ----
    if (id.startsWith ("part_")) return "Part / Play";
    if (id.startsWith ("arp_"))  return "Arp";

    return "Other";
}

void ParamPage::buildGroups (const std::vector<const PatchParamDescriptor*>& descriptors)
{
    // Partition descriptors into named groups, preserving first-appearance
    // order of the groups and the descriptor order within each group.
    for (int i = 0; i < (int) descriptors.size(); ++i)
    {
        const juce::String gname = groupForId (descriptors[i]->paramID);
        GroupLayout* g = nullptr;
        for (auto& existing : groups_)
            if (existing.name == gname) { g = &existing; break; }
        if (g == nullptr)
        {
            groups_.emplace_back();
            g = &groups_.back();
            g->name = gname;
        }
        g->controlIndices.push_back (i);
    }

    // For sequencer step-grid groups, the Length control should be the LAST cell
    // (user edits steps first, then sets the length). The descriptor order has
    // seq_length_* before the step params, so reorder via stable_partition.
    for (auto& g : groups_)
    {
        if (! (g.name == "Sequencer 1" || g.name == "Sequencer 2" || g.name == "Note Sequencer"))
            continue;
        std::stable_partition (g.controlIndices.begin(), g.controlIndices.end(),
            [&descriptors] (int idx)
            { return descriptors[(size_t) idx]->paramID.find ("seq_length") == std::string::npos; });
    }
}

void ParamPage::configureGroupLayouts()
{
    // Internal column count for a generic panel, chosen so the cells stay
    // roughly square-ish (1->1, 2->2, 3->3, 4->2x2, 5/6->3, 7/8->4).
    auto generalCols = [] (int n) -> int {
        if (n <= 1) return 1;
        if (n == 2) return 2;
        if (n == 3) return 3;
        if (n == 4) return 2;
        if (n <= 6) return 3;
        return 4;
    };

    for (auto& g : groups_)
    {
        const int n = (int) g.controlIndices.size();
        g.cellW = cellWidth_;
        g.cellH = cellHeight_;

        if (g.name == "Sequencer 1" || g.name == "Sequencer 2" || g.name == "Note Sequencer")
        {
            // Dense step grid: 16 (or 33) small cells laid in 8 columns.
            g.stepGrid = true;
            g.internalCols = 8;
            g.cellW = 88;      // was 54 (~1.6x wider)
            g.cellH = 72;      // was 50 (~1.4x taller)
        }
        else if (g.name.startsWith ("Mod ") || g.name.startsWith ("Modifier "))
        {
            // Compact horizontal strip: source / dest / amount (or in1 / in2 / op).
            g.singleRow = true;
            g.internalCols = juce::jmax (1, n);
            g.cellW = 112;
            g.cellH = 64;
        }
        else if (g.name.startsWith ("Env ") || g.name.startsWith ("LFO ") || g.name == "Voice LFO")
        {
            // Env / LFO groups: one row of knobs (a wide panel). Keeps each
            // group to a single panel per page row so the row-fill pass never
            // grows two small panels into an overlap.
            g.internalCols = juce::jmax (1, n);
        }
        else
        {
            g.internalCols = generalCols (n);
        }
    }
}

void ParamPage::layoutGroups (int targetWidth)
{
    const int topY = kMargin + kHeadingH + kHeadingGap;
    const int availW = targetWidth - 2 * kMargin;

    // Natural panel size for each group (independent of placement).
    for (auto& g : groups_)
    {
        const int cols = juce::jmax (1, g.internalCols);
        {
            const int n = (int) g.controlIndices.size();
            const int rows = (n + cols - 1) / cols;
            g.naturalWidth  = cols * g.cellW + 2 * kGroupPad;
            g.naturalHeight = kGroupTitleH + rows * g.cellH + 2 * kGroupPad;
        }
        // A group with a decoration (e.g. an ADSR/LFO preview) reserves room
        // below its control cells so the panel height includes it.
        if (g.decoration != nullptr)
            g.naturalHeight += kDecorationH + kDecorationGap;
    }

    // Greedy left-to-right flow. A row wraps when the next panel would overflow
    // the available width OR when the row already holds pageCols_ panels
    // (PageInfo::cols: a tunable cap on panels-per-row; pageCols_ <= 0 =>
    // width-only wrap). rowOf[gi] tags each group with its row for the fill pass.
    int x = kMargin, y = topY, rowH = 0;
    const int rowStartX = kMargin;
    const int maxRight = kMargin + juce::jmax (0, availW);

    std::vector<int> rowOf (groups_.size(), 0);
    int currentRow = 0;

    for (int gi = 0; gi < (int) groups_.size(); ++gi)
    {
        auto& g = groups_[(size_t) gi];

        // Panels already placed on THIS row (for the pageCols_ cap). The row is
        // contiguous in gi, so walk back while the row tag matches.
        int panelsThisRow = 0;
        for (int k = gi - 1; k >= 0 && rowOf[(size_t) k] == currentRow; --k)
            ++panelsThisRow;

        if ((x != rowStartX && (x + g.naturalWidth > maxRight))
            || (pageCols_ > 0 && panelsThisRow >= pageCols_))
        {
            ++currentRow;
            x = rowStartX;
            y += rowH + kGroupGap;
            rowH = 0;
        }
        rowOf[(size_t) gi] = currentRow;
        g.rect.setBounds (x, y, g.naturalWidth, g.naturalHeight);
        x += g.naturalWidth + kGroupGap;
        rowH = juce::jmax (rowH, g.naturalHeight);
    }
    const int lastRow = groups_.empty() ? -1 : currentRow;

    // ---- Row-fill justification (flexible-width grid). For each row, grow the
    // NON-dense panels (stepGrid / singleRow are excluded) so the row fills up
    // to maxRight, eliminating the ragged right edge. Only the panel WIDTH grows;
    // height and decoration sizing are untouched, so contentHeight_ (computed
    // below from the row geometry above) is unchanged. All-dense rows are left
    // as-is (ragged is fine for sequencer / modifier strips). ----
    for (int r = 0; r <= lastRow; ++r)
    {
        // Gather this row's panel indices in left-to-right (gi) order.
        std::vector<int> rowPanels;
        int nonDense = 0;
        int rowRight = rowStartX;
        for (int gi = 0; gi < (int) groups_.size(); ++gi)
            if (rowOf[(size_t) gi] == r)
            {
                rowPanels.push_back (gi);
                rowRight = juce::jmax (rowRight, groups_[(size_t) gi].rect.getRight());
                if (! (groups_[(size_t) gi].stepGrid || groups_[(size_t) gi].singleRow))
                    ++nonDense;
            }
        if (rowPanels.empty() || nonDense == 0)
            continue;   // all-dense row: leave ragged (no grow, no re-tile)

        const int slack = maxRight - rowRight;
        if (slack > 0)
        {
            const int grow = slack / nonDense;
            if (grow > 0)
                for (int gi : rowPanels)
                {
                    auto& g = groups_[(size_t) gi];
                    if (! (g.stepGrid || g.singleRow))
                        g.rect.setWidth (g.rect.getWidth() + grow);
                }
        }

        // RE-TILE the row left-to-right from the (now-grown) widths so panels
        // never overlap — the in-place width grow above left each panel's X at
        // its natural position, which overlaps its neighbour when 2+ non-dense
        // panels share a row (e.g. Osc 1/2, Mixer + Sub Oscillator). Y is kept.
        int x = rowStartX;
        for (int gi : rowPanels)
        {
            auto& g = groups_[(size_t) gi];
            g.rect.setX (x);
            x = g.rect.getRight() + kGroupGap;
        }
    }

    contentWidth_  = juce::jmax (targetWidth, 2 * kMargin + 40);
    contentHeight_ = groups_.empty() ? (topY + kMargin)
                                     : (y + rowH + kMargin);
}

void ParamPage::applyLayout()
{
    heading_.setBounds (kMargin, kMargin,
                        juce::jmax (40, contentWidth_ - 2 * kMargin), kHeadingH);

    for (auto& g : groups_)
    {
        if (g.groupComp != nullptr)
            g.groupComp->setBounds (g.rect);

        auto inner = g.rect.reduced (kGroupPad);
        inner.removeFromTop (kGroupTitleH);   // room for the panel title text

        const int cols = juce::jmax (1, g.internalCols);
        // Flexible-width cells: a NON-dense panel distributes its cells evenly
        // across the actual (possibly row-filled) inner width; a DENSE panel
        // (sequencer step grid / mod-modifier strip) keeps its fixed cell size
        // and is left-aligned. Column width never shrinks below the natural
        // cellW, only grows to fill. Row height is always g.cellH.
        const bool dense = g.stepGrid || g.singleRow;
        const int colStep = dense ? g.cellW
                                  : juce::jmax (g.cellW, inner.getWidth() / cols);

        for (int idx = 0; idx < (int) g.controlIndices.size(); ++idx)
        {
            const int ci = g.controlIndices[idx];
            if (ci < 0 || ci >= (int) controls_.size()) continue;
            const int col = idx % cols;
            const int row = idx / cols;
            const juce::Rectangle<int> cell (inner.getX() + col * colStep,
                                             inner.getY() + row * g.cellH,
                                             colStep, g.cellH);
            controls_[ci]->setBounds (cell.reduced (3));
        }

        // A group's decoration (if any) spans the panel width below the cells.
        const int rows = ((int) g.controlIndices.size() + cols - 1) / cols;
        if (g.decoration != nullptr)
        {
            const int decY = inner.getY() + rows * g.cellH + kDecorationGap;
            g.decoration->setBounds (
                juce::Rectangle<int> (inner.getX(), decY, inner.getWidth(), kDecorationH));
        }
    }
}

bool ParamPage::layoutIsSane() const
{
    // (a) every group panel has positive size.
    for (const auto& g : groups_)
        if (g.rect.getWidth() <= 0 || g.rect.getHeight() <= 0)
            return false;

    // (b) no two group panels overlap (siblings in page coordinate space).
    for (size_t i = 0; i < groups_.size(); ++i)
        for (size_t j = i + 1; j < groups_.size(); ++j)
            if (groups_[i].rect.intersects (groups_[j].rect))
                return false;

    // (c) every control has positive size and sits inside its group's rect.
    // (ParamControl is a direct child of ParamPage, so getBoundsInParent() is in
    // the same page-space coordinates as the group rects.) For Env/LFO slots
    // only the ACTIVE mode's controls are laid out, so validate just those.
    for (const auto& g : groups_)
    {
        for (int ci : g.controlIndices)
        {
            if (ci < 0 || ci >= (int) controls_.size())
                return false;
            const auto b = controls_[(size_t) ci]->getBoundsInParent();
            if (b.getWidth() <= 0 || b.getHeight() <= 0)
                return false;
            if (! g.rect.contains (b))
                return false;
        }
    }

    // (d) the page fills its width: at least one NON-dense row reaches the right
    // margin (within a 2*kGroupGap tolerance for integer rounding), proving the
    // grid fills the row rather than leaving a ragged edge. Dense-only pages
    // are exempt.
    const int maxRight = juce::jmax (0, contentWidth_ - kMargin);
    bool anyNonDense = false, fillsWidth = false;
    for (const auto& g : groups_)
        if (! (g.stepGrid || g.singleRow))
        {
            anyNonDense = true;
            if (g.rect.getRight() >= maxRight - 2 * kGroupGap)
                fillsWidth = true;
        }
    if (anyNonDense && ! fillsWidth)
        return false;

    return true;
}

void ParamPage::setGroupDecoration (const juce::String& groupName,
                                    std::unique_ptr<juce::Component> decoration)
{
    // ParamPage always owns the component (so it never leaks / dangles), even
    // if @p groupName does not match an existing group.
    auto* raw = decoration.get();
    decorations_.push_back (std::move (decoration));
    addAndMakeVisible (raw);
    for (auto& g : groups_)
        if (g.name == groupName)
            g.decoration = raw;

    // Recompute the layout so contentHeight_ already accounts for the new
    // decoration when the editor sizes this page immediately afterwards.
    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

ParamPage::ParamPage (ParvatiAudioProcessor& processor,
                      ThemeManager& themeManager,
                      const juce::String& heading,
                      const std::vector<const PatchParamDescriptor*>& descriptors,
                      int columns, int cellWidth, int cellHeight)
    : themeManager_ (themeManager),
      cellWidth_ (cellWidth), cellHeight_ (cellHeight)
{
    // Honour the page's declared column count (PageInfo::cols) as a cap on the
    // number of group panels per row (whichever wraps first: width overflow or
    // the cap). 0 => width-only wrap (Multi page is not a ParamPage).
    pageCols_ = juce::jmax (0, columns);

    heading_.setText (heading, juce::dontSendNotification);
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    // Bright section heading: explicit accent (overrides the L&F's default dim
    // label text) to preserve the original look.
    heading_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accent);
    addAndMakeVisible (heading_);

    buildGroups (descriptors);
    configureGroupLayouts();

    // Bordered panels first (so they sit behind the control cells), one per group.
    for (auto& g : groups_)
    {
        auto gc = std::make_unique<juce::GroupComponent> (g.name, g.name);
        gc->setTextLabelPosition (juce::Justification::top | juce::Justification::left);
        // Outline + title-text colours come from the editor-wide L&F, so a theme
        // switch refreshes them automatically.
        addAndMakeVisible (*gc);
        g.groupComp = gc.get();
        groupComponents_.push_back (std::move (gc));
    }

    // Control cells on top of the panel borders.
    for (auto* d : descriptors)
    {
        controls_.emplace_back (std::make_unique<ParamControl> (processor, *d));
        addAndMakeVisible (*controls_.back());
    }

    // Seed the content size at a sensible default width; the editor reflows to
    // the real tab width on the first resized().
    layoutGroups (940);
}

void ParamPage::applyThemeColors()
{
    heading_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accent);
    // Group borders / titles are themed via the L&F; force a repaint so a theme
    // switch refreshes them (and the control cells) immediately.
    for (auto& gc : groupComponents_) gc->repaint();
    for (auto& c : controls_)         c->repaint();
    for (auto& d : decorations_)      d->repaint();   // e.g. ADSR previews read the theme live
    repaint();
}

void ParamPage::setHeadingText (const juce::String& text)
{
    heading_.setText (text, juce::dontSendNotification);
}

void ParamPage::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().windowBackground);
}

void ParamPage::resized()
{
    layoutGroups (getWidth());
    applyLayout();
}

void ParamPage::reflowToWidth (int targetWidth)
{
    if (targetWidth <= 0)
        return;
    // Lay out for the requested width, then adopt the resulting height so the
    // parent Viewport scrolls vertically only. setSize() re-triggers resized()
    // which re-lays-out to the same width (cheap rectangle math).
    layoutGroups (targetWidth);
    applyLayout();
    if (getWidth() != targetWidth || getHeight() != contentHeight_)
        setSize (targetWidth, contentHeight_);
}

//==============================================================================
MultiPage::MultiPage (ParvatiAudioProcessor& p, ThemeManager& themeManager)
    : proc_ (p), themeManager_ (themeManager)
{
    heading_.setText (TRANS ("Multi / Setup"), juce::dontSendNotification);
    heading_.setJustificationType (juce::Justification::centredLeft);
    heading_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    heading_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accent);
    addAndMakeVisible (heading_);

    partLabel_.setFont (juce::FontOptions (14.0f));
    // partLabel_ text colour from the L&F (dim).
    addAndMakeVisible (partLabel_);

    auto addCaption = [this] (juce::Label& l, const juce::String& t) {
        l.setText (t, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::FontOptions (13.0f));
        // Caption text colour from the L&F (dim).
        addAndMakeVisible (l);
    };
    addCaption (chLabel_, TRANS ("MIDI Channel"));
    addCaption (loLabel_, TRANS ("Key Zone Low"));
    addCaption (hiLabel_, TRANS ("Key Zone High"));

    // MIDI channel: Omni (0) + 1..16.
    channelCombo_.addItem ("Omni", 1);
    for (int c = 1; c <= 16; ++c)
        channelCombo_.addItem (juce::String (c), c + 1);
    // Combo + popup colours from the L&F.
    channelCombo_.onChange = [this] {
        if (refreshing_) return;
        const int part = proc_.getEngine().getCurrentPart();
        proc_.getEngine().setPartMidiChannel (part, channelCombo_.getSelectedId() - 1);
    };
    addAndMakeVisible (channelCombo_);

    auto setupZoneSlider = [this] (juce::Slider& s) {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 18);
        s.setRange (0.0, 127.0, 1.0);
        // Slider colours from the L&F.
        addAndMakeVisible (s);
    };
    setupZoneSlider (loSlider_);
    setupZoneSlider (hiSlider_);
    auto onZone = [this] {
        if (refreshing_) return;
        const int part = proc_.getEngine().getCurrentPart();
        proc_.getEngine().setPartKeyZone (part,
                                          static_cast<int> (loSlider_.getValue()),
                                          static_cast<int> (hiSlider_.getValue()));
    };
    loSlider_.onValueChange = onZone;
    hiSlider_.onValueChange = onZone;

    // Voice allocation: 6 toggles, one per firmware voicecard (vc1..vc6).
    addCaption (allocLabel_, TRANS ("Voice Allocation (voicecards)"));
    auto reapplyAlloc = [this] {
        if (refreshing_) return;
        const int part = proc_.getEngine().getCurrentPart();
        uint8_t mask = 0;
        for (int b = 0; b < 6; ++b)
            if (allocBits_[b].getToggleState())
                mask |= static_cast<uint8_t> (1 << b);
        proc_.getEngine().setPartVoiceAllocation (part, mask);
    };
    for (int b = 0; b < 6; ++b)
    {
        allocBits_[b].setButtonText ("VC" + juce::String (b + 1));
        // Toggle text + tick colours from the L&F.
        allocBits_[b].onClick = reapplyAlloc;
        addAndMakeVisible (allocBits_[b]);
    }

    setSize (640, 320);
    refresh();
}

void MultiPage::applyThemeColors()
{
    heading_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().accent);
    repaint();
}

void MultiPage::refreshLanguage()
{
    // Re-apply the static chrome captions through the active LocalisedStrings.
    // The dynamic "Editing Part X of Y" line is rebuilt by refresh().
    heading_.setText (TRANS ("Multi / Setup"), juce::dontSendNotification);
    chLabel_.setText (TRANS ("MIDI Channel"), juce::dontSendNotification);
    loLabel_.setText (TRANS ("Key Zone Low"), juce::dontSendNotification);
    hiLabel_.setText (TRANS ("Key Zone High"), juce::dontSendNotification);
    allocLabel_.setText (TRANS ("Voice Allocation (voicecards)"), juce::dontSendNotification);
    repaint();
}

void MultiPage::paint (juce::Graphics& g) { g.fillAll (themeManager_.getCurrentTheme().windowBackground); }

void MultiPage::resized()
{
    auto area = getLocalBounds().reduced (16);
    heading_.setBounds (area.removeFromTop (30));
    area.removeFromTop (8);
    partLabel_.setBounds (area.removeFromTop (24));
    area.removeFromTop (20);

    auto row = area.removeFromTop (130);
    const int colW = juce::jmax (140, row.getWidth() / 3);
    auto cell = row.removeFromLeft (colW);
    chLabel_.setBounds (cell.removeFromTop (18));
    channelCombo_.setBounds (cell.removeFromTop (30).withSizeKeepingCentre (cell.getWidth(), 24));
    cell = row.removeFromLeft (colW);
    loLabel_.setBounds (cell.removeFromTop (18));
    loSlider_.setBounds (cell);
    cell = row.removeFromLeft (colW);
    hiLabel_.setBounds (cell.removeFromTop (18));
    hiSlider_.setBounds (cell);

    area.removeFromTop (12);
    auto allocRow = area.removeFromTop (56);
    allocLabel_.setBounds (allocRow.removeFromTop (18));
    auto bits = allocRow.removeFromTop (34);
    const int bw = juce::jmax (52, bits.getWidth() / 6);
    for (int b = 0; b < 6; ++b)
        allocBits_[b].setBounds (bits.removeFromLeft (bw).reduced (3));
}

void MultiPage::refresh()
{
    refreshing_ = true;
    const int part = proc_.getEngine().getCurrentPart();
    const auto& prt = proc_.getEngine().getPart (part);
    partLabel_.setText ("Editing Part " + juce::String (part + 1) + " of "
                            + juce::String (SynthEngine::getNumParts()),
                        juce::dontSendNotification);
    channelCombo_.setSelectedId (static_cast<int> (prt.midiChannel.load()) + 1);
    loSlider_.setValue (static_cast<double> (prt.keyrangeLow.load()),  juce::dontSendNotification);
    hiSlider_.setValue (static_cast<double> (prt.keyrangeHigh.load()), juce::dontSendNotification);
    const uint8_t alloc = prt.voiceAllocation;
    for (int b = 0; b < 6; ++b)
        allocBits_[b].setToggleState ((alloc & (1u << b)) != 0, juce::dontSendNotification);
    refreshing_ = false;
    lastPart_ = part;
}

void MultiPage::refreshIfPartChanged()
{
    if (proc_.getEngine().getCurrentPart() != lastPart_)
        refresh();
}

//==============================================================================
ParvatiEditor::ParvatiEditor (ParvatiAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef_ (p)
{
    // Install the persisted chrome language BEFORE building the UI, so every
    // TRANS() below resolves to the right language at construction. English (and
    // "auto" on an English locale) clears the mappings => TRANS() is the
    // identity => the UI is byte-identical to the un-localised build.
    installLanguage (processorRef_.getUiLanguage());

    // Theme + LookAndFeel: one L&F on the editor, inherited by the whole control
    // tree, so no per-component palette is needed.
    lnf_.setTheme (themeManager_.getCurrentTheme());
    setLookAndFeel (&lnf_);
    themeManager_.addChangeListener (this);

    // Tooltips: one TooltipWindow parented to (and deleted with) the editor.
    // ParamControl is a TooltipClient returning its parameter's help text.
    tooltipWindow_ = std::make_unique<juce::TooltipWindow> (this);

    // Phase 4a: apply persisted UI preferences. The theme selection may differ
    // from the ThemeManager default (Carbon); selectByName broadcasts a change
    // (caught by changeListenerCallback) if the selection actually moves.
    themeManager_.selectByName (processorRef_.getUiTheme());
    lnf_.setTheme (themeManager_.getCurrentTheme());
    ParamControl::setTooltipsEnabled (processorRef_.getUiTooltips());

    // Apply the persisted parameter-smoothing preference to the engine (the
    // SettingsPanel toggle is seeded from getUiSmoothing() when it is built
    // below; this covers the audio side for hosts that show the editor).
    processorRef_.setParameterSmoothing (processorRef_.getUiSmoothing());

    // Group every descriptor into its section bucket; Part params (volume,
    // legato, portamento) and synth options (VCA curve) ride on the Oscillators
    // page as the "global" footer. `part_select` is intentionally skipped here:
    // it has a dedicated top-bar ComboBox (partCombo_) bound to the same APVTS
    // param, so generating a second control for it on a page would be redundant.
    std::vector<const PatchParamDescriptor*> sec[10];
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.paramID == "part_select")
            continue;
        sec[(int) sectionForId (d.paramID)].push_back (&d);
    }

    addAndMakeVisible (tabs_);

    const ParvatiTheme& theme = themeManager_.getCurrentTheme();

    // ---- Top patch bar: factory patch list + Load .PRO... + name ----
    patchCaption_.setText (TRANS ("Patch:"), juce::dontSendNotification);
    // Caption text colour from the L&F (dim).
    patchCaption_.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (patchCaption_);

    // Cascading patch menu (Templates / User / Factory banks / Multi). The
    // browser scans the dirs live on each open, so there is no pre-populate.
    presetBrowser_ = std::make_unique<PresetBrowser> (
        processorRef_.getTemplatesDir(), processorRef_.getUserPatchDir(),
        processorRef_.getFactoryPatchDir(), processorRef_.getFactoryMultiDir(),
        [this] (const juce::File& f) { applyPatchFile (f); });
    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
    addAndMakeVisible (*presetBrowser_);

    loadButton_.setButtonText (TRANS ("Load..."));
    // Button colours from the L&F.
    loadButton_.onClick = [this] { openLoadDialog(); };
    addAndMakeVisible (loadButton_);

    // Save: a single button whose popup menu picks the format. "Ambika Patch
    // (.PRO)" writes the byte-faithful hardware-shareable patch; "Parvati Patch
    // (.parvati)" writes the full-fidelity YAML (carries vca_curve / filter_card
    // / arp that the .PRO byte format drops).
    saveButton_.setButtonText (TRANS ("Save..."));
    saveButton_.onClick = [this] {
        juce::PopupMenu m;
        m.addItem (1, TRANS ("Ambika Patch (.PRO)"));
        m.addItem (2, TRANS ("Parvati Patch (.parvati)"));
        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int result) {
            if (result == 1)      openSaveDialog();
            else if (result == 2) openSaveParvatiDialog();
        });
    };
    addAndMakeVisible (saveButton_);

    // Phase 4c: Undo / Redo are Path-drawn IconButtons (curved arrows) — no
    // unicode glyph (the font stack renders U+21B6/21B7 as "..."). The APVTS
    // UndoManager records every parameter change; enable/disable is mirrored on
    // the editor timer.
    undoButton_.setTooltip (TRANS ("Undo"));
    undoButton_.onClick = [this] { processorRef_.getUndoManager().undo(); };
    addAndMakeVisible (undoButton_);
    redoButton_.setTooltip (TRANS ("Redo"));
    redoButton_.onClick = [this] { processorRef_.getUndoManager().redo(); };
    addAndMakeVisible (redoButton_);

    // ---- Top bar: Part selector (bound to the `part_select` APVTS param) ----
    partCaption_.setText (TRANS ("Part:"), juce::dontSendNotification);
    // Caption text colour from the L&F (dim).
    partCaption_.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (partCaption_);

    for (int i = 1; i <= SynthEngine::getNumParts(); ++i)
        partCombo_.addItem ("Part " + juce::String (i), i);
    // Combo colours from the L&F.
    addAndMakeVisible (partCombo_);
    partComboAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef_.getApvts(), "part_select", partCombo_);

    tabs_.setTabBarDepth (32);
    tabs_.setOutline (0);
    // TabbedComponent / TabbedButtonBar colours from the L&F.

    struct PageInfo { const char* name; Section s; int cols, cellW, cellH; };
    const PageInfo pages[] = {
        { "Oscillators",     Section::Oscillators, 4, 214, 106 },
        { "Mixer",           Section::Mixer,       4, 214, 106 },
        { "Filter",          Section::Filter,      4, 214, 106 },
        { "Envelopes",       Section::Envelopes,   3, 198, 106 },
        { "LFOs",            Section::Lfos,        4, 198, 106 },
        { "Mod Matrix",      Section::ModMatrix,   6, 164, 84 },
        { "Modifiers",       Section::Modifiers,   3, 300, 64 },
        { "Arp",             Section::Arp,         3, 214, 106 },
        { "Sequencer",       Section::Sequencer,   6, 150, 80 },
        { "Global",          Section::Global,      3, 214, 106 },
    };

    for (const auto& pg : pages)
    {
        auto* page = new ParamPage (processorRef_, themeManager_, TRANS (pg.name), sec[(int) pg.s],
                                    pg.cols, pg.cellW, pg.cellH);

        // Live previews: an ADSR curve under each Env group (Envelopes tab) and
        // an LFO waveform under each LFO group (LFOs tab). The getters read the
        // APVTS parameter's NORMALIZED value (getValue() returns 0..1) so the
        // preview tracks the knobs live. (Each env_lfo unit runs BOTH its
        // envelope and its LFO; splitting the halves onto two tabs matches that.)
        auto norm = [this] (const juce::String& id) -> float {
            auto* p = processorRef_.getApvts().getParameter (id);
            return p ? p->getValue() : 0.0f;
        };
        if (pg.s == Section::Envelopes)
        {
            const juce::String envs[3] = { "env1", "env2", "env3" };
            const juce::String envLabels[3] = { "Env 1 (Mod)", "Env 2 (Filter)", "Env 3 (Amp)" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = envs[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    envLabels[i],
                    [norm, e] { return norm (e + "_attack");  },
                    [norm, e] { return norm (e + "_decay");   },
                    [norm, e] { return norm (e + "_sustain"); },
                    [norm, e] { return norm (e + "_release"); });
                disp->setPreviewMode (0);   // ADSR curve
                page->setGroupDecoration (envLabels[i], std::move (disp));
            }
        }
        else if (pg.s == Section::Lfos)
        {
            // LFO 1/2/3: the LFO half of env_lfo[0..2] (shape drives the preview).
            const juce::String lfos[3] = { "env1", "env2", "env3" };
            for (int i = 0; i < 3; ++i)
            {
                const juce::String e = lfos[i];
                auto disp = std::make_unique<EnvelopeDisplay> (
                    "LFO " + juce::String (i + 1),
                    std::function<float()> {}, std::function<float()> {},
                    std::function<float()> {}, std::function<float()> {},
                    [norm, e] { return norm (e + "_lfo_shape"); });
                disp->setPreviewMode (1);   // LFO waveform
                page->setGroupDecoration ("LFO " + juce::String (i + 1), std::move (disp));
            }
            // Voice LFO (MOD_SRC_LFO_4).
            auto vdisp = std::make_unique<EnvelopeDisplay> (
                "Voice LFO",
                std::function<float()> {}, std::function<float()> {},
                std::function<float()> {}, std::function<float()> {},
                [norm] { return norm ("voice_lfo_shape"); });
            vdisp->setPreviewMode (1);
            page->setGroupDecoration ("Voice LFO", std::move (vdisp));
        }

        if (pg.s == Section::Global)
            globalPage_ = page;   // voice-activity cells attach here as a decoration
        generatedPages_.push_back (page);
        // Record the English (key) tab name for live language switching.
        tabKeys_.push_back (pg.name);
        auto* vp = new juce::Viewport();
        // Pages fill the tab width, so only vertical scrolling is ever needed.
        vp->setScrollBarsShown (true, false);
        vp->setViewedComponent (page, true);  // viewport owns the page
        pageViewports_.push_back (vp);
        page->setSize (page->getContentWidth(), page->getContentHeight());
        tabs_.addTab (TRANS (pg.name), theme.windowBackground, vp, true);  // tabs own the viewport
    }

    // ---- Multi / Setup tab (custom page, not descriptor-generated) ----
    multiPage_ = std::make_unique<MultiPage> (processorRef_, themeManager_);
    tabs_.addTab (TRANS ("Multi"), theme.windowBackground, multiPage_.get(), false);  // editor owns it
    tabKeys_.push_back ("Multi");

    // ---- Phase 4a: settings button + side panel ----
    // Click-toggle feedback reflects whether the Settings panel is open (the
    // "on" colour is the theme accent, via TextButton::buttonOnColourId). The
    // authoritative sync is the panel's onPanelShowHide callback below, which
    // fires on any show/hide (button click, dismiss glyph, click-outside).
    settingsButton_.setClickingTogglesState (true);
    settingsButton_.setTooltip (TRANS ("Settings"));
    settingsButton_.onClick = [this] {
        settingsPanelHost_->showOrHide (! settingsPanelHost_->isPanelShowing());
        settingsButton_.setToggleState (settingsPanelHost_->isPanelShowing(),
                                        juce::dontSendNotification);
    };
    addAndMakeVisible (settingsButton_);

    // ---- Phase 4a: virtual keyboard (bottom strip) ----
    // Click-to-play routes MIDI into the processor's MidiMessageCollector
    // (thread-safe); the timer mirrors sounding notes back as latch highlights.
    keyboardView_ = std::make_unique<KeyboardView>();
    keyboardView_->setNoteCallback ([this] (int note, bool on, float vel) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int status   = on ? (0x90 | ((ch - 1) & 0xf)) : (0x80 | ((ch - 1) & 0xf));
        const int velocity = on ? juce::jlimit (0, 127, juce::roundToInt (vel * 127.0f)) : 0;
        processorRef_.addMidiEvent (juce::MidiMessage (status, note, velocity));
    });
    addAndMakeVisible (*keyboardView_);
    keyboardView_->refresh();

    // ---- Pitch + Mod wheels (left of the keyboard) ----
    wheels_ = std::make_unique<WheelsComponent>();
    wheels_->onPitch = [this] (float v) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int pv = juce::jlimit (0, 16383, juce::roundToInt ((v * 0.5f + 0.5f) * 16383.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::pitchWheel (ch, pv));
    };
    wheels_->onMod = [this] (float v) {
        int ch = processorRef_.getEngine().getPartChannel (processorRef_.getEngine().getCurrentPart());
        if (ch == 0) ch = 1;   // Omni -> inject on channel 1
        const int mv = juce::jlimit (0, 127, juce::roundToInt (v * 127.0f));
        processorRef_.addMidiEvent (juce::MidiMessage::controllerEvent (ch, 1, mv));   // CC1 = mod wheel
    };
    addAndMakeVisible (*wheels_);
    // Computer-keyboard (musical-typing) play is a STANDALONE-only affordance.
    // In a plugin host the DAW owns the computer keyboard (e.g. Ableton's
    // "Computer MIDI Keyboard") and routes it as normal MIDI, so capturing keys
    // here would double-trigger and steal keystrokes from the host.
    keyboardView_->setComputerKeyboardEnabled (
        processorRef_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);

    // ---- Voice activity cells live on the Global page; the bottom strip shows
    // only the active-count + a hover-tooltip bar (cells + "Voices" word were
    // removed per request). Build the cells meter, wire it, and attach it to the
    // Global page's "Global" group as a decoration (owned by the page). ----
    {
        auto vm = std::make_unique<VoiceMeter>();
        vm->setViewMode (processorRef_.getUiVoiceMode() == 1
                         ? VoiceMeter::ViewMode::Extended
                         : VoiceMeter::ViewMode::Voicecard);
        vm->setStateProvider ([this]() {
            std::vector<VoiceActivity> v;
            auto& e = processorRef_.getEngine();
            v.reserve (static_cast<size_t> (e.getNumVoices()));
            for (int i = 0; i < e.getNumVoices(); ++i)
            {
                // SF-1: read the lock-free atomic snapshot instead of the
                // non-atomic SynthesiserVoice::currentlyPlayingNote.
                auto* av = e.getAmbikaVoice (i);
                v.push_back ({ av != nullptr && av->isDisplayedActive(),
                               av != nullptr ? av->getDisplayedNote() : -1 });
            }
            return v;
        });
        globalVoiceMeter_ = vm.get();
        if (globalPage_ != nullptr)
            globalPage_->setGroupDecoration ("Global", std::move (vm));
    }

    // ---- Bottom status strip: compact active-voice count + tooltip bar ----
    statusCountLabel_.setJustificationType (juce::Justification::centred);
    statusCountLabel_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    statusCountLabel_.setColour (juce::Label::textColourId, theme.accent);
    statusCountLabel_.setText ("0/" + juce::String (
        processorRef_.getEngine()
            .getPart (processorRef_.getEngine().getCurrentPart()).voiceCount_.load()),
                               juce::dontSendNotification);
    addAndMakeVisible (statusCountLabel_);
    statusTooltipLabel_.setJustificationType (juce::Justification::centredLeft);
    statusTooltipLabel_.setFont (juce::FontOptions (12.0f));
    statusTooltipLabel_.setColour (juce::Label::textColourId, theme.textDim);
    addAndMakeVisible (statusTooltipLabel_);

    // ---- Phase 4a: settings side panel (right side, always-on-top) ----
    // The SettingsPanel is owned + deleted by the SidePanel.
    // RIGHT-docked so the panel never covers the left-side Settings button
    // (which the user re-clicks to dismiss it). Was left-docked (true).
    settingsPanelHost_ = std::make_unique<juce::SidePanel> (TRANS ("Settings"), 300, false);
    settingsPanel_ = new SettingsPanel (processorRef_, themeManager_,
        [this] (double z) { setZoom (z); repaint(); },
        [] (bool b)         { ParamControl::setTooltipsEnabled (b); },
        [this] (bool b)     { processorRef_.setParameterSmoothing (b); },
        [] (int)            {},   // processor.setOversamplingFactor already applied in the panel
        [this] (const juce::String& code) {
            // Language changed: persist it, install the LocalisedStrings, then
            // re-translate every chrome string live.
            processorRef_.setUiLanguage (code);
            installLanguage (code);
            applyChromeTranslations();
        },
        [this] (int mode) {
            // Voice mode changed: persist + apply to the engine (deferred
            // rebuild) and switch the meter view to match.
            processorRef_.setUiVoiceMode (mode);
            if (globalVoiceMeter_ != nullptr)
                globalVoiceMeter_->setViewMode (mode == 1 ? VoiceMeter::ViewMode::Extended
                                                           : VoiceMeter::ViewMode::Voicecard);
        });
    settingsPanelHost_->setContent (settingsPanel_, true);
    // Keep the Settings button's toggle state in sync when the panel is
    // dismissed by other means (the dismiss glyph / clicking outside / ESC) —
    // onPanelShowHide fires after the slide animation on any show/hide.
    settingsPanelHost_->onPanelShowHide = [this] (bool isShown) {
        settingsButton_.setToggleState (isShown, juce::dontSendNotification);
    };
    addAndMakeVisible (*settingsPanelHost_);

    // Refresh the Multi page (~30 Hz) so it tracks the edited part.
    startTimerHz (30);

    setSize (980, 660);
    setResizable (true, true);
    setResizeLimits (720, 480, 1600, 1100);

    // Apply persisted zoom (global scale; only if non-default to avoid an
    // unnecessary rescale at startup).
    if (processorRef_.getUiZoom() != 1.0)
        setZoom (processorRef_.getUiZoom());
}

ParvatiEditor::~ParvatiEditor()
{
    stopTimer();
    // Clear callbacks that capture `this` before the owning components are
    // destroyed during the reverse-order member teardown (defensive: the
    // components stop their own timers in their destructors, but nulling the
    // providers avoids any lingering reference).
    if (globalVoiceMeter_ != nullptr)
        globalVoiceMeter_->setStateProvider (nullptr);
    if (keyboardView_ != nullptr)
        keyboardView_->setNoteCallback (nullptr);
    // Detach from the theme broadcaster and release the L&F BEFORE the member
    // objects (themeManager_, lnf_) and the base Component are destroyed, so the
    // ChangeBroadcaster never calls back into a half-dead editor and no child
    // component references a destroyed L&F during teardown.
    themeManager_.removeChangeListener (this);
    setLookAndFeel (nullptr);

    // SF-2: reset the process-wide global scale factor so a non-default zoom
    // does not leak to other JUCE windows / plugin instances after this editor
    // closes. (Global scale is the only zoom path today; per-editor transform
    // zoom is a documented future enhancement — see the setZoom() comment.)
    if (zoom_ != 1.0)
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);
}

void ParvatiEditor::timerCallback()
{
    // Mirror the UndoManager's undo/redo availability onto the top-bar buttons
    // (~30 Hz, same cadence as the Multi-page part-sync below). Cheap O(1)
    // canUndo/canRedo checks; setEnabled() is a no-op when unchanged.
    undoButton_.setEnabled (processorRef_.getUndoManager().canUndo());
    redoButton_.setEnabled (processorRef_.getUndoManager().canRedo());

    // ---- Bottom status strip: active-voice count + hover tooltip (~30 Hz) ----
    {
        auto& engine = processorRef_.getEngine();
        int active = 0;
        for (int i = 0; i < engine.getNumVoices(); ++i)
            if (auto* av = engine.getAmbikaVoice (i); av != nullptr && av->isDisplayedActive())
                ++active;
        const int denom = processorRef_.getEngine()
            .getPart (processorRef_.getEngine().getCurrentPart()).voiceCount_.load();
        const juce::String countText = juce::String (active) + "/" + juce::String (denom);
        if (statusCountLabel_.getText() != countText)
            statusCountLabel_.setText (countText, juce::dontSendNotification);

        // Tooltip bar: the help text of the control under the mouse (walks up
        // to the first ancestor carrying a tooltip). Empty when tooltips are
        // disabled in Settings or the mouse is over dead space.
        juce::String tip;
        if (ParamControl::tooltipsEnabled())
        {
            const auto rel = getMouseXYRelative();
            if (getLocalBounds().contains (rel))
                for (auto* c = getComponentAt (rel); c != nullptr; c = c->getParentComponent())
                {
                    const juce::String t = (dynamic_cast<juce::TooltipClient*> (c) != nullptr)
                        ? dynamic_cast<juce::TooltipClient*> (c)->getTooltip() : juce::String();
                    if (t.isNotEmpty()) { tip = t; break; }
                }
        }
        if (statusTooltipLabel_.getText() != tip)
            statusTooltipLabel_.setText (tip, juce::dontSendNotification);
    }

    // Only re-read the Multi page when the edited part actually changes (plus a
    // forced refresh after a .MUL load). This avoids re-setting the controls
    // ~30x/sec and any chance of fighting a user mid-drag.
    if (multiPage_ != nullptr)
        multiPage_->refreshIfPartChanged();

    // ---- Keyboard latching: mirror sounding notes across all voices ----
    if (keyboardView_ == nullptr)
        return;

    const int curPart = processorRef_.getEngine().getCurrentPart();
    if (curPart != lastLatchPart_)
    {
        // Edited part changed: clear all latched notes to avoid stuck lamps.
        for (int n = 0; n < 128; ++n)
            keyboardView_->latchNoteOff (n);
        latchedNotes_.clear();
        lastLatchPart_ = curPart;
        return;
    }

    // Collect the set of currently-active notes across ALL voices.
    juce::Array<int> activeNotes;
    auto& engine = processorRef_.getEngine();
    for (int i = 0; i < engine.getNumVoices(); ++i)
    {
        // SF-1: read the lock-free atomic snapshot instead of the
        // non-atomic SynthesiserVoice::currentlyPlayingNote.
        auto* voice = engine.getAmbikaVoice (i);
        if (voice != nullptr && voice->isDisplayedActive())
        {
            const int note = voice->getDisplayedNote();
            if (note >= 0 && ! activeNotes.contains (note))
                activeNotes.add (note);
        }
    }

    // New notes: latch on.
    for (int note : activeNotes)
    {
        if (! latchedNotes_.contains (note))
        {
            keyboardView_->latchNoteOn (note, 1.0f);
            latchedNotes_.add (note);
        }
    }

    // Released notes: latch off.
    for (int i = latchedNotes_.size() - 1; i >= 0; --i)
    {
        if (! activeNotes.contains (latchedNotes_[i]))
        {
            keyboardView_->latchNoteOff (latchedNotes_[i]);
            latchedNotes_.remove (i);
        }
    }
}

void ParvatiEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A new theme was selected: re-apply the L&F colours, refresh the few
    // explicitly-coloured elements, and repaint everything.
    lnf_.setTheme (themeManager_.getCurrentTheme());
    for (auto* page : generatedPages_)
        page->applyThemeColors();
    if (multiPage_ != nullptr)
        multiPage_->applyThemeColors();
    statusCountLabel_.setColour (juce::Label::textColourId,
                                 themeManager_.getCurrentTheme().accent);
    statusTooltipLabel_.setColour (juce::Label::textColourId,
                                   themeManager_.getCurrentTheme().textDim);
    // Phase 4a: refresh visualization components so they pick up the new colours.
    if (keyboardView_ != nullptr)
        keyboardView_->refresh();
    if (globalVoiceMeter_ != nullptr)
        globalVoiceMeter_->refresh();
    repaint();
}

void ParvatiEditor::setZoom (double zoom)
{
    zoom_ = juce::jlimit (0.75, 2.0, zoom);
    juce::Desktop::getInstance().setGlobalScaleFactor (static_cast<float> (zoom_));
}

bool ParvatiEditor::keyPressed (const juce::KeyPress& key)
{
    // Only Cmd/Ctrl + +/-/0 are zoom shortcuts; everything else passes through
    // so typing in combos / text boxes is never swallowed.
    if (! (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()))
        return false;

    auto applyZoom = [this] (double z)
    {
        setZoom (z);                               // clamps to [0.75, 2.0] + applies global scale
        processorRef_.setUiZoom (zoom_);           // persist the clamped value
        if (settingsPanel_ != nullptr)
            settingsPanel_->setZoomValue (zoom_);   // mirror into the slider (no re-fire)
    };

    // Accept both '=' (un-shifted) and '+' for zoom-in across keyboard layouts.
    if (key.getKeyCode() == '+' || key.getKeyCode() == '=')
    {
        applyZoom (zoom_ + 0.1);
        return true;
    }
    if (key.getKeyCode() == '-')
    {
        applyZoom (zoom_ - 0.1);
        return true;
    }
    if (key.getKeyCode() == '0')
    {
        applyZoom (1.0);
        return true;
    }

    // Phase 4c: Undo / Redo. Cmd/Ctrl+Z = undo; Cmd/Ctrl+Shift+Z or
    // Cmd/Ctrl+Y = redo. These carry the Cmd/Ctrl modifier (already required to
    // reach here), so they never collide with KeyboardView's plain-key musical
    // typing — that view returns false for modifier-key combos, letting these
    // keypresses bubble up to the editor. The keyCode is the bare letter on
    // both shifted and un-shifted presses (JUCE tracks shift in the modifiers),
    // so check 'z'/'Z' both and decide undo-vs-redo from isShiftDown().
    const int code = key.getKeyCode();
    if (code == 'z' || code == 'Z')
    {
        if (key.getModifiers().isShiftDown())
            processorRef_.getUndoManager().redo();
        else
            processorRef_.getUndoManager().undo();
        return true;
    }
    if (code == 'y' || code == 'Y')
    {
        processorRef_.getUndoManager().redo();
        return true;
    }

    return false;
}

void ParvatiEditor::applyChromeTranslations()
{
    // Re-translate every editor-chrome string through the active
    // LocalisedStrings so a live language switch updates immediately. tabKeys_
    // holds the English (key) names in tab order; both the tab button and the
    // matching page heading are re-applied for the generated pages. With no
    // mappings installed (English) TRANS() is the identity, so this is a no-op
    // for the byte-identical default.
    patchCaption_.setText (TRANS ("Patch:"), juce::dontSendNotification);
    partCaption_.setText (TRANS ("Part:"), juce::dontSendNotification);
    loadButton_.setButtonText (TRANS ("Load..."));
    saveButton_.setButtonText (TRANS ("Save..."));
    undoButton_.setTooltip (TRANS ("Undo"));
    redoButton_.setTooltip (TRANS ("Redo"));
    settingsButton_.setTooltip (TRANS ("Settings"));

    for (size_t i = 0; i < tabKeys_.size(); ++i)
    {
        const auto translated = TRANS (tabKeys_[i]);
        tabs_.setTabName (static_cast<int> (i), translated);
        if (i < generatedPages_.size())
            generatedPages_[i]->setHeadingText (translated);
    }

    if (multiPage_ != nullptr)
        multiPage_->refreshLanguage();
    if (settingsPanel_ != nullptr)
        settingsPanel_->refreshLanguage();
    // NOTE: the SidePanel's own title-bar text ("Settings") has no public setter,
    // so it updates on the next editor open (set via TRANS at construction) but
    // not live. The in-panel chrome (Language combo etc.) DOES update live.

    repaint();
}

void ParvatiEditor::paint (juce::Graphics& g)
{
    g.fillAll (themeManager_.getCurrentTheme().windowBackground);
}

void ParvatiEditor::resized()
{
    auto area = getLocalBounds();

    // ---- Bottom status strip = LOWEST band: [n/denom] + tooltip bar ----
    {
        auto strip = area.removeFromBottom (kVoiceStripH).reduced (6, 1);
        statusCountLabel_.setBounds (strip.removeFromLeft (48));
        statusTooltipLabel_.setBounds (strip);
    }

    // ---- Keyboard strip directly above the status strip ----
    constexpr int kWheelsW = 76;
    if (keyboardView_ != nullptr)
    {
        auto bottomStrip = area.removeFromBottom (kKeyboardH);
        if (wheels_ != nullptr)
            wheels_->setBounds (bottomStrip.removeFromLeft (kWheelsW));
        keyboardView_->setBounds (bottomStrip);
    }

    // ---- Top bar: Patch:[browser]  Part:[part▾]   …   [Load][Save][↶][↷][⚙] ----
    auto bar = area.removeFromTop (kBarHeight).reduced (6, 4);
    // Right cluster first (removeFromRight => the first item ends up rightmost).
    settingsButton_.setBounds (bar.removeFromRight (30));   // gear, top-right corner
    redoButton_.setBounds (bar.removeFromRight (30));
    undoButton_.setBounds (bar.removeFromRight (30));
    saveButton_.setBounds (bar.removeFromRight (96));   // carries the format popup menu
    loadButton_.setBounds (bar.removeFromRight (76));
    // Left cluster.
    patchCaption_.setBounds (bar.removeFromLeft (48));
    if (presetBrowser_ != nullptr)
        presetBrowser_->setBounds (bar.removeFromLeft (220));
    partCaption_.setBounds (bar.removeFromLeft (40));
    partCombo_.setBounds (bar.removeFromLeft (84));
    // (remaining middle space is flexible / empty)

    // ---- Middle: tabs (gains the old kMeterStripH band, loses kVoiceStripH) ----
    tabs_.setBounds (area);

    // Responsive reflow (Phase 2b): every generated page fills its tab's width so
    // the grouped panels wrap to the window size (vertical-only scrolling). All
    // tabs share the same content width, so reflow every page to it; a hair is
    // reserved so the panel borders clear a vertical scrollbar when one appears.
    const int targetW = juce::jmax (280, tabs_.getWidth() - 16);
    for (auto* page : generatedPages_)
        page->reflowToWidth (targetW);
}

//==========================================================================
void ParvatiEditor::openLoadDialog()
{
    fileChooser_ = std::make_unique<juce::FileChooser> ("Load Patch / Multi (.PRO / .MUL / .parvati)",
                                                       juce::File(), "*.PRO;*.MUL;*.parvati");
    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
            applyPatchFile (fc.getResult());
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::openSaveDialog()
{
    // Save the CURRENT part as an Ambika .PRO (byte-faithful; shareable with
    // Ambika hardware). For a full-fidelity Parvati patch (incl. vca_curve /
    // filter_card), use "Save Parvati". Defaults to the user's preset area.
    auto defaultName = processorRef_.getLoadedProgramName();
    if (defaultName.isEmpty())
        defaultName = "Parvati";
    const juce::File defaultDir = processorRef_.getUserPatchDir();
    defaultDir.createDirectory();   // ensure USER/ exists
    const juce::File defaultFile (defaultDir.getChildFile (defaultName + ".PRO"));
    fileChooser_ = std::make_unique<juce::FileChooser> ("Save Ambika Patch (.PRO)",
                                                       defaultFile, "*.PRO");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
        {
            const auto f = fc.getResult().withFileExtension (".PRO");
            if (processorRef_.saveProgramFile (f))
            {
                if (presetBrowser_ != nullptr)
                    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
            }
        }
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::openSaveParvatiDialog()
{
    // Save a full-fidelity Parvati-native patch (.parvati, YAML) that carries
    // EVERYTHING — including vca_curve / filter_card / arp, which the Ambika
    // .PRO byte format drops. Defaults to the user's preset area.
    auto defaultName = processorRef_.getLoadedProgramName();
    if (defaultName.isEmpty())
        defaultName = "Parvati";
    const juce::File defaultDir = processorRef_.getUserPatchDir();
    defaultDir.createDirectory();
    const juce::File defaultFile (defaultDir.getChildFile (defaultName + ".parvati"));
    fileChooser_ = std::make_unique<juce::FileChooser> ("Save Parvati Patch (.parvati)",
                                                       defaultFile, "*.parvati");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        if (fc.getResults().size() > 0)
        {
            const auto f = fc.getResult().withFileExtension (".parvati");
            if (processorRef_.saveParvatiPatchFile (f))
            {
                if (presetBrowser_ != nullptr)
                    presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
            }
        }
        fileChooser_ = nullptr;
    });
}

void ParvatiEditor::applyPatchFile (const juce::File& f)
{
    // .MUL -> multitimbral multi (all 6 Parts); .PRO -> single program;
    // .parvati -> Parvati-native YAML (patch or multi, sniffed by format:).
    bool ok = false;
    bool isMulti = false;

    if (f.hasFileExtension (".parvati"))
    {
        juce::String text;
        if (juce::FileInputStream in (f); in.openedOk())
            text = in.readEntireStreamAsString();
        const juce::String fmt = parvati::preset::detectParvatiFormat (text);
        if (fmt == parvati::preset::kFormatMulti)
        {
            isMulti = true;
            ok = processorRef_.loadParvatiMultiFile (f);
        }
        else
        {
            ok = processorRef_.loadParvatiPatchFile (f);
        }
    }
    else
    {
        isMulti = f.hasFileExtension (".mul");
        ok = isMulti ? processorRef_.loadMultiFile (f)
                     : processorRef_.loadProgramFile (f);
    }

    if (ok)
    {
        if (presetBrowser_ != nullptr)
            presetBrowser_->setCurrentName (processorRef_.getLoadedProgramName());
        // A multi rewrites every part's channel / key zone / voice allocation,
        // so force the Multi page to re-read even though the edited part is
        // unchanged.
        if (isMulti && multiPage_ != nullptr)
            multiPage_->forceRefresh();

        // A .parvati multi can change the global Voice Mode (Hardware/Extended);
        // keep the voice-meter view + the Settings voice-mode combo in sync with
        // the engine so the UI matches what the patch selected.
        if (globalVoiceMeter_ != nullptr)
            globalVoiceMeter_->setViewMode (processorRef_.getUiVoiceMode() == 1
                                            ? VoiceMeter::ViewMode::Extended
                                            : VoiceMeter::ViewMode::Voicecard);
        if (settingsPanel_ != nullptr)
            settingsPanel_->refreshVoiceModeCombo();
    }
}

bool ParvatiEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& fn : files)
        if (fn.endsWithIgnoreCase (".pro") || fn.endsWithIgnoreCase (".mul") || fn.endsWithIgnoreCase (".parvati"))
            return true;
    return false;
}

void ParvatiEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& fn : files)
    {
        juce::File f (fn);
        if (f.hasFileExtension (".pro") || f.hasFileExtension (".mul") || f.hasFileExtension (".parvati"))
        {
            applyPatchFile (f);
            break;
        }
    }
}
