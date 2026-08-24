// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ModMatrixView.h.

#include "ModMatrixView.h"

#include "ModDestMap.h"            // kFxModDstOffset (synth rejects the FX dest domain)
#include "ModMatrixHighlight.h"   // onAssignRequest bus (drag-and-drop -> mod slot)
#include "PluginProcessor.h"      // ParvatiAudioProcessor (complete type)
#include "ThemeManager.h"

#include "dsp/patch.h"            // ambika::dsp::kNumModulations, MOD_SRC_*, MOD_DST_*

// The view assumes the engine's 14-slot mod matrix. If that ever changes, the
// 14-row UI must be revisited — this trips the build early.
static_assert (ambika::dsp::kNumModulations == 14, "ModMatrixView assumes a 14-slot mod matrix");

//==============================================================================
namespace
{
// The synth-matrix configuration: every structural difference from the FX
// twin in one table (see MatrixViewConfig). TODAY's values; a shared fix
// lands in MatrixViewBase and applies to both views at once.
MatrixViewConfig makeSynthConfig()
{
    MatrixViewConfig cfg;
    cfg.paramPrefix      = "mod";
    cfg.numSlots         = ambika::dsp::kNumModulations;   // 14
    cfg.usedSuffixKey    = "of 14 Used";
    cfg.matrixFullKey    = "Matrix Full (14/14)";
    cfg.rowA11yPrefix    = "Mod ";
    cfg.destComboAttached   = true;    // dest combo loads its choices from the APVTS param
    cfg.comboShrinkFallback = true;   // narrow rows squeeze the source combo first (44pt floors)
    cfg.highlightSelfOnHover = false;  // the bus round-trip highlights the hovered row
    cfg.lampDiameter            = 18.0f;   // the FX-card enable/disable dot size
    cfg.lampCarriesCategoryColour = true; // lamp ON == the row's modulator category colour
    cfg.destBusOffset    = 0;                          // synth dests broadcast raw
    cfg.rejectDestAtOrAbove = parvati::ModDestMap::kFxModDstOffset;   // synth rejects FX dests
    cfg.addDefaultSource = ambika::dsp::MOD_SRC_ENV_1;
    cfg.addDefaultDest   = ambika::dsp::MOD_DST_FILTER_CUTOFF;   // a visible, classic routing
    return cfg;
}
}  // namespace

//==============================================================================
ModMatrixView::ModMatrixView (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : MatrixViewBase (processor, themeManager, makeSynthConfig())
{
    refresh();

    // Drag-drop assign handler: a pill dropped on a synth destination knob
    // assigns the next free slot. FX-domain dests never reach here — the
    // base's rejectDestAtOrAbove guard sends them back for the FX handler.
    juce::Component::SafePointer<ModMatrixView> safe (this);
    assignSub_ = parvati::ModMatrixHighlight::instance().onAssignRequest (
        [safe] (int source, int dest) -> bool { return safe != nullptr && safe->assignNextFreeSlot (source, dest); });
}

//==============================================================================
juce::String ModMatrixView::slotParam (int slot, const char* suffix)
{
    return "mod" + juce::String (slot + 1) + suffix;
}

juce::Component* ModMatrixView::rowForSlotForTest (int slot) const
{
    slot = juce::jlimit (0, 13, slot);
    return rowAtOrNull (slot);
}

juce::Colour ModMatrixView::rowCategoryColourForTest (int slot) const
{
    return parvati::matrixview::rowCategoryColour (themeManager().getCurrentTheme(),
                                                    sourceNameForSlot (slot));
}
