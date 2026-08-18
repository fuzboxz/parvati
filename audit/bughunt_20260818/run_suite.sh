#!/usr/bin/env bash
# Full deterministic test-suite baseline. Builds then runs every parvati_* test.
set -u
cd /Users/fuzboxz/parvati
TARGETS=$(grep -o 'add_executable(\(parvati_[a-z0-9_]*\)' CMakeLists.txt | sed 's/add_executable(//' | grep -v -E 'gen_templates|state_donor|screen_shots|menu_shots|alias_probe|popdiag|diffuser_diag|matrix_probe|stage$' | sort -u)
echo "Building: $TARGETS" | tr '\n' ' '
echo
cmake --build build_release --target $TARGETS -j8 2>&1 | grep -E 'error|Error' && { echo "BUILD FAILED"; exit 1; }
echo "=== BUILD OK ==="
PASS=0; FAIL=0; FAILED_TESTS=""
for t in $TARGETS; do
  if [ -x "build_release/$t" ]; then
    if timeout 300 ./build_release/$t > "audit/bughunt_20260818/last_run_$t.log" 2>&1; then
      PASS=$((PASS+1))
    else
      rc=$?
      FAIL=$((FAIL+1)); FAILED_TESTS="$FAILED_TESTS $t(rc=$rc)"
    fi
  else
    echo "SKIP (not built): $t"
  fi
done
echo "=== SUITE DONE: PASS=$PASS FAIL=$FAIL ==="
[ -n "$FAILED_TESTS" ] && echo "FAILED:$FAILED_TESTS"
exit 0
