// Copyright (c) 2024 805LABS / Parvati.
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
// Extract every embedded factory preset into @p factoryDir (*.PRO) and
// @p factoryMultiDir (*.MUL) if either directory is missing or empty. No-op
// once the directories already contain presets. Returns the number of files
// written (0 on a no-op run).
int ensureFactoryPresetsInstalled (const juce::File& factoryDir,
                                   const juce::File& factoryMultiDir);
}  // namespace parvati
