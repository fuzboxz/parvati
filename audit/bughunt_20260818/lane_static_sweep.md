# Bug hunt 2026-08-18 — Lane: static-analysis sweep
(Scanner replication by reviewer agent; supervisor-verified findings below.
 The literal clang-tidy/cppcheck commands still need a supervisor run — see lane TODO.)

## Scanner verdicts (logic replicated by reading tools/check_*.py + Source)
- check_async_this.py: CLEAN (6/6 allowlist entries verified current).
- check_combo_clear.py: CLEAN (7/7 dontSendNotification; allowlist empty).
- check_translations.py: CLEAN (FR/DE symmetric, 108 keys each).

## F-static-1: OOB LUT read — unvalidated env_lfo rate byte from .MUL / host-state — VERIFIED
- Source/dsp/voice.cpp:107 (consume at :388); entry PluginProcessor.cpp:1046 (.MUL raw), SynthEngine.cpp:686 (state restore)
- severity: medium (UB rodata read; garbage LFO rate on corrupt/crafted .MUL)
- lut_res_lfo_increments has 128 entries (resources_data.cpp:12); rate byte 143..255 → index 128..240 OOB.
- deterministic_check: ASan .MUL fixture with patch byte 45 = 0xFF; release-mode byte-compare render vs rate=142.

## F-static-2: OOB LUT read — unvalidated PartData portamento byte — VERIFIED
- Source/dsp/voice.cpp:190; push SynthEngine.cpp:1141 (setPartByte raw)
- severity: medium (UB rodata read on every note-on Trigger with corrupt byte 6 > 127)
- lut_res_env_portamento_increments 128 entries; portamento_time 128..255 OOB.
- deterministic_check: same harness with part byte 6 = 255; render byte-compare vs 127.

## F-static-3: Uncaught bad_alloc — unbounded S in .kbm parser — VERIFIED
- Source/ScalaImport.cpp:256 (reserve before the S!=12 gate at :310)
- severity: high (host crash via crafted .kbm: S=999999999 → multi-GB reserve → uncaught bad_alloc → std::terminate)
- deterministic_check: scala import test with S="999999999" expecting graceful error (pre-fix: crash).

## Verified-safe (no report): NoteStack memsets, arpStep_ clamp, PatchFile .MUL part-idx bounds,
## fn_table_ dispatch (22 entries, clamped), envelope Clip, pitch LUT bounds, arp/seq staging clamps.
