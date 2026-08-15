// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Vendored from the Mutable Instruments Ambika controller firmware:
// ambika_reference/controller/resources.cc:773-891 (scale tables) and the
// lookup_table_table[] dispatch :918-950 (including the bageshree->kafi and
// rasia->yaman aliases). Mechanical, faithful transcription — do not edit by
// hand; the tables are the tuning DNA of the hardware (unchanged).
// Upstream: (c) 2012-2015 Emilie Gillet, GPL-3.0 — see NOTICES.md.

#include "TuningTables.h"

#include <array>

namespace parvati
{
namespace
{
// 30 unique tables (32 presets; ids 16 and 32 alias kafi / yaman like the
// firmware lookup_table_table). Offsets in 1/128-semitone units, indexed by
// note class (note % 12); 32767 = muted class (firmware AcceptNote sentinel).
const int16_t kJust[]           = { 0,    15,     5,    20,   -17,    -2,   -12,     2,    17,   -20,    -5,   -15 };
const int16_t kPythagorean[]    = { 0,    15,     5,    -7,    10,    -2,   -12,     2,    17,   -20,    -5,    12 };
const int16_t kAlteredEb[]      = { 0,     0,     0,     0,   -64,     0,     0,     0,     0,     0,     0,   -64 };
const int16_t kAlteredE[]       = { 0,     0,     0,     0,   -64,     0,     0,     0,     0,     0,     0,     0 };
const int16_t kAlteredEa[]      = { 0,     0,     0,     0,   -64,     0,     0,     0,     0,   -64,     0,     0 };
const int16_t kBhairav[]        = { 0,   -12, kTuningSilence, kTuningSilence, -17, -2, kTuningSilence, 2,   -10, kTuningSilence, kTuningSilence, -15 };
const int16_t kGunakri[]        = { 0,    15, kTuningSilence, kTuningSilence, kTuningSilence, -2, kTuningSilence, 2, 17, kTuningSilence, kTuningSilence, kTuningSilence };
const int16_t kMarwa[]          = { 0,    15, kTuningSilence, kTuningSilence, -17, kTuningSilence, -12, kTuningSilence, kTuningSilence, -20, kTuningSilence, -15 };
const int16_t kShree[]          = { 0,   -12, kTuningSilence, kTuningSilence, -17, kTuningSilence, -12, 2, -10, kTuningSilence, kTuningSilence, -15 };
const int16_t kPurvi[]          = { 0,    15, kTuningSilence, kTuningSilence, -17, kTuningSilence, -12, 2, 17, kTuningSilence, kTuningSilence, -15 };
const int16_t kBilawal[]        = { 0, kTuningSilence, 5, kTuningSilence, -17, -2, kTuningSilence, 2, kTuningSilence, 7, kTuningSilence, -15 };
const int16_t kYaman[]          = { 0, kTuningSilence, 5, kTuningSilence, 10, kTuningSilence, 15, 2, kTuningSilence, 7, kTuningSilence, 12 };
const int16_t kKafi[]           = { 0, kTuningSilence, -22, -7, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, -20, -5, kTuningSilence };
const int16_t kBhimpalasree[]  = { 0, kTuningSilence, 5, 20, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, 7, 22, kTuningSilence };
const int16_t kDarbari[]        = { 0, kTuningSilence, 5, -7, kTuningSilence, -2, kTuningSilence, 2, -10, kTuningSilence, -5, kTuningSilence };
const int16_t kRageshree[]      = { 0, kTuningSilence, 5, kTuningSilence, -17, -2, kTuningSilence, 2, kTuningSilence, -20, -5, kTuningSilence };
const int16_t kKhamaj[]         = { 0, kTuningSilence, 5, kTuningSilence, -17, -2, kTuningSilence, 2, kTuningSilence, 7, -5, 12 };
const int16_t kMiMal[]          = { 0, kTuningSilence, 5, -7, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, -20, -5, -15 };
const int16_t kParameshwari[]   = { 0,   -12, kTuningSilence, -7, kTuningSilence, -2, kTuningSilence, kTuningSilence, kTuningSilence, -20, -5, kTuningSilence };
const int16_t kRangeshwari[]    = { 0, kTuningSilence, 5, -7, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, kTuningSilence, kTuningSilence, -15 };
const int16_t kGangeshwari[]    = { 0, kTuningSilence, kTuningSilence, kTuningSilence, -17, -2, kTuningSilence, 2, -10, kTuningSilence, -5, kTuningSilence };
const int16_t kKameshwari[]     = { 0, kTuningSilence, 5, kTuningSilence, kTuningSilence, kTuningSilence, -12, 2, kTuningSilence, -20, -5, kTuningSilence };
const int16_t kPaKafi[]         = { 0, kTuningSilence, 5, -7, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, 7, -5, kTuningSilence };
const int16_t kNatbhairav[]     = { 0, kTuningSilence, 5, kTuningSilence, -17, -2, kTuningSilence, 2, -10, kTuningSilence, kTuningSilence, -15 };
const int16_t kMKauns[]         = { 0, kTuningSilence, 5, kTuningSilence, 10, -2, kTuningSilence, kTuningSilence, -10, kTuningSilence, -5, kTuningSilence };
const int16_t kBairagi[]        = { 0,   -12, kTuningSilence, kTuningSilence, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, kTuningSilence, -5, kTuningSilence };
const int16_t kBTodi[]          = { 0,   -12, kTuningSilence, -7, kTuningSilence, kTuningSilence, kTuningSilence, 2, kTuningSilence, kTuningSilence, -5, kTuningSilence };
const int16_t kChandradeep[]    = { 0, kTuningSilence, kTuningSilence, -7, kTuningSilence, -2, kTuningSilence, 2, kTuningSilence, kTuningSilence, -5, kTuningSilence };
const int16_t kKaushikTodi[]    = { 0, kTuningSilence, kTuningSilence, -7, kTuningSilence, -2, -12, kTuningSilence, -10, kTuningSilence, kTuningSilence, kTuningSilence };
const int16_t kJogeshwari[]     = { 0, kTuningSilence, kTuningSilence, -7, -17, -2, kTuningSilence, kTuningSilence, kTuningSilence, -20, -5, kTuningSilence };

// The firmware lookup_table_table dispatch (scale entries only, ids 1..32 in
// order): id 16 (bageshree) -> kafi, id 32 (rasia) -> yaman, mirroring
// resources.cc:918-950 where lut_res_scale_bageshree / lut_res_scale_rasia
// are not separate arrays but repeated pointers.
const std::array<const int16_t*, kNumTuningPresets> kPresetTable = {{
    kJust,           // 1
    kPythagorean,    // 2
    kAlteredEb,      // 3
    kAlteredE,       // 4
    kAlteredEa,      // 5
    kBhairav,        // 6
    kGunakri,        // 7
    kMarwa,          // 8
    kShree,          // 9
    kPurvi,          // 10
    kBilawal,        // 11
    kYaman,          // 12
    kKafi,           // 13
    kBhimpalasree,   // 14
    kDarbari,        // 15
    kKafi,           // 16 bageshree (firmware alias)
    kRageshree,      // 17
    kKhamaj,         // 18
    kMiMal,          // 19
    kParameshwari,   // 20
    kRangeshwari,    // 21
    kGangeshwari,    // 22
    kKameshwari,     // 23
    kPaKafi,         // 24
    kNatbhairav,     // 25
    kMKauns,         // 26
    kBairagi,        // 27
    kBTodi,          // 28
    kChandradeep,    // 29
    kKaushikTodi,    // 30
    kJogeshwari,     // 31
    kYaman,          // 32 rasia (firmware alias)
}};

// Names follow the generator's scales list order
// (controller/resources/lookup_tables.py:151-261).
const std::array<const char*, kNumTuningPresets> kPresetName = {{
    "Just",           "Pythagorean",     "1/4 Eb",           "1/4 E",
    "1/4 Ea",         "Bhairav",         "Gunakri",          "Marwa",
    "Shree",          "Purvi",           "Bilawal",          "Yaman",
    "Kafi",           "Bhimpalasree",    "Darbari",          "Bageshree",
    "Rageshree",      "Khamaj",          "Mi'Mal",           "Parameshwari",
    "Rangeshwari",    "Gangeshwari",     "Kameshwari",       "Pa. Kafi",
    "Natbhairav",     "M.Kauns",         "Bairagi",          "B.Todi",
    "Chandradeep",    "Kaushik Todi",    "Jogeshwari",       "Rasia"
}};

const int16_t kEdo[kNumNoteClasses] = {};
}  // namespace

const int16_t* tuningPresetTable (int id)
{
    if (id < 1 || id > kNumTuningPresets)
        return nullptr;
    return kPresetTable[static_cast<size_t> (id - 1)];
}

const char* tuningPresetName (int id)
{
    if (id < 1 || id > kNumTuningPresets)
        return nullptr;
    return kPresetName[static_cast<size_t> (id - 1)];
}

const int16_t* tuningEdoTable() { return kEdo; }
}  // namespace parvati
