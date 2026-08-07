// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ModSourceCatalog.h.

#include "ModSourceCatalog.h"

#include <cassert>

namespace parvati
{

const SourceEntry& entryFor (int modSrcEnum)
{
    // The array is in cluster order, not enum order, so a linear scan is the
    // correct (and trivially cheap) lookup for a 32-entry catalogue. The scan
    // matches BOTH real MOD_SRC_* values (>= 0) and the bar-only Note Sequencer
    // sentinel (kNoteSeqSentinel == -1), so entryFor() is safe to call with the
    // sentinel from any generator-vs-dragOnly check.
    for (const auto& e : kAllSources)
        if (e.enumValue == modSrcEnum)
            return e;

    // Out of range: a programming error. Range-check at call sites if needed.
    assert (false && "ModSourceCatalog::entryFor: modSrcEnum out of range");
    return kAllSources.front();
}

juce::Colour clusterAccent (Cluster c, const ParvatiTheme& theme)
{
    switch (c)
    {
        case Cluster::Env:    return theme.catEnv;
        case Cluster::Lfo:    return theme.catLfo;
        case Cluster::SeqArp: return theme.catSeq;
        case Cluster::Perf:   return theme.catPerf;
        case Cluster::Util:   return theme.catUtil;
        case Cluster::Mod:    return theme.catMod;
        case Cluster::Const:  return theme.catConst;
    }

    // Defensive default (silences -Wreturn-type on some toolchains); unreachable.
    return theme.catAudio;
}

const std::array<Cluster, 7>& clustersInOrder()
{
    static constexpr std::array<Cluster, 7> kClustersInOrder =
    {{
        Cluster::Env, Cluster::Lfo, Cluster::SeqArp, Cluster::Mod,
        Cluster::Perf, Cluster::Util, Cluster::Const
    }};
    return kClustersInOrder;
}

}  // namespace parvati
