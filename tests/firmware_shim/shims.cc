// Desktop shim definitions for the firmware parity oracle
// (tests/firmware_parity_test). The recording shims in
// tests/firmware_shim/ declare `extern` singletons in the exact shape the
// vendored firmware expects (voicecard_tx / midi_dispatcher /
// system_settings / parameter_manager); this TU defines them plus the
// SystemSettings static data (reception fully enabled = factory default).
//
// This TU deliberately includes ONLY shim + firmware headers — never any
// Parvati header — so the firmware namespace (ambika:: / avrlib::) and
// Parvati's (ambika::dsp::) never meet in one translation unit.

#include "controller/midi_dispatcher.h"
#include "controller/parameter.h"
#include "controller/system_settings.h"
#include "controller/voicecard_tx.h"

namespace ambika
{

/* extern (firmware-visible singletons, recording shims) */
VoicecardProtocolTx voicecard_tx;
MidiDispatcher midi_dispatcher;
SystemSettings system_settings;
ParameterManager parameter_manager;

/* static */
SystemSettingsData SystemSettings::data_ = { 0, 0, 0, 0, 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 };

}  // namespace ambika
