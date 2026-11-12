# `rr_contrast_v2_motion` Supplement & OpenRR Stage 2/3 Architecture Review

> **Scope of this document (`v2-motion` branch)**
> 1. **Round-1 (`rr_contrast_v1`) Verification Summary**: Confirms `285/285` SHA-256 integrity on the RTX 5090 (`310.9.1.0`) delivery and summarizes locked anchors + the 4 subtle scene-synthesis silent no-ops uncovered by Round-1 `05_probes`.
> 2. **Task A — `v2_motion` Code Audit & One-Shot RTX 5090 Supplement (`<15s`, `~420 MB` raw)**: Exact shader-verified root causes, `v2_motion` fixes (`100%` backward-compatible with `v1`), 20-case matrix (`4` `QualityExt` + `16` micro-probes), pre-flight audit checklist for reviewers/agents, and single-command execution (`run_suite_v2_motion.ps1`).
> 3. **Task B — Cross-Review of OpenRR RDNA4/ACO Progress & Stage 2/3 Roadmap**: Links to [`TheAutomatic/OpenRR-ASI@codex/mesa-263-comparison`](https://github.com/TheAutomatic/OpenRR-ASI/tree/codex/mesa-263-comparison) (`4510cc1`), summarizes closed Stage 2.0A fixes and daytime negative-result falsifications (including `§6 Post-Review Errata`), and lists 4 concrete engineering questions for external review.

---

## 1. Round-1 (`rr_contrast_v1`) Verification & Locked Anchors

All `285/285` files in the Round-1 RTX 5090 (`nvngx_dlssd.dll` `310.9.1.0`, whole-PE SHA-256 `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80`, Driver `595.71`) archive passed SHA-256 verification and `validation_summary.json = PASS`:

- **Clean GPU Timing & VRAM (`03_timing_core`, zero readback, steady 240 frames)**:
  - `1080p Quality (1280×720 -> 1920×1080)`: **`0.870 ms`** median (`1508 MiB` DXGI local delta)
  - `1440p Quality (1706×960 -> 2560×1440)`: **`1.409 ms`** median (`1581 MiB` DXGI local delta)
  - `4K Quality (2560×1440 -> 3840×2160)`: **`2.905 ms`** median (`1709 MiB` DXGI local delta)
  - `4K Performance (1920×1080 -> 3840×2160)`: **`1.958 ms`** median (`1640 MiB` DXGI local delta; vs OpenRR RDNA4 RX 9070 XT `789 MB` persistent allocation — `<50%` of official NGX VRAM)
  - Dynamic resolution subrect switches (`85% <-> 100%`, both `Reset = 0` and `Reset = 1`) succeeded natively (`0x00000001`, `recreated = 0`) across all resolutions.
- **Confirmed ABI & Preset Routing (`02_quality_core` & `05_probes`)**:
  - `S0` (`g0_core7` & `g1_full16`) steady + DRS outputs locked (`02_quality_core` needs **no** re-run).
  - `P12_preset_default_hint0` is bit-for-bit identical (`0.000000` max abs diff) to `P00` (`Preset_F = 6`), confirming `310.9.1.0` routes `Default (0)` directly to `Preset_F` (`rr2`).
  - `P07_guide_dof_presetF` is a `0.000000` no-op while `P08_guide_dof_presetE` is active (`max_abs_diff = 0.2588`), confirming `DLSSD.DepthOfFieldGuide` is gated exclusively on `Preset_E (5)`.
  - `P01_guide_disocc` (`max_abs_diff = 0.0684`), `P06_guide_resp_r16f` (`0.2289`), `P10_no_hit_plain` (`0.1635`), and `P11_odd_subrect_1281x721` (`1281×721` inside `1288×728`) are all active and finite.
- **NGX Runtime Fixes Integrated on `v2-motion` (`54c908e`)**:
  - Merged Round-1 `NVSDK_NGX_D3D12_Init_ProjectID` + `NVSDK_NGX_D3D12_Shutdown1` + `--process-exit-teardown` (`-ProcessExitTeardown`) directly into `v2-motion`.

---

## 2. Task A — `rr_contrast_v2_motion` Audit & One-Shot RTX 5090 Supplement

### 2.1 Four Root-Cause Bugs in `v1` Scene Synthesis (Cross-Verified Against `rr2` Shaders)

Round-1 `05_probes` revealed unexpected `supported_noop` (`0.000000` diff) results on `P03 vs P02`, `P04 vs P00`, and `P05 vs P00`, plus an anomalous luminance drop on `P02`. Cross-checking `include/rr_ref_scenes.hpp` against the reverse-engineered `rr2` shader graph (`enc0_attr.comp` and `downsample_tl.comp`) isolated **four exact bugs in `v1` synthetic inputs**:

1. **Double `>> 20` shift in `S1`/`S2` motion vectors (`rr_ref_scenes.hpp`)**:
   - `du_bg_q20` is Q20 UV delta; multiplying `du_bg_q20 * rw` already yields Q20 pixel displacement! `v1` applied an extra `>> 20` (`>> 21` on `tlmv`) before `Q20ToFloat`, shrinking all `S1`/`S2` `mv`, `smv`, and `tlmv` values by `2^-20` (`~3e-6` px -> FP16 subnormal/zero).
   - **Fix in `v2_motion`**: Removed the extra shift (`mv` up to `~11.5` px/frame on the moving sphere, `~2.5` px/frame background, `~12` px/frame output-res `tlmv`), and replaced the `% 64` wrap on `sph_cx_q20(f - 1)` at `f = 0` with continuous `(f * speed) & 63` arithmetic (`sph_cx(0) - sph_cx(-1) == +speed`).

2. **Static `world_to_view` zeroed `reflection_mv` (`P05` vs `P00` & `P09` vs `P05`)**:
   - In `enc0_attr.comp` (`reflection_mv`, lines `304–351`), when `SpecularHitDistance` is bound (`!(C(0x374) & OPTF_NO_HIT)`), the encoder tests `smv != 0` first (`use_smv`), and otherwise computes analytical camera-motion virtual-point reflection MVs from `world_to_view_prev * inv(world_to_view_curr)`. Because `v1` used a static camera (`world_to_view_curr == world_to_view_prev`, producing `reflection_mv == 0`) **and** `smv ~ 0`, `P05` equaled `P00` bit-for-bit!
   - **Matrix convention check**: Verified against `OpenRR/gpu/src/engine.cpp:20,130` and `OpenRR/asi/src/camera_and_sl.hpp:466,504` (`v' = v * M`, row-major row-vector convention: translation resides in `world_to_view[12..14]`, with `view_to_clip[11] = +1.0f`, `view_to_clip[14] = z_near = 0.1f` for inverted infinite projection).
   - **Fix in `v2_motion`**: Added smooth per-frame camera translation in `world_to_view[12..13]` (`cam_x`, `cam_y`) + matching camera-pan pixel MVs on non-sphere surfaces, plus non-zero glossy floor `hit_t` and reflected `smv`.
   - **Architectural invariant verified by `v2_motion`**:
     - `P05 != P00` (`smv` overrides `reflection_mv`).
     - `P10 != P00` and `P10 != P05` (`OPTF_NO_HIT` falls back to `mv_c` instead of `reflection_mv`).
     - `P09 vs P05`: In OpenRR's reverse-engineered `enc0_attr.comp:466–470`, `if (any(notEqual(smv, vec2(0.0))))` executes **before** `else if ((C(0x374) & OPTF_NO_HIT) == 0u)`. Probing `P09 vs P05` with real non-zero `smv` directly tests whether official `310.9.1.0` shares this exact `P09 == P05` precedence (`supported_equiv_exact`).

3. **Flat `TransparencyLayer` swallowed by `3×3` clamp + inverted `TransparencyLayerOpacity` outside glass (`P02` & `P03`)**:
   - In `downsample_tl.comp:196–213, 236`:
     1) Reprojected `hist_tl` (`sample_catmull_rom(tl_prev, uv_out + tlmv_px * inv_out_size)`) is clamped to the current frame's `3×3` neighborhood `[lo4, hi4]`. `v1`'s glass region was spatially uniform `(0.08, 0.09, 0.12, 0.35)`, so `lo4 == hi4` clamped **any** `tlmv` offset back to the exact same value (`P03 == P02`)!
     2) Final composition computes `fma(y[c], 1.0 - ores[c], res[c])`, meaning `TransparencyLayerOpacity` is **layer coverage / opacity** (`0.0` = transparent / no overlay, `alpha` = glass opacity), **NOT** transmittance (`1 - alpha`)! `v1` wrote `1.0 - tl_a = 1.0` outside the glass box, causing `1.0 - ores = 0.0` to black out `75%` of the opaque image in `P02`/`P03`!
   - **Fix in `v2_motion`**: Outside the glass slab `tl = (0,0,0,0)` and `tl_op = (0,0,0,0)`; inside the glass slab, `tl` + `tl_op` carry a moving 16-px smooth triangle-wave ramp (`period >= 16 px > 3×3 clamp radius`) rigidly advected by `tlmv` (`(-6, +3)` render px = `(-9, +4.5)` output px/frame), while `P02` leaves `tlmv = 0` so `P03 vs P02` cleanly isolates `TransparencyLayerMvecs`.

4. **`ColorBeforeTransparency == Color` zeroed the `CBT` residual (`P04`)**:
   - In `enc0_attr.comp:599–608`, the `CBT` guide channel encodes `signed_log(luma(expo * col - expo * cbt))`. `v1` copied `rad` directly into `cbt` (`col - cbt == 0` everywhere), making `CBT` identically zero and entangling `SSS` + `CBT` in one probe.
   - **Fix in `v2_motion`**: In the lower-half veil region (`iy >= rh/2`), `cbt = (5/8) * rad` while keeping `color` **bit-identical** to `P00` (`col - cbt = (3/8) * rad > 0`), and `P04` is split into `P04a_guide_sss_only` and `P04b_guide_cbt_only`.

### 2.2 Complete `v2_motion` Suite Matrix (`20` Cases, `545` Eval Frames Incl. Smoke, `<15s` GPU Time)

`--suite v1` (default) remains **100% untouched** (`76/76` canonical SHA-256s in `cases/cases.json` verified). Passing `--suite v2_motion` runs `cases/cases_v2_motion.json` (`42/42` canonical SHA-256s across `20` cases):

| Pass | Case ID | Frames | What It Tests / Locks Down | Pair Ref |
|---|---|---:|---|---|
| `QualityExt` | `Q_S1_1080p_qual`, `Q_S1_4k_qual` | `80 × 2` | `S1` Halton temporal + DRS (`Reset=0` & `Reset=1`) with active pixel MVs & moving camera | — |
| `QualityExt` | `Q_S2_1080p_qual`, `Q_S2_4k_qual` | `80 × 2` | `S2` `16×` HDR moving highlights + `0.04` glossy floor + dark tiles (validates Stage 3 `hist_` FP16->4B format) | — |
| `Probes` | `P00_baseline_s1_g0` | `16` | `v2_motion` `S1` `1080p_qual` baseline (`g0_core7`) | — |
| `Probes` | `P02_guide_transp_rgba` | `16` | Moving textured `TransparencyLayer` + fixed `TransparencyLayerOpacity` (`0` outside glass), `tlmv=0` | `P00` |
| `Probes` | `P03_guide_transp_tlmv` | `16` | `P02` + matching output-res `TransparencyLayerMvecs` | **`P02`** |
| `Probes` | `P04a_guide_sss_only` | `16` | Isolated `DLSSD.ScreenSpaceSubsurfaceScatteringGuide` (`R16F`) | `P00` |
| `Probes` | `P04b_guide_cbt_only` | `16` | Isolated `DLSSD.ColorBeforeTransparency` (`cbt = 5/8 * color` in veil, `color == P00`) | `P00` |
| `Probes` | `P05_guide_spec_mvec` | `16` | Active pixel `GBuffer.SpecularMvec` overriding `reflection_mv` | `P00` |
| `Probes` | `P09_no_hit_with_spec_mv` | `16` | Omit `SpecularHitDistance` while binding active `SpecularMvec` (tests `smv` vs `OPTF_NO_HIT` precedence) | **`P05`** |
| `Probes` | `P10_no_hit_plain` | `16` | Omit `SpecularHitDistance` without `SpecularMvec` (`mv_c` fallback vs moving-camera `reflection_mv`) | `P00` |
| `Probes` | `P11_odd_subrect_1281x721` | `16` | Odd render subrect `1281×721` inside `1288×728` under active motion | — |
| `Probes` | `P13_manual_exposure_2x_pre05` | `16` | Clear `AutoExposure` (`create_flags = 0x0B`), bind `Exposure = 2.0f` (`1×1 R32F`), `Pre.Exposure = 0.5f`, `Exposure.Scale = 1.5f` | `P00` |
| `Probes` | `P14_input_subrect_base_32_24` | `16` | `1280×720` shifted to `Subrect.Base = (32, 24)` inside `1344×768` textures (`CreateFeature` still `(1280, 720)`), surrounded by `(64, 0, 64)` magenta sentinel | **`P00` (equiv)** |
| `Probes` | `P15_mv_scale_neg_half` | `16` | Stored `mv` & `smv` multiplied by `-2.0` + `MV.Scale.X/Y = -0.5f` (`stored * scale == P05`) | **`P05` (equiv)** |
| `Probes` | `P16a_mv_jittered_raw` | `16` | `create_flags |= MVJittered (0x04)` (`0x4F`) with unjittered MVs (`delta_sign = 0`) | `P00` |
| `Probes` | `P16b_mv_jittered_prev_minus_curr` | `16` | `create_flags |= MVJittered` with `mv += (jitter_prev - jitter_curr)` (`delta_sign = +1`, tests which sign cancels internal unjittering vs `P00`) | **`P00` (equiv)** |
| `Probes` | `P16c_mv_jittered_curr_minus_prev` | `16` | `create_flags |= MVJittered` with `mv += (jitter_curr - jitter_prev)` (`delta_sign = -1`, opposite sign control) | **`P00` (equiv)** |
| `Probes` | `P17_roughness_unpacked_r16f` | `16` | `DLSS.Roughness.Mode = 0 (Unpacked)` + dedicated `GBuffer.Roughness` (`R16F`) while `Normals.w = 0.9375f` poison value | **`P00` (equiv)** |

### 2.3 Pre-Flight Review Checklist & Run Instructions (RTX 5090)

Before or during execution on the RTX 5090 machine, reviewers/agents (`Carbon` + `6astra`) are explicitly invited to inspect `v2-motion` (`include/rr_ngx_abi.hpp`, `include/rr_ref_scenes.hpp`, `src/ngx_rr_bench.cpp`, `run_suite_v2_motion.ps1`), specifically checking:
1. **`P14` (`Subrect.Base = (32, 24)`)**: Whether every NGX `310.9.1.0` input `Subrect.Base.X/Y` parameter string is covered (`Color`, `DiffuseAlbedo`, `SpecularAlbedo`, `Normals`, `Roughness`, `MotionVectors`, `Depth`, `GBuffer.SpecularMvec`, `DLSSD.SpecularHitDistance`, etc.) so none of the magenta `(64, 0, 64)` border leaks into `P14 vs P00`.
2. **`P13` (Manual `DLSS.Exposure`) & `P17` (`Roughness.Mode = Unpacked`)**: Whether `NVSDK_NGX_D3D12_EvaluateFeature` expects any additional parameter key or resource state on `310.9.1.0`.
3. **Authorization to Patch Locally if Needed**: If your code audit or smoke/probe run uncovers any bug, missing NGX parameter key, or driver quirk, **please feel free to fix it directly on the 5090 machine**, re-dump `cases/cases_v2_motion.json` (`.\bin\ngx_rr_bench.exe --suite v2_motion --dump-fixture-hashes cases\cases_v2_motion.json`) if canonical inputs changed, and run once (`run_suite_v2_motion.ps1` automatically saves `00_meta/git_diff.patch` for 1:1 AMD replay).

#### Build & Run Commands
```powershell
git fetch origin
git checkout v2-motion
.\build.ps1
.\run_suite_v2_motion.ps1 -DlssdPath "C:\path\to\nvngx_dlssd.dll" -OutDir results_v2_motion -ProcessExitTeardown
```
Expected runtime: **`~10–15 seconds`**, raw output size **`~420 MB`** (`~180–220 MB` compressed `.7z` / `.tar.zst`), with instant pass/equiv readout in `results_v2_motion\05_probes\probes_summary.json`.

---

## 3. Task B — Architecture & Roadmap Review (`OpenRR-ASI@codex/mesa-263-comparison`)

For anyone interested in cross-reviewing our AMD RDNA4 (`gfx1201`, RX 9070 XT) Vulkan implementation and compiler/ISA findings, our latest code and reports are pushed at:
- **Branch**: [`https://github.com/TheAutomatic/OpenRR-ASI/tree/codex/mesa-263-comparison`](https://github.com/TheAutomatic/OpenRR-ASI/tree/codex/mesa-263-comparison) (`4510cc1`)
- **Overnight Report (`2.0A` fix + `2.0B` occupancy diagnosis)**: [`docs/test-evidence/v015-stage2-overnight-results.md`](https://github.com/TheAutomatic/OpenRR-ASI/blob/codex/mesa-263-comparison/docs/test-evidence/v015-stage2-overnight-results.md)
- **Daytime Report + `§6 Post-Review Errata`**: [`docs/test-evidence/v015-stage2-day-results.md`](https://github.com/TheAutomatic/OpenRR-ASI/blob/codex/mesa-263-comparison/docs/test-evidence/v015-stage2-day-results.md)

### 3.1 Current RDNA4 Status Summary
1. **Stage 2.0A Closed (`guides=1` compiler divergence fixed + IEEE guards)**:
   - Replaced native SFU `log2`/`exp2` in `enc0_attr.comp` (`signed_log`, `signed_exp`) with degree-5 minimax / Cody-Waite polynomial helpers (`attr_log2`, `attr_exp2`) + exact IEEE-754 `NaN`/`±0`/`±Inf`/subnormal guards (`703271f`).
   - `guides=1` ACO-vs-LLPC parity jumped from **`32.74 dB` -> `126.76 dB`** (`guides=0`: `59.52 dB`, Legacy FP16 path: `0 ULP`), zero GPU time cost.
2. **Stage 2.0B Compiler Gap (`Fast-ACO` vs `Fast-LLPC` on custom Mesa `26.3` RADV)**:
   - On custom Mesa `26.3` (`VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8`), `Fast-ACO` runs `1080p Quality` in **`3.357 ms`** (`Fast-LLPC`: `3.008 ms`, `+11.6%`) and `4K Quality` in **`12.525 ms`** (`Fast-LLPC`: `11.378 ms`, `+10.1%`).
   - Loop-unroll suppression (`[[dont_unroll]]` on `hd` and `j` loops) was experimentally **falsified**: VGPRs stayed pinned at `256` on all 10 `128ch`/`160ch` (`TR=1`) kernels while dynamic loop overhead degraded runtime by `+1.3%..+2.5%`.
   - Crucially, `enc3_b0..b4` (`C=96`, `TR=2`, using `W2_DEFER` deferred `c0..c7` accumulation) **already match LLPC (`0.98×–1.01×`, `184–192 VGPR`)**, whereas all 10 `TR=1` (`128ch`/`160ch`) kernels in `gpu/shaders/onr_block_fast.glsl` hit the **`256 VGPR` wall (`5 waves` vs LLPC `216 VGPR / 7 waves`)** and emit `+148 s_wait_alu` / `+129 ds_load_b64` / `+113 v_perm_b32` per kernel.

### 3.2 Four Concrete Questions for External Review (`6astra` / `Carbon` / Graphics & Compiler Reviewers)

*(Note: Please read `§6 Post-Review Errata` of `v015-stage2-day-results.md` first — three daytime claims about "`cmat-address` scope", "VK coopmat disallowing mixed precision", and "`shared` LDS layout" were already corrected against our own Mesa/GLSL source.)*

1. **Breaking the `256 VGPR` Ceiling in `TR=1` (`128ch`/`160ch`) `onr_block_fast.glsl`**:
   - In `TR=2` (`W2_DEFER`), `c0..c7` `coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>` fragments stay live across `hd` without writing back to `out[CO]` until the end of the spatial `k2` pass (`184–192 VGPR`). In `TR=1` (`C=128`, lines `258–288`), `acc` is materialized into `out[CO]` (`8` `f16mat`s) **simultaneously** while `xp[CI]` (`8` `f16mat`s) and prefetched `B` tiles are live (`256 VGPR`).
   - Beyond porting `W2_DEFER`-style deferred `out` accumulation / splitting `CI` staging in `TR=1`, do you see any better register-pressure scheduling pattern for RDNA4 `v_wmma_f16_16x16x16_f16` / `v_wmma_f32_16x16x16_fp8_fp8` under ACO?
2. **RADV NIR Lowering (`radv_nir_lower_cooperative_matrix.c`) vs ACO Scheduling**:
   - Our Mesa patch `0002-radv-bound-cmat-address.patch` only touched `OpCooperativeMatrixLoadKHR` pointer `uadd_carry`; the remaining `+129 ds_load_b64` and `+113 v_perm_b32` inflation in ACO vs LLPC appears to originate in `radv_nir_lower_cooperative_matrix.c` (`ROW_MAJOR` global weight loads & `shared` `uint64_t` LDS loads). Is fixing transpose/swizzle lowering in `radv_nir_lower_cooperative_matrix.c` the highest-leverage compiler-side win?
3. **Zero-LDS-Growth FP8 (`floate4m3_t`) Activation Path for `2B` (`ENC_MERGE`) & `2A-2` (`C>=96 DEC_UP`)**:
   - `onr_stage_prelude.glsl:97–108` (`qv_bytes()`) already decodes NVIDIA's `sm_89` E4M3 weights (`0x7f` finite `448` -> OCP `0x7e` `240` + exact residual scale) and already drives `ONR_WMMA_FP8` (`coopmat<floate4m3_t, ..., 16, 32, gl_MatrixUseA/B>` -> `float` accumulator) in `onr_block_fast.glsl`!
   - For `ENC_MERGE` (`gpu/shaders/enc_merge.comp`, currently near RDNA4's `32 KiB` LDS cap at `30,976 B` for `C=160`) and `DEC_UP`, declaring a block-typed `GL_EXT_shared_memory_block` alias (`shared LdsE { floate4m3_t lds8[2u * LDS_A]; };` or halving the activation staging window in FP8) keeps LDS `<= 30,976 B` and unlocks `16×16×32` FP8 WMMA (`2×` K-throughput over FP16 WMMA) without reverse-engineering private RADV register-lane layouts. Any hazards (`barriers`, dynamic per-workgroup max-abs scaling vs fixed pre-scale for activations up to `~16×` HDR) we should watch out for?
4. **OpenRR Engine/ASI Parity Follow-Ups Informed by `v2_motion`**:
   - Once `v2_motion` locks `P13` (`Manual Exposure`), `P14` (`Subrect.Base`), `P15` (`MV.Scale`), `P16a/b/c` (`MVJittered` jitter delta sign — `OpenRR/gpu/src/engine.cpp:116` currently hardcodes `C(0x328)=0, C(0x32c)=0`), and `P17` (`Roughness Unpacked`), plus `S2` moving `16×` HDR highlights for Stage 3 `hist_` (`RGBA16F 8B -> 4B` packed float), do you spot any remaining NGX `310.9.1.0` contract gap that OpenRR should close?
