// Desktop shim: avrlib/gpio.h — the firmware's pin/port template library.
//
// The voicecard audio oracle's include chain (voicecard/audio_out.h) pulls
// this header in, but the oracle exercises Voice::ProcessBlock only: the
// rendered bytes leave through avrlib::RingBuffer (audio_buffer), never
// through GPIO. The real header cannot compile off-device (it instantiates
// AVR port registers from <avr/io.h>), and stubbing those registers would
// silently fake hardware access — so this shadow provides the empty
// surface instead: anything that actually names a Port/Pin type from
// gpio.h fails to compile on the host, by design.
#pragma once
