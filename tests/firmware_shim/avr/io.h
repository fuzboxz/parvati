// Desktop shim: avr/io.h — the firmware's MCU register/constant header.
//
// Only what the parity oracle's firmware closure actually references is
// defined (the voicecard audio oracle needs E2END for one static pointer
// that is never dereferenced outside firmware-update flows we do not run).
// Registers themselves are NOT defined: any TU that actually toggles AVR
// hardware registers must be shimmed at the avrlib/<header> level instead
// (see tests/firmware_shim/avrlib/), so accidental hardware access on the
// host is a compile error, not a silent no-op.
#pragma once

// ATmega644P family: end of EEPROM address space (voicecard.h points a
// firmware-update flag at it; never read in the oracle scenarios).
#define E2END 0x0FFF

// Bit-value macro (avr/io.h on device; used by the real avrlib/avrlib.h
// enum initializers compiled into the audio oracle).
#define _BV(bit) (1 << (bit))
