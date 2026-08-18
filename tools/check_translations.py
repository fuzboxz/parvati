#!/usr/bin/env python3
# tools/check_translations.py
#
# Deterministic i18n completeness check (TOOL 10 of the deterministic-tooling
# wave): every TRANS() string literal used in Source/ must exist as a key in
# BOTH language tables in Source/ui/Translations.cpp (FR + DE), unless it is
# listed in tools/trans_allowlist.txt.
#
# Faithfulness notes (why this scanner can be trusted):
#   * The tables are parsed the way juce::LocalisedStrings parses them at
#     runtime (juce_LocalisedStrings.cpp::loadFromText): line-based, a line
#     must start with '"', keys/values are found with the same
#     escaped-quote scanning (a '"' only closes when the PREVIOUS char is not
#     a backslash) and the same unescape set (\" \' \t \r \n).
#   * C++ literal decoding handles the escapes actually used in this tree:
#     \" \\ \n \t \r \' and \xNN HEX BYTE ESCAPES (e.g. "Custom\xE2\x80\xA6"
#     == "Custom…" as raw UTF-8 bytes) plus octal, so a TRANS literal written
#     with hex escapes byte-matches a table key written as raw UTF-8 and vice
#     versa.
#   * Adjacent C++ literals inside ONE TRANS(...) argument are merged (the
#     compiler concatenates them into a single runtime key); literals joined
#     by '+' between SEPARATE TRANS() calls are each their own key (the
#     tables' documented suffix-key fragment convention).
#   * TRANS(<non-literal>) calls are counted and reported as out-of-scope
#     (dynamic keys — their prose lives in arrays, not at the call site).
#
# Allowlist (tools/trans_allowlist.txt) has two sections:
#   ## intentional   — deliberately untranslated (proper nouns, glyphs)
#   ## known-missing — real gaps (tooltip prose etc.), tracked so the check
#                     stays green while still FAILING on any NEW omission
#                     (a new missing key is not in either section -> FAIL).
#
# Usage:
#   python3 tools/check_translations.py --self-test-and-scan   # ctest entry
#   python3 tools/check_translations.py                        # plain scan
#
# Deterministic: no randomness, no time, no filesystem mtime use; the output
# is byte-identical across runs on the same tree.

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRANSLATIONS_CPP = ROOT / "Source" / "ui" / "Translations.cpp"
ALLOWLIST = ROOT / "tools" / "trans_allowlist.txt"

# ---------------------------------------------------------------------------
# C++ string-literal level
# ---------------------------------------------------------------------------

def decode_cpp_literal(body: str) -> bytes:
    """Decode the inside of one C++ string literal into raw bytes.

    Handles the escape forms present in this tree (hex byte escapes are the
    load-bearing one: the UTF-8 table keys are written either raw or as
    \\xNN sequences and must compare equal after decoding).
    """
    out = bytearray()
    i = 0
    simple = {
        '"': b'"', "'": b"'", "\\": b"\\", "n": b"\n", "t": b"\t",
        "r": b"\r", "a": b"\a", "b": b"\b", "f": b"\f", "v": b"\v",
        "?": b"?",
    }
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "x":
                j = i + 2
                h = ""
                while j < len(body) and len(h) < 2 and body[j] in "0123456789abcdefABCDEF":
                    h += body[j]
                    j += 1
                if h:
                    out.append(int(h, 16))
                    i = j
                    continue
            if n in "01234567":
                j = i + 1
                o = ""
                while j < len(body) and len(o) < 3 and body[j] in "01234567":
                    o += body[j]
                    j += 1
                out.append(int(o, 8) & 0xFF)
                i = j
                continue
            if n in simple:
                out += simple[n]
                i += 2
                continue
            out += n.encode("utf-8")
            i += 2
            continue
        out += c.encode("utf-8")
        i += 1
    return bytes(out)


def scan_cxx_literals(text: str):
    """Yield (kind, payload, index) for every token in C++ source text.

    kind is one of 'lit' (payload = decoded bytes of a "..." literal body),
    'comment', 'other' (payload = the single char). Skips nothing else, so
    callers can distinguish adjacency (only whitespace/comments between two
    literals => compiler concatenation).
    """
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j == -1 else j
            yield ("comment", text[i:j], i)
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            yield ("comment", text[i:j], i)
            i = j
            continue
        if c == '"':
            j = i + 1
            body = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    body.append(text[j])
                    body.append(text[j + 1])
                    j += 2
                else:
                    body.append(text[j])
                    j += 1
            yield ("lit", decode_cpp_literal("".join(body)), i)
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            yield ("other", c, i)
            i = j + 1
            continue
        yield ("other", c, i)
        i += 1


# ---------------------------------------------------------------------------
# juce::LocalisedStrings text-table level (loadFromText, faithfully)
# ---------------------------------------------------------------------------

def juce_find_close_quote(line: str, start: int) -> int:
    """juce_LocalisedStrings.cpp findCloseQuote: a '"' closes only when the
    previous char is not a backslash."""
    last = ""
    pos = start
    while pos < len(line):
        c = line[pos]
        if c == '"' and last != "\\":
            return pos
        last = c
        pos += 1
    return -1


def juce_unescape(s: str) -> str:
    for a, b in (('\\"', '"'), ("\\'", "'"), ("\\t", "\t"), ("\\r", "\r"), ("\\n", "\n")):
        s = s.replace(a, b)
    return s


def parse_localised_table(table_text: str):
    """Emulate LocalisedStrings::loadFromText over the runtime table text.

    Returns (keys_in_order, duplicates)."""
    keys = []
    for raw in table_text.split("\n"):
        line = raw.strip()
        if not line.startswith('"'):
            continue
        cq = juce_find_close_quote(line, 1)
        if cq < 1:
            continue
        key = juce_unescape(line[1:cq])
        oq = juce_find_close_quote(line, cq + 1)
        if oq < 0:
            continue
        cq2 = juce_find_close_quote(line, oq + 1)
        if cq2 < 0:
            continue
        value = juce_unescape(line[oq + 1 : cq2])
        if key and value:
            keys.append(key)
    seen, dups = set(), []
    for k in keys:
        if k in seen and k not in dups:
            dups.append(k)
        seen.add(k)
    return keys, dups


def extract_table_block(text: str, func_name: str) -> str:
    """Concatenate the adjacent C++ literals forming the CharPointer_UTF8
    table argument of @p func_name into the runtime table text."""
    m = re.search(re.escape(func_name) + r"\s*\(\s*\)\s*\{.*?juce::CharPointer_UTF8\s*\(", text, re.S)
    if m is None:
        raise RuntimeError(f"table function {func_name} not found")
    i = m.end()
    parts = []
    while i < len(text):
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = len(text) if j == -1 else j
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = len(text) if j == -1 else j + 2
            continue
        if text[i].isspace():
            i += 1
            continue
        if text[i] == '"':
            j = i + 1
            body = []
            while j < len(text) and text[j] != '"':
                if text[j] == "\\" and j + 1 < len(text):
                    body.append(text[j])
                    body.append(text[j + 1])
                    j += 2
                else:
                    body.append(text[j])
                    j += 1
            parts.append(decode_cpp_literal("".join(body)))
            i = j + 1
            continue
        if text[i] == ")":
            break
        if parts:
            break  # a non-literal token after literals ends the run
        i += 1
    if not parts:
        raise RuntimeError(f"no literals found for {func_name}")
    return b"".join(parts).decode("utf-8", errors="strict")


# ---------------------------------------------------------------------------
# TRANS() call-site extraction
# ---------------------------------------------------------------------------

def find_matching_paren(text: str, open_idx: int) -> int:
    depth = 0
    for kind, payload, idx in scan_cxx_literals(text[open_idx:]):
        pos = open_idx + idx
        if kind == "other":
            if payload == "(":
                depth += 1
            elif payload == ")":
                depth -= 1
                if depth == 0:
                    return pos
    return -1


def extract_trans_literals(text: str):
    """Return (keys, dynamic_count) for one source file.

    Keys: one entry per runtime key literal — adjacent C++ literals inside a
    single TRANS(...) argument are merged (compiler concatenation); '+-
    joined' separate TRANS calls contribute one key each. Dynamic (no literal
    in the argument) TRANS calls are counted, not failed.
    """
    keys = []
    dynamic = 0
    for m in re.finditer(r"\bTRANS\s*\(", text):
        open_idx = m.end() - 1
        close_idx = find_matching_paren(text, open_idx)
        if close_idx < 0:
            continue
        arg = text[open_idx + 1 : close_idx]
        merged = bytearray()
        have_lit = False
        expect_adjacent = False  # next literal continues the current key
        for kind, payload, _ in scan_cxx_literals(arg):
            if kind == "lit":
                if have_lit and not expect_adjacent:
                    # '+' or another token separated the literals: the
                    # previous key is complete; start a new one.
                    keys.append(merged.decode("utf-8", errors="replace"))
                    merged = bytearray()
                merged += payload
                have_lit = True
                expect_adjacent = True
            elif kind == "comment":
                continue  # comments do not break compiler concatenation
            else:
                if payload.isspace():
                    continue
                expect_adjacent = payload == "+"
        if have_lit:
            keys.append(merged.decode("utf-8", errors="replace"))
        else:
            dynamic += 1
    return keys, dynamic


# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------

def load_allowlist(path: pathlib.Path):
    """Return (intentional, known_missing) key sets.

    Entries are EXACT key text: leading/trailing spaces are significant (the
    tables' suffix-key fragment convention), so lines are taken verbatim up
    to the newline; only blank lines and '#' comments are skipped.
    """
    intentional, known_missing = set(), set()
    section = intentional
    for raw in path.read_text(encoding="utf-8").split("\n"):
        line = raw.rstrip("\r")
        # section headers first (they start with '#', so the comment skip
        # below must not swallow them)
        if line.strip() == "## intentional":
            section = intentional
            continue
        if line.strip() == "## known-missing":
            section = known_missing
            continue
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        section.add(line)
    return intentional, known_missing


# ---------------------------------------------------------------------------
# The check itself (parameterised so the canary can run it on synthetic data)
# ---------------------------------------------------------------------------

def run_check(table_fr_text, table_de_text, used_keys, allowlist_intentional,
              allowlist_known_missing):
    violations = []
    fr_keys, fr_dups = parse_localised_table(table_fr_text)
    de_keys, de_dups = parse_localised_table(table_de_text)
    fr, de = set(fr_keys), set(de_keys)

    for d in fr_dups:
        violations.append(f"duplicate key in FR table: {d!r}")
    for d in de_dups:
        violations.append(f"duplicate key in DE table: {d!r}")

    for k in sorted(fr - de):
        violations.append(f"asymmetric: FR-only key: {k!r}")
    for k in sorted(de - fr):
        violations.append(f"asymmetric: DE-only key: {k!r}")

    missing = sorted(k for k in set(used_keys) if k not in fr and k not in de)
    unlisted = [k for k in missing
                if k not in allowlist_intentional and k not in allowlist_known_missing]
    for k in unlisted:
        violations.append(f"TRANS key missing from BOTH tables (not allowlisted): {k!r}")

    return violations, missing, len(fr_keys), len(de_keys)


# ---------------------------------------------------------------------------
# Canary (embedded, in-memory)
# ---------------------------------------------------------------------------

CANARY_TABLE_FR = (
    "language: French\n"
    "\n"
    "\"Settings\" = \"Réglages\"\n"
    "\"Load\" = \"Charger\"\n"
)
CANARY_TABLE_DE = (
    "language: German\n"
    "\n"
    "\"Settings\" = \"Einstellungen\"\n"
    "\"Load\" = \"Laden\"\n"
)
CANARY_USED_KEYS = ["Settings", "Load", "Save"]


def canary() -> bool:
    """Prove the checker detects a stripped key BEFORE scanning the real tree.

    The canary embeds an in-memory copy of a miniature table with ONE key
    ('Save') used but present in NEITHER block; the check must report exactly
    that key. A second pass with a seeded asymmetric table must report the
    asymmetry (guards the symmetry arm), and a seeded duplicate must report
    the duplicate arm.
    """
    ok = True
    v, missing, _, _ = run_check(CANARY_TABLE_FR, CANARY_TABLE_DE,
                                 CANARY_USED_KEYS, set(), set())
    if v != ["TRANS key missing from BOTH tables (not allowlisted): 'Save'"]:
        print("canary: FAIL — stripped-key detection did not report exactly 'Save'")
        print("        got:", v)
        ok = False
    else:
        print("canary: stripped key 'Save' reported (missing-key arm OK)")

    v2, _, _, _ = run_check(CANARY_TABLE_FR + '"Zoom" = "Zoom"\n',
                            CANARY_TABLE_DE, ["Settings", "Load"], set(), set())
    if not any("FR-only" in x for x in v2) or len(v2) != 1:
        print("canary: FAIL — asymmetry arm did not fire alone:", v2)
        ok = False
    else:
        print("canary: FR-only asymmetry reported (symmetry arm OK)")

    v3, _, _, _ = run_check(CANARY_TABLE_FR + '"Load" = "Charger 2"\n',
                            CANARY_TABLE_DE, CANARY_USED_KEYS, set(), set())
    if not any("duplicate key in FR" in x for x in v3):
        print("canary: FAIL — duplicate arm did not fire:", v3)
        ok = False
    else:
        print("canary: duplicate FR key reported (duplicate arm OK)")

    # Adjacency semantics: 'a' 'b' inside one TRANS() is ONE key 'ab'.
    keys, _ = extract_trans_literals('TRANS ("a" "b");')
    if keys != ["ab"]:
        print("canary: FAIL — adjacent literals must merge into one key:", keys)
        ok = False
    else:
        print("canary: adjacent C++ literals merge into one runtime key")

    # Escape semantics: hex-byte-escape literal == raw UTF-8 key.
    lit = decode_cpp_literal("Custom\\xE2\\x80\\xA6")
    if lit != "Custom…".encode("utf-8"):
        print("canary: FAIL — hex byte escape did not decode to UTF-8 bytes")
        ok = False
    else:
        print("canary: hex byte escapes decode to raw UTF-8 bytes")

    return ok


# ---------------------------------------------------------------------------
# Real scan
# ---------------------------------------------------------------------------

def real_scan() -> int:
    text = TRANSLATIONS_CPP.read_text(encoding="utf-8")
    table_fr = extract_table_block(text, "frenchChromeStrings")
    table_de = extract_table_block(text, "germanChromeStrings")

    used_keys = []
    dynamic_calls = 0
    files = sorted(p for p in (ROOT / "Source").rglob("*")
                   if p.suffix in (".cpp", ".h"))
    for p in files:
        keys, dyn = extract_trans_literals(p.read_text(encoding="utf-8", errors="replace"))
        used_keys += keys
        dynamic_calls += dyn

    intentional, known_missing = load_allowlist(ALLOWLIST)

    violations, missing, nfr, nde = run_check(
        table_fr, table_de, used_keys, intentional, known_missing)

    print("== check_translations scan ==")
    print(f"scanned {len(files)} Source files; {len(used_keys)} TRANS literal keys, "
          f"{dynamic_calls} dynamic (non-literal) TRANS calls (out of scope)")
    print(f"FR table: {nfr} keys; DE table: {nde} keys")

    intentional_missing = [k for k in missing if k in intentional]
    known = [k for k in missing if k in known_missing]
    print(f"missing from BOTH: {len(missing)} "
          f"({len(intentional_missing)} intentional-untranslated, "
          f"{len(known)} known-missing/tracked, "
          f"{len(missing) - len(intentional_missing) - len(known)} unlisted)")
    if known:
        print("known-missing (tracked in tools/trans_allowlist.txt, awaiting "
              "translation prose):")
        for k in known:
            print("  -", repr(k))
    if dynamic_calls:
        print("dynamic TRANS() calls (keys not extractable at the call site; "
              "their prose lives in arrays — manual coverage):")
        print(f"  count: {dynamic_calls}")

    if violations:
        print("check_translations: FAIL")
        for v in violations:
            print("  VIOLATION:", v)
        return 1
    print("check_translations: CLEAN — every TRANS literal is keyed in BOTH "
          "language tables (or explicitly allowlisted)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test-and-scan", action="store_true",
                    help="run the embedded canary first, then scan the tree")
    args = ap.parse_args()
    if args.self_test_and_scan:
        if not canary():
            print("check_translations: FAIL (canary)")
            return 1
        print()
    return real_scan()


if __name__ == "__main__":
    sys.exit(main())
