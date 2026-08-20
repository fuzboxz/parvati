# Env→VCA / Env→VCF "Double Modulation" Investigation

**Verdict: CORRECT PARITY — the hardware Ambika does exactly the same. Double modulation (pre-route + matrix slot both active) is inherent Ambika firmware behavior, faithfully replicated by Parvati. No engine change needed.**

## Firmware ground truth (ambika_reference/)

Per block, firmware runs `LoadSources()` → `ProcessModulationMatrix()` → `UpdateDestinations()` (`voicecard/voice.cc:490-494`, `ProcessBlock`).

**VCF pre-routes (real, hardcoded):** `voicecard/voice.cc:307-315` — after the matrix has already added its slot modulations into `dst_[MOD_DST_FILTER_CUTOFF]`, `UpdateDestinations()` ADDS on top:
- `cutoff += S8U8Mul(patch_.filter_env, modulation_sources_[MOD_SRC_ENV_2])` — **Env 2 × `filter_env`** (a dedicated patch param, `common/patch.h:262`)
- `cutoff += S8S8Mul(patch_.filter_lfo, MOD_SRC_LFO_2 + 128)` — **LFO 2 × `filter_lfo`** (`common/patch.h:263`)
- plus keyboard tracking seeded at load: `dst_[MOD_DST_FILTER_CUTOFF] = S16ClipU14(cutoff + pitch_value_ - 8192)` (`voice.cc:254`; the comment at :308-311 notes a *negative NOTE→CUTOFF matrix routing* is the intended way to cancel tracking).

Note: the pre-routed filter envelope is **Env 2**, not Env 1 (task premise slightly off). Amount is user-controlled via the `filter_env`/`filter_lfo` knobs, not fixed.

**VCA "pre-route" (not hardcoded):** firmware has NO hidden env→VCA path. `modulation_destinations_[MOD_DST_VCA]` is seeded to `part_.volume << 1` (`voice.cc:238`), and the amp envelope reaches the VCA only as a **normal matrix slot**: the controller factory part is `MOD_SRC_ENV_3, MOD_DST_VCA, 63` + `MOD_SRC_VELOCITY, MOD_DST_VCA, 16` (`controller/part.cc:68-69`). (The voicecard's own standalone init uses `ENV_2→VCA@32`, `voice.cc:92`.) The only VCA special case is that matrix amounts are **multiplicative** instead of additive (`voice.cc:286-299`, comment "Yet another special case :(").

**No matrix exclusion:** the hardware controller exposes the full source list (0..MOD_SRC_CONSTANT_256) and full destination list (0..MOD_DST_LAST−1) for every slot — `controller/parameter.cc:602-614`. So on real hardware a user can freely route ENV_1/2/3→FILTER_CUTOFF on top of the `filter_env` pre-route, or stack a second env into VCA. Double modulation is a designed-in hardware capability.

## Parvati

- `Source/dsp/voice.cpp` is a faithful line-by-line port: VCA seed (`:293`), VCA multiplicative case (`:342-363`), and the identical hardcoded filter pre-routes env2×filter_env + lfo2×filter_lfo applied AFTER the matrix (`:373-383`).
- Default patch matches the controller factory part exactly, incl. `ENV_3→VCA@63`, `VELOCITY→VCA@16` (`Source/ParameterLayout.cpp:208-209` vs `part.cc:68-69`).
- UI offers the full 31-source/19-dest lists with **no exclusions** (`ParameterLayout.cpp:95-112`, `makeModSources`/`makeModDests`; `ui/ModSourceCatalog.h`). ModDestMap is knob↔dest mapping only — it contains no source filtering (`Source/ui/ModDestMap.h:36-48`).

## Tests
- `tests/synth_param_coverage_test.cpp:568-593` — tests `filter_env` pre-route AND routes ENV_1→FILTER_CUTOFF via the matrix in the same patch (both active = double modulation, asserted audible).
- `tests/mod_audio_test.cpp:3-94` — pins ENV3→VCA slot default 63.
- `tests/controller_mod_test.cpp:13-14` — controllers routed to FILTER_CUTOFF at amount 63.

## Recommendation
Document, don't "fix": add a UI tooltip/help note that (a) Filter Cutoff always receives Env 2 × `filter_env` + LFO 2 × `filter_lfo` + key tracking on top of matrix slots, and (b) VCA matrix amounts are multiplicative. Greying out env sources for cutoff would *break* parity (the firmware comment at `voice.cc:308-311` relies on matrix NOTE→CUTOFF being allowed to counteract tracking — the same coexistence applies to envs).
