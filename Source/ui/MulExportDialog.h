// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MulExportDialog — the .MUL hardware-export fallback dialog. Shown when a
// multi uses the per-part voice-slot extension beyond what the 6 hardware
// voicecards can express: the user picks a STRATEGY for mapping the requested
// voices onto the cards (Proportional / Priority / Even Split / Mono Fold /
// Chain Split / As-Is), with a live preview (per part: "requested -> got
// voices", mode rewrites marked). The dialog is pure UI: the mapping itself
// lives in Source/MulExport.h/.cpp (unit-tested there), and the actual file
// writing in ParvatiAudioProcessor::saveMultiFile.

#ifndef PARVATI_MUL_EXPORT_DIALOG_H_
#define PARVATI_MUL_EXPORT_DIALOG_H_

#include <juce_gui_basics/juce_gui_basics.h>

#include "MulExport.h"

class MulExportDialog : public juce::Component
{
public:
    // Called with the chosen strategy value (int form, for saveMultiFile) when
    // the user confirms; with -1 on cancel.
    using DoneCallback = std::function<void (int)>;

    MulExportDialog (const parvati::mul_export::Setup& setup, DoneCallback onDone);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Show modally over @p parent (desktop: a DialogWindow; headless tests
    // instantiate the component directly). Callback fires exactly once.
    static void launch (juce::Component* parent,
                        const parvati::mul_export::Setup& setup,
                        DoneCallback onDone);

private:
    void refreshPreview();

    parvati::mul_export::Setup setup_;
    DoneCallback onDone_;
    bool fired_ = false;

    juce::Label heading_;
    juce::Label strategyCaption_;
    juce::ComboBox strategyCombo_;
    juce::Label previewLabel_;
    juce::TextButton saveButton_ { "Save" },
                     cancelButton_ { "Cancel" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MulExportDialog)
};

#endif  // PARVATI_MUL_EXPORT_DIALOG_H_
