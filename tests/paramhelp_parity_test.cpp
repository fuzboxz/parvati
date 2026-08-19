// ParamHelp descriptor<->help parity verification (ParamHelp.cpp).
//
// Pins the documented contract (ParamHelp.h): getParamHelp() returns
// non-empty text for EVERY paramID in getPatchParamDescriptors() — the
// curated map (now including the 78 FX entries) plus the 64 generated
// step-sequencer entries — and an empty string ONLY for genuinely unknown
// IDs. A new descriptor without help (the historical state of the whole FX
// family) fails here instead of shipping silent tooltips.
//
// Built by default. Run with: ./build/parvati_paramhelp_parity_test

#include <cstdio>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "ui/ParamHelp.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}
}  // namespace

int main()
{
    const auto& descs = getPatchParamDescriptors();
    std::printf ("[1] Every descriptor paramID has help text\n");
    {
        std::vector<std::string> missing;
        for (const auto& d : descs)
            if (! hasParamHelp (juce::String (d.paramID)))
                missing.push_back (d.paramID);
        std::printf ("     descriptors: %zu, missing help: %zu\n",
                     descs.size(), missing.size());
        check (missing.empty(), "no descriptor id is help-less (incl. options + FX)");
        if (! missing.empty())
            for (const auto& id : missing)
                std::printf ("       missing: %s\n", id.c_str());

        // The FX family is exactly the 78 loop-generated entries: every one of
        // them must answer with non-empty text (the historical gap was 0/78).
        int fxWithHelp = 0, fxDenominator = 0;
        for (const auto& d : descs)
        {
            const juce::String jid (d.paramID);
            if (! d.isFx) continue;
            ++fxDenominator;
            if (hasParamHelp (jid)) ++fxWithHelp;
        }
        std::printf ("     FX ids with help: %d/%d\n", fxWithHelp, fxDenominator);
        check (fxWithHelp == fxDenominator && fxDenominator >= 78,
               "all 78 FX ids (slots + chain/master + fxmod) have help");
    }

    std::printf ("\n[2] Generated step-sequencer help\n");
    check (hasParamHelp ("seq1_step0"), "seq1_step0 (generated) has help");
    check (hasParamHelp ("seq2_step31"), "seq2_step31 (generated) has help");
    check (hasParamHelp ("seqnote_step15"), "seqnote_step15 (generated) has help");
    check (hasParamHelp ("seqnote_vel15"), "seqnote_vel15 (generated) has help");
    {
        const auto velHelp = getParamHelp ("seqnote_vel15");
        check (velHelp.contains ("velocity"), "seqnote_vel15 help mentions velocity");
        const auto stepHelp = getParamHelp ("seqnote_step15");
        check (stepHelp.contains ("MIDI note"), "seqnote_step15 help mentions the note");
        check (getParamHelp ("seq1_step7").contains ("8"),
               "seq1_step7 help shows the 1-based step number (8)");
    }

    std::printf ("\n[3] Unknown / malformed ids return empty\n");
    check (! hasParamHelp ("bogus"), "unknown id -> no help");
    check (! hasParamHelp (""), "empty id -> no help");
    // A step prefix with a NON-NUMERIC suffix must not match the generated
    // family (the digit-only guard in getParamHelp).
    check (! hasParamHelp ("seq1_stepX"), "seq1_stepX (non-numeric suffix) -> no help");
    check (! hasParamHelp ("seqnote_vel"), "seqnote_vel (bare prefix) -> no help");

    std::printf ("\n[4] FX help content spot-checks (one per family)\n");
    check (getParamHelp ("fx2_param3").contains ("algorithm"),
           "fx2_param3 help says the meaning depends on the algorithm");
    check (getParamHelp ("fx1_enabled").contains ("bypass"),
           "fx1_enabled help mentions bypass");
    check (getParamHelp ("fx_topo").contains ("FX1 -> FX2 -> FX3"),
           "fx_topo help names the topologies");
    check (getParamHelp ("fx_eq_mid").contains ("64 = 0 dB"),
           "fx_eq_mid help pins the unity byte");
    check (getParamHelp ("fxmod16_amount").contains ("-63..+63"),
           "fxmod16_amount help pins the bipolar range");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
