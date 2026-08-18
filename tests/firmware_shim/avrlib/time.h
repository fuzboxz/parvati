// Desktop shim: avrlib/time.h — the firmware's AVR timer helpers. Only the
// ConstantDelay macro is reached by the parity oracle's translation units
// (controller/part.cc TouchPatch busy-waits ~5 ms on hardware between SPI
// block-write commands); on the host it is a no-op. The clock helpers below
// are inert (nothing in the compared paths reads them).
#pragma once

#include "avrlib/base.h"

#define ConstantDelay(x) ((void) 0)

namespace avrlib
{
inline uint32_t milliseconds() { return 0; }
inline uint32_t Delay (uint32_t) { return 0; }
inline void InitClock() {}
}  // namespace avrlib
