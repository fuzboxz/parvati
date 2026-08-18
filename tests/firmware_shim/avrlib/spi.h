// Desktop shim: avrlib/spi.h — hardware SPI registers exist only on AVR.
// The real header is pulled solely by controller/voicecard_tx.h, which the
// parity oracle replaces with a recording shim (see
// tests/firmware_shim/controller/voicecard_tx.h); this stub exists only so
// any accidental include resolves to nothing on the host build.
#pragma once
