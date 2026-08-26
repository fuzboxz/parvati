// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.  See ModSourceCatalog.h.

#include "ModSourceCatalog.h"

#include <cassert>

namespace hellcat
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

std::optional<Cluster> clusterForSourceName (const juce::String& name)
{
    // Prefix matching (the historical matrix/tint test, kept verbatim so
    // every displayed name keeps its exact colour). "Wheel" also covers
    // "Wheel 2"; "LFO" misses "Voice LFO", so that exact name is listed.
    if (name.startsWith ("Env"))                                     return Cluster::Env;
    if (name.startsWith ("LFO") || name == "Voice LFO")              return Cluster::Lfo;
    if (name.startsWith ("Seq") || name.startsWith ("Arp"))          return Cluster::SeqArp;
    if (name.startsWith ("Op"))                                      return Cluster::Mod;
    if (name.startsWith ("Const"))                                   return Cluster::Const;
    if (name == "Velocity" || name == "Aftertouch" || name == "Pitch Bend"
        || name.startsWith ("Wheel") || name == "Expression" || name == "Note")
                                                                       return Cluster::Perf;
    if (name == "Gate" || name == "Noise" || name == "Random")       return Cluster::Util;
    return std::nullopt;
}

juce::Colour clusterAccent (Cluster c, const HellcatTheme& theme)
{
    // Y2K: every indicator and mod pill carries ONE accent — the LCD green
    // (theme.accentPrimary, the calm #3FBF3F). The category rainbow stays
    // off the indicators; other themes keep their per-cluster colours.
    if (theme.name == "Y2K")
        return theme.accentPrimary;
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

}  // namespace hellcat
