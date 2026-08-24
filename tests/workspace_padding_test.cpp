// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// workspace_padding_test — pins batch-3 part C (2026-08-20):
//   [1] SynthWorkspace's top-row padding == FxWorkspace's (FX-page parity for
//       the synth module-panel whitespace): kRowGap constants equal, 8pt, and
//       the rowPaddingForTest() accessors agree.
//   [2] FX-matrix rows use the synth-matrix icon idiom: the delete X
//       (IconButton) is present and the RIGHTMOST control; the mute/bypass
//       lamp is present with a >=44pt hit target; NO TextButton with the old
//       "Clear"/"M" labels remains anywhere in a row; and the ACTIONS still
//       fire through the same view seams (X clears the slot, lamp toggles
//       the session mute).
// Run: ./build_unified/parvati_unified_tests workspace_padding_test

#include <array>
#include <cstdio>
#include <memory>
#include "unified_test_runner.h"
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ParameterLayout.h"        // getPatchParamDescriptors (FX card ctor descriptors)
#include "ui/FxMatrixView.h"
#include "ui/FxRoutingBar.h"        // FxRoutingBar (fixed-height module pin [3])
#include "ui/FxSlotCard.h"          // FxSlotCard (fixed-height module pin [3])
#include "ui/FxWorkspace.h"
#include "ui/IconButton.h"
#include "ui/SynthWorkspace.h"
#include "ui/ThemeManager.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void collectAll (juce::Component* c, std::vector<juce::Component*>& out)
{
    if (c == nullptr)
        return;
    out.push_back (c);
    for (auto* child : c->getChildren())
        collectAll (child, out);
}

IconButton* findIconButton (juce::Component* row)
{
    std::vector<juce::Component*> children;
    collectAll (row, children);
    for (auto* c : children)
        if (auto* b = dynamic_cast<IconButton*> (c))
            return b;
    return nullptr;
}

juce::Button* findTitledButton (juce::Component* row, const juce::String& title)
{
    std::vector<juce::Component*> children;
    collectAll (row, children);
    for (auto* c : children)
        if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getTitle() == title)
            return b;
    return nullptr;
}

// A synthetic clean click (down + up, no movement) — fires Button::onClick
// synchronously (the mod_matrix_ui_test technique).
void click (juce::Component* comp)
{
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto pos = comp->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);

    comp->mouseDown (juce::MouseEvent (source, pos, mods,
                                       juce::MouseInputSource::defaultPressure,
                                       juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation,
                                       juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY,
                                       comp, comp, now, pos, now, 1, false));
    comp->mouseUp (juce::MouseEvent (source, pos, juce::ModifierKeys(),
                                     juce::MouseInputSource::defaultPressure,
                                     juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY,
                                     comp, comp, now, pos, now, 1, false));
}
}  // namespace

TEST(workspace_padding_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[1] workspace top-row padding parity\n");
    {
        ThemeManager themeManager;
        SynthWorkspace synth (themeManager);
        FxWorkspace   fx (themeManager);

        check (SynthWorkspace::kRowGap == 8, "SynthWorkspace::kRowGap == 8 (FX-page value)");
        check (FxWorkspace::kRowGap == 8, "FxWorkspace::kRowGap == 8");
        check (SynthWorkspace::kRowGap == FxWorkspace::kRowGap,
               "synth top-row gap equals the FX top-row gap");
        check (synth.rowPaddingForTest() == fx.rowPaddingForTest(),
               "rowPaddingForTest() accessors agree");
        // INTER-MODULE GAP HARMONIZATION (2026-08-23 user request — "the
        // padding for synth and fx modules are not the same"): both pages
        // use ONE shared between-module value (the FX page's tuned 12) with
        // the shared 8 outer margins, so switching SYNTH<->FX keeps a single
        // visual rhythm. Pinned here so neither page can drift again.
        check (SynthWorkspace::kColGap == 12 && FxWorkspace::kColGap == 12,
               "inter-module gap is 12 on BOTH pages");
        check (SynthWorkspace::kColGap == FxWorkspace::kColGap,
               "synth inter-module gap equals the FX one (harmonized)");
        check (synth.moduleGapForTest() == FxWorkspace::kColGap,
               "moduleGapForTest() accessor agrees");
    }

    std::printf ("\n[3] FX top-row modules: FIXED heights, TOP-pinned (synth parity)\n");
    {
        // 2026-08-23 user requests: "all FX module heights fixed like FX
        // Routing", then "cards +20px spacious, routing UNCHANGED, and NO
        // centred band — work just like the synth page's controls". Pins:
        // (a) the routing bar + the slot cards sit at their FIXED class
        // constants (routing NOT resized by the cards' +20 bump), (b) growing
        // the workspace TALLER changes NEITHER module height NOR their y —
        // the modules stay top-pinned at kRowGap (the old code stretched them
        // to viewH - 2*kGap; the interim fix centred the band — both
        // rejected), and (c) the modules stay inside the workspace.
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        ThemeManager themeManager;

        const juce::String prefix = "fx1_";
        const PatchParamDescriptor *p1 = nullptr, *p2 = nullptr, *p3 = nullptr,
                                   *p4 = nullptr, *p5 = nullptr, *dw = nullptr;
        for (const auto& d : getPatchParamDescriptors())
        {
            if (! (d.isFx && juce::String (d.paramID).startsWith (prefix)))
                continue;
            if      (d.paramID == prefix + "param1") p1 = &d;
            else if (d.paramID == prefix + "param2") p2 = &d;
            else if (d.paramID == prefix + "param3") p3 = &d;
            else if (d.paramID == prefix + "param4") p4 = &d;
            else if (d.paramID == prefix + "param5") p5 = &d;
            else if (d.paramID == prefix + "drywet") dw = &d;
        }

        FxWorkspace fx (themeManager);
        auto routing = std::make_unique<FxRoutingBar> (proc, themeManager);
        fx.setFxRoutingBar (routing.get());
        std::array<std::unique_ptr<FxSlotCard>, 3> cards;
        for (int slot = 0; slot < 3; ++slot)
        {
            const juce::String pfx = "fx" + juce::String (slot + 1) + "_";
            const PatchParamDescriptor *c1 = nullptr, *c2 = nullptr, *c3 = nullptr,
                                       *c4 = nullptr, *c5 = nullptr, *cdw = nullptr;
            for (const auto& d : getPatchParamDescriptors())
            {
                if (! (d.isFx && juce::String (d.paramID).startsWith (pfx)))
                    continue;
                if      (d.paramID == pfx + "param1") c1 = &d;
                else if (d.paramID == pfx + "param2") c2 = &d;
                else if (d.paramID == pfx + "param3") c3 = &d;
                else if (d.paramID == pfx + "param4") c4 = &d;
                else if (d.paramID == pfx + "param5") c5 = &d;
                else if (d.paramID == pfx + "drywet") cdw = &d;
            }
            cards[(size_t) slot] = std::make_unique<FxSlotCard> (proc, slot, c1, c2, c3, c4, c5, cdw);
            fx.setFxSlotCard (slot, cards[(size_t) slot].get());
        }
        // 900x800: TALL enough that the top row needs no scrollbar (the
        // natural row is kCardModuleH + 2*kOuterMargin = 276 < the ~440pt
        // main row this leaves) — the right-margin pin below then checks the
        // UNSCROLLED geometry (the scrollbar-aware re-layout narrows the
        // columns by design; layout_overlap_test covers that path).
        fx.setBounds (0, 0, 900, 800);

        FxRoutingBar* routeBar = nullptr;
        FxSlotCard*   card = nullptr;    // slot 0 (the height pins)
        FxSlotCard*   fx3Card = nullptr; // slot 2 (the right-margin pin)
        {
            std::vector<juce::Component*> all;
            collectAll (&fx, all);
            for (auto* c : all)
            {
                if (routeBar == nullptr) routeBar = dynamic_cast<FxRoutingBar*> (c);
                if (card == nullptr) card = dynamic_cast<FxSlotCard*> (c);
                if (auto* sc = dynamic_cast<FxSlotCard*> (c))
                    fx3Card = sc;   // DFS order -> the LAST card found is FX3
            }
        }
        check (routeBar != nullptr && card != nullptr && fx3Card != nullptr,
               "routing bar + all three slot cards present in the FX workspace");
        if (routeBar != nullptr && card != nullptr && fx3Card != nullptr)
        {
            check (routeBar->getHeight() == FxWorkspace::kRouteModuleH
                   && card->getHeight() == FxWorkspace::kCardModuleH,
                   "module heights == the fixed class constants (routing exempt from +20)");
            check (card->getHeight() == FxWorkspace::kRouteModuleH + 20,
                   "cards carry exactly the +20px spaciousness bump");

            // BETWEEN-module whitespace + the fx3 right margin (2026-08-23
            // user follow-ups: "a tiny bit more whitespace between the FX
            // modules" and "the right edge of the FX3 module is truncated,
            // all modules should fit including whitespace"). All four
            // modules share the top-row host as parent, so parent-space
            // bounds compare directly.
            check (FxWorkspace::kColGap > FxWorkspace::kRowGap,
                   "inter-module gap is a few px wider than the outer margin");
            {
                // Gap routing->FX1 and the fx3 right margin (the gap between
                // FX1/FX2/FX3 is covered implicitly: equal cardW columns).
                const int hostW = card->getParentComponent()->getWidth();
                check (card->getX() - routeBar->getRight() == FxWorkspace::kColGap,
                       "routing->FX1 gap == kColGap (the wider module spacing)");
                check (fx3Card->getRight() <= hostW - FxWorkspace::kOuterMargin
                       && fx3Card->getRight() >= hostW - FxWorkspace::kOuterMargin - 2,
                       "FX3 right edge ends AT the right margin (never truncated)");
                check (FxWorkspace::kOuterMargin == 16,
                       "FX outer margin == the synth page's effective outer whitespace (8+8)");
            }

            // Grow the workspace 160pt taller: neither the heights NOR the y
            // positions move (top-pinned like the synth page — no stretch,
            // no centred band).
            const int y0card = card->getY();
            const int y0route = routeBar->getY();
            fx.setBounds (0, 0, 900, 960);
            check (card->getHeight() == FxWorkspace::kCardModuleH
                   && routeBar->getHeight() == FxWorkspace::kRouteModuleH,
                   "module heights unchanged when the workspace grows taller");
            check (card->getY() == y0card && routeBar->getY() == y0route
                   && y0card == FxWorkspace::kOuterMargin,
                   "modules stay TOP-pinned at kOuterMargin (no centring)");

            // The fixed band stays inside the workspace.
            check (card->getBottom() <= fx.getHeight()
                   && routeBar->getBottom() <= fx.getHeight(),
                   "module band fits inside the workspace");
        }

        // ---- [3b] MOD-BAR OPAQUENESS (2026-08-23 CPU fix, finding 3): the
        // bar + its scrolled pill content paint their FULL bounds and are
        // flagged opaque, so the pill strips' dirty rects stop AT the bar
        // (the pre-fix cascade climbed ~15 non-transparent ancestors to the
        // DocumentWindow, re-painting the workspace/editor/window fills
        // inside every small strip dirty rect at display rate). The pills
        // themselves stay NON-opaque by design (rounded corners need alpha —
        // pinned below so the opaque pass can never silently widen to them).
        // Read through PUBLIC JUCE APIs only (isOpaque + the Viewport's
        // viewed component), so no product-code test seam is required. ----
        if (fx.modBar() != nullptr)
        {
            auto* bar = fx.modBar();
            check (bar->isOpaque(),
                   "mod bar is opaque (paint covers its full bounds)");
            // The Viewport is the bar's FIRST child (created before the nav
            // buttons); its viewed component is the scrolled PillContent.
            if (auto* vp = dynamic_cast<juce::Viewport*> (bar->getChildComponent (0)))
            {
                auto* content = vp->getViewedComponent();
                check (content != nullptr && content->isOpaque(),
                       "scrolled pill content is opaque (segments fillAll)");
            }
            else
            {
                check (false,
                       "bar child 0 is the pill Viewport (layout seam moved?)");
            }
            // The pills (direct children of the scrolled content) must stay
            // NON-opaque: rounded-corner chrome needs the alpha channel.
            // Discovery: FIX 2's headless seam — each pill carries a child
            // named "modPillStrip" (the strip VIEW), so no name is needed on
            // the pill itself.
            {
                int nonOpaquePills = 0, pillCount = 0;
                if (auto* vp = dynamic_cast<juce::Viewport*> (bar->getChildComponent (0)))
                    if (auto* content = vp->getViewedComponent())
                        for (auto* ch : content->getChildren())
                        {
                            bool hasStrip = false;
                            for (auto* gc : ch->getChildren())
                                if (gc->getName() == "modPillStrip") hasStrip = true;
                            if (! hasStrip) continue;
                            ++pillCount;
                            if (! ch->isOpaque()) ++nonOpaquePills;
                        }
                char m[96];
                std::snprintf (m, sizeof (m),
                               "pills stay non-opaque (%d/%d)", nonOpaquePills, pillCount);
                check (pillCount > 0 && nonOpaquePills == pillCount, m);
            }
        }
    }

    std::printf ("\n[2] FX-matrix rows use the icon idiom (lamp + X, no text buttons)\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        ThemeManager themeManager;

        juce::Component host;
        host.setBounds (0, 0, 900, 600);
        FxMatrixView view (proc, themeManager);
        host.addAndMakeVisible (view);
        view.setBounds (0, 0, 880, 560);

        // Activate slot 0 directly through the APVTS (the same seam a loaded
        // preset / the add-button path writes), then refresh so the row shows.
        // NOTE: fxmod1_amount spans -63..+63, so normalized 0.5 is EXACTLY
        // ZERO — use a normalized value that maps to a nonzero depth.
        if (auto* p = proc.getApvts().getParameter ("fxmod1_amount"))
            p->setValueNotifyingHost (p->convertTo0to1 (32.0f));
        view.refresh();

        auto* row = view.rowForSlotForTest (0);
        check (row != nullptr, "rowForSlotForTest(0) returns the row");
        if (row == nullptr)
        {
            std::printf ("\nWORKSPACE PADDING TEST: %s (%d failures)\n",
                         g_failures ? "FAILURES" : "ALL CHECKS PASSED", g_failures);
            return g_failures == 0;
        }

        // (a) No text "Clear"/"M" button remains anywhere in the row.
        {
            std::vector<juce::Component*> children;
            collectAll (row, children);
            int oldLabelled = 0;
            for (auto* c : children)
            {
                auto* tb = dynamic_cast<juce::TextButton*> (c);
                if (tb == nullptr)
                    continue;
                const auto text = tb->getButtonText();
                if (text == "M" || text == "Clear" || text == TRANS ("M") || text == TRANS ("Clear"))
                    ++oldLabelled;
            }
            check (oldLabelled == 0, "no 'Clear'/'M' TextButton remains in the row");
        }

        // (b) The delete X (IconButton) is present, titled, and the RIGHTMOST
        // direct child of the row.
        auto* x = findIconButton (row);
        check (x != nullptr, "row has an IconButton (delete X)");
        if (x != nullptr)
        {
            check (x->getTitle().isNotEmpty(), "X carries an accessible title");
            check (x->getWidth() >= 44, "X hit target >= 44pt wide");
            int maxRight = -1;
            for (auto* c : row->getChildren())
                maxRight = juce::jmax (maxRight, c->getBounds().getRight());
            check (x->getBounds().getRight() == maxRight, "X is the rightmost control in the row");

            // Action parity: the X clears the slot (the old "Clear" action).
            check (view.amountForSlot (0) != 0, "slot 0 carries a nonzero depth before the X click");
            click (x);
            view.refresh();
            check (view.amountForSlot (0) == 0, "X click clears the slot (old Clear action)");
        }

        // (c) The mute/bypass lamp replaces the old text "M" button: present,
        // titled, 44pt hit target, and the action still toggles the mute.
        auto* lamp = findTitledButton (row, TRANS ("Mute / bypass this modulation"));
        check (lamp != nullptr, "mute/bypass lamp present (titled button)");
        if (lamp != nullptr)
        {
            check (lamp->getWidth() >= 44, "lamp hit target >= 44pt wide");
            // Border-ring stroke pin (2026-08-20 "a tiny bit thicker"): the
            // lamp draws the shared constant (asserted in mod_matrix_ui_test
            // too — one value, two pins).
            check (ParvatiModuleLamp::kLampBorderWidth >= 2.0f
                       && ParvatiModuleLamp::kLampBorderWidth <= 3.0f,
                   "lamp border ring stroke is the slightly-thicker value (2-3pt)");
            const bool before = view.isSlotMuted (0);
            click (lamp);
            const bool after = view.isSlotMuted (0);
            check (before != after, "lamp click toggles the session mute (old M action)");
            // Restore (session-only state; leave the view tidy).
            if (after)
                click (lamp);
        }
    }

    std::printf ("\nWORKSPACE PADDING TEST: %s (%d failures)\n",
                 g_failures ? "FAILURES" : "ALL CHECKS PASSED", g_failures);
    return g_failures == 0;
}
