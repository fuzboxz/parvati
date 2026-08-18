// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ScalaImport.h.

#include "ScalaImport.h"

#include "TuningTables.h"   // kTuningSilence

#include <cmath>
#include <cstdlib>
#include <vector>

namespace parvati
{
namespace
{
// ----------------------------------------------------------------------------
// Tokenizing / parsing helpers (C-locale only; deterministic).
// ----------------------------------------------------------------------------

// Strip a '!' comment (may start anywhere) and trim blanks. Returns an empty
// string for blank / pure-comment lines.
juce::String cleanLine (const juce::String& raw)
{
    const int bang = raw.indexOfChar ('!');
    juce::String s = bang >= 0 ? raw.substring (0, bang) : raw;
    return s.trim();
}

bool isDigits (const juce::String& t)
{
    if (t.isEmpty())
        return false;
    for (int i = 0; i < t.length(); ++i)
        if (t[i] < '0' || t[i] > '9')
            return false;
    return true;
}

// Parse an optionally-negative decimal in the C locale. Accepts exactly:
// [-] digits [. digits] — no exponents, no thousands separators, no ','.
// Rejects everything else (returns false, value untouched).
bool parseCDouble (const juce::String& t, double& out)
{
    juce::String s = t.trim();
    if (s.isEmpty())
        return false;
    int i = 0;
    if (s[i] == '-')
        ++i;
    int digits = 0, dots = 0;
    for (; i < s.length(); ++i)
    {
        const juce::juce_wchar c = s[i];
        if (c >= '0' && c <= '9') { ++digits; continue; }
        if (c == '.')            { if (++dots > 1) return false; continue; }
        return false;   // '+', ',', 'e', letters, ... => error
    }
    if (digits == 0)
        return false;
    out = std::strtod (s.toRawUTF8(), nullptr);   // C locale is default & unmodified here
    return true;
}

// A .scl pitch token: cents (contains '.'), ratio a/b, or bare integer n/1.
struct PitchValue { double cents = 0.0; bool valid = false; };

PitchValue parsePitchToken (const juce::String& token)
{
    PitchValue pv;
    if (token.isEmpty())
        return pv;
    if (token.contains ("."))
    {
        double c = 0.0;
        if (! parseCDouble (token, c))
            return pv;
        pv.cents = c;
        pv.valid = true;
        return pv;
    }
    const int slash = token.indexOfChar ('/');
    if (slash < 0)
    {
        // Bare integer: ratio n/1. Must be digits (a sign is rejected — a
        // negative ratio has no meaning as a scale step).
        if (! isDigits (token))
            return pv;
        const long long n = token.getLargeIntValue();
        if (n <= 0)
            return pv;
        pv.cents = 1200.0 * std::log2 (static_cast<double> (n));
        pv.valid = true;
        return pv;
    }
    const juce::String num = token.substring (0, slash);
    const juce::String den = token.substring (slash + 1);
    if (! isDigits (num) || ! isDigits (den))
        return pv;
    const long long n = num.getLargeIntValue();
    const long long d = den.getLargeIntValue();
    if (n <= 0 || d <= 0)
        return pv;
    pv.cents = 1200.0 * std::log2 (static_cast<double> (n) / static_cast<double> (d));
    pv.valid = true;
    return pv;
}

struct SclFile
{
    juce::String error;
    int n = 0;                       // declared tone count
    std::vector<double> toneCents;   // tones 1..n (cents above 1/1), index 0..n-1
};

SclFile parseScl (const juce::String& text)
{
    SclFile f;
    const auto lines = juce::StringArray::fromLines (text);
    // Line 1 is the description (by position, whatever it contains). The tone
    // count is the first NON-comment / non-blank line after it — the normative
    // format allows '!' comment lines between the description and the count
    // (the canonical corpus files all have one there).
    if (lines.size() < 2)
    {
        f.error = "scl: missing tone count (need a description line + a count line)";
        return f;
    }
    int i = 1;
    juce::String countToken;
    while (i < lines.size())
    {
        const juce::String s = cleanLine (lines[i]);
        ++i;
        if (s.isEmpty())
            continue;   // comment / blank line before the count
        countToken = s;
        break;
    }
    if (countToken.isEmpty())
    {
        f.error = "scl: missing tone count (need a description line + a count line)";
        return f;
    }
    if (! isDigits (countToken))
    {
        f.error = "scl: tone count is not a non-negative integer: \"" + countToken + "\"";
        return f;
    }
    f.n = countToken.getIntValue();
    if (f.n < 1 || f.n > 1024)
    {
        f.error = "scl: tone count out of range (1..1024): " + juce::String (f.n);
        return f;
    }
    while (i < lines.size() && (int) f.toneCents.size() < f.n)
    {
        const juce::String s = cleanLine (lines[i]);
        ++i;
        if (s.isEmpty())
            continue;   // blank / pure-comment line inside the pitch block
        // First whitespace-delimited token is the value; anything after it is
        // ignored ("9/8 supermajor second", "100.0 ! comment").
        juce::StringArray parts = juce::StringArray::fromTokens (s, " \t", "");
        if (parts.size() == 0)
            continue;
        const juce::String token = parts[0];
        if (token.isEmpty())
            continue;
        const PitchValue pv = parsePitchToken (token);
        if (! pv.valid)
        {
            f.error = "scl: unparseable pitch (line " + juce::String (i) + "): \"" + token + "\"";
            return f;
        }
        f.toneCents.push_back (pv.cents);
    }
    if ((int) f.toneCents.size() < f.n)
    {
        f.error = "scl: declared " + juce::String (f.n) + " tones but found only "
                + juce::String ((int) f.toneCents.size());
        return f;
    }
    return f;
}

struct KbmFile
{
    juce::String error;
    int s = -1, first = 0, last = 0, m = 60, r = 60, o = -1;
    double freq = 261.6255653;
    std::vector<int> keys;   // -1 = 'x' (unmapped)
};

KbmFile parseKbm (const juce::String& text)
{
    KbmFile f;
    // Non-empty lines after comment/blank stripping, in order.
    juce::StringArray content;
    for (const auto& raw : juce::StringArray::fromLines (text))
    {
        const juce::String s = cleanLine (raw);
        if (s.isNotEmpty())
            content.add (s);
    }
    // 7 header values (S, first, last, M, R, F, o), then S key lines.
    juce::StringArray toks;
    for (const auto& l : content)
        toks.addTokens (l, " \t", "");

    const auto nextToken = [&] (const juce::String& what, juce::String& out) -> bool
    {
        if (toks.size() == 0)
        {
            f.error = "kbm: truncated file: missing " + what;
            return false;
        }
        out = toks[0];
        toks.remove (0);
        return true;
    };

    juce::String t;
    if (! nextToken ("size (S)", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: size (S) is not a non-negative integer: \"" + t + "\""; return f; }
    f.s = t.getIntValue();
    // Hostile-input gate (bug hunt 2026-08-18, F-static-3): S is used for
    // keys.reserve() below, so a crafted S (e.g. 999999999) would try to
    // reserve gigabytes and throw bad_alloc (an uncaught host crash). Cap the
    // UPPER bound at parse time, mirroring parseScl's tone-count gate. S = 0
    // deliberately flows on: reserve(0) is free, and the specific
    // "S = 0 (linear map) is unsupported" diagnostic in importScala is the
    // better error for that semantic case (pinned by the existing test).
    if (f.s < 0 || f.s > 1024)
    {
        f.error = "kbm: size (S) out of range (1..1024): \"" + t + "\"";
        return f;
    }

    if (! nextToken ("first note", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: first note is not an integer: \"" + t + "\""; return f; }
    f.first = t.getIntValue();

    if (! nextToken ("last note", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: last note is not an integer: \"" + t + "\""; return f; }
    f.last = t.getIntValue();

    if (! nextToken ("middle note (M)", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: middle note (M) is not an integer: \"" + t + "\""; return f; }
    f.m = t.getIntValue();

    if (! nextToken ("reference note (R)", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: reference note (R) is not an integer: \"" + t + "\""; return f; }
    f.r = t.getIntValue();

    if (! nextToken ("reference frequency (F)", t)) return f;
    if (! parseCDouble (t, f.freq)) { f.error = "kbm: reference frequency (F) is not a decimal number: \"" + t + "\""; return f; }
    if (! (f.freq > 0.0)) { f.error = "kbm: reference frequency (F) must be positive"; return f; }

    if (! nextToken ("formal octave (o)", t)) return f;
    if (! isDigits (t)) { f.error = "kbm: formal octave key (o) is not an integer: \"" + t + "\""; return f; }
    f.o = t.getIntValue();

    // Key lines: exactly S entries (stricter than the official template's
    // optional trailing-unmapped rule — deterministic, matches Surge).
    f.keys.reserve ((size_t) juce::jmax (0, f.s));
    for (int k = 0; k < f.s; ++k)
    {
        if (! nextToken ("key entry " + juce::String (k + 1), t)) return f;
        if (t == "x" || t == "X")
        {
            f.keys.push_back (-1);
            continue;
        }
        if (! isDigits (t)) { f.error = "kbm: key entry is not an integer or 'x': \"" + t + "\""; return f; }
        f.keys.push_back (t.getIntValue());
    }
    return f;
}
}  // namespace

ScalaImportResult importScala (const juce::String& sclText, const juce::String& kbmText)
{
    ScalaImportResult res;

    const SclFile scl = parseScl (sclText);
    if (! scl.error.isEmpty())
    {
        res.error = scl.error;
        return res;
    }

    // Resolve the effective mapping (defaults when no kbm — see header).
    int S, first, last, M, R, o;
    double F;
    std::vector<int> keys;
    if (kbmText.trim().isNotEmpty())
    {
        const KbmFile kbm = parseKbm (kbmText);
        if (! kbm.error.isEmpty())
        {
            res.error = kbm.error;
            return res;
        }
        S = kbm.s; first = kbm.first; last = kbm.last;
        M = kbm.m; R = kbm.r; F = kbm.freq; o = kbm.o;
        keys = kbm.keys;
        juce::ignoreUnused (first, last);   // decorative for a full 12-key map
        if (S == 0)
        {
            res.error = "kbm: S = 0 (linear map) is unsupported — Parvati needs a 12-key pattern "
                       "(octave-repeating table)";
            return res;
        }
    }
    else
    {
        S = scl.n; M = 60; R = 60; F = 261.6255653; o = scl.n;
        keys.resize ((size_t) S);
        for (int k = 0; k < S; ++k) keys[(size_t) k] = k;
    }

    // Semantic gates (each cites its numbers in the error).
    if (S != 12)
    {
        res.error = "unsupported mapping size: S = " + juce::String (S)
                  + " (Parvati tables are 12-key / octave-repeating)";
        return res;
    }
    if (o < 1 || o > scl.n)
    {
        res.error = "kbm: formal octave key o = " + juce::String (o)
                  + " is outside the scale (1.." + juce::String (scl.n) + ")";
        return res;
    }
    for (int k = 0; k < S; ++k)
        if (keys[(size_t) k] > o)
        {
            res.error = "kbm: key " + juce::String (k) + " maps to degree " + juce::String (keys[(size_t) k])
                      + " beyond the formal octave (o = " + juce::String (o) + ")";
            return res;
        }
    if (std::abs (M - R) > 1024 * S)
    {
        res.error = "kbm: |M - R| = " + juce::String (std::abs (M - R)) + " is implausibly large";
        return res;
    }
    // Reference key must itself be mapped (there is no defensible offset for a
    // scale whose anchor is muted).
    const int rPrime = ((R - M) % 12 + 12) % 12;   // keys[] index holding key R
    if (keys[(size_t) rPrime] < 0)
    {
        res.error = "kbm: reference note R = " + juce::String (R) + " falls on an unmapped ('x') key";
        return res;
    }

    // Hardware gate: the period must be the 2/1 octave (in 1/128-st units the
    // octave is exactly 1536; any other period cannot be represented by an
    // octave-repeating per-class table — rejected, not approximated).
    const double periodCents = scl.toneCents[(size_t) (o - 1)];
    if (std::llround (periodCents * 1.28) != 1536)
    {
        res.error = "non-octave scale: formal-octave period is " + juce::String (periodCents, 4)
                  + " cents (1200 required) — Parvati tables are octave-repeating";
        return res;
    }

    // The 12 consecutive keys n = M + x cover every note class exactly once;
    // for x in 0..11 the octave term floor((n - M) / 12) is always 0.
    double dev[12];
    bool unmapped[12];
    for (int x = 0; x < 12; ++x)
    {
        const int d = keys[(size_t) x];
        if (d < 0)
        {
            unmapped[x] = true;
            dev[x] = 0.0;
            continue;
        }
        unmapped[x] = false;
        const double rel = (d == 0) ? 0.0 : scl.toneCents[(size_t) (d - 1)];
        dev[x] = rel - 100.0 * x;
    }

    // Reference terms: the standing deviation of key R's class and the offset
    // of its actual frequency from 12-EDO at that MIDI number.
    const double devR = dev[rPrime];
    const double refHz = 440.0 * std::pow (2.0, (R - 69) / 12.0);
    const double refOffCents = 1200.0 * std::log2 (F / refHz);

    juce::StringArray clamped, muted;
    for (int x = 0; x < 12; ++x)
    {
        const int noteClass = (((M + x) % 12) + 12) % 12;
        if (unmapped[x])
        {
            res.offsets[noteClass] = kTuningSilence;
            muted.add (juce::String (noteClass));
            continue;
        }
        const double offCents = dev[x] - devR + refOffCents;
        long long units = std::llround (offCents * 1.28);   // the ONLY rounding
        if (units > 127)
        {
            units = 127;
            clamped.add (juce::String (noteClass));
        }
        else if (units < -127)
        {
            units = -127;
            clamped.add (juce::String (noteClass));
        }
        res.offsets[noteClass] = static_cast<int16_t> (units);
    }

    // Always-on / situational warnings.
    res.warnings.add ("Offsets quantized to 1/128 semitone (~0.78 cents) — "
                      "hardware oscillator resolution");
    if (clamped.size() > 0)
        res.warnings.add ("Classes clamped to ±127 (≈ ±99 cents): " + clamped.joinIntoString (", "));
    if (muted.size() > 0)
        res.warnings.add ("Unmapped note classes muted (" + juce::String (kTuningSilence)
                          + " sentinel): " + muted.joinIntoString (", "));
    if (scl.n > 12)
        res.warnings.add ("Scale has " + juce::String (scl.n)
                          + " tones; only the 12 mapped degrees are used (subset mapping)");
    if (scl.n < 12)
        res.warnings.add ("Scale has " + juce::String (scl.n)
                          + " tones; some degrees serve multiple keys (degree duplication)");

    res.ok = true;
    return res;
}
}  // namespace parvati
