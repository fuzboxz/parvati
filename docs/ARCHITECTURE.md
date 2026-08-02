# Parvati — Architecture & Developer Guide

This is a map of the codebase for contributors. For the DSP port rationale see
[`DSP_PORT_SPEC.md`](DSP_PORT_SPEC.md); for UI design see
[`UI_MODERNIZATION_PLAN.md`](UI_MODERNIZATION_PLAN.md).

## High-level data flow

```
MIDI in ──► PluginProcessor ──► SynthEngine (juce::Synthesiser)
              │   ▲                   │
              │   │ APVTS bytes       │ 6 Parts (multitimbral)
              ▼   │                   ▼
            APVTS ◄─── byte bridge ──► AmbikaVoice (16) ──► ambika::dsp::Voice
              │                       │  resample 39216 Hz → host rate
              ▼                       ▼
           GUI (PluginEditor)    per-voicecard mono buffers ──► main + 6 aux buses
```

- **`ParvatiAudioProcessor`** (`Source/PluginProcessor.*`) owns the APVTS
  parameter surface, the `SynthEngine`, and the MIDI→parameter map. Every APVTS
  parameter writes its faithful Patch/Part byte into the current Part's voices
  (`syncAllParamsToEngine`).
- **`ParameterLayout`** (`Source/ParameterLayout.*`) is the single source of
  truth: the ordered table of `PatchParamDescriptor`s drives **both** the APVTS
  layout and the GUI control generation, so the two can never drift.
- **`SynthEngine`** (`Source/SynthEngine.*`) is a `juce::Synthesiser` holding 16
  `AmbikaVoice`s split among up to 6 `Part`s, each with its own patch, PartData,
  arpeggiator, sequencer, MIDI channel, key zone and voice allocator.
- **`AmbikaVoice`** (`Source/AmbikaVoice.*`) wraps `ambika::dsp::Voice`, runs the
  analog filter + VCA, oversampling, and Lagrange resampling to the host rate.
- **`ambika::dsp`** (`Source/dsp/`) is the bit-exact firmware port (oscillators,
  envelopes, LFOs, filters, the modulation matrix, fixed-point math).

## Key subsystems

- **Patch I/O** — `Source/PatchFile.*` parses/writes Ambika `.PRO` (program) and
  `.MUL` (multi) RIFF/MBKS containers. `PluginProcessor::saveProgramFile` /
  `loadProgramFile` / `saveMultiFile` / `loadMultiFile` bridge them to/from the
  APVTS. Round-trip correctness is enforced by `tests/roundtrip_test.cpp`.
- **Factory presets** — `Source/ui/FactoryPresetInstaller.*` extracts the
  embedded GPL-3.0 banks (CMake `juce_add_binary_data`) into the user app-data
  dir on first run; the GUI Patch combo reads them
  (`PluginEditor::populateFactoryPatches`).
- **GUI** — `Source/PluginEditor.*` builds a tabbed editor entirely from the
  descriptor table. `ParamPage` partitions controls into bordered
  `GroupComponent`s by param-ID prefix and lays them out as a flexible-width grid
  (`layoutGroups` row-fill + `PageInfo::cols`). `Source/ui/` holds the look &
  feel, theme manager, translations, parameter help, keyboard view, voice meter,
  envelope display and settings panel.
- **MIDI mapping** — `Source/MidiParameterMap.*` reproduces the hardware CC/NRPN
  → parameter mapping.
- **Arp / Sequencer / Transport** — `Source/Arpeggiator.*`, `Source/Sequencer.*`,
  `Source/TransportClock.h`.

## Internal audio rate

The Ambika engine runs at a fixed **39216 Hz** internal sample rate; `AmbikaVoice`
Lagrange-resamples to the host rate. Filter oversampling (1×/2×/4×) oversamples
only the digital filter **model**, not the oscillators, to preserve authenticity.

## Memory model

- Parameter edits go: host/GUI → APVTS → `parameterChanged` (push only) → engine
  bytes on the audio thread. Bulk loads use explicit `syncAllParamsToEngine`.
- Per-voice oversampling changes are staged on the message thread and applied on
  the audio thread (`osFactorDirty_`) to avoid freeing under concurrent use.
- Plugin latency (resampler + active oversampling) is reported via
  `setLatencySamples` for host PDC.

## Tests

`tests/` (16 executables, built by default) + `tools/` (headless probes,
`EXCLUDE_FROM_ALL`). Each targets one subsystem; see `CMakeLists.txt` for the
full list and the `PARVATI_SOURCE_DIR` macro used to locate the vendored
reference presets at test time.
