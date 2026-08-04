#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/run_sanitizers.sh — Parvati dynamic memory-safety + concurrency checker.
# ---------------------------------------------------------------------------
# Builds the full test suite under AddressSanitizer+UBSan and ThreadSanitizer and
# runs every test, surfacing:
#   * ASan  — heap/stack/global buffer overflows, use-after-free, double-free.
#   * UBSan — signed-integer overflow, out-of-bounds array indexing, null
#             deref, misaligned access (the class of bug that bit the wavequence
#             and vowel oscillators: faithful-port code that is only "safe" on
#             bare-metal AVR's flat PROGMEM).
#   * TSan  — message<->audio-thread data races (the plugin's real threading).
#
# Data races are timing-dependent, so the concurrency test is run REPEAT times.
# (ASan and TSan are mutually exclusive -> two separate build dirs.)
#
# Usage:
#   tools/run_sanitizers.sh [REPEAT]      # REPEAT defaults to 3
#   JUCE=~/JUCE tools/run_sanitizers.sh   # override the JUCE checkout
#   SKIP_TSAN=1 tools/run_sanitizers.sh   # ASan+UBSan only
#
# Exits non-zero if any sanitizer reports a finding or any test fails.
# ---------------------------------------------------------------------------
set -uo pipefail

REPEAT="${1:-3}"
JUCE="${JUCE:-$HOME/JUCE}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
fail=0

if [ ! -f "$JUCE/CMakeLists.txt" ]; then
    echo "JUCE not found at '$JUCE'. Pass JUCE=/path/to/JUCE." >&2
    exit 2
fi

# Run every test binary in a build dir. $1=build dir, $2=label, $3=repeats(1).
run_suite () {
    local dir="$1" label="$2" reps="${3:-1}"
    local pass=0 ran=0
    for t in "$dir"/parvati_*_test "$dir"/parvati_tests "$dir"/parvati_tests.exe "$dir"/parvati_*_test.exe; do
        [ -e "$t" ] || continue
        local name; name="$(basename "$t")"
        local r
        for ((r=0; r<reps; ++r)); do
            ran=$((ran+1))
            if "$t" > "/tmp/parvati_san_${name}_${r}.log" 2>&1; then
                pass=$((pass+1))
            else
                echo "    FAIL: $name (run $((r+1))/$reps) — see /tmp/parvati_san_${name}_${r}.log"
                # Surface the sanitizer summary / first finding.
                grep -m3 -iE "runtime error|ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:" "/tmp/parvati_san_${name}_${r}.log" 2>/dev/null | sed 's/^/        /'
                fail=1
            fi
        done
    done
    echo "  [$label] $pass/$ran test runs passed"
}

echo "=== Parvati sanitizer sweep (concurrency x${REPEAT}) ==="
echo "  JUCE=$JUCE  jobs=$JOBS"

# --- ASan + UBSan ----------------------------------------------------------
echo ""
echo "--- configuring + building ASan+UBSan (build_san_asan) ---"
cmake -S "$SRC" -B "$SRC/build_san_asan" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Debug -DJUCE_GLOBAL_PATH="$JUCE" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON > /tmp/parvati_cmake_asan.log 2>&1 || { tail -20 /tmp/parvati_cmake_asan.log; exit 2; }
cmake --build "$SRC/build_san_asan" -j "$JOBS" > /tmp/parvati_build_asan.log 2>&1 || { tail -30 /tmp/parvati_build_asan.log; exit 2; }
echo "  running suite under ASan+UBSan..."
ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:halt_on_error=1" \
UBSAN_OPTIONS="abort_on_error=1:halt_on_error=1:print_stacktrace=1" \
    run_suite "$SRC/build_san_asan" "ASan+UBSan"

# --- TSan (races; concurrency test repeated) -------------------------------
if [ "${SKIP_TSAN:-0}" != "1" ]; then
    echo ""
    echo "--- configuring + building TSan (build_san_tsan) ---"
    cmake -S "$SRC" -B "$SRC/build_san_tsan" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Debug -DJUCE_GLOBAL_PATH="$JUCE" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DPARVATI_ENABLE_TSAN=ON > /tmp/parvati_cmake_tsan.log 2>&1 || { tail -20 /tmp/parvati_cmake_tsan.log; exit 2; }
    cmake --build "$SRC/build_san_tsan" -j "$JOBS" > /tmp/parvati_build_tsan.log 2>&1 || { tail -30 /tmp/parvati_build_tsan.log; exit 2; }

    echo "  running suite under TSan (concurrency_test x${REPEAT})..."
    # Non-concurrency tests: once each.
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
        run_suite "$SRC/build_san_tsan" "TSan" 1

    # The concurrency test specifically: repeated (races are timing-dependent).
    echo "  repeating parvati_concurrency_test under TSan x${REPEAT}..."
    for ((i=1; i<=REPEAT; ++i)); do
        if TSAN_OPTIONS="halt_on_error=1" "$SRC/build_san_tsan/parvati_concurrency_test" > "/tmp/parvati_san_conc_${i}.log" 2>&1; then
            echo "    concurrency_test TSan run $i/$REPEAT: PASS"
        else
            echo "    concurrency_test TSan run $i/$REPEAT: FAIL"
            grep -m3 -iE "WARNING: ThreadSanitizer|SUMMARY:" "/tmp/parvati_san_conc_${i}.log" | sed 's/^/        /'
            fail=1
        fi
    done
fi

echo ""
if [ "$fail" -eq 0 ]; then
    echo "=== SANITIZER SWEEP: ALL CLEAN ==="
else
    echo "=== SANITIZER SWEEP: FAILURES (see /tmp/parvati_san_*.log) ==="
fi
exit "$fail"
