#!/usr/bin/env python3
"""check_async_this.py — raw-this-in-async-callback static guard (tool 6).

THE BUG CLASS (found in the 2026-08 deep hunt lanes, fixed in waves 2/7):
A lambda capturing bare `this` handed to an ASYNCHRONOUS UI API outlives the
Component it belongs to. JUCE popups/dialogs open their own windows and fire
their completion callbacks after the host may already have torn the plugin
editor down (project close, AUv3 view dismissal) — dereferencing freed
memory. The house pattern (ParamControl::showContextMenu, FxSlotCard,
PresetBrowser, MulExportDialog caller) captures a
juce::Component::SafePointer and null-checks it in the body.

WHAT THIS CHECKS
Async-sink statements in Source/**:
  * PopupMenu   .showMenuAsync( / ModalCallbackFunction::create(
  * PopupMenu::Item .setAction( / ->setAction(
  * PopupMenu   .addItem( / ->addItem( on a receiver tracked as a
    juce::PopupMenu (declaration tracking, same approach as tool 5)
  * <chooser>.launchAsync( (FileChooser completions)
  * <Dialog>::launch( / ->launch( (static dialog helpers: TuningEditor,
    MulExportDialog — the caller-side callback is the async surface)
  * DialogWindow::LaunchOptions ... .launchAsync() spans
Inside each sink statement's argument span, a lambda is VIOLATING iff
  * its capture list contains `this` (bare or in a list), AND
  * neither the statement span NOR the 5 source lines preceding the sink
    mention SafePointer / WeakReference (the nearby-guard heuristic: the
    house idiom declares the guard immediately before the async call).
Over-approximation is deliberate: a false positive costs one hand-verified
allowlist entry; a false negative is a shipped use-after-free window.

CANARY (--self-test): the scan refuses to run until the checker flags a
seeded bad lambda AND passes (a) a SafePointer-captured one and (b) a raw
`this` one with a guard declared just above it. A checker that cannot catch
the class it guards is a green lie; fail hard instead.

USAGE
  python3 tools/check_async_this.py --self-test-and-scan   # ctest entry
  python3 tools/check_async_this.py --scan                 # scan only
  python3 tools/check_async_this.py --self-test            # canary only
Exit 0 = clean (or allowlisted); exit 1 = violation; exit 2 = self-test failed.

Allowlist: tools/check_async_this_allowlist.txt — one `file:line:reason`
entry per line (exact `path:line` prefix match), `#` comments allowed. AIM
FOR EMPTY: every entry needs a justification and a tracking TODO.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO / "Source"
ALLOWLIST = REPO / "tools" / "check_async_this_allowlist.txt"

# ---------------------------------------------------------------------------
# Sink tokens (regex over raw text; matched case-sensitively).
SINK_TOKENS = [
    re.compile(r"\.showMenuAsync\s*\("),
    re.compile(r"->showMenuAsync\s*\("),
    re.compile(r"ModalCallbackFunction::create\s*\("),
    re.compile(r"\.setAction\s*\("),
    re.compile(r"->setAction\s*\("),
    re.compile(r"\.launchAsync\s*\("),
    re.compile(r"->launchAsync\s*\("),
    re.compile(r"::launch\s*\("),
    re.compile(r"->launch\s*\("),
]

GUARD_TOKENS = ("SafePointer", "WeakReference")
GUARD_WINDOW_LINES = 5

# Lambdas: capture-list (no nested brackets in our codebase's captures).
LAMBDA_RE = re.compile(r"\[([^\[\]]*)\]\s*(?:\([^)]*\)\s*)?(?:->\s*[\w:<>]+\s*)?\{")

# PopupMenu receiver declaration tracking. The delimiter after the
# identifier must be a declarator-ish char so juce::PopupMenu::Options
# (scope resolution) never binds a name. Covers `juce::PopupMenu m;`, the
# reference/pointer parameter forms `juce::PopupMenu& m` / `* m`, and
# initialized locals (`= (...)`).
PM_DECL = re.compile(
    r"\bjuce::PopupMenu\s*([&*]\s*)?([A-Za-z_]\w*)\s*[;,)=(]")
PM_ADD = re.compile(r"([A-Za-z_]\w*)\s*(?:\.|->)\s*addItem\s*\(")

SOURCE_GLOBS = ("*.cpp", "*.h")


def find_matching(text: str, open_idx: int, open_ch: str, close_ch: str) -> int:
    """Index of the close char matching text[open_idx], honoring strings and
    comments coarsely (nested brackets exact). Deterministic."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in ("'", '"'):
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = (end + 1) if end != -1 else n
        elif ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def line_of(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def strip_comments(text: str) -> str:
    """Blank out // and /* */ content (strings preserved coarsely) so prose in
    comments cannot satisfy a guard-token search — a stale comment next to an
    unguarded capture must not mask it."""
    out = []
    i = 0
    n = len(text)
    in_str = None
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            # preserve line structure for line-based lookups
            skipped = text[i:(end + 2) if end != -1 else n]
            out.append("\n" * skipped.count("\n"))
            i = (end + 2) if end != -1 else n
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def scan_text(text: str, rel: str, pm_names, findings: list):
    """Scan one translation unit's text. `pm_names` = identifiers bound to a
    juce::PopupMenu in this TU (for addItem-sink receiver tracking)."""
    lines = text.split("\n")
    line_starts = []
    pos = 0
    for ln in lines:
        line_starts.append(pos)
        pos += len(ln) + 1

    def preceding_lines(idx: int, count: int) -> str:
        li = line_of(text, idx) - 1  # 0-based
        lo = max(0, li - count)
        # comments stripped: prose cannot stand in for a code guard
        return strip_comments("\n".join(lines[lo:li + 1]))

    # -- collect sink spans -------------------------------------------------
    spans = []  # (start_idx, end_idx, kind)
    for tok in SINK_TOKENS:
        for m in tok.finditer(text):
            paren = text.find("(", m.end() - 1)
            if paren == -1:
                continue
            close = find_matching(text, paren, "(", ")")
            if close == -1:
                continue
            spans.append((m.start(), close, m.group(0).strip()))
    # addItem on tracked PopupMenu receivers
    for m in PM_ADD.finditer(text):
        if m.group(1) not in pm_names:
            continue
        paren = text.find("(", m.end() - 1)
        close = find_matching(text, paren, "(", ")")
        if close == -1:
            continue
        spans.append((m.start(), close, "addItem"))
    spans.sort()
    # drop spans nested inside another span (e.g. setAction inside addItem)
    pruned = []
    last_end = -1
    for s, e, k in spans:
        if s > last_end:
            pruned.append((s, e, k))
            last_end = e
    spans = pruned

    # -- examine lambdas inside each sink span ------------------------------
    for s, e, kind in spans:
        span_text = text[s:e + 1]
        for lm in LAMBDA_RE.finditer(span_text):
            captures = lm.group(1)
            if not re.search(r"(^|\s|,)this(\s*(,|\]|$))", " " + captures + " "):
                continue
            # lambda body for context (brace-matched inside the span slice)
            body_start = lm.end() - 1
            depth = 0
            i = body_start
            body_end = len(span_text) - 1
            in_str = None
            while i < len(span_text):
                ch = span_text[i]
                if in_str:
                    if ch == "\\":
                        i += 2
                        continue
                    if ch == in_str:
                        in_str = None
                elif ch in ('"', "'"):
                    in_str = ch
                elif span_text.startswith("//", i):
                    while i < len(span_text) and span_text[i] != "\n":
                        i += 1
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        body_end = i
                        break
                i += 1
            body = span_text[body_start:body_end + 1]
            # Position-aware guard window: the guard must appear BEFORE the
            # lambda's capture (a SafePointer init INSIDE the capture list, a
            # guard in the statement's earlier arguments, or one declared in
            # the preceding source lines). A guard appearing later in the same
            # statement belongs to a DIFFERENT lambda and must not mask this
            # capture (the openSaveMultiDialog mixed case).
            capture_abs = s + lm.start()
            span_before = strip_comments(span_text[: lm.start()])
            window = preceding_lines(capture_abs, GUARD_WINDOW_LINES)
            guarded = (
                any(g in captures for g in GUARD_TOKENS)
                or any(g in strip_comments(captures) for g in GUARD_TOKENS)
                or any(g in span_before for g in GUARD_TOKENS)
                or any(g in window for g in GUARD_TOKENS)
            )
            _ = body  # body text kept for the report context only
            if not guarded:
                findings.append(
                    (rel, line_of(text, s + lm.start()), kind,
                     "lambda captures raw `this` with no SafePointer/WeakReference guard"))


def scan_source() -> list:
    findings = []
    for path in sorted(SOURCE_DIR.rglob("*")):
        if path.suffix.lstrip(".") not in [g.lstrip("*.") for g in SOURCE_GLOBS]:
            continue
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path.relative_to(REPO))
        pm_names = {m.group(2) for m in PM_DECL.finditer(text)}
        scan_text(text, rel, pm_names, findings)
    return findings


def load_allowlist() -> set:
    allowed = set()
    if ALLOWLIST.exists():
        for raw in ALLOWLIST.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # entries look like "Source/x.cpp:123: reason"
            parts = line.split(":", 2)
            if len(parts) >= 2 and parts[1].strip().isdigit():
                allowed.add(f"{parts[0].strip()}:{int(parts[1].strip())}")
    return allowed


def self_test() -> bool:
    bad = (
        "void f (HellcatEditor& ed) {\n"
        "    juce::PopupMenu m;\n"
        "    m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r) { onMenuDone (r); });\n"
        "}\n"
    )
    good_capture = (
        "void f (HellcatEditor& ed) {\n"
        "    juce::Component::SafePointer<HellcatEditor> safe (this);\n"
        "    juce::PopupMenu m;\n"
        "    m.showMenuAsync (juce::PopupMenu::Options(), [safe] (int) { if (safe != nullptr) safe->done(); });\n"
        "}\n"
    )
    good_window = (
        "void f (HellcatEditor& ed) {\n"
        "    juce::Component::SafePointer<HellcatEditor> safe (this);\n"
        "    juce::PopupMenu m;\n"
        "    m.addItem (TRANS (\"x\"), [this] { doThing (); });\n"
        "}\n"
    )
    for name, snippet, expect_hit in (
        ("bad snippet (raw this, no guard)", bad, True),
        ("good snippet (SafePointer capture)", good_capture, False),
        ("good snippet (guard declared in preceding window)", good_window, False),
    ):
        findings = []
        pm = {m.group(2) for m in PM_DECL.finditer(snippet)}
        scan_text(snippet, "canary.cpp", pm, findings)
        hit = bool(findings)
        if hit != expect_hit:
            print(f"SELF-TEST FAILED: {name}: expected hit={expect_hit}, got hit={hit}",
                  file=sys.stderr)
            for f in findings:
                print(f"  -> {f}", file=sys.stderr)
            return False
        print(f"canary: {name}: {'flagged' if hit else 'passed'} (as expected)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true", help="canary only")
    ap.add_argument("--scan", action="store_true", help="scan only")
    ap.add_argument("--self-test-and-scan", action="store_true",
                    help="canary gate then scan (ctest entry)")
    args = ap.parse_args()

    if not (args.self_test or args.scan or args.self_test_and_scan):
        args.self_test_and_scan = True

    if args.self_test or args.self_test_and_scan:
        if not self_test():
            return 2
        if args.self_test:
            print("check_async_this: self-test OK")
            return 0

    print("== check_async_this scan ==")
    findings = scan_source()
    allowed = load_allowlist()
    violations = []
    for rel, lineno, kind, msg in findings:
        key = f"{rel}:{lineno}"
        if key in allowed:
            continue
        violations.append((rel, lineno, kind, msg))
    if not violations:
        print(f"scanned {sum(1 for _ in SOURCE_DIR.rglob('*.cpp'))} translation units; "
              f"{len(findings)} async-this capture(s) checked")
        print("check_async_this: CLEAN — every async callback is SafePointer-guarded "
              "or allowlisted with a justification")
        return 0
    print(f"check_async_this: {len(violations)} UNGUARDED raw-this async callback(s):",
          file=sys.stderr)
    for rel, lineno, kind, msg in violations:
        print(f"  {rel}:{lineno}: [{kind}] {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
