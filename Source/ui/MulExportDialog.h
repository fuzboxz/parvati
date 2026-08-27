// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// MulExportDialog — the .MUL hardware-export fallback dialog. Shown when a
// multi uses the per-part voice-slot extension beyond what the 6 hardware
// voicecards can express: the user picks a plain-language strategy to fit
// the voices onto the cards ("Share the voicecards fairly", "Use two or more
// chained Ambikas", ...), with a live description line, a one-line outcome
// summary, and a per-part preview. The dialog is pure UI: the mapping itself
// lives in Source/MulExport.h/.cpp (unit-tested there), and the actual file
// writing in HellcatAudioProcessor::saveMultiFile.

#ifndef HELLCAT_MUL_EXPORT_DIALOG_H_
#define HELLCAT_MUL_EXPORT_DIALOG_H_

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "MulExport.h"
#include "HellcatLookAndFeel.h"   // F-ui-2: the dialog's OWNED theme copy

class MulExportDialog : public juce::Component
{
public:
    // Called with the chosen strategy value (int form, for saveMultiFile) when
    // the user confirms; with -1 on cancel.
    using DoneCallback = std::function<void (int)>;

    // @p partNames: display names per part ("Kick", "Lead", ...) used in the
    // preview lines; entries may be empty ("Part N" fallback).
    MulExportDialog (const hellcat::mul_export::Setup& setup,
                     const std::vector<juce::String>& partNames,
                     DoneCallback onDone);

    void resized() override;
    void paint (juce::Graphics&) override;
    ~MulExportDialog() override;

    // Show modally (desktop: a DialogWindow; headless tests instantiate the
    // component directly). Callback fires exactly once.
    static void launch (juce::Component* parent,
                        const hellcat::mul_export::Setup& setup,
                        const std::vector<juce::String>& partNames,
                        DoneCallback onDone);

private:
    void refreshPreview();

public:
    // Test hook: re-render the preview for the current selection (headless
    // smoke of the strategy->preview wiring without a peer).
    void refreshPreviewPublic() { refreshPreview(); }

    // Test hook: the current preview text (warning-line assertions).
    juce::String previewTextForTest() const { return previewLabel_.getText(); }

private:

    hellcat::mul_export::Setup setup_;
    hellcat::mul_export::PreviewContext ctx_;
    DoneCallback onDone_;

    // F-ui-2 (bug hunt 2026-08-18): the DialogWindow is its OWN desktop window
    // and can OUTLIVE the launching editor (host closes the plugin window
    // while the export dialog is open) — borrowing the editor's LookAndFeel
    // painted through freed memory. The dialog therefore OWNS a
    // HellcatLookAndFeel copied from the parent's active theme (the owned-L&F
    // pattern; the builtin theme structs are function-local statics in
    // HellcatTheme.cpp, so the copy stays valid after the editor dies).
    std::unique_ptr<HellcatLookAndFeel> ownedLnf_;   // null => default look (tests/null parent)
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

#endif  // HELLCAT_MUL_EXPORT_DIALOG_H_
