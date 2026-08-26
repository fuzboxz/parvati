#!/usr/bin/env bash
# profile_ui.sh — reproducible local UI performance gate for Hellcat (macOS).
#
# Measures the Release standalone under three budgets and fails loudly when
# any is exceeded, so a repaint/timer regression cannot land unnoticed again
# (this class of regression landed twice: the PartRow combo onChange
# ping-pong ~100% of one core at idle, and a per-paint jassert that fired
# ~20x/sec in Debug builds).
#
#   1. idle   : mean CPU% of >=5 `top` samples 2 s apart after steady state
#   2. active : mean CPU% while tools/ui_drag_helper.swift drags the mouse
#               in circles (knob-drag class of interaction, ~80 Hz events)
#   3. asserts: "JUCE Assertion" lines in the steady-state measurement window
#               (a firing jassert is a Debug-build CPU multiplier and usually
#               marks an encoding bug — must be 0)
#
# Usage:
#   tools/profile_ui.sh [build_dir]        (default: build_release)
#   PROFILE_IDLE_MAX=8 PROFILE_ACTIVE_MAX=50 tools/profile_ui.sh   # budgets
#
# Requirements: cmake, Xcode tools, and — for the active measurement only —
# Accessibility permission for the calling terminal (CGEvent posting). CI
# runs the headless tests/perf_smoke_test instead (see CONTRIBUTING.md,
# "Performance regression testing").

set -u

# ---------------------------------------------------------------- budgets ---
# Headroom note: the healthy Release standalone measures 4.2-5.2% idle on
# the reference machine run-to-run (the audio render thread processes FX
# continuously — engine behaviour, not UI). 8% keeps the gate stable against
# that variance while still tripping on any real regression (the historical
# idle churn sat at 6%+ WITH a jassert storm, and the combo ping-pong at
# ~100%). Tighten via PROFILE_IDLE_MAX if your machine is quieter.
idle_max_pct="${PROFILE_IDLE_MAX:-8}"      # % of one core, mean, at idle
active_max_pct="${PROFILE_ACTIVE_MAX:-40}" # % of one core, mean, while dragging
max_asserts="${PROFILE_MAX_ASSERTS:-0}"    # jassert lines in the window

build_dir="${1:-build_release}"
app="$build_dir/Hellcat_artefacts/Release/Standalone/Hellcat.app"
binary="$app/Contents/MacOS/Hellcat"
repo_root="$(cd "$(dirname "$0")/.." && pwd)"

# Fail fast with the exact configure line: build_release is a release-workflow
# dir, not part of a fresh clone. build_unified is Debug-only and cannot serve
# this Release measurement.
prof_cache="$repo_root/$build_dir/CMakeCache.txt"
case "$build_dir" in /*) prof_cache="$build_dir/CMakeCache.txt";; esac
if [ ! -f "$prof_cache" ]; then
    echo "No configured build dir at '$build_dir'. This script measures a Release build." >&2
    echo "Configure it first:  cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release" >&2
    exit 2
fi
helper_src="$repo_root/tools/ui_drag_helper.swift"
helper_bin="${TMPDIR:-/tmp}/hellcat_ui_drag_helper"

# ---------------------------------------------------------------- helpers ---
app_pid=""
cleanup() {
    [ -n "$app_pid" ] && kill "$app_pid" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

fail=0
# Emits PASS/FAIL only; the caller records the verdict. (A fail flag set
# inside a command substitution would be lost in the parent shell.)
verdict() {
    if [ "$(echo "$1 <= $2" | bc)" -eq 1 ]; then echo PASS; else echo FAIL; fi
}
row() { # $1 label, $2 value, $3 budget, $4 verdict
    printf '  %-24s %-10s %-8s %s\n' "$1" "$2" "$3" "$4"
    [ "$4" = FAIL ] && fail=1
    true
}

# Mean CPU% from `top` output lines ("<pid> <pct>"). Discards the first two
# samples (launch spike + first full interval) and averages the rest.
mean_cpu() { # $1=pid $2=sample_count
    top -l "$(( $2 + 2 ))" -pid "$1" -stats pid,cpu -s 2 2>/dev/null \
        | grep -E '^[[:space:]]*[0-9]+[[:space:]]+-?[0-9]+\.[0-9]+' \
        | awk 'NR > 2 { n++; s += $2 } END { if (n > 0) printf "%.1f", s / n; else print "999" }'
}

# ------------------------------------------------------------- build check ---
if ! cmake --build "$repo_root/$build_dir" --target Hellcat_Standalone -j8 1>&2; then
    echo "profile_ui.sh: failed to build Hellcat_Standalone in $build_dir" >&2
    exit 1
fi
cd "$repo_root" || exit 1
[ -x "$binary" ] || { echo "profile_ui.sh: standalone binary not found: $binary" >&2; exit 1; }

# Compile the drag helper on first use (fast no-op afterwards).
if [ ! -x "$helper_bin" ] || [ "$helper_src" -nt "$helper_bin" ]; then
    swiftc -O "$helper_src" -o "$helper_bin" || { echo "profile_ui.sh: swiftc failed" >&2; exit 1; }
fi

# --------------------------------------------------------------- launch ------
log="$(mktemp -t hellcat_profile_log)"
"$binary" > "$log" 2>&1 &
app_pid=$!
sleep 8   # steady state: window up, first paint + patch serialization done

if ! kill -0 "$app_pid" 2>/dev/null; then
    echo "profile_ui.sh: app exited during startup — log tail:" >&2
    tail -5 "$log" >&2
    exit 1
fi

# Window position/size (AppleScript) — drag inside the editor body.
# AppleScript lists are comma-separated; turn them into spaces so read's
# word-splitting yields the four numbers (a bare tr -d ' ' would leave
# "74,33,1282,662" in wx and empty the rest, dragging at y=0).
read -r wx wy ww wh < <(osascript -e 'tell application "System Events" to get {position, size} of window 1 of process "Hellcat"' 2>/dev/null | tr -d ' ' | tr ',' ' ')
if [ -z "${wx:-}" ]; then wx=0; wy=0; ww=1280; wh=700; fi
drag_cx=$(( wx + ww / 2 ))
drag_cy=$(( wy + wh / 3 ))

log_lines_at_idle_start=$(wc -l < "$log" | tr -d ' ')

# ------------------------------------------------- 1. idle CPU measurement ---
idle_cpu=$(mean_cpu "$app_pid" 5)
idle_asserts=$(tail -n +"$log_lines_at_idle_start" "$log" | grep -c "JUCE Assertion" || true)

# ----------------------------------------------- 2. active CPU measurement ---
# 14 s of circular dragging; top samples concurrently (12 s inside the drag).
"$helper_bin" "$drag_cx" "$drag_cy" 60 14 &
drag_pid=$!
sleep 2
active_cpu=$(mean_cpu "$app_pid" 5)
kill "$drag_pid" 2>/dev/null; wait "$drag_pid" 2>/dev/null

kill "$app_pid" 2>/dev/null; wait "$app_pid" 2>/dev/null; app_pid=""

# ----------------------------------------------------------------- report ---
hdr=$(printf '%.0s=' {1..62})
printf '\n%s\n' "$hdr"
printf '  %-24s %-10s %-8s %s\n' "measurement" "value" "budget" "result"
v=$(verdict "$idle_cpu" "$idle_max_pct");   row "idle CPU (mean of 5)"      "${idle_cpu}%"   "${idle_max_pct}%"   "$v"
v=$(verdict "$active_cpu" "$active_max_pct"); row "active CPU (drag, mean)" "${active_cpu}%" "${active_max_pct}%" "$v"
v=$(verdict "$idle_asserts" "$max_asserts"); row "jasserts (window)"        "$idle_asserts"  "$max_asserts"      "$v"
printf '%s\n' "$hdr"

if [ "$fail" -ne 0 ]; then
    echo "RESULT: FAIL — a budget was exceeded (see above)."
    exit 1
fi
echo "RESULT: PASS — all budgets met."
exit 0
