// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See CentralModBar.h.
//
// Layout geometry (px):
//   kBarHeight = 38 (declared in the header), pill height 28 -> 5px top/bottom.
//   Pills are left-aligned within each cluster; clusters are separated by a
//   gap. Each cluster is prefixed by a small caption (cluster name) drawn by
//   CentralModBar::paint(). preferredWidth() returns the exact width needed so
//   every pill + caption fits with no clipping.

#include "CentralModBar.h"

#include "ModSourceCatalog.h"
#include "ParvatiLookAndFeel.h"   // appFont() when the bar is reparented
#include "ParvatiTheme.h"

namespace
{
    constexpr int kPillH           = 28;   // pill height (spec)
    constexpr int kPillHPad        = 8;    // horizontal padding inside a pill
    constexpr int kPillMinW        = 30;   // minimum pill width
    constexpr int kPillGap         = 4;    // gap between pills within a cluster
    constexpr int kClusterGap      = 14;   // gap between clusters
    constexpr int kEdgePad         = 6;    // left/right outer padding
    constexpr int kClusterLabelW   = 30;   // per-cluster caption width
    constexpr int kClusterLabelGap = 5;    // gap after the caption, before pills

    const char* clusterLabel (parvati::Cluster c)
    {
        switch (c)
        {
            case parvati::Cluster::Env:    return "ENV";
            case parvati::Cluster::Lfo:    return "LFO";
            case parvati::Cluster::SeqArp: return "SEQ";
            case parvati::Cluster::Perf:   return "PERF";
            case parvati::Cluster::Util:   return "UTIL";
            case parvati::Cluster::Mod:    return "MOD";
            case parvati::Cluster::Const:  return "CONST";
        }
        return "";
    }
}  // namespace

//==============================================================================
// ModPill — a single modulation-source micro-pill. Generators (Env/LFO/Seq/Arp/
// Op) draw a solid 1px accent border + an active-state underline glow; drag-only
// sources (Perf/Util/Const) draw a subtle dotted left-handle and no glow.
//
// Mouse handling mirrors the existing drag sources (DraggableTabButton /
// ModSourceDragGrip / WheelDragLabel): a drag past ~5px starts an INTERNAL
// DragAndDropContainer drag carrying "parvatiModSrc:<enum>"; a clean click (no
// drag) fires the bar's click callback. The same payload keeps the existing
// drop-side feedback (concentric rings, padlock cursor, ModMatrixHighlight)
// working with zero changes on the destination side.
//
// ModPill is a PRIVATE nested struct of CentralModBar, so its public data
// members are only reachable from CentralModBar (which needs them for layout,
// theme resolution and active-state).
struct CentralModBar::ModPill : public juce::Component,
                                public juce::SettableTooltipClient
{
    ModPill (CentralModBar& owner, const parvati::SourceEntry& e)
        : owner_ (owner),
          enumValue_ (e.enumValue),
          shortLabel_ (e.shortLabel),
          fullName_ (e.fullName),
          cluster_ (e.cluster),
          isGenerator_ (e.isGenerator)
    {
        setTooltip (fullName_);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setInterceptsMouseClicks (true, false);
    }

    /** Preferred width for this pill given the pill font. */
    int widthForFont (const juce::Font& f) const
    {
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, shortLabel_);
        return juce::jmax (kPillMinW, textW + 2 * kPillHPad);
    }

    void setActive (bool a)
    {
        if (a != active_) { active_ = a; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto&      t = owner_.theme();
        const juce::Font f = owner_.pillFont();
        const juce::Rectangle<float> r = getLocalBounds().toFloat().reduced (0.5f);

        if (isGenerator_)
        {
            // MONOCHROME: flat dark-gray fill for every inactive pill; only the
            // selected/active generator is highlighted with the accent colour
            // (solid 1.5px border + a glowing underline). Geometry unchanged.
            const juce::Colour inactiveFill = t.tabUnselectedBg;
            const juce::Colour fill = (active_ || ! hovered_)
                                    ? inactiveFill
                                    : inactiveFill.brighter (0.20f);   // hover: slightly lighter
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);

            if (active_)
            {
                g.setColour (accent_);
                g.drawRoundedRectangle (r, 5.0f, 1.5f);

                // active-state underline glow (generators only) — stays, in accent
                const float uy = r.getBottom() - 1.5f;
                g.setColour (accent_.withAlpha (0.30f));
                g.fillRoundedRectangle (r.withTop (uy - 3.0f), 5.0f);
                g.setColour (accent_);
                g.fillRect (juce::Rectangle<float> (r.getX() + 3.0f, uy - 1.0f,
                                                    r.getWidth() - 6.0f, 2.0f));
            }

            g.setColour (active_ ? t.textValue : t.text);
            g.setFont (f);
            g.drawText (shortLabel_, getLocalBounds(), juce::Justification::centred, true);
        }
        else
        {
            // drag-only: flat dark-gray fill (monochrome) + a subtle dotted
            // left-handle in neutral gray (no per-cluster colour) + faint label.
            // No border / glow. Geometry unchanged.
            const juce::Colour inactiveFill = t.tabUnselectedBg;
            const juce::Colour fill = hovered_ ? inactiveFill.brighter (0.20f) : inactiveFill;
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);

            const float hx  = r.getX() + 2.0f;
            const float hy0 = r.getY() + 7.0f;
            const float hy1 = r.getBottom() - 7.0f;
            const float step = 3.0f;
            g.setColour (t.textDim);
            for (int i = 0;; ++i)
            {
                const float y = hy0 + static_cast<float> (i) * step;
                if (y > hy1)
                    break;
                g.fillEllipse (juce::Rectangle<float> (1.6f, 1.6f).withCentre ({ hx, y }));
            }

            g.setColour (t.textDim);
            g.setFont (f);
            g.drawText (shortLabel_, getLocalBounds().reduced (5, 0), juce::Justification::centred, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered_ = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered_ = false; repaint(); }

    void mouseDown (const juce::MouseEvent&) override { dragStarted_ = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Bar-only sentinel pills (e.g. the Note Sequencer, enumValue < 0) have
        // no real MOD_SRC_* enum, so a drag would route a non-existent source.
        // They are click-only: skip startDragging entirely and let mouseUp fire
        // the plain-click callback (it opens the generator editor).
        if (enumValue_ < 0)
            return;

        // One drag per press; ignore sub-threshold jitter so a stray wiggle never
        // fires a phantom drag (same threshold as the other drag sources).
        if (dragStarted_ || e.getDistanceFromDragStart() <= 5)
            return;

        auto* ddc = findParentComponentOfClass<juce::DragAndDropContainer>();
        if (ddc == nullptr)
            return;   // no DragAndDropContainer ancestor (e.g. a headless test)

        dragStarted_ = true;
        ddc->startDragging ("parvatiModSrc:" + juce::String (enumValue_), this, buildDragImage(), true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasDrag = dragStarted_;
        dragStarted_ = false;
        // A real click (no drag, negligible movement) fires the callback.
        if (! wasDrag && e.getDistanceFromDragStart() <= 5)
            owner_.invokeClicked (enumValue_);
    }

    // ---- public state (only reachable from CentralModBar) ----
    CentralModBar&    owner_;
    int               enumValue_;
    juce::String      shortLabel_;
    juce::String      fullName_;
    parvati::Cluster  cluster_;
    bool              isGenerator_;
    bool              dragStarted_ = false;
    bool              active_      = false;
    bool              hovered_     = false;
    juce::Colour      accent_;          // resolved from the active theme (monochrome accent)

private:
    // A small themed drag chip (mirrors ModSourceDragGrip / DraggableTabButton):
    // the cluster accent bar + short label on a container-fill rounded tile.
    juce::Image buildDragImage() const
    {
        const auto&      t = owner_.theme();
        const juce::Font f = owner_.pillFont();
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, shortLabel_);
        const int w = juce::jmax (48, 12 + 8 + textW + 10);
        const int h = 22;

        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        g.setColour (t.containerFill);
        g.fillRoundedRectangle (img.getBounds().toFloat(), 5.0f);

        g.setColour (accent_);
        g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 5.0f, 7.0f, static_cast<float> (h) - 10.0f), 2.0f);

        g.setColour (t.text);
        g.setFont (f);
        g.drawText (shortLabel_, juce::Rectangle<int> (17, 0, w - 17, h), juce::Justification::centredLeft, true);

        g.setColour (t.accent.withAlpha (0.6f));
        g.drawRoundedRectangle (img.getBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        return img;
    }
};

//==============================================================================
CentralModBar::CentralModBar (ThemeManager& themeManager)
    : themeManager_ (themeManager)
{
    // One pill per catalogue entry, in cluster display order.
    for (const auto& e : parvati::kAllSources)
        pills_.push_back (std::make_unique<ModPill> (*this, e));
    for (auto& p : pills_)
        addAndMakeVisible (*p);

    applyThemeColors();
}

CentralModBar::~CentralModBar() = default;

void CentralModBar::setOnPillClicked (std::function<void (int)> cb)
{
    onPillClicked_ = std::move (cb);
}

void CentralModBar::setActiveGenerator (int modSrcEnum)
{
    activeEnum_ = modSrcEnum;
    for (auto& p : pills_)
        p->setActive (p->enumValue_ == modSrcEnum && p->isGenerator_);
}

void CentralModBar::applyThemeColors()
{
    const auto& t = theme();
    // MONOCHROME: every pill shares a single accent palette. Inactive pills are
    // a flat dark-gray (tabUnselectedBg); only the selected/active generator
    // pill is highlighted with the accent colour (its underline glow stays, in
    // accent). The per-cluster cat* tokens remain in ParvatiTheme — they are
    // still used for the knob modulation rings — but the BAR no longer reads
    // them. Hover is resolved in ModPill::paint() as a slightly lighter fill.
    for (auto& p : pills_)
        p->accent_ = t.accent;
    repaint();
}

void CentralModBar::invokeClicked (int modSrcEnum)
{
    if (onPillClicked_)
        onPillClicked_ (modSrcEnum);
}

const ParvatiTheme& CentralModBar::theme() const
{
    return themeManager_.getCurrentTheme();
}

juce::Font CentralModBar::pillFont() const
{
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        return lnf->appFont (12.0f, juce::Font::plain);
    return juce::Font (juce::FontOptions (12.0f));
}

juce::Font CentralModBar::labelFont() const
{
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        return lnf->appFont (9.0f, juce::Font::plain);
    return juce::Font (juce::FontOptions (9.0f));
}

void CentralModBar::paint (juce::Graphics& g)
{
    const auto&      t  = theme();
    const juce::Font lf = labelFont();
    g.setFont (lf);

    const auto& clusters = parvati::clustersInOrder();
    for (size_t ci = 0; ci < clusters.size() && ci < clusterLabelRects_.size(); ++ci)
    {
        // Monochrome: neutral-gray cluster captions (the gaps still segment the
        // clusters); the bar no longer uses per-cluster accent colours.
        g.setColour (t.textDim);
        g.drawText (clusterLabel (clusters[ci]), clusterLabelRects_[ci],
                    juce::Justification::centredLeft, true);
    }
}

void CentralModBar::resized()
{
    computeLayout (true, &clusterLabelRects_);
}

int CentralModBar::preferredWidth() const
{
    return computeLayout (false, nullptr);
}

int CentralModBar::computeLayout (bool positionChildren,
                                  std::vector<juce::Rectangle<int>>* outLabelRects) const
{
    const juce::Font f = pillFont();
    const int        yOff = (kBarHeight - kPillH) / 2;   // vertical centre of the pills

    const auto& clusters = parvati::clustersInOrder();
    if (outLabelRects != nullptr)
        outLabelRects->assign (clusters.size(), {});

    int x = kEdgePad;
    size_t idx = 0;   // walks kAllSources, which is already in cluster order

    for (size_t ci = 0; ci < clusters.size(); ++ci)
    {
        const parvati::Cluster c = clusters[ci];

        // Cluster caption region (drawn by paint()).
        if (outLabelRects != nullptr)
            (*outLabelRects)[ci] = juce::Rectangle<int> (x, 0, kClusterLabelW, kBarHeight);
        x += kClusterLabelW + kClusterLabelGap;

        // Pills belonging to this cluster (kAllSources is ordered to match).
        bool first = true;
        for (; idx < parvati::kAllSources.size() && parvati::kAllSources[idx].cluster == c; ++idx)
        {
            if (! first) x += kPillGap;
            first = false;

            const int w = pills_[idx]->widthForFont (f);
            if (positionChildren)
                pills_[idx]->setBounds (x, yOff, w, kPillH);
            x += w;
        }

        x += kClusterGap;   // separator after every cluster
    }

    // Drop the trailing cluster gap and add the right edge padding.
    return (x - kClusterGap) + kEdgePad;
}
