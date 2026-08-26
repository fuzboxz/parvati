// fx_slot_tooltip_test — FX-slot knob tooltips follow the loaded FX module.
//
// The slot cards relabel their param knobs per algorithm (FxSlotLabels), but
// the knob TOOLTIPS stay generic today. This test pins the target contract:
// an ACTIVE param knob shows help for the loaded module. An INACTIVE param,
// a None slot and a Diffuser slot revert to the generic ParamHelp text. The
// module text follows a user pick, a replacement, a host write and a state
// restore. The child slider surface matches the cell. The global tooltips
// toggle hides and restores the module text.
//
// The card refresh is deferred through AsyncUpdater. The test therefore pumps
// the macOS run loop (perf_smoke_test pattern). Non-Apple hosts skip the test.
//
// Run: ./build_unified/hellcat_unified_tests fx_slot_tooltip_test

#include "unified_test_runner.h"

#if ! __APPLE__

#include <cstdio>

#else

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParameterLayout.h"   // getPatchParamDescriptors
#include "PluginProcessor.h"
#include "dsp/fx/FxTypes.h"    // FxType
#include "test_utils.h"        // setChoice
#include "ui/FxSlotCard.h"
#include "ui/FxSlotLabels.h"   // activeParamCount / paramLabel
#include "ui/ParamControl.h"
#include "ui/ParamHelp.h"      // getParamHelp

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Deliver posted AsyncUpdater messages. JUCE posts them onto the main run
// loop; each slice runs that loop for 20 ms (perf_smoke_test pattern).
void pump (int totalMs)
{
    const auto endMs = juce::Time::getMillisecondCounter() + (unsigned) totalMs;
    while (juce::Time::getMillisecondCounter() < endMs)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
}

// Descriptor lookup by paramID (the PluginEditor ctor pattern).
const PatchParamDescriptor* findDescriptor (const juce::String& id)
{
    for (const auto& d : getPatchParamDescriptors())
        if (juce::String (d.paramID) == id)
            return &d;
    return nullptr;
}

using KnobMap = std::map<juce::String, ParamControl*>;

// Collect the ParamControl knobs of a card, keyed by paramID.
KnobMap collectKnobs (FxSlotCard& card)
{
    KnobMap m;
    for (int i = 0; i < card.getNumChildComponents(); ++i)
        if (auto* pc = dynamic_cast<ParamControl*> (card.getChildComponent (i)))
            m.emplace (pc->getParamID(), pc);
    return m;
}

// ParamID of slot (1..3) and knob number (1..5).
juce::String paramId (int slot, int knob)
{
    return "fx" + juce::String (slot) + "_param" + juce::String (knob);
}

juce::String tipFor (const KnobMap& knobs, const juce::String& id)
{
    const auto it = knobs.find (id);
    return it != knobs.end() ? it->second->getTooltip() : juce::String();
}

ParamControl* knobFor (const KnobMap& knobs, const juce::String& id)
{
    const auto it = knobs.find (id);
    return it != knobs.end() ? it->second : nullptr;
}

// First child slider of a cell (the surface juce::TooltipWindow reads).
juce::Slider* childSliderOf (ParamControl& cell)
{
    for (int i = 0; i < cell.getNumChildComponents(); ++i)
        if (auto* s = dynamic_cast<juce::Slider*> (cell.getChildComponent (i)))
            return s;
    return nullptr;
}

struct Verdict { bool ok; std::string detail; };

// ACTIVE-param contract: the tooltip (a) is non-empty, (b) names the slot,
// (c) contains the semantic label, (d) differs from the generic help text.
Verdict judgeActive (int slot, FxType t, int idx, const juce::String& tip)
{
    const juce::String id      = paramId (slot, idx + 1);
    const juce::String generic = getParamHelp (id);
    std::string why;
    if (tip.isEmpty())
        why = "tooltip is empty";
    else if (! tip.contains ("FX slot " + juce::String (slot)))
        why = "tooltip does not name the slot";
    else if (! tip.contains (juce::String (paramLabel (t, idx))))
        why = std::string ("tooltip lacks the label ") + paramLabel (t, idx);
    else if (tip == generic)
        why = "tooltip equals the generic text";

    if (why.empty())
        return { true, {} };
    return { false, why + " | tooltip: \"" + tip.toStdString() + "\"" };
}

// INACTIVE-param contract: the tooltip equals the generic ParamHelp text.
Verdict judgeInactive (int slot, int idx, const juce::String& tip)
{
    const juce::String id      = paramId (slot, idx + 1);
    const juce::String generic = getParamHelp (id);
    if (tip == generic)
        return { true, {} };
    return { false, "tooltip must equal the generic help | tooltip: \""
                        + tip.toStdString() + "\" | generic: \""
                        + generic.toStdString() + "\"" };
}

void reportActive (int slot, FxType t, int idx, const juce::String& tip,
                   const char* where)
{
    const Verdict v = judgeActive (slot, t, idx, tip);
    check (v.ok, (std::string (where) + ": "
                  + paramId (slot, idx + 1).toStdString()
                  + " tooltip names the module parameter ("
                  + paramLabel (t, idx) + ")").c_str());
    if (! v.ok)
        std::printf ("      %s\n", v.detail.c_str());
}

void reportInactive (int slot, int idx, const juce::String& tip,
                     const char* where)
{
    const Verdict v = judgeInactive (slot, idx, tip);
    check (v.ok, (std::string (where) + ": "
                  + paramId (slot, idx + 1).toStdString()
                  + " tooltip equals the generic help").c_str());
    if (! v.ok)
        std::printf ("      %s\n", v.detail.c_str());
}
}  // namespace

TEST(fx_slot_tooltip_test)
{
#if ! __APPLE__
    std::printf ("  SKIPPED: fx_slot_tooltip_test is macOS-only (CFRunLoop pump)\n");
    return true;
#else
    juce::ScopedJuceInitialiser_GUI gui;

    // The toggle is static; the test restores the state it found.
    const bool tooltipsWereEnabled = ParamControl::tooltipsEnabled();

    std::printf ("=== FX slot knob tooltips follow the loaded module ===\n");

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    // ------------------------------------------------------------------
    // [0] Setup: one card per slot, constructed directly
    // (mod_matrix_ui_test precedent). No editor is needed.
    // ------------------------------------------------------------------
    std::printf ("\n[0] setup: three cards, six knobs each\n");
    std::unique_ptr<FxSlotCard> cards[3];
    KnobMap knobs[3];
    {
        bool descsFound = true, knobsPresent = true;
        for (int s = 0; s < 3; ++s)
        {
            const juce::String prefix = "fx" + juce::String (s + 1) + "_";
            const PatchParamDescriptor* pd[6] = {};
            const char* tails[6] = { "param1", "param2", "param3",
                                     "param4", "param5", "drywet" };
            for (int i = 0; i < 6; ++i)
            {
                pd[i] = findDescriptor (prefix + tails[i]);
                descsFound = descsFound && (pd[i] != nullptr);
            }
            cards[s] = std::make_unique<FxSlotCard> (proc, s,
                                                     pd[0], pd[1], pd[2],
                                                     pd[3], pd[4], pd[5]);
            cards[s]->setBounds (0, 0, 400, 260);
            knobs[s] = collectKnobs (*cards[s]);
            knobsPresent = knobsPresent && knobs[s].size() == 6;
        }
        check (descsFound, "all 18 fx descriptors found (3 slots x 6 knobs)");
        check (knobsPresent, "every card exposes 6 param knobs");
    }
    pump (250);   // settle any async work from construction

    // ------------------------------------------------------------------
    // [1] Default type None: every tooltip equals the generic help.
    // ------------------------------------------------------------------
    std::printf ("\n[1] default type None: tooltips equal the generic help\n");
    for (int idx = 0; idx < 5; ++idx)
        reportInactive (1, idx, tipFor (knobs[0], paramId (1, idx + 1)),
                        "default None");
    check (tipFor (knobs[0], "fx1_drywet") == getParamHelp ("fx1_drywet"),
           "default None: drywet tooltip equals the generic help");

    // ------------------------------------------------------------------
    // [2] User pick Echo: the four active knobs follow the module.
    // ------------------------------------------------------------------
    std::printf ("\n[2] user pick Echo: tooltips follow the module\n");
    juce::String staleParam1Tip;
    {
        cards[0]->simulateUserTypePickForTest ((int) FxType::Echo);
        pump (250);
        for (int idx = 0; idx < 4; ++idx)
            reportActive (1, FxType::Echo, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "Echo pick");
        reportInactive (1, 4, tipFor (knobs[0], paramId (1, 5)),
                        "Echo pick");
        check (tipFor (knobs[0], "fx1_drywet") == getParamHelp ("fx1_drywet"),
               "Echo pick: drywet tooltip stays the generic help");
        staleParam1Tip = tipFor (knobs[0], paramId (1, 1));

        // Canaries: the pick wrote the type param, and the pump delivered the
        // deferred card refresh (the card hides an inactive knob). Without
        // these, a dead write or pump path would fail every tooltip check the
        // same way as a missing feature.
        if (auto* raw = proc.getApvts().getRawParameterValue ("fx1_type"))
            check (juce::roundToInt (raw->load()) == (int) FxType::Echo,
                   "Echo pick: the type param holds Echo");
        check (knobFor (knobs[0], paramId (1, 5)) != nullptr
               && ! knobFor (knobs[0], paramId (1, 5))->isVisible(),
               "Echo pick: the card refresh hid the inactive knob 5");
    }

    // ------------------------------------------------------------------
    // [3] Replacement Reverb: all five tooltips change; the stale text is
    // gone.
    // ------------------------------------------------------------------
    std::printf ("\n[3] replacement Reverb: tooltips are replaced\n");
    {
        cards[0]->simulateUserTypePickForTest ((int) FxType::Reverb);
        pump (250);
        for (int idx = 0; idx < 5; ++idx)
            reportActive (1, FxType::Reverb, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "Reverb replace");
        check (tipFor (knobs[0], paramId (1, 1)) != staleParam1Tip,
               "Reverb replace: the stale Echo tooltip is gone");
    }

    // ------------------------------------------------------------------
    // [4] Host write Flanger: the automation path refreshes the tooltips.
    // ------------------------------------------------------------------
    std::printf ("\n[4] host write Flanger: tooltips follow the automation path\n");
    {
        setChoice (proc, "fx1_type", (int) FxType::Flanger);
        pump (250);
        for (int idx = 0; idx < 4; ++idx)
            reportActive (1, FxType::Flanger, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "Flanger host write");
        reportInactive (1, 4, tipFor (knobs[0], paramId (1, 5)),
                        "Flanger host write");
    }

    // ------------------------------------------------------------------
    // [5] State restore: the preset path refreshes the tooltips. The state
    // is snapshotted while Reverb is loaded, the type changes to Gate, then
    // the snapshot is restored.
    // ------------------------------------------------------------------
    std::printf ("\n[5] state restore: tooltips follow the preset\n");
    {
        setChoice (proc, "fx1_type", (int) FxType::Reverb);
        pump (250);
        for (int idx = 0; idx < 5; ++idx)
            reportActive (1, FxType::Reverb, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "preset base Reverb");

        juce::MemoryBlock snapshotBlock;
        proc.getStateInformation (snapshotBlock);
        check (snapshotBlock.getSize() > 64, "state snapshot has payload");

        cards[0]->simulateUserTypePickForTest ((int) FxType::Gate);
        pump (250);
        for (int idx = 0; idx < 4; ++idx)
            reportActive (1, FxType::Gate, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "Gate after snapshot");

        proc.setStateInformation (snapshotBlock.getData(),
                                  (int) snapshotBlock.getSize());
        pump (250);
        for (int idx = 0; idx < 5; ++idx)
            reportActive (1, FxType::Reverb, idx,
                          tipFor (knobs[0], paramId (1, idx + 1)),
                          "restored preset Reverb");
    }

    // ------------------------------------------------------------------
    // [6] Revert to None: every tooltip returns to the generic help.
    // ------------------------------------------------------------------
    std::printf ("\n[6] revert to None: tooltips return to the generic help\n");
    {
        cards[0]->simulateUserTypePickForTest (0);
        pump (250);
        for (int idx = 0; idx < 5; ++idx)
            reportInactive (1, idx, tipFor (knobs[0], paramId (1, idx + 1)),
                            "None revert");
        check (tipFor (knobs[0], "fx1_drywet") == getParamHelp ("fx1_drywet"),
               "None revert: drywet tooltip equals the generic help");
    }

    // ------------------------------------------------------------------
    // [7] Sweep: every FxType on slots 2 and 3, driven by host writes. A
    // type with 0 active params (None, Diffuser) must stay fully generic.
    // Per-type detail prints only the first failure; every knob still counts.
    // ------------------------------------------------------------------
    std::printf ("\n[7] sweep: every type on slots 2 and 3\n");
    {
        const int lastType = static_cast<int> (FxType::Count) - 1;
        for (int t = 0; t <= lastType; ++t)
        {
            const auto  type   = static_cast<FxType> (t);
            const int   active = activeParamCount (type);
            int         typeFails = 0;
            std::string firstWhy;

            setChoice (proc, "fx2_type", t);
            setChoice (proc, "fx3_type", t);
            pump (200);

            for (int slot = 2; slot <= 3; ++slot)
                for (int idx = 0; idx < 5; ++idx)
                {
                    const juce::String tip =
                        tipFor (knobs[slot - 1], paramId (slot, idx + 1));
                    const std::string where = "fx" + std::to_string (slot)
                        + " type " + std::to_string (t) + " "
                        + paramId (slot, idx + 1).toStdString();
                    const Verdict v = (idx < active)
                        ? judgeActive (slot, type, idx, tip)
                        : judgeInactive (slot, idx, tip);
                    if (! v.ok)
                    {
                        ++g_failures;
                        ++typeFails;
                        if (firstWhy.empty())
                            firstWhy = where + ": " + v.detail;
                    }
                }

            if (typeFails == 0)
                std::printf ("  ok  : fx2+fx3 type %d (%d active params) meet the contract\n",
                             t, active);
            else
                std::printf ("  FAIL: fx2+fx3 type %d: %d knob(s) off-contract (first: %s)\n",
                             t, typeFails, firstWhy.c_str());
        }
    }

    // ------------------------------------------------------------------
    // [8] Tooltip surface: the cell's child slider shows the same tooltip
    // as the cell. The editor TooltipWindow reads the leaf component.
    // ------------------------------------------------------------------
    std::printf ("\n[8] tooltip surface: the child slider matches the cell\n");
    {
        setChoice (proc, "fx1_type", (int) FxType::Echo);
        pump (250);
        auto* cell = knobFor (knobs[0], "fx1_param1");
        check (cell != nullptr, "fx1_param1 knob present");
        if (cell != nullptr)
        {
            const juce::String cellTip = cell->getTooltip();
            const Verdict v = judgeActive (1, FxType::Echo, 0, cellTip);
            check (v.ok, "cell tooltip names the module parameter (Time)");
            if (! v.ok)
                std::printf ("      %s\n", v.detail.c_str());

            auto* slider = childSliderOf (*cell);
            check (slider != nullptr, "the cell owns a child slider");
            if (slider != nullptr)
                check (slider->getTooltip() == cellTip && cellTip.isNotEmpty(),
                       "the child slider shows the same tooltip as the cell");
        }
    }

    // ------------------------------------------------------------------
    // [9] Global toggle: off hides every fx tooltip, on restores the module
    // text. The static flag returns to the state found at start.
    // ------------------------------------------------------------------
    std::printf ("\n[9] global tooltips toggle hides and restores the module text\n");
    {
        ParamControl::setTooltipsEnabled (false);
        bool allHidden = true;
        for (int s = 0; s < 3; ++s)
            for (const auto& kv : knobs[s])
                if (kv.second->getTooltip().isNotEmpty())
                    allHidden = false;
        check (allHidden, "toggle off: every fx knob tooltip is empty");
        if (auto* cell = knobFor (knobs[0], "fx1_param1"))
            if (auto* slider = childSliderOf (*cell))
                check (slider->getTooltip().isEmpty(),
                       "toggle off: the child slider tooltip is empty");

        ParamControl::setTooltipsEnabled (true);
        const juce::String tip = tipFor (knobs[0], paramId (1, 1));
        const Verdict v = judgeActive (1, FxType::Echo, 0, tip);
        check (v.ok, "toggle on: the module tooltip returns");
        if (! v.ok)
            std::printf ("      %s\n", v.detail.c_str());

        ParamControl::setTooltipsEnabled (tooltipsWereEnabled);
    }

    pump (150);   // deliver pending async work before teardown

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
#endif  // __APPLE__ (TEST body branch)
}

#endif  // __APPLE__ (file: helpers and includes)
