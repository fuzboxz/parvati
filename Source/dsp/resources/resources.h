// Ported from ambika_reference/voicecard/resources.h
//
// Declarations of every wavetable / lookup table transcribed faithfully from
// the firmware PROGMEM data (see resources_data.cpp). The string resources
// (str_res_dummy / string_table) are controller-UI only and are NOT part of
// the voice DSP, so they are intentionally omitted.
//
// All PROGMEM / pgm_read_* semantics collapse to plain RAM arrays: a Lookup is
// just `table[index]` (see resources_manager.h). Every table's element count
// matches the firmware *_SIZE macros exactly (enforced by static_asserts in
// resources_data.cpp).

#ifndef AMBIKA_DSP_RESOURCES_RESOURCES_H_
#define AMBIKA_DSP_RESOURCES_RESOURCES_H_

#include <cstdint>
#include <cstddef>

namespace ambika::dsp {

// ---------------------------------------------------------------------------
// Lookup-table IDs (uint16_t element tables) — indices into lookup_table_table.
// Values match the firmware LUT_RES_* macros verbatim.
// ---------------------------------------------------------------------------
inline constexpr int LUT_RES_LFO_INCREMENTS                 = 0;
inline constexpr int LUT_RES_ENV_PORTAMENTO_INCREMENTS      = 1;
inline constexpr int LUT_RES_OSCILLATOR_INCREMENTS          = 2;
inline constexpr int LUT_RES_FM_FREQUENCY_RATIOS            = 3;
inline constexpr int LUT_RES_VCA_LINEARIZATION              = 4;
inline constexpr int LUT_RES_CZ_PHASE_RESET                 = 5;

// Table COUNTS (memory-safety migration): bounds for the indirection tables
// in resources_data.cpp, enforced by static_assert there and used by
// ResourcesManager::Lookup to reject out-of-range resource ids.
// (kNumWaveformTables is defined below the WAV_RES_* ids that bound it.)
inline constexpr std::size_t kNumLookupTables = LUT_RES_CZ_PHASE_RESET + 1;        // 6

inline constexpr std::size_t LUT_RES_LFO_INCREMENTS_SIZE                = 128;
inline constexpr std::size_t LUT_RES_ENV_PORTAMENTO_INCREMENTS_SIZE     = 128;
inline constexpr std::size_t LUT_RES_OSCILLATOR_INCREMENTS_SIZE         = 768;
inline constexpr std::size_t LUT_RES_FM_FREQUENCY_RATIOS_SIZE           = 25;
inline constexpr std::size_t LUT_RES_VCA_LINEARIZATION_SIZE             = 256;
inline constexpr std::size_t LUT_RES_CZ_PHASE_RESET_SIZE                = 4;

// ---------------------------------------------------------------------------
// Wavetable IDs (uint8_t byte tables) — indices into waveform_table.
// Values match the firmware WAV_RES_* macros verbatim.
//
// NOTE (aliasing): waveform_table indices 9, 16 and 23 are placeholders that
// alias `wav_res_sine` (the firmware only defines bandlimited zones 0..5 for
// square/saw and 0,3,4,5 for triangle). Indices 18/19 alias
// `wav_res_bandlimited_triangle_0`. The oscillators clamp wave_index so these
// placeholders are unreachable at runtime, but they must occupy the slot so
// the table layout matches the firmware (30 entries, indices 0..29).
// The firmware's reserved-identifier names (WAV_RES__BANDLIMITED_TRIANGLE_0,
// WAV_RES___BANDLIMITED_TRIANGLE_0) are renamed here to valid C++ identifiers.
// ---------------------------------------------------------------------------
inline constexpr int WAV_RES_FORMANT_SINE                  = 0;
inline constexpr int WAV_RES_FORMANT_SQUARE                = 1;
inline constexpr int WAV_RES_SINE                          = 2;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_0          = 3;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_1          = 4;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_2          = 5;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_3          = 6;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_4          = 7;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_5          = 8;
inline constexpr int WAV_RES_BANDLIMITED_SQUARE_6          = 9;   // -> wav_res_sine
inline constexpr int WAV_RES_BANDLIMITED_SAW_0             = 10;
inline constexpr int WAV_RES_BANDLIMITED_SAW_1             = 11;
inline constexpr int WAV_RES_BANDLIMITED_SAW_2             = 12;
inline constexpr int WAV_RES_BANDLIMITED_SAW_3             = 13;
inline constexpr int WAV_RES_BANDLIMITED_SAW_4             = 14;
inline constexpr int WAV_RES_BANDLIMITED_SAW_5             = 15;
inline constexpr int WAV_RES_BANDLIMITED_SAW_6             = 16;  // -> wav_res_sine
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_0        = 17;
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_1        = 18;  // -> wav_res_bandlimited_triangle_0 (firmware: WAV_RES__BANDLIMITED_TRIANGLE_0)
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_2        = 19;  // -> wav_res_bandlimited_triangle_0 (firmware: WAV_RES___BANDLIMITED_TRIANGLE_0)
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_3        = 20;
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_4        = 21;
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_5        = 22;
inline constexpr int WAV_RES_BANDLIMITED_TRIANGLE_6        = 23;  // -> wav_res_sine
inline constexpr int WAV_RES_VOWEL_DATA                    = 24;
inline constexpr int WAV_RES_DISTORTION                    = 25;
inline constexpr int WAV_RES_LFO_WAVEFORMS                 = 26;
inline constexpr int WAV_RES_ENV_EXPO                      = 27;
inline constexpr int WAV_RES_WAVES                         = 28;
inline constexpr int WAV_RES_WAVETABLES                    = 29;

inline constexpr std::size_t WAV_RES_FORMANT_SINE_SIZE           = 256;
inline constexpr std::size_t WAV_RES_FORMANT_SQUARE_SIZE         = 256;
inline constexpr std::size_t WAV_RES_SINE_SIZE                   = 257;  // 256 + 1 guard for interpolation
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_0_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_1_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_2_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_3_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_4_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_5_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SQUARE_6_SIZE   = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_0_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_1_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_2_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_3_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_4_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_5_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_SAW_6_SIZE      = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_0_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_1_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_2_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_3_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_4_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_5_SIZE = 257;
inline constexpr std::size_t WAV_RES_BANDLIMITED_TRIANGLE_6_SIZE = 257;
inline constexpr std::size_t WAV_RES_VOWEL_DATA_SIZE             = 63;   // 9 vowels x 7 bytes
inline constexpr std::size_t WAV_RES_DISTORTION_SIZE             = 256;
inline constexpr std::size_t WAV_RES_LFO_WAVEFORMS_SIZE          = 2;
inline constexpr std::size_t WAV_RES_ENV_EXPO_SIZE               = 257;  // 256 + 1 guard
inline constexpr std::size_t WAV_RES_WAVES_SIZE                  = 10320; // 80 single-cycle waves x 129 bytes
inline constexpr std::size_t WAV_RES_WAVETABLES_SIZE             = 288;  // 16 definitions x 18 bytes

// (See kNumLookupTables above.) Bounds for the waveform indirection table.
inline constexpr std::size_t kNumWaveformTables = static_cast<std::size_t>(WAV_RES_WAVETABLES) + 1; // 30

// ---------------------------------------------------------------------------
// Extern declarations (defined in resources_data.cpp). Arrays use plain
// external linkage (not constexpr) so they can be referenced from every TU.
// ---------------------------------------------------------------------------
extern const uint16_t lut_res_lfo_increments[LUT_RES_LFO_INCREMENTS_SIZE];
extern const uint16_t lut_res_env_portamento_increments[LUT_RES_ENV_PORTAMENTO_INCREMENTS_SIZE];
extern const uint16_t lut_res_oscillator_increments[LUT_RES_OSCILLATOR_INCREMENTS_SIZE];
extern const uint16_t lut_res_fm_frequency_ratios[LUT_RES_FM_FREQUENCY_RATIOS_SIZE];
extern const uint16_t lut_res_vca_linearization[LUT_RES_VCA_LINEARIZATION_SIZE];
extern const uint16_t lut_res_cz_phase_reset[LUT_RES_CZ_PHASE_RESET_SIZE];

extern const uint8_t wav_res_formant_sine[WAV_RES_FORMANT_SINE_SIZE];
extern const uint8_t wav_res_formant_square[WAV_RES_FORMANT_SQUARE_SIZE];
extern const uint8_t wav_res_sine[WAV_RES_SINE_SIZE];
extern const uint8_t wav_res_bandlimited_square_0[WAV_RES_BANDLIMITED_SQUARE_0_SIZE];
extern const uint8_t wav_res_bandlimited_square_1[WAV_RES_BANDLIMITED_SQUARE_1_SIZE];
extern const uint8_t wav_res_bandlimited_square_2[WAV_RES_BANDLIMITED_SQUARE_2_SIZE];
extern const uint8_t wav_res_bandlimited_square_3[WAV_RES_BANDLIMITED_SQUARE_3_SIZE];
extern const uint8_t wav_res_bandlimited_square_4[WAV_RES_BANDLIMITED_SQUARE_4_SIZE];
extern const uint8_t wav_res_bandlimited_square_5[WAV_RES_BANDLIMITED_SQUARE_5_SIZE];
extern const uint8_t wav_res_bandlimited_saw_0[WAV_RES_BANDLIMITED_SAW_0_SIZE];
extern const uint8_t wav_res_bandlimited_saw_1[WAV_RES_BANDLIMITED_SAW_1_SIZE];
extern const uint8_t wav_res_bandlimited_saw_2[WAV_RES_BANDLIMITED_SAW_2_SIZE];
extern const uint8_t wav_res_bandlimited_saw_3[WAV_RES_BANDLIMITED_SAW_3_SIZE];
extern const uint8_t wav_res_bandlimited_saw_4[WAV_RES_BANDLIMITED_SAW_4_SIZE];
extern const uint8_t wav_res_bandlimited_saw_5[WAV_RES_BANDLIMITED_SAW_5_SIZE];
extern const uint8_t wav_res_bandlimited_triangle_0[WAV_RES_BANDLIMITED_TRIANGLE_0_SIZE];
extern const uint8_t wav_res_bandlimited_triangle_3[WAV_RES_BANDLIMITED_TRIANGLE_3_SIZE];
extern const uint8_t wav_res_bandlimited_triangle_4[WAV_RES_BANDLIMITED_TRIANGLE_4_SIZE];
extern const uint8_t wav_res_bandlimited_triangle_5[WAV_RES_BANDLIMITED_TRIANGLE_5_SIZE];
extern const uint8_t wav_res_vowel_data[WAV_RES_VOWEL_DATA_SIZE];
extern const uint8_t wav_res_distortion[WAV_RES_DISTORTION_SIZE];
extern const uint8_t wav_res_lfo_waveforms[WAV_RES_LFO_WAVEFORMS_SIZE];
extern const uint8_t wav_res_env_expo[WAV_RES_ENV_EXPO_SIZE];
extern const uint8_t wav_res_waves[WAV_RES_WAVES_SIZE];
extern const uint8_t wav_res_wavetables[WAV_RES_WAVETABLES_SIZE];

// Pointer indirection tables (firmware parity). Indexed by the LUT_RES_* /
// WAV_RES_* IDs above. waveform_table placeholders alias wav_res_sine (slots
// 9/16/23) and wav_res_bandlimited_triangle_0 (slots 18/19); see resources_data.cpp.
extern const uint16_t* const lookup_table_table[];
extern const uint8_t*  const waveform_table[];

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_RESOURCES_RESOURCES_H_
