// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MulExportDialog — the .MUL hardware-export fallback dialog. Shown when a
// multi uses the per-part voice-slot extension beyond what the 6 hardware
// voicecards can express: the user picks a plain-language strategy for fitting
// the voices onto the cards ("Share the voicecards fairly", "Use two or more
// chained Ambikas", ...), with a live description line, a one-line outcome
// summary, and a per-part preview. The dialog is pure UI: the mapping itself
// lives in Source/MulExport.h/.cpp (unit-tested there), and the actual file
// writing in ParvatiAudioProcessor::saveMultiFile.

#ifndef PARVATI_MUL_EXPORT_DIALOG_H_
#define PARVATI_MUL_EXPORT_DIALOG_H_

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "MulExport.h"

class MulExportDialog : public juce::Component
{
public:
    // Called with the chosen strategy value (int form, for saveMultiFile) when
    // the user confirms; with -1 on cancel.
    using DoneCallback = std::function<void (int)>;

    // @p partNames: display names per part ("Kick", "Lead", ...) used in the
    // preview lines; entries may be empty ("Part N" fallback).
    // @p customTuningParts: parts whose tuning is a Parvati CUSTOM table
    // (engine resolvedTuningMode == 33). The .MUL format cannot carry custom
    // tables (only the raga preset byte), so the preview shows a lossy-export
    // warning naming those parts. All-false = no warning line.
    MulExportDialog (const parvati::mul_export::Setup& setup,
                     const std::vector<juce::String>& partNames,
                     DoneCallback onDone,
                     const std::array<bool, parvati::mul_export::kParts>& customTuningParts = {});

    void resized() override;
    void paint (juce::Graphics&) override;
    ~MulExportDialog() override;

    // Show modally (desktop: a DialogWindow; headless tests instantiate the
    // component directly). Callback fires exactly once.
    static void launch (juce::Component* parent,
                        const parvati::mul_export::Setup& setup,
                        const std::vector<juce::String>& partNames,
                        DoneCallback onDone,
                        const std::array<bool, parvati::mul_export::kParts>& customTuningParts = {});

private:
    void refreshPreview();

public:
    // Test hook: re-render the preview for the current selection (headless
    // smoke of the strategy->preview wiring without a peer).
    void refreshPreviewPublic() { refreshPreview(); }

    // Test hook: the current preview text (warning-line assertions).
    juce::String previewTextForTest() const { return previewLabel_.getText(); }

private:

    parvati::mul_export::Setup setup_;
    parvati::mul_export::PreviewContext ctx_;
    std::array<bool, parvati::mul_export::kParts> customTuningParts_ {};
    DoneCallback onDone_;
    bool fired_ = false;

    juce::Label heading_;
    juce::Label strategyCaption_;
    juce::ComboBox strategyCombo_;
    juce::Label descriptionLabel_;   // plain-language "how it works" of the selection
    juce::Label summaryLabel_;       // one-line honest outcome ("6 of 24 voices kept")
    juce::Viewport previewViewport_;   // scrolls long chain summaries (touch-drag)
    juce::Label previewLabel_;
    int previewLineCount_ = 1;
    juce::TextButton saveButton_, cancelButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MulExportDialog)
};

#endif  // PARVATI_MUL_EXPORT_DIALOG_H_
