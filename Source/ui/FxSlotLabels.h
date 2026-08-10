// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxSlotLabels — declarations for the per-FX-type active-parameter count and
// semantic short parameter labels. The definitions live in FxSlotCard.cpp (they
// are the source of truth for the live param-knob labels on an FX-slot card).
// This tiny dependency-free shard exposes them to FxMatrixView so the FX
// mod-matrix's destination combo can show each slot's ACTUAL parameter names
// (e.g. "FX1 Position/Size/Pitch/Freeze" for a Looping Delay) instead of the
// static "FX{N} Param K" labels — without FxMatrixView having to pull in all of
// FxSlotCard.h (ParamControl / the editor).

#pragma once

#include "dsp/fx/FxTypes.h"   // FxType

// Number of active (visible) generic params for an effect type (0..4). idx
// 0..activeParamCount-1 are real; idx >= activeParamCount are inactive/hidden.
int activeParamCount (FxType t) noexcept;

// Semantic short label for generic param @p idx (0..3) of effect type @p t
// ("Amount", "Position", "Pitch", ...). Returns "-" for an inactive idx.
const char* paramLabel (FxType t, int idx) noexcept;
