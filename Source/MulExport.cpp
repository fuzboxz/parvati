// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// MulExport implementation — see MulExport.h for the design notes.

#include "MulExport.h"

#include "VoiceMasks.h"    // hellcat::popcount8 (bitmask slot counting)

#include <algorithm>
#include <string>

namespace hellcat::mul_export
{
namespace
{
// Contiguous bitmasks from per-Part card counts (part p takes the next
// counts[p] cards, in Part order).
std::array<uint8_t, kParts> masksFromCounts (const std::array<int, kParts>& counts)
{
    std::array<uint8_t, kParts> masks {};
    int cursor = 0;
    for (int p = 0; p < kParts; ++p)
    {
        for (int c = 0; c < counts[(size_t) p] && cursor < kParts; ++c, ++cursor)
            masks[(size_t) p] = static_cast<uint8_t> (masks[(size_t) p] | (1u << cursor));
    }
    return masks;
}

Solution finish (const Setup& s, const std::array<int, kParts>& counts,
                 bool foldToMono)
{
    Solution sol;
    sol.masks = masksFromCounts (counts);
    sol.polyMode = s.polyMode;
    for (int p = 0; p < kParts; ++p)
    {
        // MonoFold: a Part that ended up with fewer voices than it requested
        // folds to MONO — all its cards fire the (single) note, trading
        // polyphony for the fuller unison character.
        if (foldToMono && s.active[(size_t) p] && counts[(size_t) p] < s.requested[(size_t) p]
            && s.polyMode[(size_t) p] != 0 /*already MONO*/)
        {
            sol.polyMode[(size_t) p] = 0;
            sol.polyOverridden[(size_t) p] = true;
        }
    }
    return sol;
}

// Largest-remainder split of the 6 cards weighted by each active Part's
// request, with a guaranteed minimum of one card per active Part (when there
// are <= 6 active Parts, which the hardware guarantees).
std::array<int, kParts> proportionalCounts (const Setup& s)
{
    std::array<int, kParts> counts {};
    int sumR = 0;
    for (int p = 0; p < kParts; ++p)
        if (s.active[(size_t) p]) sumR += s.requested[(size_t) p];

    if (sumR <= kParts)
    {
        // The requests already fit the hardware: hand each Part its request.
        for (int p = 0; p < kParts; ++p)
            if (s.active[(size_t) p]) counts[(size_t) p] = s.requested[(size_t) p];
        return counts;
    }

    // Largest remainder: integer quota first, then the leftover cards go to
    // the largest fractional remainders (ties -> the lower Part index).
    std::array<double, kParts> rem {};
    int used = 0;
    for (int p = 0; p < kParts; ++p)
    {
        if (! s.active[(size_t) p]) continue;
        const double quota = 6.0 * s.requested[(size_t) p] / sumR;
        counts[(size_t) p] = static_cast<int> (quota);
        rem[(size_t) p] = quota - counts[(size_t) p];
        used += counts[(size_t) p];
    }
    int leftover = kParts - used;
    while (leftover > 0)
    {
        int best = -1;
        for (int p = 0; p < kParts; ++p)
            if (s.active[(size_t) p] && (best < 0 || rem[(size_t) p] > rem[(size_t) best]))
                best = p;
        ++counts[(size_t) best];
        rem[(size_t) best] = -1.0;   // spent (also outranks nothing else now)
        --leftover;
    }
    // Minimum 1 per active Part: steal from the richest Part with > 1 card.
    for (int p = 0; p < kParts; ++p)
    {
        if (! s.active[(size_t) p] || counts[(size_t) p] > 0) continue;
        int rich = -1;
        for (int q = 0; q < kParts; ++q)
            if (counts[(size_t) q] > 1 && (rich < 0 || counts[(size_t) q] > counts[(size_t) rich]))
                rich = q;
        if (rich < 0) break;   // nothing to steal (cannot happen with <= 6 active)
        --counts[(size_t) rich];
        counts[(size_t) p] = 1;
    }
    return counts;
}
}  // namespace

bool needsFallback (const Setup& s)
{
    for (int p = 0; p < kParts; ++p)
        if (s.active[(size_t) p] && s.requested[(size_t) p] > s.cards[(size_t) p])
            return true;
    return false;
}

std::array<uint8_t, kParts> deriveMasks (const std::array<int, kParts>& requested)
{
    // Reuse the solver's own proportional path: a Setup whose `active` flags
    // come from the requested counts themselves (count > 0 => active) drives
    // proportionalCounts, and masksFromCounts packs the cards contiguously.
    Setup s;
    s.requested = requested;
    for (int p = 0; p < kParts; ++p)
        s.active[(size_t) p] = requested[(size_t) p] > 0;
    return masksFromCounts (proportionalCounts (s));
}

Solution solve (const Setup& s, Strategy strategy)
{
    switch (strategy)
    {
        case Strategy::AsIs:
        {
            // The engine's own card counts, unchanged (slots silently ignored
            // — the pre-extension behaviour). No mode rewrites.
            return finish (s, s.cards, false);
        }
        case Strategy::Proportional:
            return finish (s, proportionalCounts (s), false);
        case Strategy::MonoFold:
            return finish (s, proportionalCounts (s), true);
        case Strategy::Priority:
        {
            // First-wins in Part order: each Part gets its full request until
            // the cards run out (the shape of firmware Multi::AssignVoices,
            // which honours Parts in order).
            std::array<int, kParts> counts {};
            int remaining = kParts;
            for (int p = 0; p < kParts; ++p)
            {
                if (! s.active[(size_t) p]) continue;
                counts[(size_t) p] = std::min (s.requested[(size_t) p], remaining);
                remaining -= counts[(size_t) p];
            }
            return finish (s, counts, false);
        }
        case Strategy::EvenSplit:
        {
            // Equal share per active Part, capped by each Part's request; the
            // cards freed by the caps are redistributed round-robin (lowest
            // Part first) to Parts that can use more.
            int active = 0;
            for (int p = 0; p < kParts; ++p)
                if (s.active[(size_t) p]) ++active;
            std::array<int, kParts> counts {};
            const int share = kParts / std::max (1, active);
            int extra = kParts % std::max (1, active);
            int used = 0;
            for (int p = 0; p < kParts; ++p)
            {
                if (! s.active[(size_t) p]) continue;
                counts[(size_t) p] = std::min (s.requested[(size_t) p], share + (extra > 0 ? 1 : 0));
                if (extra > 0 && counts[(size_t) p] >= share + 1) --extra;   // only spend the bonus if it fit
                used += counts[(size_t) p];
            }
            int leftover = kParts - used;
            while (leftover > 0)
            {
                bool gave = false;
                for (int p = 0; p < kParts && leftover > 0; ++p)
                    if (s.active[(size_t) p] && counts[(size_t) p] < s.requested[(size_t) p])
                    {
                        ++counts[(size_t) p];
                        --leftover;
                        gave = true;
                    }
                if (! gave) break;   // nobody can take more
            }
            return finish (s, counts, false);
        }
        case Strategy::ChainSplit:
        default:
        {
            // Single-file degrade of the chain strategy: just the first unit
            // (solveChain owns the real packing).
            const auto units = solveChain (s);
            return units.empty() ? finish (s, s.cards, false) : units.front();
        }
    }
}

std::vector<Solution> solveChain (const Setup& s)
{
    // Sequential packing: each Part's request is split into segments of at
    // most (the unit's remaining) cards. A Part that continues on a later unit
    // gets CHAIN on every non-final segment (the firmware forwards that
    // Part's overflow to the next unit, which matches it on channel +
    // key zone). The final segment keeps the original mode.
    std::vector<std::array<int, kParts>> units (1);
    int remaining = kParts;

    for (int p = 0; p < kParts; ++p)
    {
        if (! s.active[(size_t) p]) continue;
        int r = s.requested[(size_t) p];
        while (r > 0)
        {
            if (remaining == 0)
            {
                units.emplace_back();
                remaining = kParts;
            }
            const int take = std::min (r, remaining);
            units.back()[(size_t) p] += take;
            remaining -= take;
            r -= take;
        }
    }

    // Build a Solution per unit: segments that CONTINUE (this unit's count for
    // the Part is a non-final piece of its request) are CHAIN. A Part's final
    // piece keeps its original mode. Track how much of each Part's request has
    // been placed so far to know which segments are final.
    std::vector<Solution> out;
    std::array<int, kParts> placed {};
    for (const auto& u : units)
    {
        Solution sol;
        sol.masks = masksFromCounts (u);
        sol.polyMode = s.polyMode;
        for (int p = 0; p < kParts; ++p)
        {
            placed[(size_t) p] += u[(size_t) p];
            if (u[(size_t) p] > 0 && placed[(size_t) p] < s.requested[(size_t) p]
                && s.polyMode[(size_t) p] != 4 /*already CHAIN*/)
            {
                sol.polyMode[(size_t) p] = 4;
                sol.polyOverridden[(size_t) p] = true;
            }
        }
        out.push_back (sol);
    }
    return out;
}

std::vector<std::string> previewLines (const Setup& s, const Solution& sol,
                                       int unitIndex, const PreviewContext* ctx)
{
    static const char* kModeNames[] = { "Mono", "Poly", "Unison 2x", "Cyclic", "Chain" };
    std::vector<std::string> lines;
    for (int p = 0; p < kParts; ++p)
    {
        if (! s.active[(size_t) p]) continue;
        const int got = popcount8 (sol.masks[(size_t) p]);
        if (unitIndex > 0 && got == 0)
            continue;   // this Part has no segment on this unit

        std::string name = (ctx != nullptr && (size_t) p < ctx->names.size()
                                && ! ctx->names[(size_t) p].empty())
                               ? ctx->names[(size_t) p]
                               : "Part " + std::to_string (p + 1);
        std::string line = name + ": " + std::to_string (s.requested[(size_t) p])
                           + " -> " + std::to_string (got)
                           + (got == 1 ? " voice" : " voices");
        line += " (";
        line += kModeNames[sol.polyMode[(size_t) p] % 5];
        if (sol.polyOverridden[(size_t) p])
            line += ", switched";
        line += ")";
        lines.push_back (std::move (line));
    }
    return lines;
}

std::string summarize (const Setup& s, Strategy strategy)
{
    int requested = 0;
    for (int p = 0; p < kParts; ++p)
        if (s.active[(size_t) p]) requested += s.requested[(size_t) p];

    if (strategy == Strategy::ChainSplit)
    {
        const auto units = solveChain (s);
        if (units.size() <= 1)
            return "Fits on one Ambika. All " + std::to_string (requested)
                   + " voices are kept.";
        return "Needs " + std::to_string (units.size())
               + " chained Ambikas (connect them by MIDI, load one file into "
                 "each). All " + std::to_string (requested) + " voices are kept.";
    }

    std::string out = "Fits on one Ambika. ";
    if (requested > kParts)
        out += "Only 6 of your " + std::to_string (requested)
               + " voices will play at once on the hardware.";
    else
        out += "All " + std::to_string (requested) + " voices are kept.";
    return out;
}

}  // namespace hellcat::mul_export
