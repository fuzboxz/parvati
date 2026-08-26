#!/usr/bin/env bash
#
# tools/run_tests_parallel.sh — parallel driver for the unified test binary.
#
#   tools/run_tests_parallel.sh                 # all tests, N lanes (default: P-cores)
#   tools/run_tests_parallel.sh name1 name2     # subset
#   HELLCAT_TEST_JOBS=4 tools/run_tests_parallel.sh
#
# WHY THIS IS SAFE (fork-per-test + one-process-per-lane):
#   * Tests are self-contained (fresh processor per test, no cross-test state)
#     and the binary already runs each test in a fork()ed child.
#   * Every shared mutable path in the suite goes through JUCE
#     File::tempDirectory, which on macOS resolves to
#     $TMPDIR/<exe-name>/  (juce_Files_mac.mm:201). Each lane therefore gets a
#     PRIVATE temp tree via its own TMPDIR, so fixed-name temp files
#     ("b.PRO", "a.yml", ...) cannot collide across concurrent tests.
#   * No test binds sockets/ports or writes outside tempDirectory.
#
# Scheduling is a greedy work queue (xargs -P): known-slow tests
# (loader_fuzz_test ~10 min, perf_smoke_test, lifecycle_test, ...) are fed
# first so the long poles start immediately and don't tail out the run.
#
# Output: per-test logs + a combined summary in the run directory
# (kept for inspection; per-lane TMPDIRs are removed). Exit code = number of
# failed tests, matching the sequential runner's contract.
#
set -uo pipefail

BIN=${HELLCAT_TESTS_BIN:-build_unified/hellcat_unified_tests}
KEEP_TMP=${HELLCAT_TEST_KEEP_TMP:-0}

if [[ ! -x "$BIN" ]]; then
    echo "error: test binary not found/executable: $BIN" >&2
    echo "       build it: cmake --build build_unified --target hellcat_unified_tests -j8" >&2
    exit 2
fi

# Default lane count: performance cores (avoids packing efficiency cores that
# stretch latency-sensitive GUI/audio tests), fall back to nproc, cap at 8 so a
# 16 GB machine never runs 12 heavy DSP/GUI tests at once.
if [[ -z ${HELLCAT_TEST_JOBS:-} ]]; then
    if [[ "$(uname -s)" == "Darwin" ]] && sysctl -n hw.perflevel0.logicalcpu >/dev/null 2>&1; then
        JOBS=$(sysctl -n hw.perflevel0.logicalcpu)
    else
        JOBS=$(nproc 2>/dev/null || echo 4)
    fi
    ((JOBS > 8)) && JOBS=8
    ((JOBS < 1)) && JOBS=1
else
    JOBS=$HELLCAT_TEST_JOBS
fi

RUNROOT=$(mktemp -d "${TMPDIR:-/tmp}/hellcat_ptests.XXXXXX")
LOGDIR="$RUNROOT/logs"
mkdir -p "$LOGDIR"
RESULTS="$RUNROOT/results.tsv"

# --- Build the test list: explicit subset, or all (slow-poles first) --------
# (bash-3.2 compatible: no mapfile — /bin/bash on macOS is 3.2.57)
ALL_TESTS=()
while IFS= read -r line; do
    [[ -n $line ]] && ALL_TESTS+=("$line")
done < <("$BIN" list | sed -n 's/^  //p')

if [[ $# -gt 0 ]]; then
    TESTS=("$@")
    for t in "${TESTS[@]}"; do
        [[ " ${ALL_TESTS[*]} " == *" $t "* ]] || { echo "error: unknown test: $t" >&2; exit 2; }
    done
else
    # loader_fuzz_test is SKIPPED by default (2026-08-22, user call): it is a
    # ~9-minute fuzz soak that gates the wall time of the whole suite while
    # rarely being the thing under test. Run it explicitly when the DSP
    # changes:
    #   ./build_unified/hellcat_unified_tests loader_fuzz_test
    #   tools/run_tests_parallel.sh loader_fuzz_test
    # (Passing it as an explicit arg still works — the exclusion below only
    # applies to the no-args default roster.)
    SKIP=(loader_fuzz_test)
    # Long poles first so the greedy queue starts them immediately.
    SLOW=(perf_smoke_test lifecycle_test render_quality_test hellcat_clouds_fx_test)
    TESTS=()
    for s in "${SLOW[@]}"; do
        [[ " ${ALL_TESTS[*]} " == *" $s "* ]] && TESTS+=("$s")
    done
    for t in "${ALL_TESTS[@]}"; do
        do_skip=0
        for x in "${SKIP[@]}"; do [[ $t == "$x" ]] && do_skip=1; done
        [[ $do_skip -eq 1 ]] && continue
        [[ " ${TESTS[*]} " == *" $t "* ]] || TESTS+=("$t")
    done
    echo "skipped (run explicitly when the DSP changes): ${SKIP[*]}"
fi

echo "hellcat parallel test runner"
echo "  binary : $BIN"
echo "  tests  : ${#TESTS[@]}"
echo "  lanes  : $JOBS"
echo "  run dir: $RUNROOT"
echo

export BIN LOGDIR RESULTS RUNROOT KEEP_TMP

run_one() {
    local name=$1
    local t0 t1 rc lane_tmp
    t0=$(date +%s)
    lane_tmp=$(mktemp -d "$RUNROOT/tmp.XXXXXX")
    # Private TMPDIR => JUCE tempDirectory ($TMPDIR/<exe>/) is lane-private.
    TMPDIR="$lane_tmp" "$BIN" "$name" >"$LOGDIR/$name.log" 2>&1
    rc=$?
    t1=$(date +%s)
    printf '%s\t%s\t%s\n' "$rc" "$((t1 - t0))" "$name" >>"$RESULTS"
    if [[ $KEEP_TMP != 1 ]]; then rm -rf "$lane_tmp"; fi
    # Progress line for the terminal (per-test detail is in the log).
    printf '[%4ds] %s: %s\n' "$((t1 - t0))" "$name" "$([[ $rc -eq 0 ]] && echo PASS || echo "FAIL (exit $rc)")"
    return 0  # never fail xargs mid-queue; rc is recorded per test
}
export -f run_one

START=$(date +%s)
printf '%s\n' "${TESTS[@]}" | xargs -P "$JOBS" -n 1 bash -c 'run_one "$0"'
XARGS_RC=${PIPESTATUS[1]}
END=$(date +%s)

# --- Summary ----------------------------------------------------------------
echo
echo "========================================"
echo "Test Summary (parallel: $JOBS lanes, $((END - START))s wall)"
echo "========================================"
if [[ ! -s $RESULTS ]]; then
    echo "error: no results recorded (xargs rc=$XARGS_RC)" >&2
    rm -rf "$RUNROOT"
    exit 2
fi

PASS=$(awk -F'\t' '$1 == 0' "$RESULTS" | wc -l | tr -d ' ')
FAIL=$(awk -F'\t' '$1 != 0' "$RESULTS" | wc -l | tr -d ' ')
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo "  Total:  $((PASS + FAIL))"

if [[ $FAIL -gt 0 ]]; then
    echo
    echo "Failures (exit-code  duration  test):"
    awk -F'\t' '$1 != 0 { printf "  exit %-3s %5ds  %s\n", $1, $2, $3 }' "$RESULTS"
    echo
    echo "Failing-test logs (last 15 lines each):"
    for name in $(awk -F'\t' '$1 != 0 { print $3 }' "$RESULTS"); do
        echo "--- $LOGDIR/$name.log ---"
        tail -15 "$LOGDIR/$name.log"
    done
fi

echo
echo "Per-test logs: $LOGDIR"
if [[ $KEEP_TMP == 1 ]]; then echo "Lane TMPDIRs kept under: $RUNROOT"; fi

exit "$FAIL"
