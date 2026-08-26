#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/run_release_goldens.sh — pin the Release-config FX render digests.
# ---------------------------------------------------------------------------
# tests/fx_render_golden_test pins the full-render bytes of both rate-bridge
# families. Debug codegen (build_unified) and Release codegen can contract
# float expressions differently, so each config pins its own digest table.
# The test selects its table by NDEBUG.
#
# This script closes the Release gap:
#   1. Configures the EXISTING canonical build_release dir (see the build
#      policy: no new build dirs) as a TESTS-ONLY Release tree
#      (-DPARVATI_FORMATS= -DPARVATI_BUILD_CLAP=OFF). No plugin bundle is
#      built there; only hellcat_unified_tests.
#   2. Harvests the observed Release digests (HELLCAT_GOLDEN_HARVEST=1:
#      the test prints paste-ready rows, keeps canary/determinism/energy
#      checks, skips only the golden compare).
#   3. Verifies: a normal run of fx_render_golden_test must be green once
#      the harvest rows sit in the releaseScenarios table of the test.
#
# NOTE on build_release: the tests-only configure REPLACES a plugin-format
# configure of that dir. To run the release workflow (deploy) again:
#   cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
#
# Exit codes:
#   0 = Release digests pinned and green.
#   2 = JUCE missing, or configure/build failed.
#   3 = harvest printed rows; the table still needs the paste (see below).
#   4 = the Release render broke determinism/energy/canary — do NOT pin.
#
# Usage:
#   tools/run_release_goldens.sh
#   JUCE=~/JUCE tools/run_release_goldens.sh
#   JOBS=8 tools/run_release_goldens.sh
# ---------------------------------------------------------------------------
set -uo pipefail

JUCE="${JUCE:-$HOME/JUCE}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Linux has no hw.ncpu sysctl; macOS has no nproc. Try both, then fall back.
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
LOGDIR="${TMPDIR:-/tmp}"
LOGTAG="$$"
DIR="$SRC/build_release"
UNIFIED=hellcat_unified_tests
TEST=fx_render_golden_test

if [ ! -f "$JUCE/CMakeLists.txt" ]; then
    echo "JUCE not found at '$JUCE'. Pass JUCE=/path/to/JUCE." >&2
    exit 2
fi

# -DCMAKE_OSX_ARCHITECTURES is an Apple-only configure flag.
os_arch_flag=""
[ "$(uname -s)" = "Darwin" ] && os_arch_flag="-DCMAKE_OSX_ARCHITECTURES=arm64"

echo "=== Hellcat Release golden pin ($DIR, tests-only Release) ==="

echo "--- configuring build_release as tests-only Release ---"
cmake -S "$SRC" -B "$DIR" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release -DJUCE_GLOBAL_PATH="$JUCE" \
    $os_arch_flag \
    -DPARVATI_FORMATS= -DPARVATI_BUILD_CLAP=OFF \
    > "$LOGDIR/hellcat_release_cfg.$LOGTAG.log" 2>&1 \
    || { tail -20 "$LOGDIR/hellcat_release_cfg.$LOGTAG.log"; exit 2; }

echo "--- building $UNIFIED (Release) ---"
cmake --build "$DIR" --target "$UNIFIED" -j "$JOBS" \
    > "$LOGDIR/hellcat_release_build.$LOGTAG.log" 2>&1 \
    || { tail -30 "$LOGDIR/hellcat_release_build.$LOGTAG.log"; exit 2; }

# Harvest: paste-ready rows. Determinism, energy and canary still gate.
harvest_log="$LOGDIR/hellcat_release_harvest.$LOGTAG.log"
echo "--- harvesting Release digests ($TEST) ---"
if ! HELLCAT_GOLDEN_HARVEST=1 "$DIR/$UNIFIED" "$TEST" > "$harvest_log" 2>&1; then
    echo "Harvest run FAILED — the Release render is not safe to pin:" >&2
    grep -m6 -E "FAIL|broken" "$harvest_log" | sed 's/^/    /' >&2
    exit 4
fi
if [ "$(grep -c '^HARVEST' "$harvest_log")" -ne 4 ]; then
    echo "Expected 4 HARVEST rows, got $(grep -c '^HARVEST' "$harvest_log")." >&2
    exit 4
fi

# Verify: the pinned table must make a normal run green.
verify_log="$LOGDIR/hellcat_release_verify.$LOGTAG.log"
if "$DIR/$UNIFIED" "$TEST" > "$verify_log" 2>&1; then
    echo "  RELEASE GOLDENS: PINNED AND GREEN"
    grep '^HARVEST' "$harvest_log" | sed 's/^HARVEST /  pinned: /'
    echo "=== RELEASE GOLDEN PIN: CLEAN ==="
    exit 0
fi

echo "  Release table needs a paste. Paste these rows into the"
echo "  releaseScenarios block of tests/fx_render_golden_test.cpp:"
grep '^HARVEST' "$harvest_log" | sed 's/^HARVEST /        /'
echo "  (then rerun this script — it must report CLEAN)"
echo "=== RELEASE GOLDEN PIN: ROWS NEED PASTE ==="
exit 3
