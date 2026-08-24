// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See GroupPager.h.

#include "GroupPager.h"

#include "ParamPage.h"        // ParamPage complete type (ParamControl statics via it)
#include "ParvatiLookAndFeel.h"   // appFont()/getTheme() + parvatiTabCategoryColourId
#include "ThemeManager.h"

namespace
{
//==============================================================================
// DraggableTabButton — a TabbedButtonBar tab button that can itself be DRAGGED
// onto a destination knob to assign the tab's modulation source. Mirrors the
// mod-pill drag in CentralModBar: mouseDrag past ~5px starts
// an internal DragAndDropContainer drag carrying "parvatiModSrc:<enum>" and a
// themed chip image; a clean click still switches the sub-tab.
//
// Click-vs-drag disambiguation (no headless test: needs a real
// DragAndDropContainer ancestor + a real pointer):
//   * mouseDown   resets dragStarted_ and runs the base Button machinery so the
//     button enters its "down" state (a normal click then registers on mouseUp).
//   * mouseDrag   (one-time, past the threshold) calls startDragging and latches
//     dragStarted_.
//   * clicked()   is where the base TabBarButton would setCurrentTabIndex(). If
//     this press became a drag, suppress the switch (a drag must NOT also flip
//     the tab) and clear the flag; otherwise defer to the base. clicked() is the
//     definitive gate regardless of whether the drag moved the pointer off the
//     button, so a tab never flips spuriously after a drag. dragStarted_ is also
//     reset on the next mouseDown in case clicked() was never reached.
// When @p map is null (or returns -1 for a label) the button is inert and
// behaves exactly like a plain TabBarButton.
class DraggableTabButton : public juce::TabBarButton
{
public:
    DraggableTabButton (const juce::String& name, juce::TabbedButtonBar& bar,
                        GroupPager::TabSourceMap map)
        : juce::TabBarButton (name, bar), map_ (std::move (map)) {}

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStarted_ = false;              // fresh press
        juce::Button::mouseDown (e);       // enter the down state (default click path)
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        juce::Button::mouseDrag (e);       // keep default "mouse over" state tracking

        if (dragStarted_ || e.getDistanceFromDragStart() <= 5)
            return;                        // one drag per press; ignore sub-threshold jitter
        if (! map_)
            return;                        // this pager's sub-tabs are not draggable

        const int src = map_ (getButtonText());
        if (src < 0)
            return;                        // this label is not a draggable generator (NOTES/VEL/...)

        auto* ddc = findParentComponentOfClass<juce::DragAndDropContainer>();
        if (ddc == nullptr)
            return;                        // no DragAndDropContainer ancestor (e.g. a headless test)

        dragStarted_ = true;
        ddc->startDragging ("parvatiModSrc:" + juce::String (src), this,
                            juce::ScaledImage (buildDragImage()), true);
    }

    // The base TabBarButton::clicked(mods) switches the tab. Suppress it when
    // this press became a DnD drag so a drag never also flips the sub-tab.
    void clicked (const juce::ModifierKeys& mods) override
    {
        if (dragStarted_)
        {
            dragStarted_ = false;
            return;
        }
        // Tap-to-assign: surface this tab's generator as the selected mod
        // source and SUPPRESS the sub-tab flip (mirrors the drag guard above so
        // an assign tap does not also switch the sub-tab). Inert unless [MOD] on.
        // Only a tab that actually mapped to a source consumes the tap — a
        // NON-source sub-tab (a pager with no map_, or NOTES/VEL-style labels
        // that resolve to -1) falls through to the normal flip so the pager
        // stays navigable in [MOD] mode (returning unconditionally would
        // dead-end it with no hint why).
        if (ParamControl::tapAssignActive())
        {
            const int src = map_ ? map_ (getButtonText()) : -1;
            if (src >= 0)
            {
                ParamControl::setTapSelectedSource (src);
                return;
            }
        }
        juce::TabBarButton::clicked (mods);
    }

private:
    // A small themed drag chip: the tab's category colour bar + its label, on a
    // container-fill rounded tile (mirrors ModSourceDragGrip::buildDragImage).
    juce::Image buildDragImage() const
    {
        const ParvatiTheme* t = parvati::themeFor (*this);
        const juce::Font f = parvati::appFontFor (*this, 13.0f);

        const juce::String name = getButtonText();
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, name);
        const int w = juce::jmax (48, 12 + 8 + textW + 10);
        const int h = 22;

        const juce::Colour containerFill = (t != nullptr) ? t->containerFill : juce::Colours::lightgrey;
        const juce::Colour textCol       = (t != nullptr) ? t->textPrimary          : juce::Colours::black;
        const juce::Colour accent        = (t != nullptr) ? t->accentPrimary         : juce::Colours::darkgrey;

        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        g.setColour (containerFill);
        g.fillRoundedRectangle (img.getBounds().toFloat(), 5.0f);

        // Category colour chip (this tab's parvatiTabCategoryColourId; falls
        // back to accent for a tab without a category assigned).
        const juce::Colour cat = findColour (parvatiTabCategoryColourId, false);
        g.setColour (cat.isTransparent() ? accent : cat);
        g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 5.0f, 7.0f, static_cast<float> (h) - 10.0f), 2.0f);

        g.setColour (textCol);
        g.setFont (f);
        g.drawText (name, juce::Rectangle<int> (17, 0, w - 17, h), juce::Justification::centredLeft, true);

        g.setColour (accent.withAlpha (0.6f));
        g.drawRoundedRectangle (img.getBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        return img;
    }

    GroupPager::TabSourceMap map_;
    bool dragStarted_ = false;
};

//==============================================================================
// A TabbedButtonBar that creates DraggableTabButtons instead of plain
// TabBarButtons, so each sub-tab can carry the drag payload. @p map is forwarded
// to every button; a null map => inert buttons (plain-TabBarButton behaviour),
// used by non-generator pagers (MODIFIERS).
class DraggableTabButtonBar : public juce::TabbedButtonBar
{
public:
    explicit DraggableTabButtonBar (GroupPager::TabSourceMap map)
        : juce::TabbedButtonBar (juce::TabbedButtonBar::TabsAtTop), map_ (std::move (map)) {}

    juce::TabBarButton* createTabButton (const juce::String& tabName, int /*tabIndex*/) override
    {
        return new DraggableTabButton (tabName, *this, map_);
    }

private:
    GroupPager::TabSourceMap map_;
};
}   // namespace

//==============================================================================
GroupPager::GroupPager (ThemeManager& tm, ParamPage* page, std::vector<Subset> subsets,
                        juce::Colour categoryColour, TabSourceMap tabDragSource)
    : themeManager_ (tm), page_ (page), subsets_ (std::move (subsets)),
      bar_ (std::make_unique<DraggableTabButtonBar> (std::move (tabDragSource))),
      tabCategoryColour_ (categoryColour)
{
    bar_->setMinimumTabScaleFactor (0.25);
    addAndMakeVisible (*bar_);
    bar_->addChangeListener (this);   // TabbedButtonBar broadcasts on every click

    const auto bg = themeManager_.getCurrentTheme().backgroundBase;
    for (const auto& s : subsets_)
        bar_->addTab (s.first, bg, -1);   // (name, tab fill colour, append)

    // Colour every sub-tab with the bar's parent-category hue (ENV*->cyan,
    // LFO*->magenta, SEQ*->green, MOD MATRIX/MODIFIERS ->amber).
    applySubTabCategoryColours();

    // The paginated page stays editor-owned (generatedPages_); it is merely
    // reparented here so every APVTS attachment survives. addAndMakeVisible does
    // NOT transfer ownership, so GroupPager never deletes it.
    if (page_ != nullptr)
        addAndMakeVisible (*page_);

    if (! subsets_.empty())
        selectSubset (0);               // show the first sub-tab's groups
    else if (page_ != nullptr)
        page_->setVisibleGroups ({});   // empty => show ALL groups
}

//==============================================================================
void GroupPager::paint (juce::Graphics& g)
{
    // Void-free fill behind the bar + page (transparent children sit on this).
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

void GroupPager::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    bar_->setBounds (area.removeFromTop (kBarH));

    // Reflow the page to the content area width/height. reflowToWidth sizes the
    // page to (width, max(naturalH, viewH)); the per-sub-tab pagination keeps each
    // subset short enough to fit the height, so the page fills the area with NO
    // scrollbar (vertical scroll was removed workspace-wide).
    if (page_ != nullptr)
    {
        page_->setBounds (area);
        page_->reflowToWidth (juce::jmax (150, area.getWidth()), juce::jmax (0, area.getHeight()));
    }
}

//==============================================================================
void GroupPager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The bar broadcasts on every tab change: show the new sub-tab's group subset.
    selectSubset (bar_->getCurrentTabIndex());
}

void GroupPager::selectSubset (int index)
{
    if (index < 0 || index >= (int) subsets_.size())
        return;
    current_ = index;
    if (page_ == nullptr)
        return;

    // Switch the page to the new group subset, then REFLOW to the real content
    // area (below the bar). reflowToWidth lays out at the GroupPager's actual
    // width/height and re-seats the page — defence-in-depth so a runtime sub-tab
    // switch never leaves a stale-width layout (reviewer blocker B1).
    page_->setVisibleGroups (subsets_[(size_t) index].second);

    auto area = getLocalBounds();
    if (area.getHeight() > kBarH)
    {
        area.removeFromTop (kBarH);
        page_->setBounds (area);
        page_->reflowToWidth (juce::jmax (150, area.getWidth()),
                              juce::jmax (0, area.getHeight()));
    }
}

//==============================================================================
void GroupPager::setTabCategoryColour (juce::Colour colour)
{
    tabCategoryColour_ = colour;
    applySubTabCategoryColours();
}

void GroupPager::applySubTabCategoryColours()
{
    for (int i = 0; i < bar_->getNumTabs(); ++i)
        if (auto* btn = bar_->getTabButton (i))
            btn->setColour (parvatiTabCategoryColourId, tabCategoryColour_);
}

//==============================================================================
void GroupPager::applyThemeColors()
{
    const auto bg = themeManager_.getCurrentTheme().backgroundBase;
    for (int i = 0; i < bar_->getNumTabs(); ++i)
        bar_->setTabBackgroundColour (i, bg);
    applySubTabCategoryColours();   // re-colour sub-tabs (snapshot set by setTabCategoryColour)
    if (page_ != nullptr)
        page_->applyThemeColors();
    repaint();
}
