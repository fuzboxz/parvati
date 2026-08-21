// Keyboard fast-drag crackle repro (2026-08-21): full engine, NO FX, the
// exact event stream a fast mouse sweep produces — note-off(old)+note-on(new)
// every ~13 ms (75 keys/s) + a CHANNEL PRESSURE message on every drag move
// (~120 Hz, the y-position=pressure feature). Impulse census vs a plain held
// note. Exit 1 = crackle reproduced.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/KeyboardView.h"
#include <juce_gui_basics/juce_gui_basics.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace
{
int g_failures = 0;

int census (const std::vector<float>& capL, int from, int to, float* worstOut)
{
    std::vector<float> d ((size_t) to, 0.f);
    for (int i = from + 1; i < to; ++i)
        d[(size_t) i] = std::fabs (capL[(size_t) i] - capL[(size_t) (i - 1)]);
    int count = 0; float worst = 0;
    for (int i = from + 65; i < to; ++i)
    {
        float w[64];
        for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
        std::sort (w, w + 64);
        if (d[(size_t) i] > 8.f * w[60] && d[(size_t) i] > 0.004f) { ++count; worst = std::fmax (worst, d[(size_t) i]); }
    }
    if (worstOut) *worstOut = worst;
    return count;
}
} // namespace

int main()
{
    ::setenv ("PARVATI_HEADLESS", "1", 1);
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 44100.0;

    // ---- REAL-UI DRAG: a desktop-attached editor, synthetic mouse events
    // sweeping the actual KeyboardView (the true UI event path). ----
    {
        auto proc = std::make_unique<ParvatiAudioProcessor>();
        proc->prepareToPlay (sr, 256);
        auto* ed = proc->createEditor();
        auto win = std::make_unique<juce::DocumentWindow> ("DragRepro",
            juce::Colours::black, juce::DocumentWindow::allButtons);
        win->setUsingNativeTitleBar (true);
        win->setContentNonOwned (ed, false);
        win->centreWithSize (1280, 700);
        win->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
        win->setVisible (true);
        #ifdef __APPLE__
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.200, false);
        #endif

        // find the KeyboardView
        KeyboardView* kv = nullptr;
        {
            std::function<KeyboardView* (juce::Component*)> find =
                [&] (juce::Component* c) -> KeyboardView*
            {
                if (auto* k = dynamic_cast<KeyboardView*> (c)) return k;
                for (auto* ch : c->getChildren())
                    if (auto* r = find (ch)) return r;
                return nullptr;
            };
            kv = find (ed);
        }
        std::printf ("UI drag: KeyboardView %s\n", kv != nullptr ? "found" : "NOT FOUND");
        if (kv != nullptr)
        {
            const int totalUi = (int) (2.5 * sr);
            std::vector<float> uiCap ((size_t) totalUi, 0.f);
            const auto bounds = kv->getLocalBounds();
            const float y = (float) bounds.getCentreY();
            // events must land on the CHILD at the position (the inner key
            // component covers the area and owns the handlers).
            auto targetAt = [kv] (juce::Point<float> p) -> juce::Component*
            { return kv->getComponentAt (p.toInt()); };
            const auto now = juce::Time::getCurrentTime();
            const auto src = juce::Desktop::getInstance().getMainMouseSource();
            juce::Point<float> startPos ((float) bounds.getX() + 4.0f, y);
            // press
            {
                auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
                auto* tgt = targetAt (startPos);
                if (tgt != nullptr) tgt->mouseDown (juce::MouseEvent (src, startPos, mods,
                    juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                    juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                    juce::MouseInputSource::defaultTiltY, tgt, tgt, now, startPos, now, 1, false));
            }
            for (int w = 0; w < totalUi; )
            {
                juce::AudioBuffer<float> b (2, 256);
                b.clear();
                // a fast sweep: full width in 0.8 s, 8 ms per move event
                const double tIn = (w - 0.5 * sr) / (0.8 * sr);
                if (w > (int) (0.5 * sr) && w < (int) (1.3 * sr))
                {
                    const float x = (float) bounds.getX() + 4.0f
                        + (float) tIn * ((float) bounds.getWidth() - 8.0f);
                    const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
                    const juce::Point<float> pos (x, y);
                    auto* tgt2 = targetAt (pos);
                    if (tgt2 != nullptr) tgt2->mouseDrag (juce::MouseEvent (src, pos, mods,
                        juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                        juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                        juce::MouseInputSource::defaultTiltY, tgt2, tgt2, now, startPos, now, 1, false));
                }
                { juce::MidiBuffer emptyMidi; proc->processBlock (b, emptyMidi); }
                const int n = std::min (256, totalUi - w);
                for (int i = 0; i < n; ++i) uiCap[(size_t) (w + i)] = b.getSample (0, i);
                w += n;
                #ifdef __APPLE__
                if (w % 512 == 0) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001, false);
                #endif
            }
            float wDrag = 0;
            const int c = census (uiCap, (int) (0.55 * sr), (int) (1.3 * sr), &wDrag);
            std::printf ("UI drag window: impulses=%d worst=%.4f\n", c, wDrag);
        }
        win->setVisible (false);
        delete ed;
    }

    const int total = (int) (3.0 * sr);
    std::vector<float> capL ((size_t) total, 0.f);

    {
        auto proc = std::make_unique<ParvatiAudioProcessor>();
        proc->prepareToPlay (sr, 256);
        // NO FX; default patch; render with the drag event stream.
        const int dragStart = (int) (1.0 * sr), dragEnd = (int) (2.0 * sr);
        int curNote = -1;
        int lastPressureBlock = -1;
        for (int w = 0; w < total; )
        {
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            juce::MidiBuffer m;
            if (w < dragStart && curNote < 0)
            {
                curNote = 60;
                proc->addMidiEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
            }
            if (w >= dragStart && w < dragEnd)
            {
                // ~75 keys/s upward sweep: retarget every ~13 ms (~5.7 blocks)
                const double tIn = (w - dragStart) / sr;
                const int note = 60 + (int) (tIn * 75.0);
                if (note != curNote)
                {
                    proc->addMidiEvent (juce::MidiMessage::noteOff (1, (uint8_t) curNote, (uint8_t) 0));
                    proc->addMidiEvent (juce::MidiMessage::noteOn (1, (uint8_t) note, (uint8_t) 100));
                    curNote = note;
                }
                // pressure flood on EVERY block (~172 Hz here; real drags ~120 Hz)
                if (lastPressureBlock != w)
                {
                    proc->addMidiEvent (juce::MidiMessage::channelPressureChange (1, (uint8_t) (60 + (w / 7 % 60))));
                    lastPressureBlock = w;
                }
            }
            if (w == dragEnd)
                proc->addMidiEvent (juce::MidiMessage::noteOff (1, (uint8_t) curNote, (uint8_t) 0));
            proc->processBlock (b, m);
            const int n = std::min (256, total - w);
            for (int i = 0; i < n; ++i) capL[(size_t) (w + i)] = b.getSample (0, i);
            w += n;
        }
    }

    float wPre = 0, wDrag = 0, wPost = 0;
    const int pre  = census (capL, (int) (0.3 * sr), (int) (1.0 * sr), &wPre);
    const int drag = census (capL, (int) (1.0 * sr), (int) (2.0 * sr), &wDrag);
    const int post = census (capL, (int) (2.1 * sr), (int) (3.0 * sr), &wPost);
    std::printf ("kbd drag: pre=%d/%.4f DRAG=%d/%.4f post=%d/%.4f\n", pre, wPre, drag, wDrag, post, wPost);

    char msg[128];
    std::snprintf (msg, sizeof (msg), "drag window worst %.4f <= 0.05", wDrag);
    const bool ok = wDrag <= 0.05f;
    if (! ok) ++g_failures;
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", msg);
    std::printf ("%s (%d failure%s)\n", g_failures ? "KBD DRAG TEST: FAILURES" : "KBD DRAG TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
