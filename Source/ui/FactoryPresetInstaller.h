// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Factory preset installer. The GPL-3.0 Ambika "goldencard" factory banks are
// EMBEDDED into the plugin binary at build time (CMake juce_add_binary_data,
// namespace FactoryPresets) so a freshly-installed Parvati ships with patches
// out of the box. On first run this extracts them from the embedded resources
// into the user app-data Factory/ (.PRO) and FactoryMulti/ (.MUL) directories,
// which the Patch combo reads (PluginEditor::populateFactoryPatches).
//
// Idempotent + process-once: extraction only runs if a target directory is
// missing or empty, and a static flag guarantees the directory check happens at
// most once per process (tests create many processors). Safe to call from the
// message thread (AudioProcessor construction); all failures are non-fatal.

#pragma once

#include <juce_core/juce_core.h>

namespace parvati
{
// Extract every embedded factory preset into @p factoryDir (*.PRO, organized as
// FACTORY/<bank>/), @p factoryMultiDir (*.MUL, FACTORY_MULTI/), and @p templatesDir
// (full-fidelity *.parvati multis, TEMPLATES/), and make sure @p userDir (USER/)
// exists for user saves. Factory banks are written only if missing; the stock
// TEMPLATES set is SYNCED every run (writes new/changed templates and removes a
// local *.parvati no longer in the embedded set) so renames/removals propagate
// to an already-installed tree. Returns the number of files (re)written.
int ensureFactoryPresetsInstalled (const juce::File& factoryDir,
                                   const juce::File& factoryMultiDir,
                                   const juce::File& templatesDir,
                                   const juce::File& userDir);

// Bump when the EMBEDDED factory content changes (new/renamed .PRO/.MUL, edited
// templates). A bump invalidates the on-disk install marker so every already-
// installed tree re-runs the full (self-healing) extraction pass on next launch;
// an unchanged version takes the marker fast path (F-ios-perf-5: one marker stat
// instead of ~365 per-resource stats at every process start, which was
// measurable inside iOS AUv3 instantiate).
constexpr int kFactoryInstallVersion = 1;

// Test-only: forget the process-once install guard so a single test binary can
// exercise BOTH the first-run (full pass) and the later-run (marker fast
// path) behaviours. Single-threaded use only (tests). Resets nothing on disk.
void resetInstallOnceForTest();
}  // namespace parvati
