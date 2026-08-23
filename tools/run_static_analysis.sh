#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/run_static_analysis.sh — Parvati static "linter" for memory safety.
# ---------------------------------------------------------------------------
# Runs two complementary static analyzers over Source/:
#   * clang-tidy — bugprone-* / cert-* / performance-* checks (.clang-tidy),
#                  including bounds and null-deref diagnostics.
#   * cppcheck   — buffer overflows, null pointers, memory leaks, UB (the
#                  --enable=all buffer/unsafe-class set, incl. inline-suppression).
#
# Both are STATIC (compile-time) checks; pair with tools/run_sanitizers.sh for
# DYNAMIC (runtime) memory/race detection. A clean build dir with
# compile_commands.json is required (any configured build dir provides it).
#
# Usage:
#   tools/run_static_analysis.sh            # uses the first available build dir
#   BUILD=build_release tools/run_static_analysis.sh
#   SKIP_CPPCHECK=1 tools/run_static_analysis.sh
#
# Exits non-zero if cppcheck finds a real (non-suppressed) issue. clang-tidy is
# advisory (its findings are printed; it does not fail the run unless you pass
# them via .clang-tidy WarningsAsErrors).
# ---------------------------------------------------------------------------
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-}"
if [ -z "$BUILD" ]; then
    for d in build_unified build build_release build_san_asan; do
        if [ -f "$SRC/$d/compile_commands.json" ]; then BUILD="$SRC/$d"; break; fi
    done
fi
if [ -z "$BUILD" ] || [ ! -f "$BUILD/compile_commands.json" ]; then
    echo "No compile_commands.json found. Configure a build first, e.g.:" >&2
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DJUCE_GLOBAL_PATH=~/JUCE" >&2
    exit 2
fi

CLANG_TIDY="${CLANG_TIDY:-/opt/homebrew/opt/llvm/bin/clang-tidy}"
RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-/opt/homebrew/opt/llvm/bin/run-clang-tidy}"
CPPCHECK="${CPPCHECK:-cppcheck}"

echo "=== Parvati static analysis ==="
echo "  build (compile_commands.json): $BUILD"
echo ""

# --- clang-tidy (advisory) -------------------------------------------------
if [ -x "$RUN_CLANG_TIDY" ]; then
    echo "--- clang-tidy (Source/) ---"
    # run-clang-tidy walks compile_commands.json and applies .clang-tidy. -quiet
    # trims the per-file header noise; -p points at the build dir.
    ( cd "$SRC" && "$RUN_CLANG_TIDY" -p "$BUILD" -quiet 'Source/*.cpp' ) 2>&1 \
        | grep -vE "^[0-9]+ warnings generated\.?$|^Suppressed [0-9]+ warnings|^Use -header-filter" \
        | sed 's/^/  /' | head -200
    echo "  (clang-tidy findings are advisory; not fail-on.)"
elif [ -x "$CLANG_TIDY" ]; then
    echo "--- clang-tidy (Source/, one file at a time) ---"
    for f in "$SRC"/Source/*.cpp; do
        "$CLANG_TIDY" -p "$BUILD" "$f" 2>&1 | grep -E "warning:|error:" | sed 's/^/  /' || true
    done
else
    echo "  (clang-tidy not found at $CLANG_TIDY; skipping)"
fi
echo ""

# --- cppcheck (fail-on real issues) ----------------------------------------
if [ "${SKIP_CPPCHECK:-0}" != "1" ] && command -v "$CPPCHECK" > /dev/null 2>&1; then
    echo "--- cppcheck (Source/) ---"
    # --inline-suppr honours // cppcheck-suppress; --library=gnu gives std types;
    # -DPARVATI_UNIT_TEST off keeps it on the plugin path. force + check-level
    # exhaustive trade speed for deeper interprocedural analysis.
    ( cd "$SRC" && "$CPPCHECK" \
        --project="$BUILD/compile_commands.json" \
        --enable=warning,style,performance,portability \
        --inline-suppr \
        --library=gnu \
        --check-level=exhaustive \
        --suppress=useStlAlgorithm \
        --quiet \
        -I Source ) 2>&1 | sed 's/^/  /'
    # cppcheck exits non-zero only with --error-exitcode; re-scan its output.
    # Rerun with the exit code so CI can gate on it:
    if ( cd "$SRC" && "$CPPCHECK" \
        --project="$BUILD/compile_commands.json" \
        --enable=warning,style,performance,portability \
        --inline-suppr --library=gnu --check-level=exhaustive \
        --suppress=useStlAlgorithm --quiet --error-exitcode=1 -I Source ) > /dev/null 2>&1; then
        echo "  cppcheck: no issues found"
    else
        echo "  cppcheck: ISSUES FOUND (see above)"
        exit 1
    fi
else
    echo "  (cppcheck not found; install with: brew install cppcheck)"
fi

echo ""
echo "=== static analysis complete ==="
exit 0
