#!/usr/bin/env python3
"""check_combo_clear.py — stale-async-onChange static guard (deterministic tool 5).

THE BUG CLASS (found twice in the wild, waves 2/4 of the 2026-08 deep hunt):
JUCE's `ComboBox::clear()` defaults to `sendNotificationAsync`: it queues an
async update, and a LATER `setSelectedId(x, juce::dontSendNotification)` does
NOT cancel the queued update — so the combo's `onChange` fires ~next message
loop for a REBUILD the user never touched (language switch, theme retheme,
reflow). In this editor every onChange writes the engine, so a rebuild could
re-apply a possibly-stale template/oversampling factor over real state.
The fix idiom everywhere else is `combo.clear (juce::dontSendNotification)`.

WHAT THIS CHECKS
Every `.clear(`/`->clear(` call whose receiver is a juce::ComboBox-typed
identifier must pass `juce::dontSendNotification` as an argument. Receivers
are resolved per translation unit by declaration tracking:

  * `juce::ComboBox name;` / `juce::ComboBox nameA, nameB;` (members + locals)
  * `std::unique_ptr<juce::ComboBox> name;` (->clear form)
  * `juce::ComboBox* name;`
  * known combo member names (suffix heuristic, see KNOWN_COMBO_IDENTS) so a
    declaration the tracker misses is still checked (over-approximation is
    safe: a false positive here is a human-reviewed allowlist entry, a false
    negative is a shipped async-onChange hazard).

A clear() on a known-NON-ComboBox type (AudioBuffer, atomics, containers such
as Array/Vector/String/Array<int>, ValueTree) is never flagged — the receiver
identifier list below only ever contains ComboBox-typed names.

CANARY (--self-test): the scan refuses to run until the checker flags a
seeded bad snippet (`comboX.clear();` etc. on a tracked ComboBox) AND passes
a compliant one (`comboX.clear (juce::dontSendNotification);`). A checker
that cannot catch the class it guards is a green lie; fail hard instead.

USAGE
  python3 tools/check_combo_clear.py --self-test-and-scan   # ctest entry
  python3 tools/check_combo_clear.py --scan                 # scan only
  python3 tools/check_combo_clear.py --self-test            # canary only
Exit 0 = clean (or allowlisted); exit 1 = violation; exit 2 = self-test failed.

Allowlist: tools/check_combo_clear_allowlist.txt — one `file:line:reason`
entry per line (exact `path:line` prefix match), `#` comments allowed. AIM
FOR EMPTY: every entry needs a justification and a tracking TODO.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO / "Source"
ALLOWLIST = REPO / "tools" / "check_combo_clear_allowlist.txt"

# Member/local identifiers that are ComboBoxes by naming convention in this
# codebase (defence in depth: the declaration tracker is the primary signal,
# this catches a declaration form we did not enumerate). Kept to names that
# are ComboBoxes everywhere they appear in Source/.
KNOWN_COMBO_IDENTS = {
    "arrangementCombo_", "voicesCombo_", "channelCombo_", "polyCombo_",
    "tuneCombo_", "partCombo_", "themeCombo_", "osCombo_", "langCombo_",
    "strategyCombo_", "sourceCombo_", "destCombo_", "typeCombo_",
    "fxTypeCombo",
}

# Declaration forms that bind an identifier to a juce::ComboBox type in this
# codebase. Group 1 is the FIRST declarator; multi-declarator lines
# (`juce::ComboBox a_, b_;`) are expanded by the trailing-ident scan.
DECL_PATTERNS = [
    # juce::ComboBox name; / juce::ComboBox nameA_, nameB_;  (also plain
    # `ComboBox` inside a juce-using TU, and ComboBox& parameters)
    re.compile(r"\b(?:juce::)?ComboBox\s+([&*]?\s*)([A-Za-z_]\w*)"),
    # std::unique_ptr<juce::ComboBox> name;
    re.compile(r"unique_ptr\s*<\s*(?:juce::)?ComboBox\s*>\s+([A-Za-z_]\w*)"),
]
# A `juce::ComboBox x_, y_, z_;` line: after the first ident, the remaining
# comma-separated plain identifiers until ';' are ComboBoxes too.
MULTI_DECL_TAIL = re.compile(r"^\s*juce::ComboBox\s+[&*]?\s*[A-Za-z_]\w*\s*((?:,\s*[A-Za-z_]\w*\s*)*);")

# A clear() call on any identifier: `.clear (` / `->clear (` / `.clear(` /
# `->clear(`. This codebase's clang-format style puts a space before '('.
CLEAR_CALL = re.compile(r"([A-Za-z_]\w*)\s*(\.|->)\s*clear\s*\(([^)]*)\)")
DONT_SEND = "dontsendnotification"


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments so commented-out code is never flagged."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def combos_in_file(path: Path):
    """Identifiers that are ComboBox-typed in this file (per the tracker)."""
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    found = set()
    for pat in DECL_PATTERNS:
        for m in pat.finditer(text):
            ident = m.group(m.lastindex).strip("&* ").strip()
            if ident:
                found.add(ident)
    # Multi-declarator lines: `juce::ComboBox a_, b_;` — expand the tail.
    for line in text.splitlines():
        m = MULTI_DECL_TAIL.match(line)
        if m and m.group(1):
            for ident in re.findall(r"[A-Za-z_]\w*", m.group(1)):
                found.add(ident)
    return found


def check_snippet(name: str, code: str, expect_flag: bool):
    """Run the receiver+argument logic on an embedded snippet (canary)."""
    flagged = []
    for m in CLEAR_CALL.finditer(code):
        ident, args = m.group(1), m.group(3)
        if ident not in KNOWN_COMBO_IDENTS and not ident.endswith("Combo_"):
            continue  # receiver not ComboBox-typed
        if DONT_SEND not in args.replace(" ", "").lower():
            flagged.append((name, ident, args.strip()))
    if bool(flagged) != expect_flag:
        return False, flagged
    return True, flagged


def self_test() -> bool:
    """Canary: the checker MUST flag a bad clear and pass a compliant one."""
    bad = (
        "juce::ComboBox langCombo_;\n"
        "void rebuild() { langCombo_.clear(); }\n"
        "void other() { strategyCombo_->clear(); }\n"
        "void partial() { langCombo_.clear (juce::sendNotification); }\n"
    )
    good = (
        "juce::ComboBox langCombo_;\n"
        "void rebuild() { langCombo_.clear (juce::dontSendNotification); }\n"
        "void other() { strategyCombo_->clear (juce::dontSendNotification); }\n"
    )
    ok1, f1 = check_snippet("bad", bad, expect_flag=True)
    ok2, _ = check_snippet("good", good, expect_flag=False)
    print(f"  canary: bad snippet flagged: {'yes' if f1 else 'NO'} ({len(f1)} hits)")
    print(f"  canary: compliant snippet flagged: {'no' if ok2 else 'YES (false positive)'}")
    if not (ok1 and ok2):
        print("SELF-TEST FAILED: the checker cannot detect the class it guards.")
        return False
    print("  canary: OK (checker detects missing dontSendNotification)")
    return True


def load_allowlist():
    if not ALLOWLIST.exists():
        return {}
    entries = {}
    for line in ALLOWLIST.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key = ":".join(line.split(":")[:2])  # "path:line"
        entries[key] = line
    return entries


def scan() -> int:
    allow = load_allowlist()
    files = sorted(SOURCE_DIR.rglob("*") )
    files = [f for f in files if f.suffix in (".cpp", ".h", ".mm")]
    violations = []
    checked = 0
    for path in files:
        rel = str(path.relative_to(REPO))
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        combo_idents = combos_in_file(path) | KNOWN_COMBO_IDENTS
        for lineno, line in enumerate(text.splitlines(), start=1):
            for m in CLEAR_CALL.finditer(line):
                ident, args = m.group(1), m.group(3)
                if ident not in combo_idents:
                    continue
                checked += 1
                if DONT_SEND not in args.replace(" ", "").lower():
                    key = f"{rel}:{lineno}"
                    if key in allow:
                        continue
                    violations.append(f"{rel}:{lineno}: {ident}.clear({args.strip()})"
                                      f" lacks juce::dontSendNotification")
    print(f"scanned {len(files)} Source files; {checked} ComboBox clear() calls checked")
    if allow:
        print(f"allowlist entries active: {len(allow)}")
    if violations:
        print("VIOLATIONS (stale-async-onChange class):")
        for v in violations:
            print(f"  {v}")
        return 1
    print("check_combo_clear: CLEAN — every ComboBox clear() passes dontSendNotification")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="ComboBox::clear() notification guard")
    ap.add_argument("--self-test", action="store_true", help="run the canary only")
    ap.add_argument("--scan", action="store_true", help="run the scan only")
    ap.add_argument("--self-test-and-scan", action="store_true",
                    help="canary first, then the real scan (ctest entry)")
    args = ap.parse_args()
    if not any([args.self_test, args.scan, args.self_test_and_scan]):
        args.self_test_and_scan = True
    if args.self_test or args.self_test_and_scan:
        print("== check_combo_clear self-test (canary) ==")
        if not self_test():
            return 2
    if args.scan or args.self_test_and_scan:
        print("== check_combo_clear scan ==")
        return scan()
    return 0


if __name__ == "__main__":
    sys.exit(main())
