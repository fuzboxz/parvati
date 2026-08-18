// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// TuningEditor — the per-part custom tuning popover (Patch page "Tune" column,
// "Custom…" item). Twelve note-class rows (C..B) each with a drag/step control
// in 1-unit steps (±127 units of 1/128 semitone ≈ ±99 ¢), a quantized cents
// readout, plus [Import .scl/.kbm…] (parvati::importScala — see
// Source/ScalaImport.h for the hardware-limitation contract it enforces),
// [Clear] and [Done].
//
// Applying is LIVE: every row edit writes the whole table through
// SynthEngine::setPartTuningCustom (message-thread safe by design — the
// engine stages it to the audio thread via tuningDirty_), so closing without
// [Done] never loses an edit. Rows prefill from the part's currently RESOLVED
// table (preset or custom), so "Custom…" opened on top of a raga preset
// starts from that preset's offsets — the natural "tweak this scale" flow.
//
// Muted classes (the firmware 32767 sentinel — arrives from Scala imports of
// keymaps with 'x' keys) display "—" and are kept verbatim while untouched;
// dragging such a row replaces the mute with a numeric offset.
//
// Hosting: the app's popover precedent is MulExportDialog — a plain Component
// launched modally in a juce::DialogWindow with the parent's LookAndFeel (so
// it follows the active Parvati theme). Headless tests instantiate the
// component directly (no dialog, no peer).

#ifndef PARVATI_TUNING_EDITOR_H_
#define PARVATI_TUNING_EDITOR_H_

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

#include "SynthEngine.h"

class ParvatiLookAndFeel;

class TuningEditor : public juce::Component
{
public:
    // Fired after every change that landed in the engine (row edit, Clear,
    // Scala import) so the owner can refresh its Tune combo display.
    using ChangeCallback = std::function<void()>;

    TuningEditor (SynthEngine& engine, int partIndex, ChangeCallback onChanged);
    ~TuningEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // Show modally (desktop + AUv3 panes). Mirrors MulExportDialog::launch:
    // hosts this component in a DialogWindow inheriting @p parent's
    // LookAndFeel; height caps to the usable screen area (the row viewport
    // scrolls in short panes — the MulExportDialog preview pattern). Null
    // parent (tests) keeps the default look.
    static void launch (juce::Component* parent, SynthEngine& engine, int partIndex,
                        ChangeCallback onChanged);

    // ---- test hooks (drive the exact UI code paths, headlessly) ----
    // Current stored units for a note class (0..11): -127..127, or
    // 32767 (parvati::kTuningSilence) when the row is muted.
    int rowUnits (int noteClass) const;
    // A user-style row edit: sets the control and runs the apply path.
    void setRowUnitsForTest (int noteClass, int units);
    // The row's displayed readout text ("+15.63ct" / "0.00ct" / "—").
    juce::String rowReadout (int noteClass) const;
    // Runs the exact [Import .scl/.kbm…] conversion path with file CONTENTS
    // (no FileChooser): fills + applies on success, shows the inline error on
    // failure. Returns the parser's ok flag.
    bool importScalaForTest (const juce::String& sclText, const juce::String& kbmText);
    // The currently shown warning/error text ("" when none).
    juce::String messageText() const;
    // A user-style [Clear] (zeros + applies).
    void clearForTest();

private:
    // One note-class row: class label + horizontal drag control + readout.
    // Defined in the .cpp.
    class Row;
    std::array<std::unique_ptr<Row>, 12> rows_;

    // ---- launch-time lifetime guards (see launch) ----
    // The dialog is its OWN desktop window (launchAsync), so it can outlive
    // the editor that opened it (host closes the plugin window while the
    // popover is up). TWO hazards that previously dangled in that window:
    //   (1) the raw parent LookAndFeel pointer (freed with the editor) —
    //       fixed by owning a ParvatiLookAndFeel COPIED from the parent's
    //       active theme (the builtin theme structs are function-local
    //       statics in ParvatiTheme.cpp, so the copy's theme_ pointer stays
    //       valid even after the editor dies);
    //   (2) the SynthEngine& / ChangeCallback (owned by the processor / the
    //       Patch page) — every engine-touching interaction is gated on the
    //       LAUNCH PARENT still existing; once it is gone the dialog closes
    //       itself instead of touching freed state (a paint-only dialog is
    //       harmless: nothing reads the engine without an interaction).
    std::unique_ptr<ParvatiLookAndFeel> ownedLnf_;
    bool watchParent_ = false;
    juce::Component::SafePointer<juce::Component> launchParent_;
    // True when launched from a parent that has since been deleted: the
    // popover must close (see applyTable / launch).
    bool parentGone() const noexcept { return watchParent_ && launchParent_ == nullptr; }

    // Collect the rows into the 12-entry table (muted rows -> sentinel) and
    // push it to the engine + fire the change callback.
    void applyTable();
    // Shared import path (file callback and the test hook): convert, fill,
    // apply, surface warnings / the inline error (table untouched on error).
    void applyScalaText (const juce::String& sclText, const juce::String& kbmText);
    void showMessage (const juce::String& text, bool isError);
    void clearMessage();
    // [Clear] + [Done] handlers.
    void onClearClicked();
    void onDoneClicked();

    SynthEngine& engine_;
    const int partIndex_;
    ChangeCallback onChanged_;

    juce::Label heading_;
    juce::Label hintLabel_;
    juce::Viewport rowsViewport_;        // rows scroll in short AUv3 panes
    juce::Component rowsHost_;           // the scrolled row container
    juce::Label messageLabel_;           // inline warnings (multi-line)
    juce::Label errorLabel_;             // inline import error (single line)
    juce::TextButton importButton_;
    juce::TextButton clearButton_;
    juce::TextButton doneButton_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuningEditor)
};

#endif  // PARVATI_TUNING_EDITOR_H_
