// Min-width header layout sweep (deterministic tooling, tool 11).
//
// The bug class: ParvatiEditor::resized() packs the header from FIXED budgets
// (right cluster ~486pt of icon buttons + Load/Save, logo block, then the left
// cluster preset/Patch/Part/Synth/FX), and juce::Rectangle::removeFromLeft/
// Right CLIP to the remaining width. Below the budget the left-cluster controls
// collapse first — historically presetW=168 squeezed [FX] to ~38px of its
// designed 50 — and below ~575pt even the icon buttons go zero-width
// (invisible AND untouchable). The desktop resize floor is 1024x500
// (setResizeLimits), so this sweep pins the WHOLE legal desktop range.
//
// BELOW 1024 IS DELIBERATELY NOT SWEPT: AUv3 hosts force-resize the editor
// below the floor (JUCE's AUv3 wrapper setBounds bypasses the constrainer) and
// that sub-floor collapse is the KNOWN DEFERRED item (audit/IPAD_TOUCH_TODO.md,
// "AUv3 pane header collapse — adaptive header design"). The sweep floor IS
// the desktop resize minimum; a fix for the AUv3 item should LOWER this floor,
// not raise it.
//
// What is asserted at every swept size (headless editor, real resized() pass):
//   [1] PLACED INTERACTIVE HEADER CHILDREN KEEP POSITIVE EXTENT: every
//       effectively-visible juce::Button / juce::ComboBox descendant (direct
//       children + ONE level deep — PresetBrowser's inner name button is a
//       grandchild) whose editor-relative bounds intersect the header strip
//       (kHeaderH + rule clearance = 50px) and is not parked fully offscreen
//       (the closed Settings SidePanel idiom) has width > 0 AND height > 0.
//       ONE documented exception: the folded zoom trio "+"/"-"/"0" —
//       constructed + addAndMakeVisible'd but deliberately NEVER placed
//       (their logic is reused by the "..." overflow popup; see the resized()
//       header comment) — stay parked at (0,0,0,0) by design.
//   [2] THE HISTORICAL TRUNCATION CLASS: presetBrowser_ / the Patch page
//       button / partCombo_ / the [FX] mode button keep >= 80% of their
//       DOCUMENTED natural widths (156 / 64 / 88 / 50 — the resized()
//       left-cluster constants). The >= 1024 floor must never squeeze a
//       header control below its designed width budget again.
//   [3] NO PAIRWISE SIBLING OVERLAP across the header row (Rectangle
//       intersection with the same 2px hairline tolerance as
//       layout_overlap_test).
//
// [0] is a CANARY on the raw predicates with hand-broken rectangles: the
// checker must flag a seeded zero-extent control, a seeded overlapping pair
// and the historical 38-of-50 squeeze, and must PASS a compliant layout — a
// checker that cannot fail cannot gate anything.
//
// Built by default. Run: ./build/parvati_layout_minwidth_test

#include <cxxabi.h>
#include <algorithm>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/PresetBrowser.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

//==============================================================================
// The raw predicates, free of any Component so the [0] canary can drive them
// with hand-broken rectangles.

// [1] A PLACED control must occupy positive extent on BOTH axes.
bool rectHasPositiveExtent (juce::Rectangle<int> r)
{
    return r.getWidth() > 0 && r.getHeight() > 0;
}

// [3] Two header controls must not intersect beyond the hairline tolerance
// (same kTolerance as layout_overlap_test: borders / centring geometry).
constexpr int kTolerance = 2;
bool rectsOverlapBeyondTolerance (juce::Rectangle<int> a, juce::Rectangle<int> b)
{
    const auto ix = a.getIntersection (b);
    return (! ix.isEmpty()) && ix.getWidth() > kTolerance && ix.getHeight() > kTolerance;
}

// [2] width keeps >= 80% of the documented natural width. Integer-safe
// (w * 5 >= natural * 4); the historical [FX] squeeze was 38/50 = 76%.
bool widthKeepsShare (int w, int natural)
{
    return w * 5 >= natural * 4;
}

// [2b] FULL designed width — the resized() left-cluster budget comment's own
// contract at the 1024 floor: "156 lands exactly on the budget so every
// control keeps its designed width at the minimum frame". A squeeze at 1024
// is a budget regression even at 82% (mutation-measured: presetW 156->168
// squeezed [FX] to 41 of 50 — above the 80% floor, below the designed width).
bool widthKeepsFullShare (int w, int natural)
{
    return w >= natural;
}

// Documented natural widths (PluginEditor.cpp resized(), the left-cluster
// budget comment: "156 lands exactly on the budget so every control keeps its
// designed width at the minimum frame"). Keep in sync with that budget.
constexpr int kNaturalPresetW  = 156;   // presetW
constexpr int kNaturalPatchW   = 64;    // globalW
constexpr int kNaturalPartW    = 88;    // partComboW
constexpr int kNaturalModeW    = 50;    // modeW ([Synth]/[FX])

// The header strip: kHeaderH (44) + the chrome-rule clearance below it.
constexpr int kHeaderStripH = 50;

//==============================================================================
// Component plumbing.

// Effective visibility: juce isVisible() is per-component, NOT inherited — a
// child of a hidden parent reports visible=true. The keyboard overlay is
// hidden as a whole, so its keys must not be audited as header children.
bool isEffectivelyVisible (juce::Component* c)
{
    for (auto* p = c; p != nullptr; p = p->getParentComponent())
        if (! p->isVisible())
            return false;
    return true;
}

// Interactive header control types (the task's definition: Button and
// ComboBox subclasses). Labels are display chrome, not interactive targets.
bool isInteractiveType (juce::Component* c)
{
    return dynamic_cast<juce::Button*> (c) != nullptr
        || dynamic_cast<juce::ComboBox*> (c) != nullptr;
}

// The three zoom buttons folded into the "..." overflow: constructed and
// addAndMakeVisible'd but NEVER given bounds (their logic is reused by the
// popup). They park at (0,0,0,0) BY DESIGN — the one documented exception to
// the positive-extent gate. Identified by their button names ("+", "-", "0");
// if the header ever places them for real this entry simply stops matching.
bool isFoldedZoomTrio (juce::Component* c)
{
    const auto n = c->getName();
    return n == "+" || n == "-" || n == "0";
}

// Best-effort human-readable description for failure output.
juce::String describe (juce::Component* c)
{
    if (auto* b = dynamic_cast<juce::TextButton*> (c)) return "TextButton('" + b->getButtonText() + "')";
    if (dynamic_cast<juce::ComboBox*> (c) != nullptr)  return juce::String (c->getName()) + "(ComboBox)";
    if (dynamic_cast<juce::Button*> (c) != nullptr)    return juce::String (c->getName()) + "(Button)";
    int status = 0;
    char* dem = abi::__cxa_demangle (typeid (*c).name(), nullptr, nullptr, &status);
    juce::String cn = (status == 0 && dem != nullptr) ? juce::String (dem) : juce::String (typeid (*c).name());
    ::free (dem);
    const auto cut = cn.lastIndexOf (":");
    if (cut >= 0) cn = cn.substring (cut + 1);
    return juce::String (c->getName()) + "(" + cn + ")";
}

// One entry of the audited header set: the component + its EDITOR-relative
// bounds (direct children already are; grandchildren translated one level).
struct HeaderChild
{
    juce::Component* comp;
    juce::Rectangle<int> bounds;   // editor-local
    bool parkedFolded = false;     // the folded zoom trio
};

// Collect the header interactive set: effectively-visible Button/ComboBox
// descendants up to depth 2 (editor -> child -> grandchild), intersecting the
// header strip, not parked fully offscreen (the closed Settings drawer).
std::vector<HeaderChild> collectHeaderChildren (ParvatiEditor& editor)
{
    std::vector<HeaderChild> out;
    const auto editorBounds = editor.getLocalBounds();
    const auto inBand = [&editorBounds] (const juce::Rectangle<int>& b)
    {
        // Intersects the header strip vertically AND is not entirely outside
        // the editor horizontally/vertically (hidden-drawer idiom).
        const bool vertical = b.getY() < kHeaderStripH && b.getBottom() > 0;
        const bool fullyOff = b.getX() >= editorBounds.getWidth()
                              || b.getRight() <= 0 || b.getBottom() <= 0;
        return vertical && ! fullyOff;
    };

    for (auto* child : editor.getChildren())
    {
        if (! isEffectivelyVisible (child))
            continue;
        const auto cb = child->getBoundsInParent();
        if (isInteractiveType (child) && inBand (cb))
            out.push_back ({ child, cb, isFoldedZoomTrio (child) });
        // One level deep (e.g. PresetBrowser's inner name button).
        for (auto* gc : child->getChildren())
        {
            if (! isEffectivelyVisible (gc) || ! isInteractiveType (gc))
                continue;
            const auto gb = gc->getBoundsInParent().translated (cb.getX(), cb.getY());
            if (inBand (gb))
                out.push_back ({ gc, gb, isFoldedZoomTrio (gc) });
        }
    }
    return out;
}

// Locate the four reference controls for the [2] natural-width check.
// presetBrowser_ by type, partCombo_ as the lone direct ComboBox in the band,
// the buttons by their unique texts.
juce::Component* findPresetBrowser (ParvatiEditor& e)
{
    for (auto* c : e.getChildren())
        if (auto* pb = dynamic_cast<PresetBrowser*> (c); pb != nullptr && isEffectivelyVisible (pb))
            return pb;
    return nullptr;
}
juce::Component* findPartCombo (ParvatiEditor& e)
{
    for (auto* c : e.getChildren())
        if (dynamic_cast<juce::ComboBox*> (c) != nullptr && isEffectivelyVisible (c))
            return c;
    return nullptr;
}
juce::Component* findTextButton (ParvatiEditor& e, const char* text)
{
    for (auto* c : e.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*> (c))
            if (b->getButtonText() == text && isEffectivelyVisible (b))
                return b;
    return nullptr;
}

//==============================================================================
// Locate a direct-child Button by NAME (the W9 seam: the Path-drawn
// IconButtons carry no text, so the header ctor names the primary-set ones).
juce::Component* findNamedButton (ParvatiEditor& e, const char* name)
{
    for (auto* c : e.getChildren())
        if (dynamic_cast<juce::Button*> (c) != nullptr && c->getName() == name)
            return c;
    return nullptr;
}

//==============================================================================
void sweepWidth (ParvatiEditor& editor, int w)
{
    std::printf ("\n=== %dx600 ===\n", w);
    editor.setSize (w, 600);

    auto header = collectHeaderChildren (editor);
    std::printf ("  audited %zu placed interactive header children\n", header.size());
    // The placed-set size floor is BAND-scoped (W9 folding hides secondary
    // controls below the measured breakpoints 1024 / 810 / 650):
    //   >= 1024: full desktop set; 810..1023: Part/Synth/FX folded;
    //   650..809: + MOD/MAP/gear folded; < 650: + Patch/Redo folded.
    const int floorCount = (w >= 1024) ? 12 : (w >= 810) ? 11 : (w >= 650) ? 8 : 6;
    check ((int) header.size() >= floorCount,
           "header set is non-degenerate (placed interactive children >= floor)");

    // [1] positive extent for every PLACED control.
    for (const auto& h : header)
    {
        if (h.parkedFolded)
            continue;   // the folded zoom trio: unplaced by design
        if (! rectHasPositiveExtent (h.bounds))
            check (false, (describe (h.comp) + " has zero extent at "
                           + h.bounds.toString()).toRawUTF8());
    }
    check (std::all_of (header.begin(), header.end(), [] (const HeaderChild& h)
                        { return h.parkedFolded || rectHasPositiveExtent (h.bounds); }),
           "every placed interactive header child keeps positive width AND height");

    // [2] the historical truncation class: >= 80% of documented natural
    // width, over the PLACED reference controls only (W9: sub-1024 the Part
    // combo / [Synth]/[FX] fold away — findPartCombo returns nullptr — and
    // below 650 the [Patch] button folds too; folded controls are exempt by
    // design, their actions live in the "..." popup).
    const bool partClusterPlaced = w >= 1024;
    const bool patchPlaced       = w >= 650;
    struct Ref { const char* label; juce::Component* c; int natural; };
    std::vector<Ref> refs;
    refs.push_back ({ "preset browser", findPresetBrowser (editor), kNaturalPresetW });
    if (patchPlaced)
        refs.push_back ({ "Patch page button", findTextButton (editor, "Patch"), kNaturalPatchW });
    if (partClusterPlaced)
    {
        refs.push_back ({ "part combo", findPartCombo (editor), kNaturalPartW });
        refs.push_back ({ "[FX] mode button", findTextButton (editor, "FX"), kNaturalModeW });
    }
    for (const auto& r : refs)
    {
        if (r.c == nullptr)
        {
            check (false, (juce::String ("reference control not found: ") + r.label).toRawUTF8());
            continue;
        }
        char msg[128];
        std::snprintf (msg, sizeof (msg), "%s keeps >= 80%% of natural width %d [%d]",
                       r.label, r.natural, r.c->getWidth());
        check (widthKeepsShare (r.c->getWidth(), r.natural), msg);
        // At the 1024 FLOOR the designed-width contract is exact (see
        // widthKeepsFullShare): the budget comment pins every control at its
        // designed width at the minimum frame.
        if (w == 1024)
        {
            char fmsg[128];
            std::snprintf (fmsg, sizeof (fmsg),
                           "%s keeps its DESIGNED width %d at the 1024 floor [%d]",
                           r.label, r.natural, r.c->getWidth());
            check (widthKeepsFullShare (r.c->getWidth(), r.natural), fmsg);
        }
    }

    // [3] no pairwise sibling overlap across the header row.
    int overlaps = 0;
    for (size_t i = 0; i < header.size(); ++i)
        for (size_t j = i + 1; j < header.size(); ++j)
        {
            // The folded trio parks at the origin: skip pairs involving it
            // (a 0x0 rect intersects nothing beyond tolerance anyway, but be
            // explicit about the exemption).
            if (header[i].parkedFolded || header[j].parkedFolded)
                continue;
            if (rectsOverlapBeyondTolerance (header[i].bounds, header[j].bounds))
            {
                ++overlaps;
                std::printf ("    %s overlaps %s at %s\n",
                             describe (header[i].comp).toRawUTF8(),
                             describe (header[j].comp).toRawUTF8(),
                             header[i].bounds.getIntersection (header[j].bounds).toString().toRawUTF8());
            }
        }
    check (overlaps == 0, "no pairwise sibling overlap in the header row");

    // [4] W9 primary-set guarantee (every width incl. sub-1024): the
    // never-fold controls — preset browser, Load, Save, Undo, [KBD], and
    // the "..." overflow host that carries the folded actions — stay
    // VISIBLE with positive extent down to the narrowest swept pane. This
    // is the AUv3 compact-pane contract in pure layout math: real device /
    // AUv3-pane verification (AUM keyboard-open ~570pt, GarageBand ~700pt)
    // needs a host and is out of scope for the headless sweep.
    struct Primary { const char* label; juce::Component* c; };
    const Primary primaries[] = {
        { "preset browser",   findPresetBrowser (editor) },
        { "Load button",      findTextButton (editor, "Load") },
        { "Save button",      findTextButton (editor, "Save") },
        { "Undo button",      findNamedButton (editor, "headerUndo") },
        { "[KBD] toggle",     findTextButton (editor, "KBD") },
        { "... overflow",     findTextButton (editor, "...") },
    };
    for (const auto& p : primaries)
    {
        if (p.c == nullptr)
        {
            check (false, (juce::String ("PRIMARY control not found: ") + p.label).toRawUTF8());
            continue;
        }
        const bool visiblePositive = isEffectivelyVisible (p.c)
                                     && rectHasPositiveExtent (p.c->getBoundsInParent());
        char msg[128];
        std::snprintf (msg, sizeof (msg), "primary %s visible + positive at %d [%s]",
                       p.label, w, p.c->getBoundsInParent().toString().toRawUTF8());
        check (visiblePositive, msg);
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ------------------------------------------------------------------
    // [0] CANARY: the predicates must fire on hand-broken layouts. A checker
    // that cannot fail cannot gate anything.
    // ------------------------------------------------------------------
    std::printf ("[0] canary: predicates flag hand-broken rectangles\n");
    {
        // Zero extent: a control squeezed out of the budget (the collapse
        // removeFromLeft produces at its real position).
        check (! rectHasPositiveExtent ({ 400, 5, 0, 44 }), "canary: zero-width control flagged");
        check (! rectHasPositiveExtent ({ 400, 5, 156, 0 }), "canary: zero-height control flagged");
        check (rectHasPositiveExtent ({ 400, 5, 50, 44 }), "canary: healthy control passes");

        // Overlap: two header controls painted over each other (beyond the
        // 2px hairline tolerance)...
        check (rectsOverlapBeyondTolerance ({ 100, 5, 156, 44 }, { 200, 5, 64, 44 }),
               "canary: 56px-deep overlap flagged");
        // ...adjacent controls (gap 0) and a hairline 2px graze are NOT
        // violations.
        check (! rectsOverlapBeyondTolerance ({ 100, 5, 100, 44 }, { 200, 5, 64, 44 }),
               "canary: adjacent (0 gap) controls pass");
        check (! rectsOverlapBeyondTolerance ({ 100, 5, 102, 44 }, { 200, 5, 64, 44 }),
               "canary: 2px hairline graze passes");
        // Fully disjoint -> clean.
        check (! rectsOverlapBeyondTolerance ({ 100, 5, 50, 44 }, { 900, 5, 44, 44 }),
               "canary: disjoint controls pass");

        // The historical squeeze: [FX] at 38 of its designed 50 (76%) must
        // FAIL the share gate; exactly 80% must PASS; 49/50 must already FAIL
        // the FULL-share gate (the designed-width-at-the-floor contract).
        check (! widthKeepsShare (38, 50), "canary: the historical 38/50 [FX] squeeze flagged");
        check (widthKeepsShare (40, 50), "canary: exactly 80% passes");
        check (widthKeepsShare (50, 50), "canary: full natural width passes");
        check (! widthKeepsFullShare (49, 50), "canary: 49/50 fails the full designed-width gate");
        check (widthKeepsFullShare (50, 50), "canary: designed width passes the full gate");
    }

    // ------------------------------------------------------------------
    // The sweep: the whole legal desktop resize range (1024 floor .. 1800
    // ceiling from setResizeLimits) at height 600, PLUS the W9 sub-1024
    // compact band (560 .. 980) exercised by the adaptive header fold — the
    // AUv3 wrapper bypasses the constrainer, so the layout math must hold
    // at ANY pane size. iOS-vs-desktop is not distinguishable headlessly;
    // these widths assert the PURE layout contract (see [4]).
    // ------------------------------------------------------------------
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    auto* editorRaw = proc.createEditor();
    check (editorRaw != nullptr, "editor constructs");
    if (editorRaw == nullptr)
        return 1;
    auto* editor = dynamic_cast<ParvatiEditor*> (editorRaw);
    check (editor != nullptr, "editor is a ParvatiEditor");
    if (editor == nullptr)
    {
        delete editorRaw;
        return 1;
    }
    editor->setVisible (true);

    for (int w : { 560, 600, 650, 700, 780, 810, 900, 980,
                   1024, 1050, 1099, 1100, 1152, 1200, 1280, 1440, 1600, 1800 })
        sweepWidth (*editor, w);

    delete editor;
    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
