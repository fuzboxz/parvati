#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# tools/gen_manual.py — build the Hellcat user manual PDF.
#
# Inputs (all read-only):
#   docs/manual/Hellcat-Manual.md   the manual source (chapters 1..10)
#   CMakeLists.txt                  the project version (title page)
#   Source/ParameterLayout.cpp      the parameter descriptor table
#   Source/ui/ParamHelp.cpp         the parameter tooltips
#
# Output:
#   docs/manual/Hellcat-Manual.pdf
#
# The "Parameter Reference" appendix is GENERATED from the source files. It
# cannot drift from the code: a new parameter appears in the appendix on the
# next run. Parsing is conservative. A row that resists parsing prints one
# WARN line on stderr and is skipped. The script never crashes on a parse.
#
# PDF engine: fpdf2 1.7.2 (the 1.x API: cell(ln=), multi_cell, add_link,
# set_link, alias_nb_pages). No pip installs. Core fonts (Helvetica family +
# Courier for code), latin-1 output: all text is sanitized to ASCII first.
#
# Usage:
#   python3 tools/gen_manual.py            (from the repo root)
#   cmake --build build_unified --target hellcat_gen_manual
# ---------------------------------------------------------------------------

import os
import pathlib
import re
import sys

# The generator still uses the fpdf2 1.7.2-era `ln=1` calls (functional on
# 2.8.x). Silence only that deprecation so build output stays readable.
import warnings
warnings.filterwarnings("ignore", message='.*parameter "ln" is deprecated.*')
import datetime

try:
    from fpdf import FPDF
except ImportError:
    sys.stderr.write("ERROR: fpdf (fpdf2 1.7.2) is not installed. "
                     "Install fpdf2==1.7.2 first.\n")
    sys.exit(2)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MANUAL_MD = os.path.join(REPO, "docs", "manual", "Hellcat-Manual.md")
PARAM_LAYOUT = os.path.join(REPO, "Source", "ParameterLayout.cpp")
PARAM_HELP = os.path.join(REPO, "Source", "ui", "ParamHelp.cpp")
CMAKELISTS = os.path.join(REPO, "CMakeLists.txt")
OUT_PDF = os.path.join(REPO, "docs", "manual", "Hellcat-Manual.pdf")

WARNINGS = []


def warn(msg):
    WARNINGS.append(msg)
    sys.stderr.write("WARN: %s\n" % msg)


# ---------------------------------------------------------------------------
# Text sanitation: the core fonts are latin-1. Map common typography to ASCII
# and drop anything the PDF font cannot carry.
# ---------------------------------------------------------------------------

_CHAR_MAP = {
    "\u2014": "--", "\u2013": "-", "\u2192": "->", "\u2190": "<-",
    "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
    "\u2026": "...", "\u00d7": "x", "\u00b7": "-", "\u2022": "-",
    "\u2264": "<=", "\u2265": ">=", "\u00b0": " deg", "\u00a0": " ",
}


def sanitize(s):
    out = []
    for ch in s:
        if ch in _CHAR_MAP:
            out.append(_CHAR_MAP[ch])
        elif ord(ch) < 128:
            out.append(ch)
        else:
            out.append("?")
    return "".join(out)


# ---------------------------------------------------------------------------
# Markdown source parsing (the manual chapters)
# ---------------------------------------------------------------------------

def parse_manual(path):
    """Return (intro_paragraphs, blocks). Blocks are tuples:
    ('h2'|'h3'|'h4', text) | ('p', text) | ('ul'|'ol', [items]) |
    ('table', [rows]) | ('pb', None)."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    blocks = []
    intro = []
    seen_h2 = False
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith("<!--pagebreak-->"):
            blocks.append(("pb", None))
            i += 1
            continue
        if stripped.startswith("# ") and not seen_h2:
            # The document H1: the cover title. Its following paragraphs are
            # cover intro text until the first page break.
            i += 1
            while i < len(lines):
                s = lines[i].strip()
                if not s or s.startswith("<!--") or s.startswith("#"):
                    break
                intro.append(s)
                i += 1
            continue
        if stripped.startswith("#### "):
            blocks.append(("h4", stripped[5:].strip()))
            i += 1
            continue
        img = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)$", stripped)
        if img:
            blocks.append(("img", (img.group(1), img.group(2))))
            i += 1
            continue
        if stripped.startswith("### "):
            blocks.append(("h3", stripped[4:].strip()))
            i += 1
            continue
        if stripped.startswith("## "):
            blocks.append(("h2", stripped[3:].strip()))
            seen_h2 = True
            i += 1
            continue
        if stripped.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-{2,}:?", c) for c in cells):
                    rows.append(cells)
                i += 1
            blocks.append(("table", rows))
            continue
        if stripped.startswith("- "):
            items = []
            while i < len(lines) and lines[i].strip().startswith("- "):
                items.append(lines[i].strip()[2:].strip())
                i += 1
            blocks.append(("ul", items))
            continue
        m = re.match(r"^(\d+)\.\s+(.*)$", stripped)
        if m:
            items = []
            while i < len(lines):
                m2 = re.match(r"^(\d+)\.\s+(.*)$", lines[i].strip())
                if not m2:
                    break
                items.append(m2.group(2).strip())
                i += 1
            blocks.append(("ol", items))
            continue
        # Plain paragraph: gather until a blank line or a new block marker.
        para = [stripped]
        i += 1
        while i < len(lines):
            s = lines[i].strip()
            if (not s or s.startswith("#") or s.startswith("|") or s.startswith("- ")
                    or s.startswith("<!--") or s.startswith("!") or re.match(r"^\d+\.\s", s)):
                break
            para.append(s)
            i += 1
        blocks.append(("p", " ".join(para)))
    return intro, blocks


# ---------------------------------------------------------------------------
# Parameter appendix: conservative parsing of ParameterLayout.cpp + ParamHelp
# ---------------------------------------------------------------------------

# Known counts. The resolver reads the real headers first; this map is the
# fallback so a header edit alone cannot silently break the generator.
FALLBACK_CONSTS = {
    "kNumEnvelopes": 3,
    "kNumModulations": 14,
    "kNumModifiers": 4,
    "kNumSyncedLfoRates": 15,
    "kNumTuningPresets": 32,
}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def resolve_consts():
    consts = dict(FALLBACK_CONSTS)
    for header in ("Source/dsp/patch.h", "Source/dsp/constants.h",
                   "Source/TuningTables.h"):
        p = os.path.join(REPO, header)
        if not os.path.isfile(p):
            continue
        text = read(p)
        for m in re.finditer(r"\b(kNum\w+)\s*=\s*(\d+)\s*;", text):
            consts[m.group(1)] = int(m.group(2))
    return consts


def split_args(s):
    """Split a C++ argument list on top-level commas."""
    args, depth, cur, in_str = [], 0, [], False
    i = 0
    while i < len(s):
        ch = s[i]
        if in_str:
            cur.append(ch)
            if ch == '"' and (not cur[:-1] or cur[-2] != "\\"):
                in_str = False
        elif ch == '"':
            cur.append(ch)
            in_str = True
        elif ch in "([":
            depth += 1
            cur.append(ch)
        elif ch in ")]":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def strip_ns(name):
    return name.split("::")[-1].strip()


class Expr:
    """Evaluates the small argument expressions the tables use."""

    def __init__(self, consts):
        self.consts = consts

    def to_int(self, expr, env):
        expr = expr.strip()
        if re.fullmatch(r"-?\d+", expr):
            return int(expr)
        if expr in env.get("int", {}):
            return env["int"][expr]
        base = strip_ns(expr)
        if base in self.consts:
            return self.consts[base]
        for op in ("+", "-", "*"):
            parts = self._split_op(expr, op)
            if len(parts) == 2:
                a, b = self.to_int(parts[0], env), self.to_int(parts[1], env)
                if a is not None and b is not None:
                    return {"+": a + b, "-": a - b, "*": a * b}[op]
        return None

    @staticmethod
    def _split_op(expr, op):
        depth, in_str, parts, cur = 0, False, [], []
        for ch in expr:
            if in_str:
                if ch == '"':
                    in_str = False
                continue
            if ch == '"':
                in_str = True
            elif ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            if ch == op and depth == 0:
                parts.append("".join(cur))
                cur = []
            else:
                cur.append(ch)
        parts.append("".join(cur))
        return parts if len(parts) == 2 and all(p.strip() for p in parts) else None

    def to_str(self, expr, env):
        expr = expr.strip()
        # A strict single string literal (not "a" + x + "b").
        if re.fullmatch(r'"(?:[^"\\]|\\.)*"', expr, re.S):
            body = expr[1:-1]
            return re.sub(r'\\(.)', r'\1', body)
        m = re.fullmatch(r"std::to_string\s*\((.*)\)\s*", expr, re.S)
        if m:
            val = self.to_int(m.group(1), env)
            return None if val is None else str(val)
        if expr in env.get("str", {}):
            return env["str"][expr]
        # String concatenation chain.
        parts = self._split_concat(expr)
        if len(parts) >= 2:
            out = ""
            for p in parts:
                v = self.to_str(p, env)
                if v is None:
                    return None
                out += v
            return out
        return None

    @staticmethod
    def _split_concat(expr):
        parts, cur, in_str, depth = [], [], False, 0
        for ch in expr:
            if in_str:
                cur.append(ch)
                if ch == '"':
                    in_str = False
                continue
            if ch == '"':
                cur.append(ch)
                in_str = True
            elif ch in "([":
                depth += 1
                cur.append(ch)
            elif ch in ")]":
                depth -= 1
                cur.append(ch)
            elif ch == "+" and depth == 0:
                parts.append("".join(cur).strip())
                cur = []
            else:
                cur.append(ch)
        parts.append("".join(cur).strip())
        return parts


LOOP_RE = re.compile(
    r"for\s*\(\s*(?:int|const\s+int)\s+(\w+)\s*=\s*(\d+)\s*;\s*\1\s*(<=|<)\s*"
    r"([^;]+?)\s*;\s*(?:\+\+\1|\1\s*\+=\s*1)\s*\)")


def parse_statements(text):
    """Very small statement walker: returns a list of
    ('call', name, args) / ('assign', lhs, rhs) / ('loop', var, range, body)
    in SOURCE ORDER. Handles both braced and single-statement for bodies."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        m = LOOP_RE.search(text, i)
        if not m:
            out.extend(_parse_flat(text[i:]))
            break
        out.extend(_parse_flat(text[i:m.start()]))
        # Body: either a braced block or ONE statement to the next ';'.
        j = m.end()
        while j < n and text[j] in " \t\r\n":
            j += 1
        if j < n and text[j] == "{":
            depth = 0
            in_str = False
            while j < n:
                ch = text[j]
                if in_str:
                    if ch == '"':
                        in_str = False
                elif ch == '"':
                    in_str = True
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = text[m.end():j]
            i = j + 1
        else:
            # Single statement (no braces): read to the ';' at paren depth 0.
            k = j
            depth = 0
            in_str = False
            while k < n:
                ch = text[k]
                if in_str:
                    if ch == '"':
                        in_str = False
                elif ch == '"':
                    in_str = True
                elif ch in "([":
                    depth += 1
                elif ch in ")]":
                    depth -= 1
                elif ch == ";" and depth == 0:
                    break
                k += 1
            body = text[m.end():k]
            i = k + 1
        lo = int(m.group(2))
        out.append(("loop", m.group(1),
                    (lo, m.group(4).strip(), m.group(3)), body))
    return out


def _parse_flat(text):
    """Parse a flat region. Returns statements tagged with their position so
    callers see them in source order (a 'bind' must precede its 'call')."""
    found = []
    for m in re.finditer(r"\b(add|addSeq|addArp|addFx)\s*\(", text):
        args, _ = _read_parens(text, m.end() - 1)
        if args is not None:
            found.append((m.start(), ("call", m.group(1), split_args(args))))
    for m in re.finditer(r"\bm\s*\[", text):
        # m[ KEY ] = RHS ;  -- RHS may span lines and concatenate strings.
        close = text.find("]", m.end())
        if close < 0:
            continue
        key = text[m.end():close].strip()
        rest = text[close + 1:].lstrip()
        if not rest.startswith("="):
            continue
        k = 1
        depth = 0
        in_str = False
        while k < len(rest):
            ch = rest[k]
            if in_str:
                if ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            elif ch == ";" and depth == 0:
                break
            k += 1
        rhs = rest[1:k].strip()
        if rhs:
            found.append((m.start(), ("assign", key, rhs)))
    # Local string bindings: const auto n = std::to_string (...);
    for m in re.finditer(
            r"const\s+(?:auto|std::string)\s+(\w+)\s*=\s*std::to_string\s*\(([^)]*)\)\s*;",
            text):
        found.append((m.start(), ("bind", m.group(1), m.group(2))))
    found.sort(key=lambda t: t[0])
    return [st for _, st in found]


def _read_parens(text, open_idx):
    depth = 0
    in_str = False
    j = open_idx
    n = len(text)
    while j < n:
        ch = text[j]
        if in_str:
            if ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:j], j + 1
        j += 1
    return None, j


def expand(node, env, consts, sink):
    kind = node[0]
    if kind == "loop":
        _, var, (lo, bound_raw, op), body = node
        expr = Expr(consts)
        hi = expr.to_int(bound_raw, {"int": env.get("int", {})})
        if hi is None:
            warn("loop bound not resolved: for %s %s %s" % (var, op, bound_raw))
            return
        hi = hi if op == "<=" else hi - 1
        for v in range(lo, hi + 1):
            child_env = {"int": dict(env.get("int", {})), "str": dict(env.get("str", {}))}
            child_env["int"][var] = v
            expand_body(body, child_env, consts, sink)
        return
    if kind in ("call", "assign", "bind"):
        sink(node, env)


def expand_body(body, env, consts, sink):
    for st in parse_statements(body):
        expand(st, env, consts, sink)


def collect_calls(text, consts):
    """Expand all loops; yield ('call', name, args, env) and
    ('assign', key_expr, value, env)."""
    results = []

    def sink(node, env):
        if node[0] == "call":
            results.append(("call", node[1], node[2], env))
        elif node[0] == "assign":
            results.append(("assign", node[1], node[2], env))
        elif node[0] == "bind":
            expr = Expr(consts)
            val = expr.to_int(node[2], env)
            if val is not None:
                env["str"][node[1]] = str(val)
            else:
                warn("string binding not resolved: %s" % node[1])

    expand_body(text, {"int": {}, "str": {}}, consts, sink)
    return results


# --- choice tables -----------------------------------------------------------

def parse_choice_tables(src):
    tables = {}

    def add_entries(name, entries, note=None):
        tables[name] = {"entries": entries, "note": note}

    for m in re.finditer(r"const\s+juce::StringArray\s+(\w+)\s*\{([^}]*)\}\s*;", src, re.S):
        add_entries(m.group(1), re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
    for m in re.finditer(
            r"(?:static\s+)?(?:const\s+)?auto\s+(\w+)\s*=\s*juce::StringArray\s*\{([^}]*)\}\s*;",
            src, re.S):
        add_entries(m.group(1), re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
    for m in re.finditer(
            r"juce::StringArray\s+make(\w+)\s*\(\s*\)\s*(?:const)?\s*\{\s*return\s*\{(.*?)\}\s*;\s*\}",
            src, re.S):
        add_entries("make" + m.group(1), re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
    # Alias declarations: static const auto kArpModes = makeArpModes();
    for m in re.finditer(r"static\s+const\s+auto\s+(\w+)\s*=\s*(make\w+)\(\)\s*;", src):
        alias, target = m.group(1), m.group(2)
        if target in tables:
            tables[alias] = tables[target]

    # makeOscShapes: literal a.add(...) calls + the wavetable loop + tail.
    if "makeOscShapes" in src:
        fn = re.search(r"juce::StringArray\s+makeOscShapes\(\)\s*\{(.*?)\n\}", src, re.S)
        if fn:
            body = fn.group(1)
            entries = re.findall(r'a\.add\s*\(\s*"((?:[^"\\]|\\.)*)"\s*\)', body)
            # The loop line: for (int i = 1; i <= 16; ++i) a.add ("Wavetable " + ...)
            if re.search(r"for\s*\(\s*int\s+i\s*=\s*1\s*;\s*i\s*<=\s*16", body):
                entries += ["Wavetable %d" % i for i in range(1, 17)]
            add_entries("kOscShapes", entries)
    # makeArpPatterns: "Pattern " + i for i in 0..21.
    if "makeArpPatterns" in src:
        fn = re.search(r"juce::StringArray\s+makeArpPatterns\(\)\s*\{(.*?)\n\}", src, re.S)
        if fn and re.search(r"i\s*<\s*22", fn.group(1)):
            add_entries("kArpPatterns", ["Pattern %d" % i for i in range(0, 22)])
    # makeTuningPresetNames: names live in C++ code. Degrade to a count note.
    if "makeTuningPresetNames" in src:
        add_entries("kTuningPresets", ["Off"],
                    note="Off, plus 32 named scale presets")
    return tables


# --- the descriptor rows ------------------------------------------------------

def parse_parameter_rows(consts, tables):
    src = read(PARAM_LAYOUT)
    rows = []
    expr = Expr(consts)

    def make_row(pid, label, choices_ref, mn, mx, where):
        if pid is None or label is None:
            warn("unparsed descriptor row near %s (id=%r label=%r)" % (where, pid, label))
            return
        cname = None
        if choices_ref:
            cname = choices_ref.lstrip("&").strip() or None
            if cname == "nullptr":
                cname = None
        rows.append({"id": pid, "label": label, "choices": cname,
                     "min": mn, "max": mx})

    for kind, name, args, env in collect_calls(src, consts):
        if kind != "call":
            continue
        def s(idx):
            return expr.to_str(args[idx], env) if idx < len(args) else None
        def n(idx, default=0):
            if idx >= len(args):
                return default
            v = expr.to_int(args[idx], env)
            return default if v is None else v
        if name == "add" and len(args) >= 8:
            make_row(s(0), s(1), args[5], n(6), n(7), "add %s" % (s(0) or "?"))
        elif name == "addSeq" and len(args) >= 5:
            make_row(s(0), s(1), None, n(3), n(4), "addSeq %s" % (s(0) or "?"))
        elif name == "addArp" and len(args) >= 5:
            ch = args[3] if args[3].strip() != "nullptr" else None
            mn, mx = 0, 0
            if len(args) >= 7:
                mn, mx = n(5), n(6)
            make_row(s(0), s(1), ch, mn, mx, "addArp %s" % (s(0) or "?"))
        elif name == "addFx" and len(args) >= 4:
            ch = args[2] if args[2].strip() != "nullptr" else None
            mn, mx = 0, 0
            if len(args) >= 6:
                mn, mx = n(4), n(5)
            make_row(s(0), s(1), ch, mn, mx, "addFx %s" % (s(0) or "?"))

    # Standalone descriptor literals (vca_curve / part_select / filter_card /
    # filter_drive): paramID = "x"; ... label = "y"; choices = &kZ; min/max.
    for m in re.finditer(r'\.paramID\s*=\s*"([^"]+)"\s*;', src):
        pid = m.group(1)
        region = src[m.end():m.end() + 400]
        lab = re.search(r'\.label\s*=\s*"([^"]+)"\s*;', region)
        cho = re.search(r"\.choices\s*=\s*&(\w+)\s*;", region)
        mn = re.search(r"\.minValue\s*=\s*([^;]+);", region)
        mx = re.search(r"\.maxValue\s*=\s*([^;]+);", region)
        if not lab:
            warn("standalone descriptor %s has no label" % pid)
            continue
        def orint(match, default):
            if not match:
                return default
            v = expr.to_int(match.group(1), {"int": {}})
            return default if v is None else v
        make_row(pid, lab.group(1), cho.group(1) if cho else None,
                 orint(mn, 0), orint(mx, 0), "descriptor %s" % pid)

    # Attach the actual choice entries.
    for r in rows:
        c = r["choices"]
        if c:
            t = tables.get(c)
            if t:
                r["choices"] = t
            else:
                warn("choice table %s referenced but not parsed" % c)
                r["choices"] = {"entries": [], "note": "choice list"}
    return rows


def parse_tooltips(consts):
    src = read(PARAM_HELP)
    tips = {}
    expr = Expr(consts)
    for kind, key, value, env in collect_calls(src, consts):
        if kind != "assign":
            continue
        kid = expr.to_str(key, env)
        val = expr.to_str(value, env)
        if kid is None or val is None:
            warn("tooltip row not parsed: %r" % (key,))
            continue
        tips[kid] = val
    # The seq-step tooltips are generated in code (getParamHelp fallback).
    # Mirror that table so all rows carry their tooltip.
    for idx in range(16):
        tips["seq1_step%d" % idx] = ("Sequencer 1 step %d value (0..127, "
                                     "modulation source)." % (idx + 1))
        tips["seq2_step%d" % idx] = ("Sequencer 2 step %d value (0..127, "
                                     "modulation source)." % (idx + 1))
        tips["seqnote_step%d" % idx] = ("Note sequence step %d: MIDI note "
                                        "(0..127) | gate flag (bit 7)." % (idx + 1))
        tips["seqnote_vel%d" % idx] = ("Note sequence step %d: velocity "
                                       "(0..127) | legato flag (bit 7)." % (idx + 1))
    return tips


GROUP_ORDER = [
    ("Global", "Global Options"), ("Osc", "Oscillators"), ("Mix", "Mixer"),
    ("Filter", "Filter"), ("Env", "Envelopes"), ("Lfo", "LFOs"),
    ("Mod", "Mod Matrix"), ("Modif", "Modifiers"), ("Part", "Part"),
    ("Seq", "Sequencer"), ("Arp", "Arpeggiator"),
    ("Fx", "Effects"), ("FxMod", "FX Modulation"),
]


def group_for_id(pid):
    """Mirror of hostGroupForId (ParameterLayout.cpp). Order matters."""
    if pid in ("filter_card", "filter_drive", "vca_curve"):
        return "Global"
    if pid.startswith("fxmod"):
        return "FxMod"
    if pid.startswith("fx"):
        return "Fx"
    if pid.startswith("modif"):
        return "Modif"
    if pid.startswith("mod"):
        return "Mod"
    if pid.startswith("voice_lfo"):
        return "Lfo"
    if pid.startswith("env"):
        return "Lfo" if "_lfo_" in pid else "Env"
    if pid.startswith("arp"):
        return "Arp"
    if pid.startswith("seq"):
        return "Seq"
    if pid.startswith("osc"):
        return "Osc"
    if pid.startswith("mix"):
        return "Mix"
    if pid.startswith("filter"):
        return "Filter"
    if pid.startswith("part"):
        return "Part"
    return "Global"


def build_appendix_data():
    consts = resolve_consts()
    src_layout = read(PARAM_LAYOUT)
    tables = parse_choice_tables(src_layout)
    rows = parse_parameter_rows(consts, tables)
    tips = parse_tooltips(consts)
    grouped = []
    for key, title in GROUP_ORDER:
        members = [r for r in rows if group_for_id(r["id"]) == key]
        if members:
            grouped.append((title, members))
    others = [r for r in rows if group_for_id(r["id"]) not in dict(GROUP_ORDER)]
    if others:
        warn("%d rows landed outside every group" % len(others))
    return rows, tips, grouped


def range_text(row):
    c = row["choices"]
    if isinstance(c, dict):
        note = c.get("note")
        entries = c.get("entries") or []
        if note:
            return note
        if entries and len(entries) <= 8:
            return ", ".join(entries)
        if entries:
            return "Choice, %d options" % len(entries)
        return "Choice"
    lo, hi = row["min"], row["max"]
    if lo == 0 and hi == 0:
        return "Choice"
    return "%d .. %d" % (lo, hi)


# ---------------------------------------------------------------------------
# PDF rendering (fpdf2 1.7.2 API)
# ---------------------------------------------------------------------------

PAGE_W = 210.0
MARGIN = 18.0
CONTENT_W = PAGE_W - 2 * MARGIN
MANUAL_DIR = pathlib.Path(os.path.dirname(MANUAL_MD))

# --- Carbon document theme (mirrors the plugin's default theme palette in
# Source/ui/HellcatTheme.cpp, so the manual reads as part of the product) ---
PAGE_BG = (21, 23, 28)        # #15171C backgroundBase
PANEL = (30, 34, 40)          # #1E2228 backgroundPanel
TABLE_HEAD = (37, 42, 49)     # #252A31 backgroundInput
DARK = (232, 234, 240)        # body text on dark (name kept: the body color)
TEXT_STRONG = (246, 246, 250) # #F6F6FA textPrimary (headings)
GREY = (154, 154, 168)        # #9A9AA8 textSecondary (captions, footer)
BLACK = (200, 203, 214)       # caption reset / soft body (name kept)
ACCENT = (56, 189, 248)       # #38BDF8 accentPrimary (Carbon cyan)
LINE_GREY = (64, 70, 82)      # outline/hairlines on dark
FILL_GREY = PANEL             # table zebra fill

INLINE_RE = re.compile(r"(\*\*.+?\*\*|\*[^*]+?\*|`[^`]+?`)")


def parse_inline(text):
    """Split into (text, style) runs. style: '' | 'B' | 'I' | 'CODE'."""
    runs = []
    pos = 0
    for m in INLINE_RE.finditer(text):
        if m.start() > pos:
            runs.append((text[pos:m.start()], ""))
        tok = m.group(0)
        if tok.startswith("**"):
            runs.append((tok[2:-2], "B"))
        elif tok.startswith("`"):
            runs.append((tok[1:-1], "CODE"))
        else:
            runs.append((tok[1:-1], "I"))
        pos = m.end()
    if pos < len(text):
        runs.append((text[pos:], ""))
    return [(t, s) for t, s in runs if t]


class ManualPDF(FPDF):
    def __init__(self, version):
        FPDF.__init__(self)
        self.version = version
        self.section_name = ""
        self.cover = True

    def header(self):
        # Paint the page background FIRST, on every page (the cover included):
        # header() runs at add_page() before any content, so this rect sits
        # under everything the renderer draws on the page.
        self.set_fill_color(*PAGE_BG)
        self.rect(0, 0, PAGE_W, self.h, "F")
        if self.cover or self.page_no() <= 1:
            return
        # Running head: the section name, right aligned, above a hairline.
        self.set_font("Helvetica", "I", 8)
        self.set_text_color(*GREY)
        self.set_y(8)
        self.cell(0, 5, sanitize(self.section_name), align="R", ln=1)
        self.set_draw_color(*LINE_GREY)
        self.set_line_width(0.25)
        self.line(MARGIN, 15.5, PAGE_W - MARGIN, 15.5)
        self.set_y(20)

    def footer(self):
        # Hairline above the footer, then the page number.
        self.set_draw_color(*LINE_GREY)
        self.set_line_width(0.25)
        self.line(MARGIN, self.h - 17, PAGE_W - MARGIN, self.h - 17)
        self.set_y(-14)
        self.set_font("Helvetica", "", 8)
        self.set_text_color(*GREY)
        self.cell(0, 5,
                  "Hellcat %s - page %d of {nb}" % (self.version, self.page_no()),
                  align="C", ln=1)


class Renderer:
    def __init__(self, version):
        self.pdf = ManualPDF(version)
        p = self.pdf
        # Document-level accessibility metadata (PDF/UA basics): the title
        # surfaces in readers, the language lets screen readers pick a voice,
        # and the author/creator identify the generator.
        p.set_title("Hellcat User Manual")
        p.set_lang("en")
        p.set_author("805LABS")
        p.set_creator("hellcat_gen_manual (fpdf2)")
        p.alias_nb_pages()
        p.set_margins(MARGIN, 20, MARGIN)
        p.set_auto_page_break(True, 20)

    # -- low-level rich text -------------------------------------------------

    def _style(self, style):
        if style == "B":
            self.pdf.set_font("Helvetica", "B", 10)
        elif style == "I":
            self.pdf.set_font("Helvetica", "I", 10)
        elif style == "CODE":
            self.pdf.set_font("Courier", "", 9)
        else:
            self.pdf.set_font("Helvetica", "", 10)

    def rich_width(self, runs):
        w = 0.0
        for text, style in runs:
            self._style(style)
            w += self.pdf.get_string_width(sanitize(text))
        return w

    def draw_rich(self, runs, max_w, line_h, indent=0.0):
        """Word-wrap rich runs and draw them. Returns None."""
        pdf = self.pdf
        words = []
        for text, style in runs:
            parts = re.split(r"(\s+)", sanitize(text))
            for part in parts:
                if part == "":
                    continue
                words.append((part, style))
        line = []
        line_w = indent
        space_w_cache = {}

        def sw(style):
            if style not in space_w_cache:
                self._style(style)
                space_w_cache[style] = pdf.get_string_width(" ")
            return space_w_cache[style]

        def flush():
            nonlocal line, line_w
            if not line:
                return
            x = pdf.get_x()
            y = pdf.get_y()
            for text, style, w in line:
                self._style(style)
                pdf.cell(w, line_h, text)
            pdf.set_xy(x, y + line_h)
            line = []
            line_w = indent

        for text, style in words:
            self._style(style)
            w = pdf.get_string_width(text)
            if text.isspace():
                if line:
                    line.append((text, style, w))
                    line_w += w
                continue
            if line_w + w > MARGIN + CONTENT_W - indent + 0.01 and line:
                # trim trailing spaces from the line before flush
                while line and line[-1][0].isspace():
                    line_w -= line[-1][2]
                    line.pop()
                flush()
            line.append((text, style, w))
            line_w += w
        flush()

    def rich_lines(self, runs, max_w):
        """Measure wrapped line count (for table row heights)."""
        pdf = self.pdf
        words = []
        for text, style in runs:
            for part in re.split(r"(\s+)", sanitize(text)):
                if part:
                    words.append((part, style))
        lines, cur = 1, 0.0
        for text, style in words:
            self._style(style)
            w = pdf.get_string_width(text)
            if text.isspace():
                cur += w
                continue
            if cur + w > max_w and cur > 0:
                lines += 1
                cur = w
            else:
                cur += w
        return lines

    # -- block rendering ------------------------------------------------------

    def heading(self, level, text):
        pdf = self.pdf
        # Structure tree entry: screen readers expose the heading as a
        # navigable section (chapters at level 0, subsections below).
        try:
            pdf.start_section(sanitize(text), max(0, level - 2))
        except Exception:
            pass   # older fpdf2 without sections: render without the tree
        if level == 2:
            pdf.set_text_color(*TEXT_STRONG)
            pdf.set_font("Helvetica", "B", 17)
            size = 11
        elif level == 3:
            pdf.set_text_color(*TEXT_STRONG)
            pdf.set_font("Helvetica", "B", 13)
            size = 8
        else:
            pdf.set_text_color(*ACCENT)
            pdf.set_font("Helvetica", "B", 11)
            size = 6
        if pdf.get_y() + 20 > pdf.h - 22:
            pdf.add_page()
        y = pdf.get_y()
        pdf.cell(0, size + 2, sanitize(text), ln=1)
        if level == 2:
            # Cyan rule under every chapter heading (the Carbon accent).
            pdf.set_draw_color(*ACCENT)
            pdf.set_line_width(0.5)
            pdf.line(MARGIN, pdf.get_y() + 0.5, MARGIN + CONTENT_W, pdf.get_y() + 0.5)
            pdf.ln(3)
        else:
            pdf.ln(1.5)
        return y

    def paragraph(self, text):
        self.pdf.set_text_color(*DARK)
        self.draw_rich(parse_inline(text), CONTENT_W, 5.6)
        self.pdf.ln(1.6)

    def bullet_list(self, items, numbered=False):
        pdf = self.pdf
        pdf.set_text_color(*DARK)
        for i, item in enumerate(items, 1):
            marker = ("%d." % i) if numbered else "-"
            pdf.set_font("Helvetica", "", 10)
            pdf.cell(6, 5.6, marker)
            self.draw_rich(parse_inline(item), CONTENT_W - 6, 5.6)
            pdf.ln(0.8)
        pdf.ln(1.2)

    def table(self, rows, col_ratio=None):
        pdf = self.pdf
        if not rows:
            return
        ncol = max(len(r) for r in rows)
        rows = [r + [""] * (ncol - len(r)) for r in rows]
        plain = [[re.sub(r"[*`]", "", c) for c in r] for r in rows]
        # Column widths: natural max word width, then proportional scaling.
        naturals = []
        for c in range(ncol):
            widest = 0.0
            for r in plain:
                self.pdf.set_font("Helvetica", "", 9)
                for word in r[c].split():
                    widest = max(widest, self.pdf.get_string_width(word) + 2)
            widest = max(widest, 8.0)
            naturals.append(min(widest, 70.0))
        total = sum(naturals)
        scale = CONTENT_W / total
        widths = [w * scale for w in naturals]
        # Long cells that wrap force re-balance: give slack to text columns.
        for _ in range(2):
            heights = []
            for ri, r in enumerate(plain):
                bold = "B" if ri == 0 else ""
                lines = 1
                for c in range(ncol):
                    self.pdf.set_font("Helvetica", bold, 9)
                    cell_lines = self.rich_lines([(plain[ri][c], bold)], widths[c] - 2)
                    lines = max(lines, cell_lines)
                heights.append(lines)
            if max(heights) <= 3:
                break
            # shift width from the narrow column to the text-heaviest column
            src = min(range(ncol), key=lambda c: widths[c])
            dst = max(range(ncol), key=lambda c: sum(len(r[c]) for r in plain))
            if src == dst:
                break
            move = min(widths[src] - 10.0, 8.0)
            if move <= 0.5:
                break
            widths[src] -= move
            widths[dst] += move
        header = plain[0]
        body = plain[1:]
        self._table_block(header, body, widths)

    def _table_block(self, header, body, widths):
        pdf = self.pdf
        line_h = 4.6

        def row_height(cells, bold=""):
            lines = 1
            for c, txt in enumerate(cells):
                pdf.set_font("Helvetica", bold, 9)
                lines = max(lines, self.rich_lines([(txt, bold)], widths[c] - 3))
            return lines * line_h + 2.4

        def draw_row(cells, bold="", fill=False):
            h = row_height(cells, bold)
            if pdf.get_y() + h > pdf.h - 22:
                pdf.add_page()
            y = pdf.get_y()
            x = MARGIN
            for c, txt in enumerate(cells):
                pdf.set_xy(x, y)
                if bold:
                    # Header row: input-panel fill, near-white bold text.
                    pdf.set_fill_color(*TABLE_HEAD)
                    pdf.set_text_color(*TEXT_STRONG)
                else:
                    if fill:
                        pdf.set_fill_color(*FILL_GREY)
                    pdf.set_text_color(*BLACK)
                pdf.set_draw_color(*LINE_GREY)
                pdf.set_line_width(0.2)
                pdf.set_font("Helvetica", bold, 9)
                pdf.multi_cell(widths[c], line_h, sanitize(txt),
                               border=1, align="L", fill=bool(bold) or fill)
                x += widths[c]
            pdf.set_y(y + h)

        draw_row(header, bold="B", fill=True)
        for i, r in enumerate(body):
            draw_row(r, fill=(i % 2 == 1))
        pdf.ln(2)


def build_pdf(version, intro, blocks, appendix, heading_pages, out_path):
    """One full render pass. heading_pages: dict text -> ([page, y]) when set
    (the number pass); when None, this pass records heading locations into
    `anchors` and returns them."""
    r = Renderer(version)
    pdf = r.pdf
    anchors = {}
    link_ids = []

    # --- cover ---------------------------------------------------------------
    pdf.cover = True
    pdf.add_page()
    # Dark cover plate: an accent band at the top edge (the Carbon header
    # band of the plugin) and the title block centered on the dark field.
    pdf.set_fill_color(*PANEL)
    pdf.rect(0, 0, PAGE_W, 34, "F")
    pdf.set_draw_color(*ACCENT)
    pdf.set_line_width(0.8)
    pdf.line(0, 34, PAGE_W, 34)
    pdf.set_text_color(*TEXT_STRONG)
    pdf.set_y(88)
    pdf.set_font("Helvetica", "B", 40)
    pdf.cell(0, 20, "Hellcat", align="C", ln=1)
    pdf.set_font("Helvetica", "", 16)
    pdf.set_text_color(*GREY)
    pdf.cell(0, 10, "User Manual", align="C", ln=1)
    pdf.ln(6)
    pdf.set_draw_color(*ACCENT)
    pdf.set_line_width(0.8)
    pdf.line(70, pdf.get_y(), PAGE_W - 70, pdf.get_y())
    pdf.ln(8)
    pdf.set_font("Helvetica", "", 12)
    pdf.set_text_color(*DARK)
    pdf.cell(0, 8, "Version %s" % version, align="C", ln=1)
    pdf.cell(0, 8, datetime.date.today().strftime("%d %B %Y"), align="C", ln=1)
    if intro:
        pdf.ln(16)
        pdf.set_font("Helvetica", "", 10)
        pdf.set_text_color(*GREY)
        for para in intro:
            x = MARGIN + 24
            pdf.set_x(x)
            r.draw_rich(parse_inline(para), CONTENT_W - 48, 5.6)
            pdf.ln(2)
    # Bottom band mirrors the top plate.
    pdf.set_fill_color(*PANEL)
    pdf.rect(0, pdf.h - 24, PAGE_W, 24, "F")
    pdf.set_draw_color(*ACCENT)
    pdf.set_line_width(0.8)
    pdf.line(0, pdf.h - 24, PAGE_W, pdf.h - 24)
    pdf.cover = False

    # --- TOC -----------------------------------------------------------------
    pdf.add_page()
    pdf.section_name = "Contents"
    pdf.set_text_color(*TEXT_STRONG)
    pdf.set_font("Helvetica", "B", 15)
    pdf.cell(0, 10, "Contents", ln=1)
    pdf.ln(2)
    toc_entries = [("h2", t) for k, t in blocks if k == "h2"]
    toc_entries.append(("h2", "Parameter Reference"))
    for level, text in toc_entries:
        lid = pdf.add_link()
        link_ids.append((lid, text))
        pdf.set_font("Helvetica", "", 11)
        pdf.set_text_color(*DARK)
        page_no = ""
        if heading_pages is not None:
            loc = heading_pages.get(text)
            page_no = str(loc[0][0]) if loc else ""
        label = sanitize(text)
        width_label = pdf.get_string_width(label)
        width_page = pdf.get_string_width(page_no)
        # Truncate overlong labels to one line.
        while width_label + width_page + 14 > CONTENT_W and len(label) > 8:
            label = label[:-2]
            width_label = pdf.get_string_width(label)
        pdf.cell(width_label, 6.4, label, link=lid)
        dots_w = CONTENT_W - width_label - width_page - 4
        pdf.set_text_color(*LINE_GREY)
        pdf.cell(dots_w, 6.4, " " * max(0, int(dots_w / pdf.get_string_width(" "))),
                 align="R")
        pdf.set_text_color(*ACCENT)
        pdf.cell(width_page + 4, 6.4, page_no, align="R", ln=1)
        pdf.set_text_color(*DARK)

    # --- chapters ------------------------------------------------------------
    def emit_heading(level, text, anchor):
        pdf.section_name = text
        y = r.heading(level, text)
        if heading_pages is None:
            anchors.setdefault(text, []).append([pdf.page_no(), y])
        return y

    pending_pb = False
    for kind, payload in blocks:
        if kind == "pb":
            pending_pb = True
            continue
        if pending_pb and kind == "h2":
            pdf.add_page()
        elif pending_pb:
            pdf.add_page()
        pending_pb = False
        if kind == "h2":
            emit_heading(2, payload, None)
        elif kind == "h3":
            emit_heading(3, payload, None)
        elif kind == "h4":
            emit_heading(4, payload, None)
        elif kind == "p":
            r.paragraph(payload)
        elif kind == "ul":
            r.bullet_list(payload, numbered=False)
        elif kind == "ol":
            r.bullet_list(payload, numbered=True)
        elif kind == "table":
            r.table(payload)
        elif kind == "img":
            caption, rel = payload
            img_path = MANUAL_DIR / rel
            if not img_path.is_file():
                warn("manual image missing: %s" % rel)
                r.paragraph("[missing image: %s]" % rel)
                continue
            # Size policy: never enlarge past the natural print size (2x
            # captures render at 72 dpi), cap the width at 80 percent of the
            # content width so no picture dominates the page, cap the height
            # at 100 mm, then CENTER horizontally.
            from PIL import Image as _PILImage
            with _PILImage.open(img_path) as im:
                iw, ih = im.size
            natural_w = iw * 72.0 / 144.0 * (25.4 / 72.0)   # px at 2x -> mm
            w = min(CONTENT_W * 0.8, natural_w, CONTENT_W)
            h = w * ih / iw
            if h > 100.0:
                h = 100.0
                w = h * iw / ih
            x_img = (PAGE_W - w) / 2.0
            remaining = pdf.page_break_trigger - pdf.get_y()
            if h + (10 if caption else 0) > remaining:
                pdf.add_page()
            y_img = pdf.get_y()
            # alt_text: the caption doubles as the screen-reader description
            # (fpdf2 embeds it in the document structure).
            pdf.image(str(img_path), x=x_img, w=w,
                      alt_text=sanitize(caption) if caption else None,
                      title="Screenshot")
            # Hairline frame: the screenshot edges read deliberately on the
            # dark field (the captures are dark themselves).
            pdf.set_draw_color(*LINE_GREY)
            pdf.set_line_width(0.3)
            pdf.rect(x_img, y_img, w, h, style="D")
            pdf.ln(1)
            if caption:
                pdf.set_font("Helvetica", "I", 8.5)
                pdf.set_text_color(*GREY)
                pdf.multi_cell(CONTENT_W, 4.5, sanitize(caption), align="C")
                pdf.set_text_color(*BLACK)
            pdf.ln(3)

    # --- appendix ------------------------------------------------------------
    pdf.add_page()
    pdf.section_name = "Parameter Reference"
    emit_heading(2, "Parameter Reference", None)
    pdf.set_text_color(*GREY)
    pdf.set_font("Helvetica", "I", 9)
    pdf.multi_cell(CONTENT_W, 5,
                   "This chapter is generated from the program source. "
                   "It lists every parameter with its range and its tooltip. "
                   "Regenerate the manual after a parameter change.",
                   align="L")
    pdf.ln(2)
    tips, grouped, total = appendix
    for title, members in grouped:
        emit_heading(3, "%s (%d parameters)" % (title, len(members)), None)
        rows = [["Parameter", "Range / Choices", "Description"]]
        for m in members:
            tip = tips.get(m["id"], "")
            rows.append(["%s  [%s]" % (m["label"], m["id"]),
                         range_text(m), sanitize(tip)])
        r.table(rows)
    pdf.set_text_color(*GREY)
    pdf.set_font("Helvetica", "I", 9)
    pdf.multi_cell(CONTENT_W, 5,
                   "%d parameters in %d groups." % (total, len(grouped)), align="L")

    # Attach link targets (both passes: page/y recorded in pass 1).
    if heading_pages is not None:
        for lid, text in link_ids:
            loc = heading_pages.get(text)
            if loc:
                pdf.set_link(lid, y=loc[0][1], page=loc[0][0])
    pdf.output(out_path)
    return len(pdf.pages), anchors


def main():
    version = "0.0.0"
    try:
        cmake = read(CMAKELISTS)
        m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", cmake)
        if m:
            version = m.group(1)
    except OSError:
        warn("CMakeLists.txt not readable; version falls back to %s" % version)

    intro, blocks = parse_manual(MANUAL_MD)

    appendix = (None, None, 0)
    try:
        rows, tips, grouped = build_appendix_data()
        appendix = (tips, grouped, len(rows))
        sys.stderr.write("APPENDIX: %d parameters, %d groups, %d tooltips\n"
                         % (len(rows), len(grouped), len(tips)))
    except Exception as exc:  # parsing must never kill the manual
        warn("appendix generation failed: %r" % (exc,))

    # Pass 1: record heading pages. Pass 2: real TOC numbers + links.
    _, anchors = build_pdf(version, intro, blocks, appendix, None, OUT_PDF)
    build_pdf(version, intro, blocks, appendix, anchors, OUT_PDF)

    size = os.path.getsize(OUT_PDF)
    print("Wrote %s (%d bytes)" % (OUT_PDF, size))
    if WARNINGS:
        print("%d warning(s) - see stderr" % len(WARNINGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
