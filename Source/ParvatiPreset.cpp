// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ParvatiPreset.h.

#include "ParvatiPreset.h"

#include <unordered_map>
#include <vector>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace parvati::preset
{
using juce::var;
// ===========================================================================
// Minimal YAML-subset parser/emitter.
//
// We emit and parse ONLY this constrained shape:
//   key: scalar            (scalar = int / float / double-quoted string)
//   key:                   (a nested block follows, deeper indented)
//     nested_key: scalar
//   key:                   (a list block follows)
//     - mapkey: scalar
//       mapkey2: scalar
// Indentation is 2 spaces per level. `#` starts a comment to end-of-line.
// The parser is indentation-driven and recursive; it is robust to blank lines,
// comments, CRLF, and trailing whitespace. Unknown keys are simply stored (and
// ignored by the preset layer) — that is the forward-compatibility mechanism.
// ===========================================================================
namespace
{
// One logical line: its indentation (leading spaces) and its trimmed content
// (with any trailing ` # comment` stripped). Blank / comment-only lines are
// dropped during tokenization.
struct Line
{
    int indent;
    juce::String text;   // already trimmed, comment-free
};

std::vector<Line> tokenize (const juce::String& text)
{
    std::vector<Line> out;
    for (const auto& raw : juce::StringArray::fromLines (text))
    {
        juce::String s = raw.replace ("\r", "");
        // Strip a trailing comment: a '#' that is NOT inside a quoted string.
        // (Do NOT trim leading whitespace yet — indentation drives nesting.)
        bool inQuote = false;
        for (int i = 0; i < s.length(); ++i)
        {
            const juce::juce_wchar c = s[i];
            if (c == '"') inQuote = ! inQuote;
            else if (c == '#' && ! inQuote) { s = s.substring (0, i); break; }
        }
        // Count leading spaces (the indentation), then take the content trimmed.
        int indent = 0;
        while (indent < s.length() && s[indent] == ' ') ++indent;
        const juce::String content = s.substring (indent).trim();
        if (content.isEmpty()) continue;   // blank / comment-only
        out.push_back ({ indent, content });
    }
    return out;
}

// Is @p t an unadorned integer (optional sign + digits only)?
bool isIntLiteral (const juce::String& t)
{
    if (t.isEmpty()) return false;
    int i = (t[0] == '-' || t[0] == '+') ? 1 : 0;
    if (i == t.length()) return false;   // a lone sign
    for (; i < t.length(); ++i)
        if (! juce::CharacterFunctions::isDigit (t[i])) return false;
    return true;
}

// Does @p t look like a float (so a string that collides with it should be
// quoted on emit)? Permissive: digits / sign / '.' / exponent only, with a '.'.
bool looksFloat (const juce::String& t)
{
    if (t.isEmpty() || ! t.contains (".")) return false;
    for (int i = 0; i < t.length(); ++i)
    {
        const juce::juce_wchar c = t[i];
        if (! (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-'
               || c == '+' || c == 'e' || c == 'E'))
            return false;
    }
    return true;
}

// Unescape the emitter's quoted-string escapes (\\" -> ", \\\\ -> \\). The
// emitter (emitScalar + the part-name writer) escapes both characters; the
// parser must mirror it or a name like `He said "hi"` round-trips with the
// literal backslashes left in.
juce::String unescapeQuoted (const juce::String& t)
{
    juce::String out;
    out.preallocateBytes ((size_t) t.getNumBytesAsUTF8());
    for (int i = 0; i < t.length(); ++i)
    {
        const juce::juce_wchar c = t[i];
        if (c == '\\' && i + 1 < t.length())
        {
            const juce::juce_wchar n = t[i + 1];
            if (n == '"' || n == '\\')
            {
                out += n;
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// Parse a scalar token to a juce::var (int / double / quoted-or-bare string).
var parseScalar (const juce::String& v)
{
    const juce::String t = v.trim();
    if (t.length() >= 2 && t.startsWith ("\"") && t.endsWith ("\""))
        return unescapeQuoted (t.substring (1, t.length() - 1));

    if (isIntLiteral (t))
        return t.getIntValue();
    if (looksFloat (t))
        return t.getDoubleValue();
    return t;   // bare string fallback
}

// Recursive descent over the tokenized lines [start..end), all at indent >=
// baseIndent. Returns the parsed node and consumes lines.
struct ParseResult { juce::var value; int next; };

// Depth cap for the recursive descent (bug hunt 2026-08-18, F-state-1): a
// crafted .parvati with thousands of increasing-indent lines would recurse
// once per level and overflow the stack (an uncaught host crash). The emitter
// never nests deeper than ~6; 64 is a generous ceiling. At the cap the parse
// refuses: it consumes nothing, so the whole document parses to a shallow /
// truncated object and the format-sniffing load validation rejects the file
// cleanly instead of crashing.
constexpr int kMaxYamlDepth = 64;

ParseResult parseBlock (const std::vector<Line>& lines, int i, int baseIndent, int depth = 0)
{
    if (depth > kMaxYamlDepth)
        return { juce::var(), i };   // refuse: consume nothing -> invalid doc downstream
    const int n = (int) lines.size();

    // Indentation of the next content line if it is a child of the current
    // (deeper than @p parentIndent), else -1. This is what makes the parser
    // robust to the emitter's 2-space indent (we never assume a fixed step).
    const auto childIndent = [&] (int idx, int parentIndent) -> int {
        if (idx >= n) return -1;
        return lines[(size_t) idx].indent > parentIndent ? lines[(size_t) idx].indent : -1;
    };

    // A list block? (first content line starts with "- ")
    if (i < n && lines[(size_t) i].indent == baseIndent && lines[(size_t) i].text.startsWith ("-"))
    {
        juce::Array<var> arr;
        while (i < n && lines[(size_t) i].indent == baseIndent && lines[(size_t) i].text.startsWith ("-"))
        {
            const juce::String afterDash = lines[(size_t) i].text.substring (1).trim();
            if (afterDash.isEmpty())
            {
                // list item is a nested block on the following (deeper) lines
                const int ci = childIndent (i + 1, baseIndent);
                if (ci < 0) { arr.add (var()); ++i; }
                else { auto pr = parseBlock (lines, i + 1, ci, depth + 1); arr.add (pr.value); i = pr.next; }
            }
            else
            {
                // inline "  - key: value" : a map whose first entry is on this
                // line; the map's column is baseIndent + 2 (after "- ").
                auto obj = std::make_unique<juce::DynamicObject>();
                const int itemIndent = baseIndent + 2;
                const int colon = afterDash.indexOf (":");
                if (colon > 0)
                {
                    const juce::String k = afterDash.substring (0, colon).trim();
                    const juce::String v = afterDash.substring (colon + 1).trim();
                    if (v.isEmpty())
                    {
                        const int ci = childIndent (i + 1, itemIndent);
                        if (ci < 0) { obj->setProperty (k, var()); ++i; }
                        else { auto pr = parseBlock (lines, i + 1, ci, depth + 1); obj->setProperty (k, pr.value); i = pr.next; }
                    }
                    else
                    {
                        obj->setProperty (k, parseScalar (v));
                        ++i;
                    }
                }
                else
                {
                    obj->setProperty ("0", parseScalar (afterDash));   // bare list item
                    ++i;
                }
                // Consume further keys of the SAME map at itemIndent.
                while (i < n && lines[(size_t) i].indent == itemIndent && ! lines[(size_t) i].text.startsWith ("-"))
                {
                    const juce::String& ln = lines[(size_t) i].text;
                    const int c = ln.indexOf (":");
                    if (c <= 0) break;
                    const juce::String k = ln.substring (0, c).trim();
                    const juce::String v = ln.substring (c + 1).trim();
                    if (v.isEmpty())
                    {
                        const int ci = childIndent (i + 1, itemIndent);
                        if (ci < 0) { obj->setProperty (k, var()); ++i; }
                        else { auto pr = parseBlock (lines, i + 1, ci, depth + 1); obj->setProperty (k, pr.value); i = pr.next; }
                    }
                    else
                    {
                        obj->setProperty (k, parseScalar (v));
                        ++i;
                    }
                }
                arr.add (var (obj.release()));
            }
        }
        return { var (arr), i };
    }

    // A map block.
    auto obj = std::make_unique<juce::DynamicObject>();
    while (i < n && lines[(size_t) i].indent == baseIndent && ! lines[(size_t) i].text.startsWith ("-"))
    {
        const juce::String& ln = lines[(size_t) i].text;
        const int colon = ln.indexOf (":");
        if (colon <= 0) break;   // not a valid map entry -> stop
        const juce::String k = ln.substring (0, colon).trim();
        const juce::String v = ln.substring (colon + 1).trim();
        if (v.isEmpty())
        {
            const int ci = childIndent (i + 1, baseIndent);
            if (ci < 0) { obj->setProperty (k, var()); ++i; }
            else { auto pr = parseBlock (lines, i + 1, ci, depth + 1); obj->setProperty (k, pr.value); i = pr.next; }
        }
        else
        {
            obj->setProperty (k, parseScalar (v));
            ++i;
        }
    }
    return { var (obj.release()), i };
}

// Escape a QUOTED top-level string value (patch / multi `name:`): the format
// is LINE-based, so a raw newline inside a name would split the document and
// break the parse (a dropped `params:` block = silent load failure), and an
// unescaped `"` truncates the name at reload. Mirrors the per-part name
// writer's escaping (\\ -> backslash, \" -> quote) plus a control-char
// strip (anything < 0x20 is dropped — the parser's unescapeQuoted is the
// exact inverse for the two escapes).
juce::String escapeQuotedValue (const juce::String& s)
{
    juce::String out;
    out.preallocateBytes ((size_t) s.getNumBytesAsUTF8());
    for (int i = 0; i < s.length(); ++i)
    {
        const auto c = s[i];
        if (c < 0x20)
            continue;   // control chars (incl. newlines) never enter the value
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}
}  // namespace

juce::var parseParvatiYaml (const juce::String& text)
{
    const auto lines = tokenize (text);
    if (lines.empty()) return {};
    // Find the base indentation of the first content line; everything top-level
    // sits at that indent.
    const int base = lines.front().indent;
    return parseBlock (lines, 0, base, 0).value;
}

// ---- emit ------------------------------------------------------------------
namespace
{
// Quote a string scalar if it could be misread as a number/bool/empty, else
// emit it bare (cleaner files).
juce::String emitScalar (const var& v)
{
    if (v.isInt() || v.isInt64())   return juce::String (v);
    if (v.isDouble())               return juce::String (v);
    if (v.isBool())                 return v ? "true" : "false";
    const juce::String s = v.toString();
    if (s.isEmpty())                return "\"\"";
    // Quote if it could be misread as a number/bool, or contains YAML-special chars.
    bool needQuote = isIntLiteral (s) || looksFloat (s)
                     || s == "true" || s == "false"
                     || s.contains (":") || s.contains ("#")
                     || s.startsWithChar ('-') || s.startsWithChar (' ')
                     || s.endsWithChar (' ') || s.contains ("\"") || s.contains ("{");
    if (needQuote)
        return "\"" + s.replace ("\"", "\\\"") + "\"";
    return s;
}

void emitMap (const juce::String& indent, const var& v, juce::String& out);
void emitNode (const juce::String& indent, const var& v, juce::String& out)
{
    if (auto* arr = v.getArray())
    {
        for (const auto& item : *arr)
        {
            if (auto* dyn = item.getDynamicObject())
            {
                // List item is a map: emit its first property inline after
                // `- `, the remaining properties indented to align.
                const auto& props = dyn->getProperties();
                if (props.size() == 0)
                {
                    out << indent << "- {}\n";
                    continue;
                }
                bool firstDone = false;
                for (const auto& p : props)
                {
                    if (! firstDone)
                    {
                        out << indent << "- " << p.name.toString() << ":";
                        if (p.value.isObject() || p.value.isArray())
                        {
                            out << "\n";
                            emitNode (indent + "  ", p.value, out);
                        }
                        else
                        {
                            out << " " << emitScalar (p.value) << "\n";
                        }
                        firstDone = true;
                    }
                    else
                    {
                        out << indent << "  " << p.name.toString() << ":";
                        if (p.value.isObject() || p.value.isArray())
                        {
                            out << "\n";
                            emitNode (indent + "  ", p.value, out);
                        }
                        else
                        {
                            out << " " << emitScalar (p.value) << "\n";
                        }
                    }
                }
            }
            else
            {
                out << indent << "- " << emitScalar (item) << "\n";
            }
        }
        return;
    }
    emitMap (indent, v, out);
}

void emitMap (const juce::String& indent, const var& v, juce::String& out)
{
    if (auto* dyn = v.getDynamicObject())
    {
        for (const auto& p : dyn->getProperties())
        {
            out << indent << p.name.toString() << ":";
            if (p.value.isObject() || p.value.isArray())
            {
                out << "\n";
                emitNode (indent + "  ", p.value, out);
            }
            else
            {
                out << " " << emitScalar (p.value) << "\n";
            }
        }
    }
}
}  // namespace

juce::String emitParvatiYaml (const juce::var& tree)
{
    juce::String out;
    emitNode ("", tree, out);
    return out;
}

// ===========================================================================
// Descriptor helpers (shared by patch + multi)
// ===========================================================================
namespace
{
// Is this descriptor a per-part sound value we serialize? (Excludes only
// `part_select`: it is the "which part am I editing" selector, not sound.)
bool isSerializable (const PatchParamDescriptor& d)
{
    return d.paramID != "part_select";
}

// The denormalized (raw) APVTS value for a descriptor on the CURRENT part.
float currentRaw (ParvatiAudioProcessor& proc, const PatchParamDescriptor& d)
{
    if (auto v = proc.getApvts().getRawParameterValue (d.paramID))
        return v->load();
    return 0.0f;
}

// The denormalized value for a descriptor on part @p partIndex, reconstructed
// from engine storage. Patch/Part byte params use parvatiPatchByteToValue; arp
// params read the MT-authoritative pendingConfig_ (NOT the live objects, which
// lag pendingConfig_ until the audio thread services configDirty_).
float partRaw (SynthEngine& engine, int partIndex, const PatchParamDescriptor& d)
{
    auto& part = engine.getPart (partIndex);
    if (d.isArp)
    {
        const auto pc = part.readPendingConfig();
        if (d.paramID == "arp_mode")       return static_cast<float> (pc.arpMode);
        if (d.paramID == "arp_direction")  return static_cast<float> (pc.arpDirection);
        if (d.paramID == "arp_octave")     return static_cast<float> (pc.arpOctave);
        if (d.paramID == "arp_pattern")    return static_cast<float> (pc.arpPattern);
        if (d.paramID == "arp_resolution") return static_cast<float> (pc.arpResolution);
        return 0.0f;
    }
    if (d.isSequencer)
    {
        const auto pc = part.readPendingConfig();
        if (d.paramID == "seq_length_1") return static_cast<float> (pc.seqLength[0]);
        if (d.paramID == "seq_length_2") return static_cast<float> (pc.seqLength[1]);
        if (d.paramID == "seq_length_3") return static_cast<float> (pc.seqLength[2]);
        // Step params: byteOffset is the controller PartData offset; the
        // sequence_data[] region is offset by -16 within PartData. Guard the
        // region exactly like the apply side (~:824 — bug hunt 2026-08-18,
        // F-state-6): only 16..79 map into seqData[0..63]; any other
        // descriptor byteOffset reads out of bounds. (Every seq-step
        // descriptor in the table sits in 16..79 today, so this is a
        // hardening guard, not a behaviour change.)
        if (d.byteOffset < 16 || d.byteOffset >= 80)
            return 0.0f;
        return static_cast<float> (pc.seqData[(size_t) (d.byteOffset - 16)]);
    }
    if (d.isFx)
    {
        // FX is per-part: read the raw value from this Part's fxState atomics.
        // The stored value IS the denormalized APVTS value (int/choice index),
        // so it is returned directly (no patch-byte decode). Regression guard:
        // without this branch the Patch/Part byte read below would index
        // patchBytes[-1] (byteOffset=-1) and crash on .parvati multi save.
        const juce::String id (d.paramID);
        const auto& fx = part.fxState;
        if (id.length() >= 4 && id[0] == 'f' && id[1] == 'x' && id[2] >= '1' && id[2] <= '3' && id[3] == '_')
        {
            const int slot = id[2] - '1';
            const juce::String sfx = id.substring (4);
            if (sfx == "type")              return (float) fx.slotType    [(size_t) slot].load();
            if (sfx == "enabled")           return (float) fx.slotEnabled [(size_t) slot].load();
            if (sfx == "drywet")            return (float) fx.slotDryWet  [(size_t) slot].load();
            if (sfx.startsWith ("param"))
            {
                const int k = sfx.substring (5).getIntValue();
                if (k >= 1 && k <= kNumFxSlotParams)
                    return (float) fx.slotParam[(size_t) slot][(size_t) (k - 1)].load();
            }
            return 0.0f;
        }
        if (id == "fx_topo")               return (float) fx.topology.load();
        if (id == "fx_order")              return (float) fx.orderIdx.load();
        // Master section (v3).
        if (id == "fx_mix")        return (float) fx.mix.load();
        if (id == "fx_eq_low")     return (float) fx.eqLow.load();
        if (id == "fx_eq_mid")     return (float) fx.eqMid.load();
        if (id == "fx_eq_high")    return (float) fx.eqHigh.load();
        if (id.startsWith ("fxmod") && id.contains ("_"))
        {
            const int under = id.indexOf ("_");
            const int m = id.substring (5, under).getIntValue();
            if (m >= 1 && m <= kNumFxMatrixSlots)
            {
                const juce::String sfx = id.substring (under + 1);
                if (sfx == "source")       return (float) fx.modSource [(size_t) (m - 1)].load();
                if (sfx == "dest")         return (float) fx.modDest   [(size_t) (m - 1)].load();
                if (sfx == "amount")       return (float) fx.modAmount [(size_t) (m - 1)].load();
            }
        }
        return 0.0f;
    }
    // Patch / Part byte param.
    const uint8_t byte = d.isPart ? part.partBytes[(size_t) d.byteOffset]
                                  : part.patchBytes[(size_t) d.byteOffset];
    return parvatiPatchByteToValue (d, byte);
}

// Build a `params:` map (DynamicObject) for the current part.
std::unique_ptr<juce::DynamicObject> currentParamsMap (ParvatiAudioProcessor& proc)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    for (const auto& d : getPatchParamDescriptors())
    {
        if (! isSerializable (d)) continue;
        obj->setProperty (juce::Identifier (d.paramID), juce::var (juce::roundToInt (currentRaw (proc, d))));
    }
    return obj;
}

// Build a `params:` map for part @p partIndex from engine storage.
std::unique_ptr<juce::DynamicObject> partParamsMap (SynthEngine& engine, int partIndex)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    for (const auto& d : getPatchParamDescriptors())
    {
        if (! isSerializable (d) || d.isOption) continue;   // options are global
        obj->setProperty (juce::Identifier (d.paramID), juce::var (juce::roundToInt (partRaw (engine, partIndex, d))));
    }
    return obj;
}

// Choice label for a descriptor at a given raw index, or empty.
juce::String choiceLabel (const PatchParamDescriptor& d, int index)
{
    if (d.choices != nullptr && index >= 0 && index < d.choices->size())
        return (*d.choices)[index];
    return {};
}

// Descriptor lookup by paramID. One static index replaces the repeated O(n)
// linear scans over the ~230-entry descriptor table. The table is a static
// vector, so the pointers stay valid for the process lifetime.
const PatchParamDescriptor* findDescriptor (const juce::String& id)
{
    static const std::unordered_map<juce::String, const PatchParamDescriptor*> index = []
    {
        std::unordered_map<juce::String, const PatchParamDescriptor*> m;
        for (const auto& d : getPatchParamDescriptors())
            m.emplace (d.paramID, &d);
        return m;
    }();
    const auto it = index.find (id);
    return it != index.end() ? it->second : nullptr;
}

// Emit a params map as YAML with inline ` # choice-label` comments.
// @p indent is the per-key prefix: "  " for a top-level map, "      " for
// a key inside a multi part entry. The comment column stays fixed.
juce::String emitParams (const juce::DynamicObject& obj, const char* indent = "  ")
{
    juce::String out;
    for (const auto& p : obj.getProperties())
    {
        out << indent << p.name.toString() << ": " << emitScalar (p.value);
        if (p.value.isInt() || p.value.isInt64())
        {
            // Annotate choice params (their raw value is the index).
            if (const PatchParamDescriptor* d = findDescriptor (p.name.toString()))
                if (d->choices != nullptr)
                {
                    const juce::String lbl = choiceLabel (*d, (int) p.value);
                    if (lbl.isNotEmpty())
                        out << "            # " << lbl;
                }
        }
        out << "\n";
    }
    return out;
}
}  // namespace

// ===========================================================================
// Patch (single, current part)
// ===========================================================================
juce::String serializeParvatiPatch (ParvatiAudioProcessor& proc)
{
    juce::String out;
    out << "# Parvati patch — human-editable. Unknown keys are ignored on load.\n";
    out << "format: " << kFormatPatch << "\n";
    out << "version: " << kFormatVersion << "\n";
    out << "parvati_version: " << kParvatiVersion << "\n";
    out << "name: \"" << escapeQuotedValue (proc.getLoadedProgramName().isNotEmpty() ? proc.getLoadedProgramName()
                                                                                    : juce::String ("Parvati")) << "\"\n";
    out << "author: \"\"\n";
    out << "params:\n";
    out << emitParams (*currentParamsMap (proc));
    return out;
}

bool applyParvatiPatch (ParvatiAudioProcessor& proc, const juce::String& yaml)
{
    const var tree = parseParvatiYaml (yaml);
    if (! tree.isObject()) return false;
    const var params = tree["params"];
    if (! params.isObject()) return false;

    auto* dyn = params.getDynamicObject();
    if (dyn == nullptr) return false;

    for (const auto& p : dyn->getProperties())
    {
        // Look the id up in the descriptor index; clamp to range.
        const PatchParamDescriptor* d = findDescriptor (p.name.toString());
        if (d == nullptr) continue;   // unknown key -> ignored (forward-compat)

        const float raw = (float) p.value;
        const float clamped = (d->choices != nullptr)
            ? juce::jlimit (0.0f, (float) (d->choices->size() - 1), raw)
            : juce::jlimit ((float) d->minValue, (float) d->maxValue, raw);

        if (auto* param = proc.getApvts().getParameter (d->paramID))
            param->setValueNotifyingHost (param->convertTo0to1 (clamped));
    }

    proc.syncAllParamsToEngine();
    return true;
}

// ===========================================================================
// Multi (all 6 parts)
// ===========================================================================
juce::String serializeParvatiMulti (ParvatiAudioProcessor& proc)
{
    SynthEngine& engine = proc.getEngine();
    juce::String out;
    out << "# Parvati multi — human-editable. 6-part multitimbral setup.\n";
    out << "format: " << kFormatMulti << "\n";
    out << "version: " << kFormatVersion << "\n";
    out << "parvati_version: " << kParvatiVersion << "\n";
    out << "name: \"" << escapeQuotedValue (proc.getLoadedProgramName().isNotEmpty() ? proc.getLoadedProgramName()
                                                                                    : juce::String ("Parvati")) << "\"\n";
    out << "author: \"\"\n";
    out << "parts:\n";

    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        out << "  - channel: " << (int) engine.getPartChannel (i) << "\n";
        out << "    keyzone_low: " << (int) engine.getPartKeyrangeLow (i) << "\n";
        out << "    keyzone_high: " << (int) engine.getPartKeyrangeHigh (i) << "\n";
        out << "    voice_allocation: " << (int) engine.getPartVoiceAllocation (i) << "\n";
        // Parvati extension: the per-part voice count (1..16; 0 = disabled
        // Part, restored via the legacy zero-mask path) + the user-facing
        // part name. voice_allocation above stays the DERIVED mask snapshot
        // (aux-out + .MUL export context).
        out << "    voice_slots: " << engine.getPartVoiceSlots (i) << "\n";
        const juce::String pn = engine.getPartName (i).replace ("\\", "\\\\").replace ("\"", "\\\"");
        out << "    name: \"" << pn << "\"\n";

        // Tuning: the raga preset rides params: part_raga (byte 4) — the
        // dedicated tuning_mode/tuning_offsets keys are LEGACY (the custom-table
        // subsystem was removed 2026-08-19) and are no longer emitted. Old
        // files that carry them still load (see applyParvatiMulti).

        auto params = partParamsMap (engine, i);
        out << "    params:\n";
        out << emitParams (*params, "      ");
    }

    out << "options:\n";
    // Global option params, emitted in descriptor-table order via the shared
    // emitter (same map shape as `params:`, so the comments match).
    auto options = std::make_unique<juce::DynamicObject>();
    for (const auto& d : getPatchParamDescriptors())
        if (d.isOption && isSerializable (d))
            options->setProperty (juce::Identifier (d.paramID),
                                 juce::var (juce::roundToInt (currentRaw (proc, d))));
    out << emitParams (*options);

    return out;
}

bool applyParvatiMulti (ParvatiAudioProcessor& proc, const juce::String& yaml)
{
    const var tree = parseParvatiYaml (yaml);
    if (! tree.isObject()) return false;
    const var partsVar = tree["parts"];
    auto* arr = partsVar.getArray();
    if (arr == nullptr) return false;

    SynthEngine& engine = proc.getEngine();

    const int n = juce::jmin ((int) arr->size(), SynthEngine::getNumParts());
    for (int i = 0; i < n; ++i)
    {
        const var& partNode = arr->getUnchecked (i);
        auto* partObj = partNode.getDynamicObject();
        if (partObj == nullptr) continue;
        auto& part = engine.getPart (i);

        // Per-part routing. Every key is OPTIONAL in the format (loaded
        // behind hasProperty guards), so a hand-edited entry without
        // `channel` / `keyzone_*` keys must NOT silently inherit the
        // PREVIOUS multi's routing (the file was written as the whole truth):
        // absent keys fall back to the engine's INIT defaults (channel =
        // partIndex + 1, zone 0..127 — SynthEngine.cpp ctor) instead. A
        // present key still wins, and the serializer always emits all three.
        // Hand-edited values are CLAMPED to the engine's accepted ranges
        // (channel 0=Omni..16, keyzone 0..127; the engine setters store
        // uint8 verbatim, so an out-of-range value like `channel: 300` would
        // wrap and silently deaden the part -- findPartForNote would never
        // match). An inverted zone (lo > hi) is normalized by swap: a zone
        // that matches no note is never useful, and the swap preserves the
        // editor's intent (both ends kept).
        engine.setPartChannel (i, partObj->hasProperty ("channel")
                                    ? static_cast<uint8_t> (juce::jlimit (0, 16, (int) partNode["channel"]))
                                    : static_cast<uint8_t> (i + 1));
        if (partObj->hasProperty ("keyzone_low") && partObj->hasProperty ("keyzone_high"))
        {
            // WRAP ZONES PRESERVED (W8 item 1): the firmware's accept_note
            // treats low > high as the complement set (the classic hardware
            // split trick, now ported in SynthEngine::partAcceptsNote), so the
            // loader must NOT swap an inverted zone — only clamp both ends to
            // 0..127. (The old jmin/jmax normalization silently turned a wrap
            // zone into its contiguous complement, inverting the patch's
            // audible keyboard coverage.)
            const int lo = juce::jlimit (0, 127, (int) partNode["keyzone_low"]);
            const int hi = juce::jlimit (0, 127, (int) partNode["keyzone_high"]);
            engine.setPartKeyrange (i, static_cast<uint8_t> (lo),
                                        static_cast<uint8_t> (hi));
        }
        else
        {
            engine.setPartKeyrange (i, 0, 127);   // init default zone
        }
        // The legacy bitmask seed is clamped to the 6 hardware voicecards
        // (bits 0..5): a hand-edited mask with the high bits set would
        // materialize a slot count the pool cannot honor.
        if (partObj->hasProperty ("voice_allocation"))
            engine.setPartVoiceAllocation (i, static_cast<uint8_t> (
                juce::jlimit (0, 0x3F, (int) partNode["voice_allocation"])));
        // Parvati extension: per-part voice slots + name. The slots are the
        // single source of truth (the allocation above is only a legacy seed:
        // an old file without slots materializes its card count from the
        // bitmask). A saved 0 (disabled Part) must stay 0 — the PUBLIC
        // setPartVoiceSlots clamps 0 to 1, so it rides the legacy disable
        // path instead (a zero mask materializes 0 slots).
        if (partObj->hasProperty ("voice_slots"))
        {
            const int slots = (int) partNode["voice_slots"];
            if (slots > 0)
                engine.setPartVoiceSlots (i, slots);
            else
                engine.setPartVoiceAllocation (i, 0);
        }
        if (partObj->hasProperty ("name"))
            engine.setPartName (i, partNode["name"].toString());

        // Per-part params. Patch/Part byte params are written into the Part's
        // patch/part storage; arp/seq params are staged into pendingConfig_ +
        // configDirty_ (NOT the live objects -- the audio thread is the sole
        // writer of those, and pendingConfig_ is the serialize source). This
        // keeps all 6 parts configured even though only one can be "current" in
        // the APVTS at a time, and mirrors loadMultiFile. (arp descriptors carry
        // byteOffset=-1 -- they are controller-side with no Patch byte -- so they
        // are dispatched by paramID; seq descriptors carry the real PartData
        // offset.)
        const var pmap = partNode["params"];
        bool stagedArpSeq = false;   // set configDirty_ ONCE after the loop (not per param), so the audio thread only ever services a complete pendingConfig_ snapshot
        bool stagedFx = false;       // set fxDirty_ ONCE after the loop (same reason as stagedArpSeq)
        if (auto* pobj = pmap.getDynamicObject())
        {
            for (const auto& p : pobj->getProperties())
            {
                const PatchParamDescriptor* d = findDescriptor (p.name.toString());
                if (d == nullptr || d->isOption) continue;   // options are global

                const float raw = (float) p.value;
                if (d->isArp)
                {
                    // Clamp to the DESCRIPTOR's own range (choices / int
                    // bounds) BEFORE staging — the same rule applyParvatiPatch
                    // applies via the APVTS and stageArpSeqFromPartBytes
                    // applies for .MUL/.PRO/blob bytes (the wave-5 clamps).
                    // Staging raw jlimit(0,255) bytes left a hand-edited
                    // `arp_mode: 5` live as an active-but-not-Arp/Sequencer
                    // part that silently swallowed notes (caught by
                    // parvati_load_invariants_test at authoring time).
                    const int v = (d->choices != nullptr)
                        ? juce::jlimit (0, d->choices->size() - 1, (int) raw)
                        : juce::jlimit (d->minValue, d->maxValue, (int) raw);
                    const uint8_t cv = static_cast<uint8_t> (v);
                    if (d->paramID == "arp_mode")            part.writePendingConfig ([cv] (auto& c) { c.arpMode = cv; });
                    else if (d->paramID == "arp_direction")  part.writePendingConfig ([cv] (auto& c) { c.arpDirection = cv; });
                    else if (d->paramID == "arp_octave")     part.writePendingConfig ([cv] (auto& c) { c.arpOctave = cv; });
                    else if (d->paramID == "arp_pattern")    part.writePendingConfig ([cv] (auto& c) { c.arpPattern = cv; });
                    else if (d->paramID == "arp_resolution") part.writePendingConfig ([cv] (auto& c) { c.arpResolution = cv; });
                    stagedArpSeq = true;
                }
                else if (d->isSequencer)
                {
                    // Same descriptor-range clamp as the arp branch above
                    // (seq lengths are 1..16 in the descriptor table; a staged
                    // 0 length wedges the sequencer's wrap logic).
                    const int v = (d->choices != nullptr)
                        ? juce::jlimit (0, d->choices->size() - 1, (int) raw)
                        : juce::jlimit (d->minValue, d->maxValue, (int) raw);
                    const uint8_t cv = static_cast<uint8_t> (v);
                    if (d->paramID == "seq_length_1")      part.writePendingConfig ([cv] (auto& c) { c.seqLength[0] = cv; });
                    else if (d->paramID == "seq_length_2") part.writePendingConfig ([cv] (auto& c) { c.seqLength[1] = cv; });
                    else if (d->paramID == "seq_length_3") part.writePendingConfig ([cv] (auto& c) { c.seqLength[2] = cv; });
                    else if (d->byteOffset >= 16 && d->byteOffset < 80)
                    { const int off = d->byteOffset - 16; part.writePendingConfig ([off,cv] (auto& c) { c.seqData[(size_t) off] = cv; }); }
                    stagedArpSeq = true;
                }
                else if (d->isFx)
                {
                    // FX is per-part: write the value directly into this Part's
                    // fxState atomics (relaxed: the fxDirty_ release-store after
                    // the loop publishes the whole frame). The stored value IS the
                    // denormalized APVTS value (int / choice index), stored verbatim
                    // -- no patch-byte encode (byteOffset=-1 would index
                    // patchBytes[-1]). Mirror the partRaw reader. fxDirty_ is staged
                    // ONCE after the per-part loop so the audio thread services a
                    // complete fxState snapshot.
                    const juce::String id (d->paramID);
                    const int v = juce::jlimit (0, 255, (int) raw);
                    auto& fx = part.fxState;
                    if (id.length() >= 4 && id[0] == 'f' && id[1] == 'x' && id[2] >= '1' && id[2] <= '3' && id[3] == '_')
                    {
                        const int slot = id[2] - '1';
                        const juce::String sfx = id.substring (4);
                        if (sfx == "type")
                        {
                            // Slot TYPES need message-thread chain staging (the
                            // AT's fxDirty_ service pushes params/enabled/etc.
                            // but deliberately never installs types — they need
                            // a pre-built processor, audit F1). Storing only the
                            // fxState atomic left the chain on its previous
                            // processors, so a loaded multi's FX were silently
                            // absent (fresh engine) or played the previous
                            // effect. stagePartFxSlotType = atomic + staging +
                            // the same fxDirty_ publish the loop tail makes.
                            engine.stagePartFxSlotType (i, slot, v);
                        }
                        else if (sfx == "enabled")      fx.slotEnabled [(size_t) slot].store ((uint8_t) (v != 0 ? 1 : 0), std::memory_order_relaxed);
                        else if (sfx == "drywet")       fx.slotDryWet  [(size_t) slot].store ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed);
                        else if (sfx.startsWith ("param"))
                        {
                            const int k = sfx.substring (5).getIntValue();
                            if (k >= 1 && k <= kNumFxSlotParams)
                                fx.slotParam[(size_t) slot][(size_t) (k - 1)].store ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed);
                        }
                        stagedFx = true;
                    }
                    else if (id == "fx_topo")  { fx.topology.store ((uint8_t) juce::jlimit (0, 2, v), std::memory_order_relaxed); stagedFx = true; }
                    else if (id == "fx_order") { fx.orderIdx.store  ((uint8_t) juce::jlimit (0, 5, v), std::memory_order_relaxed); stagedFx = true; }
                    // Master section (v3): global wet/dry + 3-band EQ.
                    else if (id == "fx_mix")        { fx.mix.store       ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed); stagedFx = true; }
                    else if (id == "fx_eq_low")     { fx.eqLow.store     ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed); stagedFx = true; }
                    else if (id == "fx_eq_mid")     { fx.eqMid.store     ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed); stagedFx = true; }
                    else if (id == "fx_eq_high")    { fx.eqHigh.store    ((uint8_t) juce::jlimit (0, 127, v), std::memory_order_relaxed); stagedFx = true; }
                    else if (id.startsWith ("fxmod") && id.contains ("_"))
                    {
                        const int under = id.indexOf ("_");
                        const int m = id.substring (5, under).getIntValue();
                        if (m >= 1 && m <= kNumFxMatrixSlots)
                        {
                            const juce::String sfx = id.substring (under + 1);
                            if (sfx == "source")      fx.modSource [(size_t) (m - 1)].store ((uint8_t) v, std::memory_order_relaxed);
                            else if (sfx == "dest")   fx.modDest   [(size_t) (m - 1)].store ((uint8_t) v, std::memory_order_relaxed);
                            else if (sfx == "amount") fx.modAmount [(size_t) (m - 1)].store ((int8_t) juce::jlimit (-63, 63, (int) raw), std::memory_order_relaxed);
                        }
                        stagedFx = true;
                    }
                }
                else
                {
                    const uint8_t byte = parvatiValueToPatchByte (*d, raw);
                    if (d->isPart) { if (d->byteOffset >= 0 && d->byteOffset < 84)  part.partBytes[(size_t) d->byteOffset]  = byte; }
                    else           { if (d->byteOffset >= 0 && d->byteOffset < 112) part.patchBytes[(size_t) d->byteOffset] = byte; }
                }
            }
        }
        if (stagedArpSeq)
            part.configDirty_.store (true, std::memory_order_release);
        if (stagedFx)
            part.fxState.fxDirty_.store (true, std::memory_order_release);

        // LEGACY tuning fields (applied AFTER params: so a file's tuning_mode
        // stays authoritative over any part_raga the params map carried):
        // tuning_mode 1..32 -> the raga preset byte; tuning_mode 0 or 33 (the
        // former custom mode), or a missing key, -> 12-EDO (byte 0). The
        // custom-table subsystem was removed 2026-08-19, so tuning_offsets is
        // parsed-and-ignored (never fails). A writer never emits these keys.
        if (partObj->hasProperty ("tuning_mode"))
        {
            const int mode = juce::jlimit (0, 33, (int) partNode["tuning_mode"]);
            part.partBytes[4] = (mode >= 1 && mode <= 32)
                                    ? static_cast<uint8_t> (mode)   // rides the markAllocationDirty push below
                                    : 0;                            // 12-EDO (incl. legacy custom 33)
        }

    }

    engine.markAllocationDirty();

    // Global options (vca_curve, filter_card) via the APVTS bridge (current part
    // is set to 0 below; options are global so the part doesn't matter).
    const var options = tree["options"];
    if (auto* oobj = options.getDynamicObject())
    {
        // voice_mode is a legacy global pref for the removed Extended (16-voice)
        // capacity mode. Keep PARSING it so older .parvati files still load, but
        // ignore the value (the engine is now always Hardware = 6 voices).
        if (oobj->hasProperty ("voice_mode"))
        {
            // Legacy Extended-capacity pref (now removed); parsed + ignored so
            // older .parvati multis still load. Do NOT call setUiVoiceMode.
            (void) (int) oobj->getProperty ("voice_mode");
        }
        for (const auto& p : oobj->getProperties())
        {
            // W11 (F-state-5): only REAL option descriptors may be applied
            // here — the serializer emits only isOption params under
            // `options:`, but this loop used to accept ANY APVTS paramID, so a
            // hand-edited per-part key (e.g. osc1_shape) wrote into the
            // PRE-LOAD current part (part_select is reset to Part 0 only
            // below). part_select itself is excluded: applying it mid-load
            // re-enters the part-switch path against half-loaded state.
            // (voice_mode stays parsed-and-ignored above for legacy files.)
            const PatchParamDescriptor* d = findDescriptor (p.name.toString());
            if (d == nullptr || ! d->isOption || d->paramID == "part_select")
                continue;
            if (auto* param = proc.getApvts().getParameter (p.name.toString()))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) p.value));
        }
    }

    // Show Part 0 in the editor. Engine storage is authoritative after the
    // multi-load; refresh the APVTS one-way (engine→APVTS display only). NO
    // syncAllParamsToEngine() — pushing the (Part-0-only) APVTS back would
    // clobber Part 0's loaded bytes with stale values.
    proc.getApvts().getParameter ("part_select")->setValueNotifyingHost (
        proc.getApvts().getParameter ("part_select")->convertTo0to1 (1.0f));
    proc.loadPartIntoApvts (0);
    return true;
}

juce::String detectParvatiFormat (const juce::String& yaml)
{
    for (const auto& raw : juce::StringArray::fromLines (yaml))
    {
        const juce::String s = raw.trim();
        if (s.isEmpty() || s.startsWith ("#")) continue;
        if (s.startsWith ("format:"))
        {
            const juce::String v = s.substring (7).trim();
            if (v == kFormatPatch || v == kFormatMulti) return v;
            return {};
        }
        // The first non-blank/non-comment line wasn't `format:`: not ours.
        if (! s.startsWith ("format:")) return {};
    }
    return {};
}
}  // namespace parvati::preset
