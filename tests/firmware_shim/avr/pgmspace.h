// Desktop shim: AVR pgmspace compatibility for the vendored Ambika firmware
// (ambika_reference/) when compiled into the parvati_firmware_parity_test
// oracle. On AVR these macros/devices place data in flash and read it with
// special instructions; on the desktop build everything is plain memory.
// This header SHADOWS the real <avr/pgmspace.h> via the shim include path
// (tests/firmware_shim comes before ambika_reference in the target's include
// dirs), so the firmware tree itself stays untouched.
#pragma once

#include <cstring>
#include <cstdint>

// On AVR, PROGMEM places an object in program memory. On the host build all
// data is plain (the tables are small; the firmware semantics we compare are
// unchanged by the storage class).
#define PROGMEM

// The program-memory qualified typedefs degrade to their plain equivalents.
typedef uint8_t prog_uint8_t;
typedef uint16_t prog_uint16_t;
typedef int16_t prog_int16_t;
typedef uint32_t prog_uint32_t;
typedef char prog_char;

// Host-side flash reads are plain reads (see avrlib/resources_manager.h).
//
// pgm_read_word needs TWO host overloads, dispatched by pointee type, because
// the firmware uses it for two different things:
//   (1) reading a 16-bit DATA word from a table (pgm_read_word(table + i)) —
//       returns uint16_t;
//   (2) reading a POINTER entry from a PROGMEM pointer table (the generated
//       lookup_table_table[] / string_table[] in controller/resources.cc).
//       On AVR those entries are 16-bit flash addresses; on the host — where
//       PROGMEM degrades to plain storage — they are REAL pointers, and the
//       firmware code casts pgm_read_word(&table[resource]) to a pointer and
//       dereferences it. Truncating to uint16_t (the naive shim) yields a
//       garbage pointer and segfaults, so the pointer-table overload returns
//       the full host pointer instead.
// 16-bit DATA word reads (1) start here; byte reads are always data reads.
inline uint8_t pgm_read_byte (const void* p)
{
    return *static_cast<const uint8_t*> (p);
}
inline const uint16_t* pgm_read_word (const uint16_t* const* p)
{
    return *p;   // pointer-table entry (lookup_table_table[])
}
inline const char* pgm_read_word (const char* const* p)
{
    return *p;   // pointer-table entry (string_table[])
}
inline uint16_t pgm_read_word (const uint16_t* p)
{
    return *p;   // 16-bit DATA word
}
inline uint16_t pgm_read_word (const int16_t* p)
{
    uint16_t v;
    std::memcpy (&v, p, sizeof (v));
    return v;   // 16-bit DATA word (signed storage)
}
inline void* memcpy_P (void* dst, const void* src, size_t n)
{
    return std::memcpy (dst, src, n);
}
inline char* strncpy_P (char* dst, const char* src, size_t n)
{
    return std::strncpy (dst, src, n);
}
