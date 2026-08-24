// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxMatrixView.h.

#include "FxMatrixView.h"
#include "FxSlotLabels.h"          // activeParamCount / paramLabel (dynamic FX-dest labels)
#include "ModDestMap.h"            // isFxDest / kFxModDstOffset (FX-dest domain)
#include "ModMatrixHighlight.h"    // onAssignRequest bus (drag-and-drop -> fxmod slot)

#include "PluginProcessor.h"       // ParvatiAudioProcessor (complete type)
#include "ThemeManager.h"

#include "dsp/patch.h"             // ambika::dsp::MOD_SRC_* (addSlot default)

#include <limits>

// The view assumes the FX matrix's 16-slot capacity (kNumFxMatrixSlots).
// Unlike the synth matrix this count is not tied to a patch-byte stride, so
// there is no firmware static_assert — only this capacity check.
static_assert (kNumFxMatrixSlots == 16, "FxMatrixView assumes a 16-slot FX mod matrix");

//==============================================================================
namespace
{
// The FX-matrix configuration: every structural difference from the synth
// twin in one table (see MatrixViewConfig). TODAY's values; a shared fix
// lands in MatrixViewBase and applies to both views at once.
MatrixViewConfig makeFxConfig()
{
    MatrixViewConfig cfg;
    cfg.paramPrefix      = "fxmod";
    cfg.numSlots         = kNumFxMatrixSlots;   // 16
    cfg.usedSuffixKey    = "of 16 Used";
    cfg.matrixFullKey    = "Matrix Full (16/16)";
    cfg.rowA11yPrefix    = "FX Mod ";
    cfg.destComboAttached   = false;   // dest combo is manually index-bound (dynamic labels)
    cfg.highlightSelfOnHover = true;   // a hovered row highlights itself besides the broadcast
    cfg.lampDiameter            = 18.0f;  // same lamp size as the synth matrix
    cfg.destBusOffset    = parvati::ModDestMap::kFxModDstOffset;   // FX dests broadcast offset-encoded
    cfg.rejectDestAtOrAbove = std::numeric_limits<int>::max();     // raw FX_DST_* indices only
    cfg.addDefaultSource = ambika::dsp::MOD_SRC_ENV_1;
    cfg.addDefaultDest   = 0;   // FX_DST_FX1_DRYWET (the first makeFxDests() entry)
    return cfg;
}

// Per-slot Dry/Wet FX_DST_* index (slot 0/1/2 -> FX_DST_FX{1,2,3}_DRYWET).
// Used by rebuildDestItems to build the dynamic FX-dest combo with a stable
// itemId == FX_DST_* index + 1 (the stored fxmod{N}_dest value is the index
// itself, so presets/serialization are unaffected by the relabelling).
constexpr int kDryWetDestIdx[3] = { FX_DST_FX1_DRYWET, FX_DST_FX2_DRYWET, FX_DST_FX3_DRYWET };

// (Re)build a row's dest combo items from the three slots' current FX types
// so it shows each slot's ACTUAL parameter names ("FX1 Position" ...) instead
// of the static "FX1 Param K". itemId == FX_DST_* index + 1 keeps the stored
// fxmod{N}_dest value (the index) stable. Inactive params are omitted for a
// clean list (a dest routed to a now-absent param is handled by sync below).
void rebuildDestItems (juce::ComboBox& combo, FxType t0, FxType t1, FxType t2)
{
    const FxType types[3] = { t0, t1, t2 };
    combo.clear (juce::dontSendNotification);
    for (int s = 0; s < 3; ++s)
    {
        const int dryWet = kDryWetDestIdx[s];
        combo.addItem ("FX" + juce::String (s + 1) + " Dry/Wet", dryWet + 1);
        const FxType t = types[s];
        const int active = (t != FxType::None && t < FxType::Count)
                               ? activeParamCount (t) : 0;
        for (int idx = 0; idx < kNumFxSlotParams; ++idx)
        {
            if (idx < active)
            {
                const int pIdx = dryWet + 1 + idx;
                combo.addItem ("FX" + juce::String (s + 1) + " "
                                 + juce::String (paramLabel (t, idx)), pIdx + 1);
            }
        }
    }
}

// Sync a row's dest combo selection from the live fxmod{N}_dest value (the
// FX_DST_* index). Called every refresh() tick (preset load / undo /
// assignNextFreeSlot / automation can change the value without the combo). If
// the index is absent from the current list — e.g. the dest points at a param
// the slot's current type no longer exposes — the stored value is LEFT INTACT
// (the effect ignores inactive params) and the combo is cleared.
void syncDestFromParam (FxMatrixView& view, juce::ComboBox& combo, int slot)
{
    const int destIdx = view.destForSlot (slot);
    const int wantId = destIdx + 1;
    if (destIdx >= 0 && combo.indexOfItemId (wantId) >= 0)
    {
        if (combo.getSelectedId() != wantId)
            combo.setSelectedId (wantId, juce::dontSendNotification);
    }
    else if (combo.getSelectedId() != 0)
    {
        combo.setSelectedId (0, juce::dontSendNotification);
    }
}
}  // namespace

//==============================================================================
FxMatrixView::FxMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : MatrixViewBase (processor, themeManager, makeFxConfig())
{
    refresh();

    // Register the FX-domain assign handler on the ModMatrixHighlight bus so
    // a drag-and-drop onto an FX parameter knob (which calls requestAssign
    // with an FX-offset dest) assigns the next free FX mod slot. The dest
    // arrives encoded (FX_DST_* + kFxModDstOffset); decode + guard here so
    // synth dests (< kFxModDstOffset) are ignored (the synth handler owns
    // those). The guard also keeps a hovered-synth-knob drop from grabbing an
    // FX slot. assignNextFreeSlot keeps its RAW FX_DST index contract (0..17).
    juce::Component::SafePointer<FxMatrixView> safe (this);
    assignSub_ = parvati::ModMatrixHighlight::instance().onAssignRequest (
        [safe] (int source, int dest) -> bool
        {
            if (safe == nullptr || ! parvati::ModDestMap::isFxDest (dest))
                return false;   // ignore synth-dest drops
            const int raw = dest - parvati::ModDestMap::kFxModDstOffset;   // FX_DST_* index
            if (raw >= FX_DST_LAST)
                return false;   // defensive: out-of-range FX dest (unreachable via modDest_)
            return safe->assignNextFreeSlot (source, raw);
        });
}

//==============================================================================
juce::Component* FxMatrixView::rowForSlotForTest (int slot)
{
    return rowAtOrNull (slot);
}

juce::String FxMatrixView::slotParam (int slot, const char* suffix)
{
    return "fxmod" + juce::String (slot + 1) + suffix;
}

std::array<FxType, 3> FxMatrixView::currentSlotTypes() const
{
    // The per-slot FX types (fx1/2/3_type), read live so a type edit / preset
    // load / part switch is picked up on the next refresh() tick.
    // getRawParameterValue on an AudioParameterChoice returns the choice
    // index directly (= the FxType).
    std::array<FxType, 3> types { FxType::None, FxType::None, FxType::None };
    auto& apvts = processor().getApvts();
    constexpr int kLast = static_cast<int> (FxType::Count) - 1;
    for (int s = 0; s < 3; ++s)
    {
        const juce::String id = "fx" + juce::String (s + 1) + "_type";
        if (auto* raw = apvts.getRawParameterValue (id))
            types[(size_t) s] = static_cast<FxType> (
                juce::jlimit (0, kLast, juce::roundToInt (raw->load())));
    }
    return types;
}

void FxMatrixView::refresh()
{
    // Dynamic FX-dest labels: when a slot's FX type changes (a type edit or a
    // part switch, which reloads fx{N}_type) rebuild every row's dest combo
    // to the slots' actual parameter names. The dest combos carry no APVTS
    // attachments, so their item lists + selections are reconciled here every
    // tick regardless of the active-set signature in the base refresh.
    const auto types = currentSlotTypes();
    if (types != lastSlotTypes_)
    {
        lastSlotTypes_ = types;
        for (int i = 0; i < numSlots(); ++i)
            if (auto* combo = rowDestCombo (i))
                rebuildDestItems (*combo, types[0], types[1], types[2]);
    }
    for (int i = 0; i < numSlots(); ++i)
        if (auto* combo = rowDestCombo (i))
            syncDestFromParam (*this, *combo, i);

    MatrixViewBase::refresh();
}
