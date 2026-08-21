// UI live-modulation telemetry (engine side) test — the durable regression
// suite for SynthEngine's UI telemetry block (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
//
// The engine publishes, on the audio thread, a seqlock-guarded frame with:
//   * the tracked part's mod-source values + decimated recent history,
//   * the representative voice's envelope stage/progress/level,
//   * the representative voice's EFFECTIVE (modulation-applied) cutoff /
//     resonance / mode,
// and the message thread reads it via readUiTelemetry() with bounded retries.
// resetUiTelemetry() (patch load / part switch / init) invalidates the frame
// through an epoch bump the reader observes immediately.
//
// This stub is being filled in by the engine task; the target
// (parvati_ui_telemetry_test) is registered in CMakeLists.txt.
// Run with: cmake --build build --target parvati_ui_telemetry_test && ./build/parvati_ui_telemetry_test

#include <cstdio>

int main()
{
    std::printf ("ui_telemetry_test: stub (engine task fills this in)\n");
    return 0;
}
