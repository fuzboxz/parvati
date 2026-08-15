// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ParvatiPreset.h.

#include "ParvatiPreset.h"

#include <vector>

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "TuningTables.h"     // kTuningSilence (sentinel-preserving clamp below)

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

ParseResult parseBlock (const std::vector<Line>& lines, int i, int baseIndent)
{
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
                else { auto pr = parseBlock (lines, i + 1, ci); arr.add (pr.value); i = pr.next; }
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
                        else { auto pr = parseBlock (lines, i + 1, ci); obj->setProperty (k, pr.value); i = pr.next; }
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
                        else { auto pr = parseBlock (lines, i + 1, ci); obj->setProperty (k, pr.value); i = pr.next; }
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
            else { auto pr = parseBlock (lines, i + 1, ci); obj->setProperty (k, pr.value); i = pr.next; }
        }
        else
        {
            obj->setProperty (k, parseScalar (v));
            ++i;
        }
    }
    return { var (obj.release()), i };
}
}  // namespace

juce::var parseParvatiYaml (const juce::String& text)
{
    const auto lines = tokenize (text);
    if (lines.empty()) return {};
    // Find the base indentation of the first content line; everything top-level
    // sits at that indent.
    const int base = lines.front().indent;
    return parseBlock (lines, 0, base).value;
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
        // sequence_data[] region is offset by -16 within PartData.
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

// Emit the params map as YAML with inline ` # choice-label` comments.
juce::String emitParams (const juce::DynamicObject& obj)
{
    juce::String out;
    for (const auto& p : obj.getProperties())
    {
        out << "  " << p.name.toString() << ": " << emitScalar (p.value);
        if (p.value.isInt() || p.value.isInt64())
        {
            // Annotate choice params (their raw value is the index).
            const auto& descs = getPatchParamDescriptors();
            for (const auto& d : descs)
                if (d.paramID == p.name.toString() && d.choices != nullptr)
                {
                    const juce::String lbl = choiceLabel (d, (int) p.value);
                    if (lbl.isNotEmpty())
                        out << "            # " << lbl;
                    break;
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
    out << "name: \"" << (proc.getLoadedProgramName().isNotEmpty() ? proc.getLoadedProgramName() : "Parvati") << "\"\n";
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
        // Look the id up in the descriptor table; clamp to range.
        const auto& descs = getPatchParamDescriptors();
        const PatchParamDescriptor* d = nullptr;
        for (const auto& desc : descs)
            if (desc.paramID == p.name.toString()) { d = &desc; break; }
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
    out << "name: \"" << (proc.getLoadedProgramName().isNotEmpty() ? proc.getLoadedProgramName() : "Parvati") << "\"\n";
    out << "author: \"\"\n";
    out << "parts:\n";

    for (int i = 0; i < SynthEngine::getNumParts(); ++i)
    {
        out << "  - channel: " << (int) engine.getPartChannel (i) << "\n";
        out << "    keyzone_low: " << (int) engine.getPartKeyrangeLow (i) << "\n";
        out << "    keyzone_high: " << (int) engine.getPartKeyrangeHigh (i) << "\n";
        out << "    voice_allocation: " << (int) engine.getPartVoiceAllocation (i) << "\n";
        // Parvati extension: per-part voice slots (0 = AUTO: one voice per
        // allocated card, faithful hardware) + the user-facing part name.
        out << "    voice_slots: " << engine.getPartVoiceSlots (i) << "\n";
        const juce::String pn = engine.getPartName (i).replace ("\\", "\\\\").replace ("\"", "\\\"");
        out << "    name: \"" << pn << "\"\n";

        // Parvati extension: per-part microtonal tuning. tuning_mode is the
        // RESOLVED mode (0 = 12-EDO [omitted], 1..32 = raga preset [also rides
        // params: part_raga], 33 = custom table). Only mode 33 needs the table
        // itself (12 comma-separated ints, 1/128-semitone units) — the presets
        // resolve from TuningTables. Written only when the mode is non-zero so
        // old files stay byte-identical; loaded behind hasProperty guards.
        const int tuningMode = engine.resolvedTuningMode (i);
        if (tuningMode != 0)
        {
            out << "    tuning_mode: " << tuningMode;
            if (tuningMode == 33)
            {
                int16_t t[12] = {};
                engine.resolveTuningOffsets (i, t);
                out << "   # custom table";
                out << "\n    tuning_offsets: ";
                for (int c = 0; c < 12; ++c)
                {
                    if (c > 0) out << ", ";
                    out << (int) t[c];
                }
            }
            out << "\n";
        }

        auto params = partParamsMap (engine, i);
        out << "    params:\n";
        for (const auto& p : params->getProperties())
        {
            out << "      " << p.name.toString() << ": " << emitScalar (p.value);
            if (p.value.isInt() || p.value.isInt64())
            {
                const auto& descs = getPatchParamDescriptors();
                for (const auto& d : descs)
                    if (d.paramID == p.name.toString() && d.choices != nullptr)
                    {
                        const juce::String lbl = choiceLabel (d, (int) p.value);
                        if (lbl.isNotEmpty()) out << "            # " << lbl;
                        break;
                    }
            }
            out << "\n";
        }
    }

    out << "options:\n";
    for (const auto& d : getPatchParamDescriptors())
        if (d.isOption && isSerializable (d))
        {
            out << "  " << d.paramID << ": " << emitScalar (juce::var (juce::roundToInt (currentRaw (proc, d))));
            if (d.choices != nullptr)
            {
                const juce::String lbl = choiceLabel (d, juce::roundToInt (currentRaw (proc, d)));
                if (lbl.isNotEmpty()) out << "            # " << lbl;
            }
            out << "\n";
        }

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
    const auto& descs = getPatchParamDescriptors();

    const int n = juce::jmin ((int) arr->size(), SynthEngine::getNumParts());
    for (int i = 0; i < n; ++i)
    {
        const var& partNode = arr->getUnchecked (i);
        auto* partObj = partNode.getDynamicObject();
        if (partObj == nullptr) continue;
        auto& part = engine.getPart (i);

        // Per-part routing.
        if (partObj->hasProperty ("channel"))
            engine.setPartChannel (i, (uint8_t) (int) partNode["channel"]);
        if (partObj->hasProperty ("keyzone_low") && partObj->hasProperty ("keyzone_high"))
            engine.setPartKeyrange (i, (uint8_t) (int) partNode["keyzone_low"],
                                        (uint8_t) (int) partNode["keyzone_high"]);
        if (partObj->hasProperty ("voice_allocation"))
            engine.setPartVoiceAllocation (i, (uint8_t) (int) partNode["voice_allocation"]);
        // Parvati extension: per-part voice slots + name (absent in older files
        // -> AUTO slots + empty name, i.e. faithful hardware behaviour).
        if (partObj->hasProperty ("voice_slots"))
            engine.setPartVoiceSlots (i, (int) partNode["voice_slots"]);
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
                const PatchParamDescriptor* d = nullptr;
                for (const auto& desc : descs)
                    if (desc.paramID == p.name.toString()) { d = &desc; break; }
                if (d == nullptr || d->isOption) continue;   // options are global

                const float raw = (float) p.value;
                if (d->isArp)
                {
                    const uint8_t v = (uint8_t) juce::jlimit (0, 255, (int) raw);
                    if (d->paramID == "arp_mode")            part.writePendingConfig ([v] (auto& c) { c.arpMode = v; });
                    else if (d->paramID == "arp_direction")  part.writePendingConfig ([v] (auto& c) { c.arpDirection = v; });
                    else if (d->paramID == "arp_octave")     { const uint8_t o = (uint8_t) juce::jlimit (1, 4, (int) raw); part.writePendingConfig ([o] (auto& c) { c.arpOctave = o; }); }
                    else if (d->paramID == "arp_pattern")    part.writePendingConfig ([v] (auto& c) { c.arpPattern = v; });
                    else if (d->paramID == "arp_resolution") part.writePendingConfig ([v] (auto& c) { c.arpResolution = v; });
                    stagedArpSeq = true;
                }
                else if (d->isSequencer)
                {
                    const uint8_t v = (uint8_t) juce::jlimit (0, 255, (int) raw);
                    if (d->paramID == "seq_length_1")      part.writePendingConfig ([v] (auto& c) { c.seqLength[0] = v; });
                    else if (d->paramID == "seq_length_2") part.writePendingConfig ([v] (auto& c) { c.seqLength[1] = v; });
                    else if (d->paramID == "seq_length_3") part.writePendingConfig ([v] (auto& c) { c.seqLength[2] = v; });
                    else if (d->byteOffset >= 16 && d->byteOffset < 80)
                    { const int off = d->byteOffset - 16; part.writePendingConfig ([off,v] (auto& c) { c.seqData[(size_t) off] = v; }); }
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
                        if (sfx == "type")              fx.slotType    [(size_t) slot].store ((uint8_t) v, std::memory_order_relaxed);
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

        // Parvati extension: per-part tuning fields, applied AFTER params: so
        // the resolved mode stays authoritative over any part_raga value the
        // params map carried (a writer never emits both non-neutral; the
        // custom mode clears byte 4 to keep the D4 invariant — custom active
        // implies raga byte 0). Absent in older files -> tuning untouched.
        if (partObj->hasProperty ("tuning_mode"))
        {
            const int mode = juce::jlimit (0, 33, (int) partNode["tuning_mode"]);
            if (mode == 33 && partObj->hasProperty ("tuning_offsets"))
            {
                int16_t t[12] = {};   // missing/short entries stay 0 (12-EDO class)
                const juce::StringArray toks = juce::StringArray::fromTokens (
                    partNode["tuning_offsets"].toString(), ",", "");
                for (int c = 0; c < 12; ++c)
                {
                    // Clamp exactly like SynthEngine::setPartTuningCustom —
                    // including its one exception: 32767 is the firmware mute
                    // marker, not an offset. Pre-clamping it to +127 would turn
                    // a muted class (e.g. a kbm-unmapped class written by a
                    // Scala import) into a ~+99-cent detune on reload, while
                    // engine-state v7 and the TuningEditor both keep it verbatim.
                    const int v = toks[c].getIntValue();
                    t[c] = (v == (int) parvati::kTuningSilence)
                               ? parvati::kTuningSilence
                               : static_cast<int16_t> (juce::jlimit (-127, 127, v));
                }
                part.partBytes[4] = 0;   // D4: custom active implies raga byte 0
                engine.setPartTuningCustom (i, t);   // clamps again + flags tuningDirty_
            }
            else if (mode >= 1 && mode <= 32)
            {
                part.partBytes[4] = static_cast<uint8_t> (mode);   // rides the markAllocationDirty push below
                engine.clearPartTuningCustom (i);
            }
            else   // explicit 12-EDO: clear both selection paths
            {
                part.partBytes[4] = 0;
                engine.clearPartTuningCustom (i);
            }
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
            if (p.name.toString() != "voice_mode")
                if (auto* param = proc.getApvts().getParameter (p.name.toString()))
                    param->setValueNotifyingHost (param->convertTo0to1 ((float) p.value));
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
