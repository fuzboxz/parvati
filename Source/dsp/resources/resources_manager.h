// Ported from ambika_reference/avrlib/resources_manager.h
//
// All AVR PROGMEM semantics collapse to plain RAM array access:
//   Lookup<uint16_t,uint8_t>(table, i)  ->  table[i]   (uint16 element i)
//   Lookup<uint8_t, uint8_t>(table, i)  ->  table[i]   (uint8  byte   i)
//   Load(p, i, dest)                    ->  memcpy(dest, p+i, sizeof(*p))
// These match the firmware's pgm_read_word/pgm_read_byte results exactly,
// because the data arrays in resources_data.cpp are byte-identical to the
// firmware PROGMEM tables.

#ifndef AMBIKA_DSP_RESOURCES_RESOURCES_MANAGER_H_
#define AMBIKA_DSP_RESOURCES_RESOURCES_MANAGER_H_

#include <cstdint>
#include <cstring>

#include "dsp/resources/resources.h"

namespace ambika::dsp {

// Drop-in replacement for the firmware `ResourcesManager` typedef. Static
// helpers only; mirrors avrlib::ResourcesManager's API surface so ported
// call sites (ResourcesManager::Lookup<...>, ResourcesManager::Load) compile
// with no semantic change.
struct ResourcesManager {
  // --- Byte-table lookup: pgm_read_byte(p + i) == p[i] ----------------------
  template <typename ResultType, typename IndexType>
  static inline ResultType Lookup(const uint8_t* p, IndexType i) {
    return static_cast<ResultType>(p[static_cast<std::size_t>(i)]);
  }

  // --- Word-table lookup: pgm_read_word(p + i) == p[i] ----------------------
  // (pointer arithmetic on uint16_t* advances i*2 bytes -> element i)
  template <typename ResultType, typename IndexType>
  static inline ResultType Lookup(const uint16_t* p, IndexType i) {
    return static_cast<ResultType>(p[static_cast<std::size_t>(i)]);
  }

  // --- Resource-ID lookup (parity; rarely used by the voice DSP) ------------
  // Firmware: lookup_table_table()[resource], then read word i.
  // BOUNDS-GUARDED (memory-safety migration): `resource` is a runtime int; an
  // id outside 0..kNumLookupTables-1 now reads 0 instead of walking past the
  // indirection table. In-range ids are byte-for-byte firmware parity.
  template <typename ResultType, typename IndexType>
  static inline ResultType Lookup(int resource, IndexType i) {
    if (resource < 0 || static_cast<std::size_t>(resource) >= kNumLookupTables)
      return static_cast<ResultType>(0);
    const uint16_t* table = lookup_table_table[static_cast<std::size_t>(resource)];
    return static_cast<ResultType>(table[static_cast<std::size_t>(i)]);
  }

  // --- Load one element: Load(p, i, dest) copies sizeof(T) from p+i ---------
  // Used by oscillator.cc to fetch a RenderFn from fn_table_[index], and by
  // voice.cc to copy the init_patch struct (Load(&init_patch, 0, &patch_)).
  template <typename T, typename U>
  static inline void Load(const T* p, uint8_t i, U* destination) {
    static_assert(sizeof(T) == sizeof(U), "ResourcesManager::Load size mismatch");
    std::memcpy(destination, p + i, sizeof(T));
  }

  // --- Load a raw byte range: Load(p, dest, size) ---------------------------
  // Firmware parity (used by the controller for patch I/O; kept for API
  // completeness). Copies `size` bytes from p to destination.
  template <typename T, typename U>
  static inline void Load(const T* p, U* destination, std::size_t size) {
    std::memcpy(destination, p, size);
  }
};

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_RESOURCES_RESOURCES_MANAGER_H_
