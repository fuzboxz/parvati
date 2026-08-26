#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/run_static_analysis.sh — Hellcat static "linter" for memory safety.
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
# cppcheck gate + allowlist: the gate FAILS on any finding whose signature
# (path | id | message) is absent from tools/static_analysis_allowlist.txt.
# Baseline findings pass. Set HELLCAT_STATIC_ALLOWLIST_REGEN=1 to print
# paste-ready signatures instead of gating (maintain the file consciously).
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
    echo "  cmake -S . -B build_unified -DCMAKE_BUILD_TYPE=Debug" >&2
    exit 2
fi

# PATH first, Homebrew fallback second: Linux and Intel-Mac installs put
# clang-tidy on PATH, not under /opt/homebrew.
CLANG_TIDY="${CLANG_TIDY:-$(command -v clang-tidy || echo /opt/homebrew/opt/llvm/bin/clang-tidy)}"
RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy || echo /opt/homebrew/opt/llvm/bin/run-clang-tidy)}"
CPPCHECK="${CPPCHECK:-cppcheck}"

echo "=== Hellcat static analysis ==="
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

# --- cppcheck (gated against the allowlist) -------------------------------
if [ "${SKIP_CPPCHECK:-0}" != "1" ] && command -v "$CPPCHECK" > /dev/null 2>&1; then
    echo "--- cppcheck (Source/) ---"
    # --inline-suppr honours // cppcheck-suppress; --library=gnu gives std types;
    # -DPARVATI_UNIT_TEST off keeps it on the plugin path. force + check-level
    # exhaustive trade speed for deeper interprocedural analysis.
    TMP_FINDINGS="$(mktemp "${TMPDIR:-/tmp}/hellcat_cppcheck.XXXXXX")"
    trap 'rm -f "$TMP_FINDINGS"' RETURN
    ( cd "$SRC" && "$CPPCHECK" \
        --project="$BUILD/compile_commands.json" \
        --enable=warning,style,performance,portability \
        --inline-suppr \
        --library=gnu \
        --check-level=exhaustive \
        --suppress=useStlAlgorithm \
        --quiet \
        -I Source ) 2>&1 | tee "$TMP_FINDINGS" | sed 's/^/  /'

    # Signature = repo-relative path | cppcheck id | message (no line/column:
    # they drift with edits). Files outside the repo sign by basename only.
    ALLOW="$SRC/tools/static_analysis_allowlist.txt"
    python3 - "$TMP_FINDINGS" "$ALLOW" "${HELLCAT_STATIC_ALLOWLIST_REGEN:-0}" "$SRC" <<'PYEOF' || exit 1
import re, sys, os
findings_path, allow_path, regen, src = sys.argv[1:5]
sig = []
seen = set()
for line in open(findings_path, errors="replace"):
    m = re.match(r"^(.+?):(\d+):(\d+): \w+: (.*) \[([^]]+)\]$", line.rstrip())
    if not m:
        continue
    path, _ln, _col, msg, cid = m.groups()
    ap = os.path.abspath(os.path.join(src, path))
    if ap.startswith(src + os.sep):
        path = ap[len(src) + 1:]
    if os.sep in path and not path.startswith(("Source/", "tests/", "ambika_reference/")):
        path = os.path.basename(path)   # build/_deps or vendored tree: basename
    s = f"{path}|{cid}|{msg}"
    if s not in seen:
        seen.add(s)
        sig.append(s)
if regen == "1":
    print("  REGEN: paste-ready signatures (review, then replace the non-header lines of the allowlist):")
    for s in sorted(seen):
        print(s)
    sys.exit(0)
allowed = set()
for line in open(allow_path, errors="replace") if os.path.exists(allow_path) else []:
    line = line.rstrip("\n")
    if line and not line.startswith("#"):
        allowed.add(line)
new = [s for s in sig if s not in allowed]
print(f"  cppcheck: {len(sig)} finding signature(s); {len(sig) - len(new)} allowlisted, {len(new)} NEW")
if new:
    print("  NEW findings (fix, or extend the baseline consciously):")
    for s in new:
        print(f"    {s}")
    sys.exit(1)
print("  cppcheck: gate PASSED (no new findings)")
PYEOF
else
    echo "  (cppcheck not found; install with: brew install cppcheck)"
fi

echo ""
echo "=== static analysis complete ==="
exit 0
