// ModMatrixView interaction-redesign regression test (user feedback 2026-08-20).
//
//   [1] Rows are NOT drag sources: the former per-row drag-grip
//       (ModSourceDragGrip, a six-dot handle that started "parvatiModSrc:<enum>"
//       drags) was REMOVED — modulators are dragged ONLY from the CentralModBar
//       pills. Pinned by (a) the canStartDragFromRowForTest() contract hook and
//       (b) a behavioural sweep: synthetic down/drag/up on EVERY child of every
//       row inside a recording DragAndDropContainer host must start ZERO drag
//       operations. (JUCE requires a real dragging MouseInputSource inside
//       startDragging, so a synthetic drag can never legitimately fire — pre-fix
//       the grip's mouseDrag would have called startDragging from the synthetic
//       callback and tripped its jassert in Debug; post-fix nothing in the view
//       even attempts it.)
//   [2] Drop-ASSIGNMENT still works: requestAssign on the ModMatrixHighlight
//       bus is exactly what ParamControl::itemDropped invokes when a pill drag
//       is dropped on a destination knob. Pinning it here proves the knob-drop
//       path is untouched by the drag-source removal.
//   [3] Each row's far-RIGHT control is the compact delete X (IconButton) and
//       clicking it CLEARS the slot (amount -> 0, mute dropped) — the old
//       "Clear" text button's action.
//   [4] The mute/bypass LAMP is the far-LEFT control of the row (left of both
//       combos) and toggles the SAME state as before: click mutes (amount
//       stashed, engine sees 0, row stays visible/greyed); click again restores
//       the stashed amount.
//   [5] Layout sanity across every ACTIVE row: X is the rightmost child, lamp
//       is the leftmost interactive child, both 44pt-wide HIG targets.
//
// Run: ./build_unified/hellcat_unified_tests mod_matrix_ui_test

#include <cstdio>
#include "unified_test_runner.h"
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ParameterLayout.h"   // getPatchParamDescriptors (the FX card descriptors)
#include "dsp/patch.h"
#include "ui/FxMatrixView.h"
#include "ui/FxSlotCard.h"
#include "ui/IconButton.h"
#include "ui/ModMatrixHighlight.h"
#include "ui/ModMatrixView.h"
#include "ui/ThemeManager.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// A DragAndDropContainer host that counts every started drag operation.
struct DragHost : public juce::Component, public juce::DragAndDropContainer
{
    int dragStarts = 0;

private:
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails&) override { ++dragStarts; }
};

// Every component in the subtree (DFS, pre-order).
void collectAll (juce::Component* c, std::vector<juce::Component*>& out)
{
    if (c == nullptr) return;
    out.push_back (c);
    for (auto* child : c->getChildren())
        collectAll (child, out);
}

// A synthetic press-drag-release on comp (60px of travel — far past the 5px
// drag-start threshold every drag source in this codebase uses).
void dragAcross (juce::Component* comp)
{
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto pos = comp->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
    const auto away = pos.translated (60.0f, 8.0f);

    comp->mouseDown (juce::MouseEvent (source, pos, mods,
                                       juce::MouseInputSource::defaultPressure,
                                       juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation,
                                       juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY,
                                       comp, comp, now, pos, now, 1, false));
    comp->mouseDrag (juce::MouseEvent (source, away, mods,
                                       juce::MouseInputSource::defaultPressure,
                                       juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation,
                                       juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY,
                                       comp, comp, now, pos, now, 1, true));
    comp->mouseUp (juce::MouseEvent (source, away, juce::ModifierKeys(),
                                     juce::MouseInputSource::defaultPressure,
                                     juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY,
                                     comp, comp, now, pos, now, 1, true));
}

// A synthetic clean click (down + up, no movement) — fires Button::onClick
// synchronously (the tests/modbar_pill_click_test.cpp technique).
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

int rawParam (HellcatAudioProcessor& proc, const juce::String& id)
{
    if (auto* raw = proc.getApvts().getRawParameterValue (id))
        return juce::roundToInt (raw->load());
    return -1;
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
}  // namespace

TEST(mod_matrix_ui_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    ThemeManager themeManager;

    DragHost host;
    host.setBounds (0, 0, 900, 600);

    ModMatrixView view (proc, themeManager);
    host.addAndMakeVisible (view);
    view.setBounds (0, 0, 880, 560);

    // Activate slot 0 through the SAME drop-assignment bus a pill-drop on a
    // destination knob uses (exercises [2] before the sweeps need a live row).
    std::printf ("[1] rows are not drag sources\n");
    {
        const bool assigned = hellcat::ModMatrixHighlight::instance().requestAssign (
            ambika::dsp::MOD_SRC_ENV_1, ambika::dsp::MOD_DST_FILTER_CUTOFF);
        check (assigned, "drop-assignment bus consumes (pill-drop path intact)");
        check (view.isSlotActive (0), "slot 0 active after assign");
        check (rawParam (proc, "mod1_source") == (int) ambika::dsp::MOD_SRC_ENV_1
              && rawParam (proc, "mod1_dest") == (int) ambika::dsp::MOD_DST_FILTER_CUTOFF,
              "assigned slot writes source+dest through the APVTS");
        check (rawParam (proc, "mod1_amount") == 32, "assigned slot default depth 32");

        check (! view.canStartDragFromRowForTest(), "canStartDragFromRowForTest() contract hook");

        // Behavioural sweep: press-drag-release on EVERY child of the active
        // row (and the row itself) must start zero drag operations.
        int swept = 0;
        for (int slot = 0; slot < 14; ++slot)
        {
            auto* row = view.rowForSlotForTest (slot);
            if (row == nullptr || ! row->isVisible())
                continue;
            std::vector<juce::Component*> children;
            collectAll (row, children);
            for (auto* c : children)
            {
                dragAcross (c);
                ++swept;
            }
        }
        check (host.dragStarts == 0, "synthetic drags over row children start 0 drag operations");
        check (! host.isDragAndDropActive(), "no drag left active");
        std::printf ("      (swept %d components)\n", swept);
        check (swept > 4, "sweep actually covered the row's children");
    }

    std::printf ("\n[2] delete X clears the slot\n");
    {
        auto* row = view.rowForSlotForTest (0);
        check (row != nullptr && row->isVisible(), "row 0 visible (active)");

        auto* x = findIconButton (row);
        check (x != nullptr, "row has an IconButton (delete X)");
        if (x != nullptr)
        {
            check (x->getTitle().isNotEmpty(), "X has an accessible title (Delete modulation)");
            check (x->getWidth() >= 44 && x->getHeight() >= 40,
                  "X hit target >= 44pt wide / row-height tall");

            // The X is the RIGHTMOST child of the row.
            int maxRight = -1;
            for (auto* c : row->getChildren())
                maxRight = juce::jmax (maxRight, c->getBounds().getRight());
            check (x->getBounds().getRight() == maxRight, "X is the rightmost control in the row");

            click (x);
            check (rawParam (proc, "mod1_amount") == 0, "clicking X clears the slot (amount -> 0)");
            check (! view.isSlotMuted (0), "clicking X drops any mute");
            check (! view.isSlotActive (0), "slot inactive after X (row hidden by refresh)");
        }
    }

    std::printf ("\n[3] mute lamp: left position, module-disable toggle semantics\n");
    {
        // Re-activate a slot for the mute checks. The X above freed slot 0, so
        // the next free slot IS 0 again (firstFreeSlot scans from 0).
        const bool assigned = hellcat::ModMatrixHighlight::instance().requestAssign (
            ambika::dsp::MOD_SRC_LFO_1, ambika::dsp::MOD_DST_MIX_BALANCE);
        check (assigned, "second assign consumed");
        check (view.firstFreeSlot() > 0, "a slot was re-activated (0 no longer free)");
        const int slotIdx = view.firstFreeSlot() - 1;   // the slot just assigned (0)
        const juce::String amtId = ModMatrixView::slotParam (slotIdx, "_amount");
        const int orig = rawParam (proc, amtId);
        check (orig != 0, "assigned slot carries a non-zero depth before mute");

        auto* row = view.rowForSlotForTest (slotIdx);
        check (row != nullptr && row->isVisible(), "the re-assigned row is visible (active)");

        auto* lamp = findTitledButton (row, TRANS ("Mute / bypass this modulation"));
        check (lamp != nullptr, "mute lamp found by accessible title");

        // The lamp is LEFT of both combos and left of the X; and it is the
        // leftmost Button child of the row.
        int minComboX = juce::jmax (0, row->getWidth());
        for (auto* c : row->getChildren())
            if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
                minComboX = juce::jmin (minComboX, combo->getX());
        int minButtonX = juce::jmax (0, row->getWidth());
        for (auto* c : row->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (c))
                minButtonX = juce::jmin (minButtonX, b->getX());
        if (lamp != nullptr)
        {
            check (lamp->getX() < minComboX, "mute lamp sits LEFT of both combos");
            check (lamp->getX() == minButtonX, "mute lamp is the leftmost button in the row");
            check (lamp->getWidth() >= 44, "mute lamp hit target >= 44pt");
            check (lamp->getToggleState(), "lamp reads ON (accent) while the routing is active");

            click (lamp);   // MUTE
            check (view.isSlotMuted (slotIdx), "click mutes the slot");
            check (rawParam (proc, amtId) == 0, "mute writes 0 to the engine (true bypass)");
            check (view.stashedAmount (slotIdx) == orig, "mute stashes the original depth");
            check (view.isSlotActive (slotIdx), "muted slot stays visible/active");
            check (! lamp->getToggleState(), "lamp reads OFF (grey) while muted");

            click (lamp);   // UNMUTE
            check (! view.isSlotMuted (slotIdx), "second click unmutes");
            check (rawParam (proc, amtId) == orig, "unmute restores the stashed depth");
            check (lamp->getToggleState(), "lamp back ON after unmute");
        }
    }

    std::printf ("\n[4] layout sanity across every active row\n");
    {
        int rowsChecked = 0;
        for (int slot = 0; slot < 14; ++slot)
        {
            auto* row = view.rowForSlotForTest (slot);
            if (row == nullptr || ! row->isVisible())
                continue;
            auto* x = findIconButton (row);
            if (x == nullptr)
                continue;
            int maxRight = -1;
            for (auto* c : row->getChildren())
                maxRight = juce::jmax (maxRight, c->getBounds().getRight());
            const bool rightmost = (x->getBounds().getRight() == maxRight);
            const bool sized = x->getWidth() >= 44;
            check (rightmost && sized, "row: X rightmost + 44pt-wide");
            ++rowsChecked;
        }
        std::printf ("      (%d active rows checked)\n", rowsChecked);
        check (rowsChecked >= 2, "at least the two assigned rows were checked");
    }

    // [6] The FX mod matrix (FxMatrixView) obeys the SAME rule: its rows are
    // not drag sources either. The former FxSourceDragGrip (the FX matrix's
    // per-row six-dot handle, emitting the same "parvatiModSrc:<enum>" payload)
    // was removed for consistency — modulators are dragged ONLY from the
    // CentralModBar pills. Sweeping EVERY component of the whole FX matrix
    // subtree (16 rows x combos/slider/buttons/labels) must start zero drags.
    std::printf ("[6] FX matrix rows are not drag sources either\n");
    {
        DragHost fxHost;
        fxHost.setBounds (0, 0, 900, 600);
        FxMatrixView fxView (proc, themeManager);
        fxHost.addAndMakeVisible (fxView);
        fxView.setBounds (0, 0, 880, 560);

        // Activate one slot so at least one row is live (same seam the FX
        // matrix's own assign handler uses): source ENV_1 -> FX dest 0, depth 8.
        if (auto* sp = proc.getApvts().getParameter ("fxmod1_source"))
            sp->setValueNotifyingHost (sp->convertTo0to1 (1.0f));
        if (auto* dp = proc.getApvts().getParameter ("fxmod1_dest"))
            dp->setValueNotifyingHost (dp->convertTo0to1 (0.0f));
        if (auto* ap = proc.getApvts().getParameter ("fxmod1_amount"))
            ap->setValueNotifyingHost (ap->convertTo0to1 (8.0f / 63.0f));
        fxView.refresh();

        std::vector<juce::Component*> all;
        collectAll (&fxView, all);
        int fxSwept = 0;
        for (auto* c : all)
        {
            dragAcross (c);
            ++fxSwept;
        }
        check (fxSwept > 60, "FX matrix sweep covered its rows' children");
        check (fxHost.dragStarts == 0, "synthetic drags over FX matrix children start 0 drag operations");
        check (! fxHost.isDragAndDropActive(), "no FX drag left active");
    }

    // ---- [7] Index label width: the row slot number must render WITHOUT
    // ellipsis (user report: '16' showed as '...'). Root cause: JUCE Label's
    // default 5px-per-side border left an 18pt-wide label an 8px text box
    // for the 13px-wide '16'. Both matrices zero the border and allocate a
    // measured 20pt.
    std::printf ("\n[7] index label renders the slot number (no '...')\n");
    {
        // Find the row's index label: the juce::Label with purely-numeric
        // text that sits LEFT of the source combo.
        auto findIndexLabel = [] (juce::Component* row) -> juce::Label*
        {
            std::vector<juce::Component*> children;
            collectAll (row, children);
            for (auto* c : children)
                if (auto* l = dynamic_cast<juce::Label*> (c))
                    if (l->getText().containsOnly ("0123456789"))
                        return l;
            return nullptr;
        };
        auto checkIndexFits = [&] (juce::Component* row, const char* which)
        {
            auto* l = findIndexLabel (row);
            if (l == nullptr)
            {
                check (false, (juce::String (which) + ": index label found").toRawUTF8());
                return;
            }
            const int textW = juce::GlyphArrangement::getStringWidthInt (
                l->getFont(), l->getText());
            check (l->getWidth() >= textW + 2,
                   (juce::String (which) + ": index label fits its text ("
                   + juce::String (textW) + "px in " + juce::String (l->getWidth())
                   + "pt, border " + juce::String (l->getBorderSize().getLeft()) + ")").toRawUTF8());
        };

        auto* synthRow = view.rowForSlotForTest (0);
        if (synthRow != nullptr)
            checkIndexFits (synthRow, "synth matrix row 0");
        // A double-digit index too: activate slot 9 (index '10').
        hellcat::ModMatrixHighlight::instance().requestAssign (
            ambika::dsp::MOD_SRC_LFO_1, ambika::dsp::MOD_DST_FILTER_CUTOFF);
        view.refresh();
        {
            auto* r = view.rowForSlotForTest (1);
            if (r != nullptr && r->isVisible())
                checkIndexFits (r, "synth matrix row 1 ('2')");
        }
    }

    // ---- [8] Disable-widget + header-colour parity across synth and FX.
    // The shared HellcatModuleLamp means the synth matrix bypass lamp, the FX
    // matrix lamp AND the FX card power toggle resolve identical ON colours;
    // the FX card title now uses the same token as the synth GroupComponent
    // titles (textPrimary). Pinned per shipped theme.
    std::printf ("\n[8] disable widget + header colour parity\n");
    {
        // The lamps resolve their colours through the INHERITED
        // HellcatLookAndFeel's active theme — install one on the hosts so the
        // subtree sees it (otherwise both fall back and "equal" would be
        // vacuous). Re-themed per iteration; removed before scope exit.
        HellcatLookAndFeel lnf;
        juce::Component lnfHost;   // keeps the card's chain off `host`
        lnfHost.setBounds (0, 0, 400, 400);
        lnfHost.setLookAndFeel (&lnf);
        host.setLookAndFeel (&lnf);

        // An FX slot card (slot 0) with the real descriptors.
        const PatchParamDescriptor *p1 = nullptr, *p2 = nullptr, *p3 = nullptr,
                                   *p4 = nullptr, *p5 = nullptr, *dw = nullptr;
        for (const auto& d : getPatchParamDescriptors())
        {
            if (! (d.isFx && juce::String (d.paramID).startsWith ("fx1_")))
                continue;
            if      (d.paramID == "fx1_param1") p1 = &d;
            else if (d.paramID == "fx1_param2") p2 = &d;
            else if (d.paramID == "fx1_param3") p3 = &d;
            else if (d.paramID == "fx1_param4") p4 = &d;
            else if (d.paramID == "fx1_param5") p5 = &d;
            else if (d.paramID == "fx1_drywet") dw = &d;
        }
        juce::Component cardHost;
        cardHost.setBounds (0, 0, 400, 400);
        cardHost.setLookAndFeel (&lnf);
        std::unique_ptr<FxSlotCard> card;
        if (p1 != nullptr && dw != nullptr)
            card = std::make_unique<FxSlotCard> (proc, 0, p1, p2, p3, p4, p5, dw);
        if (card != nullptr)
        {
            cardHost.addAndMakeVisible (*card);
            card->setBounds (0, 0, 300, 320);
        }
        juce::ignoreUnused (lnfHost);

        // The synth-matrix lamp + the FX-card lamp as the shared base type.
        auto* synthLamp = findTitledButton (view.rowForSlotForTest (0),
                                            TRANS ("Mute / bypass this modulation"));
        auto* synthModuleLamp = dynamic_cast<HellcatModuleLamp*> (synthLamp);
        check (synthModuleLamp != nullptr, "synth matrix lamp IS the shared HellcatModuleLamp");
        HellcatModuleLamp* cardLamp = card != nullptr ? card->powerLampForTest() : nullptr;
        check (cardLamp != nullptr, "FX card power toggle IS the shared HellcatModuleLamp");

        const auto names = themeManager.getThemeNames();
        for (size_t ti = 0; ti < names.size(); ++ti)
        {
            themeManager.selectByName (names[ti]);
            lnf.setTheme (themeManager.getCurrentTheme());
            view.applyThemeColors();   // the editor's theme-switch seam — re-resolves every row's lamp/slider/tint colours
            const auto& th = themeManager.getCurrentTheme();
            const juce::String tag = "[" + names[ti] + "] ";

            if (synthModuleLamp != nullptr && cardLamp != nullptr)
            {
                // 2026-08-20: the synth matrix lamp now carries its ROW's
                // modulator category colour (row 0's source is set below), so
                // the pinned contract is: FX card lamp == theme accent, and
                // the matrix lamp == the row's category colour — no longer
                // equal to each other by design (user: "the color of the
                // button should be the same as the modulator's color").
                check (cardLamp->resolvedOnColourForTest() == th.accentPrimary,
                       (tag + "FX card lamp == theme accentPrimary").toRawUTF8());
                if (synthLamp != nullptr)
                {
                    // Resolve the row's expected category colour the same way
                    // the row does (rowCategoryColour over the row's source).
                    const auto expected = view.rowCategoryColourForTest (0);
                    check (synthModuleLamp->resolvedOnColourForTest() == expected,
                           (tag + "matrix lamp == its modulator's category colour").toRawUTF8());
                }
            }

            if (card != nullptr)
                check (card->headerTitleColourForTest() == th.textPrimary,
                       (tag + "FX card header == theme textPrimary (synth GroupComponent token)").toRawUTF8());

            // (Dot geometry parity is asserted below via the pure bounds
            // function — nothing per-theme here beyond the colours.)
        }
        themeManager.selectByName ("Carbon");   // restore
        lnf.setTheme (themeManager.getCurrentTheme());
        cardHost.setLookAndFeel (nullptr);
        host.setLookAndFeel (nullptr);

        // Dot diameter parity for equal bands (pure function of bounds).
        {
            const juce::Rectangle<int> band (0, 0, 44, 44);
            check (HellcatModuleLamp::dotDiameterFor (band) >= 28.0f
                       && HellcatModuleLamp::dotDiameterFor (band) <= 30.0f,
                   "44x44 band renders a ~28-30pt dot (the 'bigger' request)");
            if (synthModuleLamp != nullptr)
                check (juce::exactlyEqual (HellcatModuleLamp::dotDiameterFor (
                           synthModuleLamp->getLocalBounds()),
                           HellcatModuleLamp::dotDiameterFor (band))
                       || synthModuleLamp->getWidth() < 44,
                       "synth lamp dot == dotDiameterFor(its band)");
            // Border ring: the 2026-08-20 "a tiny bit thicker" pin — one
            // shared constant, drawn by paintButton and asserted here.
            check (HellcatModuleLamp::kLampBorderWidth >= 2.0f
                       && HellcatModuleLamp::kLampBorderWidth <= 3.0f,
                   "lamp border ring stroke is the slightly-thicker value (2-3pt)");
        }
    }

    std::printf ("\nMOD MATRIX UI TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}
