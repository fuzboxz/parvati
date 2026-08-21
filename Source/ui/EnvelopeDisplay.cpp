// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See EnvelopeDisplay.h.

#include "EnvelopeDisplay.h"

#include <cmath>

#include "VectorTrace.h"

//==============================================================================
// ADSR segment geometry + level — ONE definition shared by the drawn curve and
// the live stage marker (docs/LIVE_MOD_FEEDBACK_DESIGN.md) so the marker always
// rides the exact curve the panel paints.
//
// Segment widths are proportional to the a/d/r knob values EXCEPT the attack,
// which has a MINIMUM VISUAL WIDTH (kMinAttackShare of the other segments' sum,
// 2026-08-20 user request: "display the initial transient going from 0 to 100%",
// previously a sub-4 ms attack collapsed to an invisible 1-2% sliver — or to
// nothing at a == 0, where the trace started AT the peak). With the floor, a
// fast attack renders as a near-vertical ramp at the left edge — always
// visible — and slower attacks keep their proportional share unchanged.
// The sustain plateau keeps its fixed minimum so it always reads.
void EnvelopeDisplay::adsrSegmentSpans (float a, float d, float s, float r,
                                        float* wA, float* wD, float* wS, float* wR)
{
    juce::ignoreUnused (s);                     // the sustain LEVEL shapes the curve, not its width
    constexpr float kSustainMin    = 0.5f;      // sustain plateau (fixed minimum)
    constexpr float kMinAttackShare = 0.09f;    // attack floor: >= ~8% of total

    *wS = kSustainMin;
    *wA = juce::jmax (a, kMinAttackShare * (d + *wS + r));
    *wD = d;
    *wR = r;
}

// PURE ADSR curve shape (normalized 0..1 knob values -> level at normalized x),
// evaluated over the adsrSegmentSpans geometry. EXPOSED as a pure static
// (adsrCurveLevelForTest) so the shape is testable without a Graphics context;
// a test pins it, so the span helper above must keep this exact behaviour.
float EnvelopeDisplay::adsrCurveLevel (float a, float d, float s, float r, float xf)
{
    float wA, wD, wS, wR;
    adsrSegmentSpans (a, d, s, r, &wA, &wD, &wS, &wR);
    const float total = wA + wD + wS + wR;   // wS keeps total > 0 (no /0)
    const float fracA = wA / total, fracD = wD / total, fracS = wS / total, fracR = wR / total;
    const float xEndA = fracA;
    const float xEndD = fracA + fracD;
    const float xEndS = fracA + fracD + fracS;

    if (xf <= xEndA)
    {
        const float tt = fracA > 0.0f ? xf / fracA : 1.0f;
        return 1.0f - std::pow (1.0f - tt, 2.0f);                 // attack ease-out (0 -> 1)
    }
    if (xf <= xEndD)
    {
        const float tt = fracD > 0.0f ? (xf - xEndA) / fracD : 1.0f;
        return s + (1.0f - s) * std::pow (1.0f - tt, 2.0f);        // decay settle
    }
    if (xf <= xEndS)
        return s;                                                  // sustain plateau
    const float tt = fracR > 0.0f ? (xf - xEndS) / fracR : 1.0f;
    return s * std::pow (1.0f - tt, 2.0f);                         // release decay
}

//==============================================================================
EnvelopeDisplay::EnvelopeDisplay (juce::String title,
                                 std::function<float()> getAttack,
                                 std::function<float()> getDecay,
                                 std::function<float()> getSustain,
                                 std::function<float()> getRelease,
                                 std::function<float()> getShape)
    : title_ (std::move (title)),
      getAttack_   (std::move (getAttack)),
      getDecay_    (std::move (getDecay)),
      getSustain_  (std::move (getSustain)),
      getRelease_  (std::move (getRelease)),
      getShape_    (std::move (getShape))
{
    // A getter that was not supplied reads as 0 (so a default-constructed /
    // partially-bound display still renders a sane shape).
    if (! getAttack_)  getAttack_  = [] { return 0.0f; };
    if (! getDecay_)   getDecay_   = [] { return 0.0f; };
    if (! getSustain_) getSustain_ = [] { return 0.0f; };
    if (! getRelease_) getRelease_ = [] { return 0.0f; };
    if (! getShape_)   getShape_   = [] { return 0.0f; };

    // Accessibility name/description (read by the default handler). The title is
    // also mirrored onto the Component so screen readers announce e.g. "Env 1".
    juce::Component::setTitle (title_);
    setDescription ("ADSR envelope / LFO waveform preview");

    startTimerHz (30);
}

EnvelopeDisplay::~EnvelopeDisplay()
{
    stopTimer();
}

//==============================================================================
float EnvelopeDisplay::fetch (const std::function<float()>& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, v);
}

void EnvelopeDisplay::setLiveStageProvider (std::function<parvati::LiveEnvStage()> p)
{
    liveStageProvider_ = std::move (p);
    // An un-set provider must immediately hide any shown marker (the state is
    // re-resolved on the next poll tick; a repaint now avoids one stale frame).
    if (! liveStageProvider_ && markerVisible_)
    {
        markerVisible_ = false;
        repaint();
    }
}

float EnvelopeDisplay::markerXForStage (const parvati::LiveEnvStage& st) const
{
    // DEAD (4) / inactive / out-of-range stages hide the marker. The stage
    // indices mirror ambika::dsp::EnvelopeStage (ATTACK=0..DEAD=4).
    if (! st.active || st.stage < 0 || st.stage > 3)
        return -1.0f;

    // Same segment weights as the drawn curve (ONE definition — see
    // adsrSegmentSpans), so the marker's x maps onto the curve the panel
    // painted from these very knob values.
    float wA, wD, wS, wR;
    adsrSegmentSpans (dispA_, dispD_, dispS_, dispR_, &wA, &wD, &wS, &wR);

    float before = 0.0f;   // cumulative weight BEFORE this stage
    float span   = 0.0f;   // this stage's weight
    switch (st.stage)
    {
        case 0:  span = wA; break;                        // ATTACK
        case 1:  before = wA;      span = wD; break;      // DECAY
        case 2:  before = wA + wD; span = wS; break;      // SUSTAIN
        default: before = wA + wD + wS; span = wR; break; // RELEASE
    }
    // SUSTAIN pins the marker at the plateau START: the engine reports no
    // time-of-hold for the sustain segment (its visual span is a fixed
    // minimum, not a proportional one), so the dot rests where the plateau
    // begins instead of crawling across a width that carries no meaning.
    const float progress = st.stage == 2 ? 0.0f : juce::jlimit (0.0f, 1.0f, st.progress);
    const float total = wA + wD + wS + wR;
    return juce::jlimit (0.0f, 1.0f, (before + progress * span) / total);
}

void EnvelopeDisplay::timerCallback()
{
    const float a  = fetch (getAttack_);
    const float d  = fetch (getDecay_);
    const float s  = fetch (getSustain_);
    const float r  = fetch (getRelease_);
    const float sh = fetch (getShape_);

    constexpr float eps    = 1.0f / 512.0f;   // ~0.002: ignore sub-knob jitter

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). Detect a change vs the previously-shown
    // value to drive the repaint gate (eps gate: sub-knob jitter does not cause
    // constant repaints). The LFO shape is discrete and snaps.
    const bool paramChanged = std::fabs (a - dispA_) > eps
                           || std::fabs (d - dispD_) > eps
                           || std::fabs (s - dispS_) > eps
                           || std::fabs (r - dispR_) > eps;
    dispA_ = a;
    dispD_ = d;
    dispS_ = s;
    dispR_ = r;

    const bool shapeChanged = std::fabs (sh - lastShape_) > eps;
    if (shapeChanged) lastShape_ = sh;   // discrete LFO shape snaps

    // ---- Live stage marker poll (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // ONE provider call per tick, only in ADSR mode. An absent provider, an
    // inactive/DEAD stage or LFO preview mode leaves the marker hidden at zero
    // overhead. The marker state is computed AFTER the disp* fields update so
    // its segment math uses the values the curve is drawn from this tick.
    bool  markerVisible = false;
    float markerX = 0.0f;
    if (previewMode_ == 0 && liveStageProvider_)
    {
        const float x = markerXForStage (liveStageProvider_());
        if (x >= 0.0f)
        {
            markerVisible = true;
            markerX = x;
        }
    }
    constexpr float kMarkerEps = 1.0f / 256.0f;   // ~1/4 knob-step of plot width
    const bool markerChanged = (markerVisible != markerVisible_)
        || (markerVisible && std::fabs (markerX - markerX_) > kMarkerEps);

    // Repaint only when the target moved since the last tick, on a shape
    // switch, or when the live marker crossed its own eps gate (eps gates:
    // no constant repaint when idle).
    if (shapeChanged || paramChanged || markerChanged)
    {
        markerVisible_ = markerVisible;
        markerX_ = markerX;
        ++generation_;   // TEST-ONLY: a real refresh is observable (see header)
        repaint();
    }
}

//==============================================================================
void EnvelopeDisplay::paint (juce::Graphics& g)
{
    // Read the active theme from the component's LookAndFeel (null-safe: a few
    // sensible fallback colours are used if there is no ParvatiLookAndFeel, so
    // the component also renders correctly in a plain host / test harness).
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const auto outline = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent  = t ? t->accentPrimary          : parvati::parvatiFallbackAccent;
    const auto textDim = t ? t->textSecondary         : juce::Colour (0xff9a9aa8);
    // The waveform trace + its gradient fill adopt a category hue (cyan ENV /
    // magenta LFO) when set; otherwise the live theme accent. The neutral clean
    // grid backdrop uses the theme divider token so the graph reads on any theme.
    const auto trace   = hasCategoryColour_ ? categoryColour_ : accent;
    const auto gridCol = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);

    const auto bounds = getLocalBounds().toFloat();

    // Panel fill + 1px square border with a faint clean grid backdrop.
    g.setColour (panelBg);
    g.fillRect (bounds);

    parvati::vectorTrace::drawGrid (g, bounds.reduced (0.5f), gridCol, 20.0f);

    g.setColour (outline);
    g.drawRect (bounds.reduced (0.5f), 1.0f);

    // Title (top-left).
    g.setColour (textDim);
    g.setFont (lnf ? lnf->appFont (13.0f, juce::Font::plain)
                   : juce::Font (juce::FontOptions (13.0f)));
    g.drawText (title_,
                bounds.reduced (9.0f, 4.0f).removeFromTop (16),
                juce::Justification::topLeft);

    // ---- Plot area ----
    auto plot = bounds.reduced (8.0f, 0.0f);
    plot.removeFromTop (22.0f);
    plot.removeFromBottom (8.0f);

    // ---- LFO waveform preview (previewMode_ == 1): bipolar, around the midline ----
    if (previewMode_ == 1)
    {
        const float sh = lastShape_ >= 0.0f ? lastShape_ : fetch (getShape_);
        const int shapeIdx = juce::jlimit (0, 3, juce::roundToInt (sh * 3.0f));   // 0..3: Tri/Sq/S&H/Ramp

        constexpr int kBlocksPerCycle = 8;
        constexpr int cycles = 2;
        const float periodFrac = 1.0f / static_cast<float> (cycles);

        // Stable S&H staircase (fixed seed so it does not flicker between frames).
        float shLevels[cycles * kBlocksPerCycle] = {};
        if (shapeIdx == 2)
        {
            uint32_t lcg = 0x9e3779b9u;
            for (int i = 0; i < cycles * kBlocksPerCycle; ++i)
            {
                lcg = lcg * 1664525u + 1013904223u;
                shLevels[i] = (lcg >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
            }
        }

        auto lfoLevel = [&] (float xf) -> float   // xf 0..1 -> -1..1
        {
            const float ph = xf / periodFrac;
            const float f  = ph - std::floor (ph);
            switch (shapeIdx)
            {
                case 0:  return (f < 0.5f) ? (4.0f * f - 1.0f) : (3.0f - 4.0f * f);   // triangle
                case 1:  return (f < 0.5f) ? 1.0f : -1.0f;                            // square
                case 2:                                                                  // sample & hold
                {
                    const int cycle = juce::jlimit (0, cycles - 1, static_cast<int> (ph));
                    const int block = juce::jlimit (0, kBlocksPerCycle - 1, static_cast<int> (f * kBlocksPerCycle));
                    return shLevels[cycle * kBlocksPerCycle + block];
                }
                default: return 2.0f * f - 1.0f;                                       // ramp / saw
            }
        };

        // Smooth vector trace + translucent gradient fill (bipolar, around the
        // midline). The S&H shape keeps its crisp staircase (held segments);
        // square/PWM/ramp/triangle stay smooth.
        const bool isSampleAndHold = (shapeIdx == 2);
        const int sampleCount = isSampleAndHold ? (cycles * kBlocksPerCycle)
                                                : juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
        parvati::vectorTrace::render (g, plot, sampleCount, lfoLevel,
                                      trace, parvati::vectorTrace::Mode::bipolar,
                                      isSampleAndHold, 1.5f, 0.12f);

        // Midline reference (1px).
        g.setColour (accent.withAlpha (0.25f));
        g.drawHorizontalLine (juce::roundToInt (plot.getCentre().y),
                              plot.getX(), plot.getRight());

        g.setColour (textDim);
        g.setFont (lnf ? lnf->appFont (11.0f, juce::Font::plain)
                       : juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("(LFO)",
                    bounds.reduced (9.0f, 4.0f).removeFromTop (16).removeFromRight (50),
                    juce::Justification::topRight);
        return;
    }

    // ---- ADSR envelope (previewMode_ == 0): unipolar, filled from the baseline ----
    // Attack/decay/sustain/release are the SMOOTHED displayed values, so turning
    // their knobs animates the curve instead of snapping.
    const float a = dispA_ >= 0.0f ? dispA_ : fetch (getAttack_);
    const float d = dispD_ >= 0.0f ? dispD_ : fetch (getDecay_);
    const float s = dispS_ >= 0.0f ? dispS_ : fetch (getSustain_);
    const float r = dispR_ >= 0.0f ? dispR_ : fetch (getRelease_);

    // Envelope level (0..1) at a normalized x position (0..1), using the same
    // exponential attack/decay/release eases as the smooth curve did. EXPOSED
    // as a pure static (adsrLevelForTest) so the shape is testable without a
    // Graphics context.
    auto envLevel = [&] (float xf) -> float
    {
        return adsrCurveLevel (a, d, s, r, xf);
    };

    // Smooth vector trace + translucent gradient fill (unipolar, filled from the
    // baseline).
    const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, envLevel,
                                  trace, parvati::vectorTrace::Mode::unipolar,
                                  false, 1.5f, 0.12f);

    // ---- Live stage marker (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // A dot riding the DRAWN curve (same segment geometry, same level function)
    // plus a 1px hairline through the plot: quiet position feedback while the
    // key is held, in the trace colour so it inherits the category hue. Flat —
    // no glow, no bevel; the 1px panelBg rim just separates the dot from the
    // curve where they overlap.
    if (markerVisible_)
    {
        const float px = plot.getX() + markerX_ * plot.getWidth();
        const float py = parvati::vectorTrace::levelToY (plot,
                               parvati::vectorTrace::Mode::unipolar,
                               adsrCurveLevel (a, d, s, r, markerX_));

        g.setColour (trace.withAlpha (0.28f));
        g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());

        constexpr float kDotR = 1.75f;   // ~3.5px diameter
        g.setColour (panelBg);
        g.fillEllipse (juce::Rectangle<float> ((kDotR + 1.0f) * 2.0f, (kDotR + 1.0f) * 2.0f)
                           .withCentre ({ px, py }));
        g.setColour (trace);
        g.fillEllipse (juce::Rectangle<float> (kDotR * 2.0f, kDotR * 2.0f)
                           .withCentre ({ px, py }));
    }
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> EnvelopeDisplay::createAccessibilityHandler()
{
    // Role `group`: a labelled preview readout. The name (the envelope title,
    // e.g. "Env 1") and description ("ADSR envelope preview") are announced by
    // the default handler from the Component's title/description.
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

void EnvelopeDisplay::visibilityChanged()
{
    updatePollTimer();
}

void EnvelopeDisplay::parentHierarchyChanged()
{
    updatePollTimer();
}

void EnvelopeDisplay::updatePollTimer()
{
    // F-ios-perf-3 gate (see OscPreviewDisplay.h for the full rationale):
    // BOTH hooks are required — visibilityChanged fires while still unparented
    // (addAndMakeVisible's setVisible precedes parenting) and never again once
    // ancestors change; parentHierarchyChanged fires on every hierarchy change
    // including the editor gaining its peer, which is the reliable
    // "became showing" signal. Fixed the frozen-preview regression.
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}
