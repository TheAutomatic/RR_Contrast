# `rr_contrast_v2_motion` Supplement & OpenRR Stage 2/3 Architecture Review

> **Scope of this document (`v2-motion` branch)**
> 1. **Round-1 (`rr_contrast_v1`) Verification Summary**: Confirms `285/285` SHA-256 integrity on the RTX 5090 (`310.9.1.0`, Driver `621.14`) delivery and summarizes locked anchors + the input-synthesis degeneracies found while analyzing Round-1 `05_probes`.
> 2. **Task A — `v2_motion` Code Audit & One-Shot RTX 5090 Supplement (`<15s`, `~420 MB` raw)**: Exact input fixes (`100%` backward-compatible with `v1`), 21-case matrix (`4` `QualityExt` + `17` micro-probes), pre-flight audit checklist, and single-command execution (`run_suite_v2_motion.ps1`).
> 3. **Task B — Optional Cross-Review of OpenRR RDNA4/ACO Progress & Stage 2/3 Architecture**: Links to [`docs/openrr-plans/`](openrr-plans/) (the 4 original RDNA4/FP8/ACO/Mochi design docs), [`TheAutomatic/OpenRR-ASI@codex/mesa-263-comparison`](https://github.com/TheAutomatic/OpenRR-ASI/tree/codex/mesa-263-comparison) (`4510cc1`), local-directory-to-repository mapping, and 4 concrete engineering questions.

---

## 1. Round-1 (`rr_contrast_v1`) Verification & Locked Anchors

All `285/285` files in the Round-1 RTX 5090 (`nvngx_dlssd.dll` `310.9.1.0`, whole-PE SHA-256 `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80`, Driver `621.14`, `_nvngx.dll` `32.0.16.2114`) archive passed SHA-256 verification and `validation_summary.json = PASS`:

- **Clean GPU Timing & VRAM (`03_timing_core`, zero readback, steady 240 frames — locked, no re-run needed)**:
  - `1080p Quality (1280×720 -> 1920×1080)`: **`0.870 ms`** median (`1508 MiB` DXGI local delta)
  - `1440p Quality (1706×960 -> 2560×1440)`: **`1.409 ms`** median (`1581 MiB` DXGI local delta)
  - `4K Quality (2560×1440 -> 3840×2160)`: **`2.905 ms`** median (`1709 MiB` DXGI local delta)
  - `4K Performance (1920×1080 -> 3840×2160)`: **`1.958 ms`** median (`1640 MiB` DXGI local delta; vs OpenRR RDNA4 RX 9070 XT `789 MB` persistent allocation — `<50%` of official NGX VRAM)
  - Dynamic resolution subrect switches (`85% <-> 100%`, both `Reset = 0` and `Reset = 1`) succeeded natively (`0x00000001`, `recreated = 0`) across all resolutions.
- **Confirmed Static Baseline & Probe Findings (`02_quality_core` & `05_probes`)**:
  - `S0` (`g0_core7` & `g1_full16`) steady + DRS outputs locked (`02_quality_core` needs **no** re-run).
  - `P12_preset_default_hint0` is bit-for-bit identical (`0` half diffs) to `P00` (`Preset_F = 6`), confirming `310.9.1.0` routes `Default (0)` to the same preset as `Preset_F (6)`.
  - `P07_guide_dof_presetF` (`6,180,503` half diffs vs `P00`, `20.785 dB`) and `P08_guide_dof_presetE` (`6,265,856` half diffs vs `P00`, `22.662 dB`) are **both** active and produce distinct outputs on `310.9.1.0`.
  - `P01_guide_disocc` (`5,181,029` half diffs, `44.705 dB`), `P06_guide_resp_r16f` (`5,180,137` half diffs, `44.698 dB`), and `P11_odd_subrect_1281x721` (`1281×721` inside `1288×728`) are all active and finite.
  - Meanwhile, Round-1 recorded `0` half diffs on `P03 vs P02`, `P05 vs P00`, `P09 vs P00`, and `P10 vs P00`, an extreme `6.17 dB` luminance drop on `P02`/`P03`, and entangled `SSS + CBT` in `P04` (`5,300,297` half diffs, `37.649 dB`). Inspecting `include/rr_ref_scenes.hpp` uncovered four input-synthesis degeneracies sufficient to cause or mask those probe results — `v2_motion` fixes those inputs to re-verify actual RTX 5090 GPU behavior.
- **NGX Runtime Fixes Integrated on `v2-motion` (`54c908e`)**:
  - Merged Round-1 `NVSDK_NGX_D3D12_Init_ProjectID` + `NVSDK_NGX_D3D12_Shutdown1` + `--process-exit-teardown` (`-ProcessExitTeardown`) directly into `v2-motion`.

---

## 2. Task A — `rr_contrast_v2_motion` Audit & One-Shot RTX 5090 Supplement

### 2.1 Four Input-Synthesis Issues Fixed in `v2_motion` (To Be Re-Verified on RTX 5090)

1. **Extra `>> 20` shift in `S1`/`S2` motion vectors (`rr_ref_scenes.hpp`)**:
   - `du_bg_q20` is Q20 UV delta; multiplying `du_bg_q20 * rw` already yields Q20 pixel displacement. `v1` applied an extra `>> 20` (`>> 21` on `tlmv`) before `Q20ToFloat`, shrinking all `S1`/`S2` `mv`, `smv`, and `tlmv` values by `2^-20` (`~3e-6` px -> FP16 zero/subnormal).
   - **Fix in `v2_motion`**: Removed the extra shift (`mv` up to `~3.25` px/frame X on the moving sphere, `~1.5` px/frame background, `~7.5` px/frame output-res `tlmv`) and used smooth wrap-free motion across frames `[-1..95]` so frame `0` has identical velocity to frames `1..79`.

2. **Static `world_to_view` camera matrix (`P05`, `P09`, `P10`)**:
   - In `v1`, `world_to_view` was identity on every frame (`reflection_mv == 0`) while `mv` and `smv` were `~0`, leaving `P05 == P00`, `P09 == P00`, and `P10 == P00` in Round 1.
   - **Fix in `v2_motion`**: Added smooth per-frame Q20 camera translation in `world_to_view[12..14]` (row-vector convention matching `OpenRR/gpu/src/engine.cpp:20,130`) together with active pixel-scale `mv` and `smv`. Re-running `P05`, `P09` (`pair_ref = P05`), and `P10` tests how `310.9.1.0` separates surface `mv`, camera-derived reflection motion, explicit `SpecularMvec`, and omitted `SpecularHitDistance`.

3. **Spatially uniform `TransparencyLayer` + inverted `TransparencyLayerOpacity` outside glass (`P02` & `P03`)**:
   - In `v1`, the glass region had spatially constant color/alpha (so any `3×3` neighborhood clamp collapses `tlmv` reprojection to the same constant), and outside the glass box `v1` wrote `1.0 - alpha = 1.0` into `TransparencyLayerOpacity` (causing `6.17 dB` darkening across the opaque background if `TransparencyLayerOpacity` represents layer opacity rather than transmittance).
   - **Fix in `v2_motion`**: Outside the glass slab `tl = (0,0,0,0)` and `tl_op = (0,0,0,0)`; inside the glass slab, `tl` and `tl_op` carry a moving triangle-wave ramp advected by `du_tl_q20 / dv_tl_q20`, and `probes_summary.json` reports **`P03 vs P02` pairwise diffs** directly.

4. **`ColorBeforeTransparency == Color` & combined `SSS + CBT` (`P04` -> `P04a` + `P04b`)**:
   - In `v1`, `P04` bound `SSS` and `CBT` simultaneously (yielding `5,300,297` half diffs from `SSS` alone) while copying `rad` verbatim into `cbt` (`color - cbt == 0` everywhere), making `CBT`'s isolated contribution unobservable.
   - **Fix in `v2_motion`**: Split `P04` into `P04a_guide_sss_only` and `P04b_guide_cbt_only`, with `cbt = (5/8) * rad` in the lower-half veil region (`y >= rh/2`) while `color` stays bit-identical to `P00`.

### 2.2 Complete `v2_motion` Suite Matrix (`21` Cases, `44` Fixture Hashes, `561` Eval Frames Incl. Smoke, `<15s` GPU Time)

`--suite v1` (default) remains **100% untouched** (`76/76` canonical SHA-256s in `cases/cases.json` verified). Passing `--suite v2_motion` runs `cases/cases_v2_motion.json` (`44/44` canonical SHA-256s across `21` cases):

| Pass | Case ID | Frames | Exact Code Configuration (`src/ngx_rr_bench.cpp` & `include/rr_ref_scenes.hpp`) | Pair / Equiv Check |
|---|---|---:|---|---|
| `QualityExt` | `Q_S1_1080p_qual`, `Q_S1_4k_qual` | `80 + 64` | `S1` Halton temporal + DRS (`Reset=0` & `Reset=1` on `1080p_qual`) with active pixel MVs & moving camera | — |
| `QualityExt` | `Q_S2_1080p_qual`, `Q_S2_4k_perf` | `64 × 2` | `S2` `~16×` HDR moving highlights + `0.04` glossy floor + `0.017` dark tiles | — |
| `Probes` | `P00_baseline_s1_g0` | `16` | `v2_motion` `S1` `1080p_qual` baseline (`g0`, `create_flags = 0x4B`) | — |
| `Probes` | `P01_guide_disocc` | `16` | `DLSS.DisocclusionMask` (`R16F`) re-verified under true pixel motion | `vs P00` |
| `Probes` | `P02_guide_transp_rgba` | `16` | Moving textured `TransparencyLayer` + `TransparencyLayerOpacity` (`0` outside glass), `tlmv` unbound | `vs P00` |
| `Probes` | `P03_guide_transp_tlmv` | `16` | `P02` + output-res `TransparencyLayerMvecs` | **`pair_ref = P02`** |
| `Probes` | `P04a_guide_sss_only` | `16` | Isolated `DLSSD.ScreenSpaceSubsurfaceScatteringGuide` (`R16F`) | `vs P00` |
| `Probes` | `P04b_guide_cbt_only` | `16` | Isolated `DLSSD.ColorBeforeTransparency` (`cbt = 5/8 * rad` for `y >= rh/2`, `color == P00`) | `vs P00` |
| `Probes` | `P05_guide_spec_mvec` | `16` | Active pixel `GBuffer.SpecularMvec` (`RG16F`) | `vs P00` |
| `Probes` | `P09_no_hit_with_spec_mv` | `16` | Omit `SpecularHitDistance` while binding `GBuffer.SpecularMvec` | **`pair_ref = P05`** |
| `Probes` | `P10_no_hit_plain` | `16` | Omit `SpecularHitDistance` without `SpecularMvec` (`g0` minus `hit_dist`) | `vs P00` |
| `Probes` | `P13a_exposure_no_auto` | `16` | Clear `AutoExposure` (`0x0B`), `ExposureTexture = null`, `Pre.Exposure = 2.0f`, `Exposure.Scale = 0.5f` (`f0..7`) -> `2.0f` (`f8..15`) | `vs P00` |
| `Probes` | `P13b_exposure_tex_1x1` | `16` | Clear `AutoExposure` (`0x0B`), bind `1×1 R32F ExposureTexture = 0.5f` (`f0..7`) -> `2.0f` (`f8..15`), `Pre.Exposure = 2.0f`, `Exposure.Scale = 1.0f` | **`pair_ref = P13a`** |
| `Probes` | `P14_input_subrect_base_32_24` | `16` | `g0` shifted to `Subrect.Base = (32, 24)` inside `1344×768` textures (`CreateFeature` at `1280×720`), outer border filled with `(4.0, 0, 4.0)` magenta / `±16` MV sentinel | **`equiv vs P00`** |
| `Probes` | `P15_mv_scale_neg0p5` | `16` | `g0` with `MotionVectors` pre-multiplied by `-2.0` and `MV.Scale.X/Y = -0.5f` | **`equiv vs P00`** |
| `Probes` | `P16a_mv_jittered_raw` | `16` | `create_flags = 0x4F` (`MVJittered` set) with unjittered MVs (`delta_sign = 0`) | `vs P00` |
| `Probes` | `P16b_mv_jittered_prev_minus_curr` | `16` | `create_flags = 0x4F` (`MVJittered` set) with `mv += (prev_jitter - curr_jitter)` (`delta_sign = +1`) | **`equiv vs P00`** & `pair_ref = P16a` |
| `Probes` | `P16c_mv_jittered_curr_minus_prev` | `16` | `create_flags = 0x4F` (`MVJittered` set) with `mv += (curr_jitter - prev_jitter)` (`delta_sign = -1`) | **`equiv vs P00`** & `pair_ref = P16a` |
| `Probes` | `P17_roughness_unpacked_r16f` | `16` | `DLSS.Roughness.Mode = 0 (Unpacked)` + `GBuffer.Roughness` (`R16F`), while `Normals.w = 0.0` (scene roughness is strictly `>= 0.06` everywhere) | **`equiv vs P00`** |

### 2.3 Pre-Flight Review, Handoff Hygiene & Run Instructions (RTX 5090)

1. **Code & String Verification**: All `Subrect.Base.X/Y` and `ExposureTexture` (`"ExposureTexture"`) parameter strings in `include/rr_ngx_abi.hpp` have been cross-checked directly against the string table of `nvngx_dlssd.dll` (`310.9.1.0`) and `nvsdk_ngx_defs.h` / `nvsdk_ngx_defs_dlssd.h`.
2. **Local Edits & Complete Git Handoff**: Reviewers/agents (`Carbon` + `6astra`) are welcome to patch any newly discovered runtime quirk on the 5090 machine before running:
   - If canonical input synthesis is modified, re-dump `.\bin\ngx_rr_bench.exe --suite v2_motion --dump-fixture-hashes cases\cases_v2_motion.json`.
   - `run_suite_v2_motion.ps1` now records `git_head.txt`, `git_log_1.txt`, `git status --porcelain=v1`, **`git diff HEAD`** (`git_diff.patch`, capturing both staged and unstaged edits), **and copies any untracked files into `00_meta/untracked_files/`**. If local edits are made, pushing a branch/PR or sharing the exact commit alongside `results_v2_motion` is also super helpful for 1:1 AMD replay.

```powershell
git fetch origin
git checkout v2-motion
.\build.ps1
.\run_suite_v2_motion.ps1 -DlssdPath "C:\path\to\nvngx_dlssd.dll" -OutDir results_v2_motion -ProcessExitTeardown
```

---

## 3. Task B — Optional Architecture & Stage 2/3 Roadmap Review

To give external reviewers/agents the full architectural picture without touching our internal development trees, the 4 original planning & hardware-analysis documents have been included in this questioning repository under **[`docs/openrr-plans/`](openrr-plans/)**:

1. **[`docs/openrr-plans/openrr-fp8-advice.md`](openrr-plans/openrr-fp8-advice.md)**: Empirical RDNA4 (`gfx1201`) hardware cost model (`v_wmma_f32_16x16x16_fp8_fp8 = 8 cyc` vs `f16 = 16 cyc`, strict `WMMA + VALU` non-overlap `coexec = False`, VMEM/LDS load writeback cycle budget, direct `f32 -> v_cvt_pk_fp8_f32` conversion rules).
2. **[`docs/openrr-plans/fp8-perf-plan.md`](openrr-plans/fp8-perf-plan.md)**: Master 4-Anchor evaluation framework, B0/B1/B2 historical archive, Stage 2 (`2.0A`, `2.0B`, `2A-2`, `2B`, `2C` lossless/near-lossless FP8) and Stage 3 (activation FP8 + `hist_` `8B -> 4B` compression) plan.
3. **[`docs/openrr-plans/mesa-aco-plan.md`](openrr-plans/mesa-aco-plan.md)**: Offline WSL Mesa RADV (ACO) PAL ELF (`NRPAL002`) splicing into D3D12 `CreateComputePipelineFromElf()`, 61-dword root-constant preamble, and pure-BDA (`set0 = -1`) ABI.
4. **[`docs/openrr-plans/mochi-perf-borrow-23fe-draft.md`](openrr-plans/mochi-perf-borrow-23fe-draft.md)**: Architectural boundary and borrowable D3D12 optimizations between `DLSSNR-AMD` (`mochi`) and `OpenRR`.

### 3.1 Directory-to-Repository Mapping (For Local Paths Referenced Inside `docs/openrr-plans/`)
Those 4 documents are verbatim copies of our local planning docs and reference local machine paths; they map to public repositories as follows:
- `C:\Users\Kenny\Downloads\OpenRR` / `C:\Users\Kenny\.codex\worktrees\mesa-263-comparison\OpenRR` -> **[`TheAutomatic/OpenRR-ASI` (`codex/mesa-263-comparison` @ `4510cc1`)](https://github.com/TheAutomatic/OpenRR-ASI/tree/codex/mesa-263-comparison)**
  - Stage 2 overnight execution report: [`docs/test-evidence/v015-stage2-overnight-results.md`](https://github.com/TheAutomatic/OpenRR-ASI/blob/codex/mesa-263-comparison/docs/test-evidence/v015-stage2-overnight-results.md)
  - Stage 2 daytime execution report + `§6 Post-Review Errata`: [`docs/test-evidence/v015-stage2-day-results.md`](https://github.com/TheAutomatic/OpenRR-ASI/blob/codex/mesa-263-comparison/docs/test-evidence/v015-stage2-day-results.md)
- `C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki` / `/workspace/d3d12-audit-5bebcf8` (`mochi`) -> **[`mochizuki0323/DLSSNR-AMD` (`dev/0.0.8.4`)](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/tree/dev/0.0.8.4)**
- `C:\Users\Kenny\Downloads\OpenRR\build\RR_Contrast` -> **[`TheAutomatic/RR_Contrast` (`v2-motion`)](https://github.com/TheAutomatic/RR_Contrast/tree/v2-motion)**

### 3.2 Four Concrete Questions for External Review
1. **Breaking the `256 VGPR` Ceiling in `TR=1` (`128ch`/`160ch`) `onr_block_fast.glsl`**:
   - Loop-unroll suppression (`[[dont_unroll]]`) was experimentally falsified (VGPR stayed at `256`, runtime slowed `+1.3%..+2.5%`). Meanwhile `enc3_b0..b4` (`C=96, TR=2`, using `W2_DEFER` deferred `c0..c7` accumulation) already runs at `184–192 VGPR` and ties LLPC (`0.98×–1.01×`), whereas `TR=1` materializes `out[8]` while `xp[8]` + prefetched `B` tiles are live (`256 VGPR`, `5 waves` vs LLPC `216 VGPR / 7 waves`). Beyond porting `W2_DEFER`-style deferred accumulation to `TR=1`, is there a cleaner register-pressure pattern for RDNA4 WMMA under ACO?
2. **RADV NIR Lowering (`radv_nir_lower_cooperative_matrix.c`) vs ACO Scheduling**:
   - Our Mesa patch `0002-radv-bound-cmat-address.patch` only bounded `OpCooperativeMatrixLoadKHR` pointer `uadd_carry`; ACO still emits `+129 ds_load_b64`, `+113 v_perm_b32`, and `+148 s_wait_alu` vs LLPC on `160ch` kernels. Is `ROW_MAJOR` global weight / `shared` LDS `coopMatLoad` lowering in `radv_nir_lower_cooperative_matrix.c` the highest-leverage compiler-side target?
3. **Zero-LDS-Growth FP8 (`floate4m3_t`) Activation Path for `2B` (`ENC_MERGE`) & `2A-2` (`C>=96 DEC_UP`)**:
   - With `§6 Post-Review Errata` correcting the daytime misconceptions (`onr_block_fast.glsl` already uses `coopmat<floate4m3_t, ..., 16, 32>` -> `float` accumulator + `qv_bytes()` `sm_89 -> OCP` decoding, and `GL_EXT_shared_memory_block` allows a zero-LDS-growth `floate4m3_t` view inside `ENC_MERGE`'s `30,976 B` LDS budget), what requantization / scaling strategy (fixed pre-scale vs per-tile max-abs) best fits the RDNA4 `WMMA + VALU` serial cycle budget in `openrr-fp8-advice.md` under `~16×` HDR activations?
4. **Stage 3 (`hist_` `8B -> 4B`) & OpenRR NGX `310.9.1.0` Contract Completeness**:
   - Alongside `hist_` format selection (`RGB9E5` / custom packed float vs `R11G11B10F` tested against `v2_motion` `S2` moving HDR highlights) and wiring `MVJittered` / manual `ExposureTexture` + `Pre.Exposure` + `Exposure.Scale` once `P13a/b` and `P16a/b/c` results land, do you see any architectural blind spots across the 4 plan docs?
