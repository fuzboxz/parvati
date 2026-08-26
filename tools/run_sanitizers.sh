#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/run_sanitizers.sh — Hellcat dynamic memory-safety + concurrency checker.
# ---------------------------------------------------------------------------
# Builds the unified test binary (hellcat_unified_tests; one binary, every test
# fork-isolated — see tests/unified_test_runner.h) under AddressSanitizer+UBSan
# and ThreadSanitizer and runs the requested tests (default: all), surfacing:
#   * ASan  — heap/stack/global buffer overflows, use-after-free, double-free.
#   * UBSan — signed-integer overflow, out-of-bounds array indexing, null
#             deref, misaligned access (the class of bug that bit the wavequence
#             and vowel oscillators: faithful-port code that is only "safe" on
#             bare-metal AVR's flat PROGMEM).
#   * TSan  — message<->audio-thread data races (the plugin's real threading).
#
# A full sweep runs every test (loader_fuzz_test alone is ~10 min native and
# several times slower under sanitizers) — expect the better part of an hour
# per config. Pass exact test names to iterate on a subset.
#
# Sanitizer build dirs are TESTS-ONLY (-DPARVATI_FORMATS= -DPARVATI_BUILD_CLAP=OFF):
# no AU/VST3/CLAP/Standalone bundles are built — only the unified test binary
# ever runs under sanitizers, so the dirs stay a fraction of the size (the
# 2026-08-22 pre-rework dirs were 9.6G + 4.7G building full instrumented
# plugin bundles that nothing ever executed).
#
# Data races are timing-dependent, so concurrency_test is additionally re-run
# REPEAT times under TSan (env, default 3).
#
# Usage:
#   tools/run_sanitizers.sh                        # full sweep, both configs
#   tools/run_sanitizers.sh envelope_test          # only these tests, both
#   tools/run_sanitizers.sh envelope_test arp_test # configs (exact names;
#                                                  # see '<binary> list')
#   REPEAT=5 tools/run_sanitizers.sh               # extra concurrency repeats
#   JUCE=~/JUCE tools/run_sanitizers.sh            # override JUCE checkout
#   SKIP_TSAN=1 tools/run_sanitizers.sh            # ASan+UBSan only
#
# History note: before the 2026-08-22 test unification this script globbed the
# per-test hellcat_*_test binaries; those targets no longer exist, which had
# silently turned the sweep into a 0/0-runs "pass" (or stale-binary runs).
#
# Exits non-zero if any sanitizer reports a finding or any test fails.
# ---------------------------------------------------------------------------
set -uo pipefail

JUCE="${JUCE:-$HOME/JUCE}"
REPEAT="${REPEAT:-3}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Linux has no hw.ncpu sysctl; macOS has no nproc. Try both, then fall back.
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
# Per-invocation log names under TMPDIR: two CI users on one host must not
# overwrite each other's /tmp logs.
LOGDIR="${TMPDIR:-/tmp}"
LOGTAG="$$"
UNIFIED=hellcat_unified_tests
fail=0

if [ ! -f "$JUCE/CMakeLists.txt" ]; then
    echo "JUCE not found at '$JUCE'. Pass JUCE=/path/to/JUCE." >&2
    exit 2
fi

# Reject unknown test names up front against the runner registry, so a typo
# cannot silently fall through to a "0 tests" success. $1=binary, rest=names.
validate_names () {
    local bin="$1"; shift
    local known unknown=""
    known="$("$bin" list 2>/dev/null | awk 'NR > 1 { print $1 }')" || true
    for name in "$@"; do
        if ! printf '%s\n' "$known" | grep -qx -- "$name"; then
            unknown="$unknown $name"
        fi
    done
    if [ -n "$unknown" ]; then
        echo "Unknown test name(s):$unknown" >&2
        echo "Run '$bin list' for the registered tests." >&2
        exit 2
    fi
}

# Configure + build the unified test target in $1=build dir with $2=label and
# the remaining sanitizer flags. Tests-only configure (-DPARVATI_FORMATS=):
# sanitizer dirs build no plugin-format bundles (see header note).
build_config () {
    local dir="$1" label="$2"; shift 2
    # -DCMAKE_OSX_ARCHITECTURES is an Apple-only configure flag.
    local os_arch_flag=""
    [ "$(uname -s)" = "Darwin" ] && os_arch_flag="-DCMAKE_OSX_ARCHITECTURES=arm64"
    echo "--- configuring + building $label ($dir) ---"
    cmake -S "$SRC" -B "$dir" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Debug -DJUCE_GLOBAL_PATH="$JUCE" \
        $os_arch_flag \
        -DPARVATI_FORMATS= -DPARVATI_BUILD_CLAP=OFF \
        "$@" \
        > "$LOGDIR/hellcat_cmake_$(basename "$dir").$LOGTAG.log" 2>&1 \
        || { tail -20 "$LOGDIR/hellcat_cmake_$(basename "$dir").$LOGTAG.log"; exit 2; }
    cmake --build "$dir" --target "$UNIFIED" -j "$JOBS" \
        > "$LOGDIR/hellcat_build_$(basename "$dir").$LOGTAG.log" 2>&1 \
        || { tail -30 "$LOGDIR/hellcat_build_$(basename "$dir").$LOGTAG.log"; exit 2; }
}

# Run the unified binary in $1=build dir under $2=label. Any remaining args are
# exact test names (default: the whole suite). Sanitizer env must be set by the
# caller so both configs share this path.
run_suite () {
    local dir="$1" label="$2"; shift 2
    local log="$LOGDIR/hellcat_san_${label}.$LOGTAG.log"
    local what="all tests"
    if [ $# -gt 0 ]; then what="test(s): $*"; fi
    echo "  running $what under $label..."
    if "$dir/$UNIFIED" "$@" > "$log" 2>&1; then
        echo "  [$label] PASS ($what) — log: $log"
    else
        echo "  [$label] FAIL ($what) — log: $log"
        # Surface the sanitizer summary / first findings / failing tests.
        grep -m6 -iE "runtime error|ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:|^FAIL:" \
            "$log" 2>/dev/null | sed 's/^/        /'
        fail=1
    fi
}

echo "=== Hellcat sanitizer sweep (concurrency_test x${REPEAT} under TSan) ==="
echo "  JUCE=$JUCE  jobs=$JOBS  tests=${*:-all}"

# --- ASan + UBSan ----------------------------------------------------------
build_config "$SRC/build_san_asan" "ASan+UBSan" \
    -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON
if [ $# -gt 0 ]; then
    validate_names "$SRC/build_san_asan/$UNIFIED" "$@"
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:halt_on_error=1" \
UBSAN_OPTIONS="abort_on_error=1:halt_on_error=1:print_stacktrace=1" \
    run_suite "$SRC/build_san_asan" "ASan+UBSan" "$@"

# --- TSan (races; concurrency test repeated) -------------------------------
if [ "${SKIP_TSAN:-0}" != "1" ]; then
    build_config "$SRC/build_san_tsan" "TSan" -DPARVATI_ENABLE_TSAN=ON
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
        run_suite "$SRC/build_san_tsan" "TSan" "$@"

    # Races are timing-dependent: hammer the concurrency test specifically.
    if [ $# -eq 0 ] || printf '%s\n' "$@" | grep -qx -- "concurrency_test"; then
        echo "  repeating concurrency_test under TSan x${REPEAT}..."
        for ((i = 1; i <= REPEAT; ++i)); do
            if TSAN_OPTIONS="halt_on_error=1" \
                "$SRC/build_san_tsan/$UNIFIED" concurrency_test \
                > "$LOGDIR/hellcat_san_conc_${i}.$LOGTAG.log" 2>&1; then
                echo "    concurrency_test TSan run $i/$REPEAT: PASS"
            else
                echo "    concurrency_test TSan run $i/$REPEAT: FAIL"
                grep -m3 -iE "WARNING: ThreadSanitizer|SUMMARY:" \
                    "$LOGDIR/hellcat_san_conc_${i}.$LOGTAG.log" | sed 's/^/        /'
                fail=1
            fi
        done
    fi
fi

echo ""
if [ "$fail" -eq 0 ]; then
    echo "=== SANITIZER SWEEP: ALL CLEAN ==="
else
    echo "=== SANITIZER SWEEP: FAILURES (see $LOGDIR/hellcat_san_*.$LOGTAG.log) ==="
fi
exit "$fail"
