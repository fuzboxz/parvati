// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ui/ParamPage.h.

#include "ParamPage.h"

#include "NoteStepControl.h"     // sequencer step cells (buildGroups)
#include "SeqLengthStepper.h"    // sequencer length cells (buildGroups)
#include "ParvatiTheme.h"   // ParvatiTheme (getCurrentTheme return type)
#include "SynthParamLabels.h"    // parvati::paramValueTextSynth (step readouts)
#include "ParvatiLookAndFeel.h"   // parvati::isY2kTheme (Y2K window-chrome transparency)
#include "ThemeManager.h"   // themeManager_.getCurrentTheme()


// Sub-section key for the merged Mixer panel: the three logical
// groups (Mixer / Sub Oscillator / Noise) used only for internal layout +
// divider placement. (groupForId now returns ONE merged name for all mix IDs.)
static juce::String mixerSubSectionForId (const juce::String& id)
{
    if (id == "mix_balance" || id == "mix_op" || id == "mix_param")
        return "Mixer";
    if (id == "mix_sub_shape" || id == "mix_sub")
        return "Sub Oscillator";
    return "Noise / Waveshaper";   // mix_noise / mix_fuzz / mix_crush
}

// Partition a group's control indices into consecutive mixer sub-sections
// (preserving descriptor order): { name, [controlIndices...] }. Used by the
// sectioned layout so each sub-section occupies its own row-band inside the
// single Mixer panel.
static std::vector<std::pair<juce::String, std::vector<int>>>
mixerSubSectionsOf (const std::vector<std::unique_ptr<ParamControl>>& controls,
                    const std::vector<int>& controlIndices)
{
    std::vector<std::pair<juce::String, std::vector<int>>> out;
    for (int ci : controlIndices)
    {
        if (ci < 0 || ci >= (int) controls.size()) continue;
        const auto key = mixerSubSectionForId (controls[(size_t) ci]->getParamID());
        if (out.empty() || out.back().first != key)
            out.emplace_back (key, std::vector<int>{});
        out.back().second.push_back (ci);
    }
    return out;
}

// Per-control column span inside a mixer sub-section (the sectioned panel). A
// span > 1 lets a wider control occupy multiple cells in its row. Keyed on
// paramID so it travels with the descriptor (no hardcoded position): today
// only mix_sub_shape ("Sub Shape") spans 2 cells (cols 1-2) with mix_sub
// ("Sub Level") on col 3.
static int mixerControlSpan (const juce::String& id)
{
    if (id == "mix_sub_shape") return 2;
    return 1;
}

// Number of row-bands a mixer sub-section occupies, honouring per-control
// column spans. Mirrors applyLayout's column-cursor placement exactly so the
// panel height (layoutGroups) + themed divider gaps (paint) stay in sync with
// the actual cell positions (a ceil(size/cols) formula would break the moment
// a span changes the row count).
static int mixerSectionRowCount (const std::vector<std::unique_ptr<ParamControl>>& controls,
                                const std::vector<int>& controlIndices, int cols)
{
    int col = 0, row = 0, rows = 1;
    for (int ci : controlIndices)
    {
        if (ci < 0 || ci >= (int) controls.size())
            continue;
        const int span = mixerControlSpan (controls[(size_t) ci]->getParamID());
        if (col + span > cols && col > 0)   // wrap to a new row
        {
            col = 0;
            ++row;
        }
        rows = juce::jmax (rows, row + 1);
        col += span;
    }
    return rows;
}

//==============================================================================
juce::String ParamPage::groupForId (const juce::String& id)
{
    // ---- Mixer: ONE merged panel ("Mixer") holding all 8 mix
    // controls. The three logical sub-sections (Mixer / Sub Oscillator / Noise)
    // are separated inside the panel by themed dividers (see ParamPage::paint +
    // the sectioned layout path), not by separate bordered boxes. ----
    if (id == "mix_balance" || id == "mix_op"     || id == "mix_param" ||
        id == "mix_sub_shape" || id == "mix_sub" ||
        id == "mix_noise"   || id == "mix_fuzz"  || id == "mix_crush")
        return "Mixer";

    // ---- The two filter modulation amounts share one panel ----
    if (id == "filter_env" || id == "filter_lfo")
        return "Filter Mod";

    // ---- Sequencer length slots (exact ids) belong to their step group ----
    if (id == "seq_length_1") return "Sequencer 1";
    if (id == "seq_length_2") return "Sequencer 2";
    if (id == "seq_length_3") return "Note Pitch";

    // ---- Synth options with no patch byte (Global page) ----
    if (id == "vca_curve" || id == "filter_card" || id == "filter_drive")
        return "Global";

    // ---- Sequencer step grids (prefixes) ----
    if (id.startsWith ("seq1_step")) return "Sequencer 1";
    if (id.startsWith ("seq2_step")) return "Sequencer 2";
    if (id.startsWith ("seqnote_step")) return "Note Pitch";    // note byte (gate in bit 7)
    if (id.startsWith ("seqnote_vel"))  return "Note Velocity"; // vel byte (legato in bit 7)

    // ---- Oscillators / filters ----
    if (id.startsWith ("osc1_"))    return "Osc 1";
    if (id.startsWith ("osc2_"))    return "Osc 2";
    if (id.startsWith ("filter1_")) return "Filter";
    if (id.startsWith ("filter2_")) return "Filter 2";

    // ---- FX slots (fx{N}_*) — one panel per slot. Each FX-slot ParamPage holds
    // a single slot's type/enabled/drywet/param1-4; fx_topo / fx_order (the FX
    // chain topology + slot order) group as "FX Chain" on the FX1 page. ----
    if (id.startsWith ("fx1_")) return "FX1";
    if (id.startsWith ("fx2_")) return "FX2";
    if (id.startsWith ("fx3_")) return "FX3";
    if (id == "fx_topo" || id == "fx_order") return "FX Chain";

    // ---- Envelopes / LFOs (now on separate tabs) ----
    // env{N}_attack/decay/sustain/release -> "Env N (role)"; env{N}_lfo_* -> "LFO N".
    // Role checked from the dsp routing: ENV3 -> VCA (mod-matrix default,
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

    // ---- Arp (the "Part / Play" group is GONE: every part_* descriptor is
    // either absorbed into the Patch page's table or skipped from generation;
    // no part_* id can reach a page group any more) ----
    if (id.startsWith ("arp_"))  return "Arp";

    return "Other";
}

void ParamPage::buildGroups (const std::vector<const PatchParamDescriptor*>& descriptors)
{
    // Partition descriptors into named groups, preserving first-appearance
    // order of the groups and the descriptor order within each group.
    for (int i = 0; i < (int) descriptors.size(); ++i)
    {
        const juce::String gname = groupForId (descriptors[(size_t) i]->paramID);
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

    // For sequencer step-grid groups, the Length control must be the LAST cell
    // (user edits steps first, then sets the length). The descriptor order has
    // seq_length_* before the step params, so reorder via stable_partition.
    for (auto& g : groups_)
    {
        if (! (g.name == "Sequencer 1" || g.name == "Sequencer 2" || g.name == "Note Pitch"))
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

        // Dense step grids (Seq1/2 + the Note Pitch/Velocity splits). 8 columns
        // so 16 steps wrap to 2 rows (17 cells -> 3 rows); cell sizes fit the
        // narrow GroupPager content area (no horizontal scrollbar) while keeping
        // the 44px step knob legible.
        if (g.name == "Sequencer 1" || g.name == "Sequencer 2"
            || g.name == "Note Pitch" || g.name == "Note Velocity")
        {
            g.stepGrid = true;
            g.internalCols = 8;
            g.cellW = 72;
            g.cellH = 64;   // was 56: step dial 28px -> 36px (note names fit; 2 rows still <= 262 budget)
        }
        // Mod-matrix slots: source / dest / amount, one row each. Two slots fit
        // side-by-side in the 50% right-mod column; a GroupPager shows 4 per page.
        else if (g.name.startsWith ("Mod "))
        {
            g.singleRow = true;
            g.internalCols = juce::jmax (1, n);
            g.cellW = 96;
            g.cellH = 56;
        }
        // Modifier strips: in1 / in2 / op combos; 2 per GroupPager page.
        else if (g.name.startsWith ("Modifier "))
        {
            g.singleRow = true;
            g.internalCols = juce::jmax (1, n);
            g.cellW = 96;
            g.cellH = 64;
        }
        // Env / LFO generators: one row of knobs + an ADSR/LFO preview graph.
        // Sized for the 50% left-mod column width + the GroupPager content height.
        else if (g.name.startsWith ("Env ") || g.name.startsWith ("LFO ") || g.name == "Voice LFO")
        {
            g.internalCols = juce::jmax (1, n);
            g.cellW = 150;
            g.cellH = 82;   // dial = 82 - 22 = 60 px (the module-knob size)
        }
        // Mixer column (narrow 20%): ONE merged "Mixer" panel holds
        // all 8 mix controls, laid out in 3 logical sub-sections (one row each)
        // separated by themed dividers. cellH matches Filter (full-arc knobs);
        // cellW is a floor that the row-fill grows to the column width. One
        // panel of 3 rows = 232px <= 279px main-row half at 1280x620.
        else if (g.name == "Mixer")
        {
            g.sectioned    = true;
            g.internalCols = 3;   // widest sub-section (Mixer/Noise = 3 knobs)
            g.cellW = 60;         // floor: 3-col natural = 196px; the R3 clamp compresses knobs slightly at the 1024 floor
            g.cellH = 82;         // dial = 82 - 22 = 60 px (the module-knob size)
        }
        // Filter column (40%): Filter (3 knobs) + Filter Mod (2 amounts) + a
        // magnitude-response curve decoration under Filter. cellW sized so the
        // 3-knob group (3*cellW+16) fits the 420px content width at the 1100px
        // minimum (no horizontal clipping of the knobs OR the curve); the row
        // grows to fill at 1280.
        else if (g.name == "Filter" || g.name == "Filter Mod")
        {
            g.internalCols = generalCols (n);
            g.cellW = 130;  // 3-col group = 406px; the R3 clamp compresses knobs slightly at the 1024 floor
            g.cellH = 82;   // dial = 82 - 22 = 60 px (the module-knob size)
        }
        // Oscillators (40% column): Shape combo + INLINE waveform preview + the
        // other 3 knobs (param/range/detune), all in ONE row so both "Osc 1" +
        // "Osc 2" stack and fit the OSC column (R3 clamps the dials slightly
        // at the 1024 floor) with BOTH visible at once (no [OSC1][OSC2] pager).
        // The row is modelled as 5 columns: col0=Shape, col1=reserved for the
        // INLINE preview (set via setGroupInlinePreview), col2..4=param/range/detune.
        // cellW=80 is a floor sized so 5 columns (5*80+16=416px) fit the 420px
        // content width at the 1100px width (the R3 clamp compresses cells at the
        // 1024 floor instead of clipping); the row grows to fill at 1280.
        else if (g.name == "Osc 1" || g.name == "Osc 2")
        {
            g.internalCols = 5;
            g.cellW = 80;
            g.cellH = 82;   // dial = 82 - 22 = 60 px (the module-knob size)
        }
        else
        {
            g.internalCols = generalCols (n);
        }
    }
}

void ParamPage::layoutGroups (int targetWidth)
{
    const int topY = kMargin;   // no page-heading row: the tab bar already names the page
    const int availW = targetWidth - 2 * kMargin;

    // Natural panel size for each group (independent of placement).
    for (auto& g : groups_)
    {
        if (! groupVisible (g))   // a GroupPager subset hides the other groups
            continue;
        const int cols = juce::jmax (1, g.internalCols);
        {
            const int n = (int) g.controlIndices.size();
            if (g.sectioned)
            {
                // A sectioned panel (merged Mixer) stacks its sub-sections, each
                // on its own row-band, separated by kSectionGap dividers. The
                // total height must match applyLayout's placement exactly so no
                // control lands outside its group rect (layoutIsSane check c).
                const auto sections = mixerSubSectionsOf (controls_, g.controlIndices);
                int rows = 0;
                for (const auto& s : sections)
                    rows += mixerSectionRowCount (controls_, s.second, cols);
                const int gaps = juce::jmax (0, (int) sections.size() - 1);
                g.naturalWidth  = cols * g.cellW + 2 * kGroupPad;
                g.naturalHeight = kGroupTitleH + rows * g.cellH + gaps * kSectionGap + 2 * kGroupPad;
            }
            else
            {
                const int rows = (n + cols - 1) / cols;
                g.naturalWidth  = cols * g.cellW + 2 * kGroupPad;
                g.naturalHeight = kGroupTitleH + rows * g.cellH + 2 * kGroupPad;
            }
        }
        // A group with a decoration (e.g. an ADSR/LFO preview) reserves room
        // below its control cells so the panel height includes it.
        if (g.decoration != nullptr)
            g.naturalHeight += g.decorationH + kDecorationGap;
        // A group's EXTERNAL (non-owned) decoration reserves room BELOW the
        // owned one (e.g. the Patch page's part-allocation table under the
        // Global panel's knobs), so it is inside the same border.
        if (g.externalDecoration != nullptr)
            g.naturalHeight += g.externalDecorationH + kDecorationGap;
    }

    // Greedy left-to-right flow. A row wraps when the next panel would overflow
    // the available width OR when the row already holds pageCols_ panels
    // (PageInfo::cols: a tunable cap on panels-per-row; pageCols_ <= 0 =>
    // width-only wrap). rowOf[gi] tags each group with its row for the fill pass.
    int x = kMargin, y = topY, rowH = 0;
    const int rowStartX = kMargin;
    const int maxRight = kMargin + juce::jmax (0, availW);

    // Visible group indices only (a GroupPager subset hides the rest). Placement,
    // the pageCols_ count, and the row-fill pass all operate on JUST these, so a
    // hidden group neither occupies space nor overlaps a visible one, and the
    // page reflows to the subset's natural size.
    std::vector<int> vis;
    vis.reserve (groups_.size());
    for (int gi = 0; gi < (int) groups_.size(); ++gi)
        if (groupVisible (groups_[(size_t) gi]))
            vis.push_back (gi);

    std::vector<int> rowOf (groups_.size(), -1);   // -1 = hidden (never placed)
    int currentRow = 0;

    for (size_t vi = 0; vi < vis.size(); ++vi)
    {
        const int gi = vis[vi];
        auto& g = groups_[(size_t) gi];

        // Panels already placed on THIS row (for the pageCols_ cap). Walk back
        // over the VISIBLE groups only (hidden ones keep rowOf == -1).
        int panelsThisRow = 0;
        for (int k = (int) vi - 1; k >= 0 && rowOf[(size_t) vis[(size_t) k]] == currentRow; --k)
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
        // R3 width clamp: a panel whose natural width exceeds the available
        // row width SHRINKS to it instead of overflowing (its cells compress
        // down to the 44pt touch floor in applyLayout) — compact columns/
        // AUv3 panes degrade to smaller controls, never to controls painted
        // over the neighbouring column.
        g.rect.setBounds (x, y,
                          juce::jmin (g.naturalWidth, juce::jmax (kMargin, maxRight - x)),
                          g.naturalHeight);
        x += g.naturalWidth + kGroupGap;
        rowH = juce::jmax (rowH, g.naturalHeight);
    }
    const int lastRow = vis.empty() ? -1 : currentRow;

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
        int tileX = rowStartX;
        for (int gi : rowPanels)
        {
            auto& g = groups_[(size_t) gi];
            g.rect.setX (tileX);
            tileX = g.rect.getRight() + kGroupGap;
        }
    }

    contentWidth_  = juce::jmax (targetWidth, 2 * kMargin + 40);
    const int naturalH = groups_.empty() ? (topY + kMargin)
                                          : (y + rowH + kMargin);

    // Vertically centre a short page inside its viewport so sparse pages (Arp /
    // Global) do not leave a large empty void below the controls: shift the
    // whole grid down by half the slack and grow the page to fill the viewport
    // (no vertical scroll when it fits). Pages taller than the viewport keep
    // their natural height and scroll as before. The target height comes from
    // centerHeight_ (set by the editor for every tab) — NOT getViewHeight(),
    // which tracks the content size and so can never exceed the natural height.
    // Top-align the page content (no vertical centring). Previously short
    // pages (single-row group subsets like Mod Matrix [13-14] or Modifiers
    // [3-4]) floated in the vertical middle of the viewport, looking
    // inconsistent next to denser multi-row pages. Content now starts at the
    // top; the page still grows to fill the viewport (contentHeight_ >= viewH)
    // so there is no vertical scrollbar. The target height comes from
    // centerHeight_ (set by the editor for every tab) — NOT getViewHeight(),
    // which tracks the content size and so can never exceed the natural height.
    yOffset_ = 0;
    // Prefer the editor-supplied tab height (reliable for every tab); fall back
    // to the parent Viewport's physical height for standalone / headless use.
    int viewH = centerHeight_;
    if (viewH == 0)   // not supplied by the caller: parent-Viewport fallback (standalone / headless). -1 = natural-height mode (see reflowToWidth): no fill, no fallback.
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            viewH = vp->getHeight();
    contentHeight_ = juce::jmax (naturalH, viewH);   // viewH == -1 keeps naturalH
}

void ParamPage::applyLayout()
{
    for (auto& g : groups_)
    {
        const bool visible = groupVisible (g);
        if (g.groupComp != nullptr)
            g.groupComp->setVisible (visible);
        if (g.decoration != nullptr)
            g.decoration->setVisible (visible);
        if (g.externalDecoration != nullptr)
            g.externalDecoration->setVisible (visible);
        if (g.inlinePreview != nullptr)
            g.inlinePreview->setVisible (visible);
        if (! visible)
        {
            // A hidden group's controls are never positioned here; hide them so
            // they do not paint over the active subset.
            for (int ci : g.controlIndices)
                if (ci >= 0 && ci < (int) controls_.size())
                    controls_[(size_t) ci]->setVisible (false);
            continue;
        }

        if (g.groupComp != nullptr)
            g.groupComp->setBounds (g.rect);

        auto inner = g.rect.reduced (kGroupPad);
        inner.removeFromTop (kGroupTitleH);   // room for the panel title text

        const int cols = juce::jmax (1, g.internalCols);
        // Flexible-width cells: a NON-dense panel distributes its cells evenly
        // across the actual (possibly row-filled) inner width; a DENSE panel
        // (sequencer step grid / mod-modifier strip) uses its fixed cell size.
        // R3: the shared 44pt touch floor lets BOTH compress when the group was
        // width-clamped in layoutGroups (a compacted column degrades to smaller
        // controls, not to controls overlapping the next column).
        constexpr int kCellStepFloor = 44;
        // Both branches compress down to the 44pt floor when the group was
        // width-clamped (a compacted column degrades to smaller controls, not
        // to controls overlapping the next column); at natural/grown widths
        // inner/cols equals the design cellW so the layout is unchanged.
        const int step = juce::jmax (kCellStepFloor, inner.getWidth() / cols);

        if (g.sectioned)
        {
            // Sectioned panel (merged Mixer): each sub-section occupies its own
            // row-band; Y advances by the section's row count + kSectionGap so
            // the themed dividers (drawn in paint) sit in the gaps. This mirrors
            // layoutGroups' naturalHeight exactly so no control lands outside
            // its group rect.
            const auto sections = mixerSubSectionsOf (controls_, g.controlIndices);
            int y = inner.getY();
            for (const auto& s : sections)
            {
                // Per-control column span: a column cursor advances by each
                // control's span (mix_sub_shape = 2), wrapping to the next row
                // when it would overflow cols. The sub-section's row count is
                // precomputed by the SAME cursor logic (mixerSectionRowCount) so
                // the themed divider gap sits at the right Y regardless of spans.
                const int sRows = mixerSectionRowCount (controls_, s.second, cols);
                int col = 0, row = 0;
                for (int i = 0; i < (int) s.second.size(); ++i)
                {
                    const int ci = s.second[(size_t) i];
                    if (ci < 0 || ci >= (int) controls_.size())
                        continue;
                    const int span = mixerControlSpan (controls_[(size_t) ci]->getParamID());
                    if (col + span > cols && col > 0)   // wrap to a new row
                    {
                        col = 0;
                        ++row;
                    }
                    const juce::Rectangle<int> cell (inner.getX() + col * step,
                                                     y + row * g.cellH,
                                                     span * step, g.cellH);
                    auto* ctrl = controls_[(size_t) ci].get();
                    ctrl->setVisible (true);
                    ctrl->setBounds (cell.reduced (3));
                    col += span;
                }
                y += sRows * g.cellH + kSectionGap;
            }
            continue;   // sectioned: skip the standard grid + decoration
        }

        // OSC groups reserve column 1 for an INLINE waveform preview (col0=Shape,
        // col1=preview, col2..4=param/range/detune), so the shape combo + preview
        // sit side by side and the other 3 knobs shift right by one column.
        const bool hasInlinePreview = (g.inlinePreview != nullptr);

        for (int idx = 0; idx < (int) g.controlIndices.size(); ++idx)
        {
            const int ci = g.controlIndices[(size_t) idx];
            if (ci < 0 || ci >= (int) controls_.size()) continue;
            const int col = hasInlinePreview ? (idx == 0 ? 0 : idx + 1) : (idx % cols);
            const int row = hasInlinePreview ? 0 : (idx / cols);
            const juce::Rectangle<int> cell (inner.getX() + col * step,
                                             inner.getY() + row * g.cellH,
                                             step, g.cellH);
            auto* ctrl = controls_[(size_t) ci].get();
            ctrl->setVisible (true);
            ctrl->setBounds (cell.reduced (3));
        }

        // The inline preview occupies its reserved column (col 1) for the row height.
        if (hasInlinePreview)
        {
            g.inlinePreview->setVisible (true);
            const juce::Rectangle<int> pc (inner.getX() + 1 * step,
                                           inner.getY(),
                                           step, g.cellH);
            g.inlinePreview->setBounds (pc.reduced (3));
        }

        // A group's decoration (if any) spans the panel width below the cells;
        // an EXTERNAL (non-owned) decoration spans the panel width below THAT,
        // so both sit inside the same bordered panel.
        const int rows = ((int) g.controlIndices.size() + cols - 1) / cols;
        int decY = inner.getY() + rows * g.cellH + kDecorationGap;
        if (g.decoration != nullptr)
        {
            g.decoration->setBounds (
                juce::Rectangle<int> (inner.getX(), decY, inner.getWidth(), g.decorationH));
            decY += g.decorationH + kDecorationGap;
        }
        if (g.externalDecoration != nullptr)
        {
            g.externalDecoration->setBounds (
                juce::Rectangle<int> (inner.getX(), decY, inner.getWidth(), g.externalDecorationH));
        }
    }
}

bool ParamPage::layoutIsSane() const
{
    // (a) every VISIBLE group panel has positive size. (Hidden groups in an
    // active setVisibleGroups subset have no rect and are skipped.)
    for (const auto& g : groups_)
    {
        if (! groupVisible (g))
            continue;
        if (g.rect.getWidth() <= 0 || g.rect.getHeight() <= 0)
            return false;
    }

    // (b) no two VISIBLE group panels overlap (siblings in page coordinate space).
    for (size_t i = 0; i < groups_.size(); ++i)
    {
        if (! groupVisible (groups_[i])) continue;
        for (size_t j = i + 1; j < groups_.size(); ++j)
        {
            if (! groupVisible (groups_[j])) continue;
            if (groups_[i].rect.intersects (groups_[j].rect))
                return false;
        }
    }

    // (c) every control has positive size and sits inside its group's rect.
    // (ParamControl is a direct child of ParamPage, so getBoundsInParent() is in
    // the same page-space coordinates as the group rects.) Only VISIBLE groups
    // are validated (a GroupPager subset hides the rest).
    for (const auto& g : groups_)
    {
        if (! groupVisible (g))
            continue;
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
    {
        if (! groupVisible (g))
            continue;
        if (! (g.stepGrid || g.singleRow))
        {
            anyNonDense = true;
            if (g.rect.getRight() >= maxRight - 2 * kGroupGap)
                fillsWidth = true;
        }
    }
    return ! anyNonDense || fillsWidth;
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

void ParamPage::setGroupExternalDecoration (const juce::String& groupName,
                                             juce::Component* external, int height)
{
    // NON-OWNED second decoration slot (setGroupDecoration is single-slot per
    // group): this page only PARENTS + positions the component — the caller
    // keeps ownership and must outlive this page (see the header contract).
    if (external == nullptr)
        return;
    addAndMakeVisible (external);
    for (auto& g : groups_)
        if (g.name == groupName)
        {
            g.externalDecoration = external;
            g.externalDecorationH = juce::jmax (0, height);
        }

    // Recompute the layout so contentHeight_ already accounts for the reserved
    // height when the owner sizes this page immediately afterwards.
    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

void ParamPage::setGroupInlinePreview (const juce::String& groupName,
                                       std::unique_ptr<juce::Component> preview)
{
    // ParamPage always owns the component (reuse the decorations_ ownership
    // vector), even if @p groupName does not match an existing group.
    auto* raw = preview.get();
    decorations_.push_back (std::move (preview));
    addAndMakeVisible (raw);
    for (auto& g : groups_)
        if (g.name == groupName)
            g.inlinePreview = raw;

    // Re-lay out so the reserved column + remapped knobs take effect immediately.
    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

void ParamPage::setGroupDecorationHeight (const juce::String& groupName, int height)
{
    // Override the reserved room for the named group's decoration (below its
    // control cells). Used for the compact Filter response curve (smaller
    // than the 80px reserved for the Env/LFO ADSR/LFO previews). Re-lays out
    // so the new height takes effect immediately.
    for (auto& g : groups_)
        if (g.name == groupName)
            g.decorationH = juce::jmax (0, height);

    layoutGroups (juce::jmax (940, getWidth()));
    applyLayout();
}

juce::Component* ParamPage::getGroupDecorationForTest (const juce::String& groupName) const
{
    for (const auto& g : groups_)
        if (g.name == groupName)
            return g.decoration;
    return nullptr;
}

juce::Component* ParamPage::getGroupInlinePreviewForTest (const juce::String& groupName) const
{
    for (const auto& g : groups_)
        if (g.name == groupName)
            return g.inlinePreview;
    return nullptr;
}

void ParamPage::setVisibleGroups (const juce::StringArray& groupNames)
{
    visibleGroups_ = groupNames;

    // Not yet sized (construction / pre-layout): defer entirely. The owning
    // GroupPager::resized() does the first real layout at the true content
    // width; laying out here at a guessed width would be wasted and wrong.
    if (getWidth() <= 0)
        return;

    // Re-lay-out at the CURRENT (real) width — never a 940px floor. The 940 floor
    // made the row-fill justification grow non-dense groups (OSC/ENV/LFO) far
    // wider than the narrow GroupPager content area, clipping their right column
    // of knobs on a runtime sub-tab switch (reviewer blocker B1). Dense groups
    // (stepGrid/singleRow) are unaffected by the fill pass either way.
    layoutGroups (getWidth());
    applyLayout();
    setSize (getWidth(), contentHeight_);
}

ParamPage::ParamPage (ParvatiAudioProcessor& processor,
                      ThemeManager& themeManager,
                      const std::vector<const PatchParamDescriptor*>& descriptors,
                      int columns, int cellWidth, int cellHeight)
    : themeManager_ (themeManager),
      cellWidth_ (cellWidth), cellHeight_ (cellHeight)
{
    // Honour the page's declared column count (PageInfo::cols) as a cap on the
    // number of group panels per row (whichever wraps first: width overflow or
    // the cap). 0 => width-only wrap (Patch page is not a ParamPage).
    pageCols_ = juce::jmax (0, columns);

    buildGroups (descriptors);
    configureGroupLayouts();

    // Bordered panels first (so they sit behind the control cells), one per group.
    for (auto& g : groups_)
    {
        // The component NAME keeps the English key (stable identity for
        // setGroupDecoration matching); only the displayed TITLE is translated.
        auto gc = std::make_unique<juce::GroupComponent> (g.name, TRANS (g.name));
        gc->setTextLabelPosition (juce::Justification::top | juce::Justification::left);
        // Outline + title-text colours come from the editor-wide L&F, so a theme
        // switch refreshes them automatically.
        addAndMakeVisible (*gc);
        g.groupComp = gc.get();
        groupComponents_.push_back (std::move (gc));
    }

    // Control cells on top of the panel borders.
    for (const auto* d : descriptors)
    {
        const juce::String pid = d->paramID;
        // Sequencer step cells use purpose-built controls instead of a plain
        // knob: the note byte (0..255, half-dead) -> a remapped rotary (one
        // Rest stop + a full note range); the length (1..16) -> a − [n] +
        // stepper. Both subclass ParamControl so they live in controls_ and
        // inherit the mod ring / right-click menu / step-dimming / label.
        if (pid.startsWith ("seqnote_step"))
            controls_.emplace_back (std::make_unique<NoteStepControl> (processor, *d));
        else if (pid.startsWith ("seq_length_"))
            controls_.emplace_back (std::make_unique<SeqLengthStepper> (processor, *d));
        else
            controls_.emplace_back (std::make_unique<ParamControl> (processor, *d));
        addAndMakeVisible (*controls_.back());
        // User-friendly readout (Hz / ms / semitones / % / note names / ...) on
        // raw-numeric SYNTH knobs only. Choice params already show their text;
        // FX params use their own formatter (FxSlotCard). The note-step rotary
        // owns its own Rest/note readout (remapped range) and the length stepper
        // shows its own number, so both are skipped here. Display-only.
        if (! d->isFx && d->choices == nullptr
            && ! pid.startsWith ("seqnote_step")
            && ! pid.startsWith ("seq_length_"))
            controls_.back()->setDisplayValueText (
                [id = juce::String (d->paramID)] (double v) {
                    return paramValueTextSynth (id, v);
                });
    }

    // Seed the content size at a sensible default width; the editor reflows to
    // the real tab width on the first resized().
    layoutGroups (940);
}

void ParamPage::applyThemeColors()
{
    // Group borders / titles are themed via the L&F; force a repaint so a theme
    // switch refreshes them (and the control cells) immediately.
    for (auto& gc : groupComponents_) gc->repaint();
    for (auto& c : controls_)         c->repaint();
    for (auto& d : decorations_)      d->repaint();   // e.g. ADSR previews read the theme live
    repaint();
}

void ParamPage::refreshLanguage()
{
    // The group-component NAME is the stable English key (used for
    // setGroupDecoration matching); only the displayed TITLE is re-translated.
    for (auto& g : groups_)
        if (g.groupComp != nullptr)
            g.groupComp->setText (TRANS (g.name));
    repaint();
}

void ParamPage::paint (juce::Graphics& g)
{
    const auto& theme = themeManager_.getCurrentTheme();
    // Y2K: NO fill — the liquid-chrome WINDOW sweep shows through; the
    // module cards (dark-steel chrome) pop off it.
    if (! parvati::isY2kTheme (&theme))
        g.fillAll (theme.backgroundBase);

    // Sub-section dividers inside a sectioned panel (merged Mixer):
    // a 1px muted line in each inter-section gap, drawn from the theme divider
    // token (never a literal colour). The gap positions mirror applyLayout's
    // section walk so each line sits cleanly between knob rows.
    for (const auto& grp : groups_)
    {
        if (! grp.sectioned || ! groupVisible (grp))
            continue;
        const auto sections = mixerSubSectionsOf (controls_, grp.controlIndices);
        if (sections.size() < 2)
            continue;
        auto inner = grp.rect.reduced (kGroupPad);
        inner.removeFromTop (kGroupTitleH);
        const int cols = juce::jmax (1, grp.internalCols);
        int y = inner.getY();
        g.setColour (theme.divider);
        for (size_t si = 0; si + 1 < sections.size(); ++si)
        {
            const int sRows = mixerSectionRowCount (controls_, sections[si].second, cols);
            y += sRows * grp.cellH;                 // top of the gap
            const int dy = y + kSectionGap / 2;      // centre of the gap
            g.drawHorizontalLine (dy, (float) inner.getX(), (float) inner.getRight());
            y += kSectionGap;
        }
    }
}

void ParamPage::resized()
{
    layoutGroups (getWidth());
    applyLayout();
}

void ParamPage::reflowToWidth (int targetWidth, int viewportHeight)
{
    if (targetWidth <= 0)
        return;
    // Record the tab content height so layoutGroups can vertically centre short
    // pages consistently across ALL tabs (not just the current one).
    // viewportHeight < 0 = NATURAL-HEIGHT mode: layoutGroups skips BOTH the
    // fill-to-viewport grow and the parent-Viewport fallback, so the page keeps
    // exactly its natural content height. Used by a host that stacks more
    // content below the page inside a scroll body (PatchPage hosts the Global
    // page above the 6 part rows): the fallback would stretch the page to the
    // scroll view's full height and leave a large void between the page's
    // last panel and whatever follows it.
    centerHeight_ = (viewportHeight < 0) ? -1 : juce::jmax (0, viewportHeight);
    // Lay out for the requested width, then adopt the resulting height so the
    // parent Viewport scrolls vertically only. setSize() re-triggers resized()
    // which re-lays-out to the same width (cheap rectangle math).
    layoutGroups (targetWidth);
    applyLayout();
    if (getWidth() != targetWidth || getHeight() != contentHeight_)
        setSize (targetWidth, contentHeight_);
}
