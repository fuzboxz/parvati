// Global instance for ambika::dsp::Random.
//
// Original: ambika_reference/avrlib/random.h had a static singleton; Ambika
// shares one noise source across the synth. A single global instance is the
// faithful, simplest equivalent.

#include "dsp/random.h"

namespace ambika::dsp {

namespace {
Random globalRandomInstance;
}  // namespace

Random& random() {
    return globalRandomInstance;
}

}  // namespace ambika::dsp
