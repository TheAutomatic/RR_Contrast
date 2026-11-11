# RR_Contrast Technical Specification (`rr_contrast_v1`)

> **Engineering Note**: This harness and test protocol are designed to establish a clean, reproducible RTX 5090 baseline for `nvngx_dlssd.dll (310.9.1.0)`. Improvements or adjustments to the runner/scripts based on real NGX / Nsight behavior on Blackwell are welcome as long as core parity principles hold — once finalized, please run the locked configuration verbatim and include any `git diff` in `00_meta/` so the AMD side can replay the exact same inputs and conventions 1:1.

---

## 1. Input Determinism & Cross-Compiler Bit-Identity

A recurring pitfall in cross-machine GPU parity tests is subtle CPU-side input drift across compilers (`MSVC /O2` vs `GCC/Clang -O2` FMA contraction, `sqrtf`/`sinf` ULP differences, or half-float rounding bugs).

`include/rr_ref_scenes.hpp` (`fixture_version = "rr_contrast_v1"`) eliminates CPU drift by construction:
1. **Pure Integer / Q20 Fixed-Point Synthesis**: All procedural UV coordinates, sphere intersections (`Isqrt64`), shading, Monte-Carlo noise (`PcgHash`), motion vectors, and 16-phase Halton `(2, 3)` jitters are computed in exact 64-bit integer arithmetic (`|q| <= 2^24`) and scaled by the exact binary32 power-of-two `2^-20` (`0x35800000`).
2. **Exact IEEE-754 RNE `FloatBitsToHalfRNE`**: Fixes the mantissa-overflow carry bug (`FloatToHalfRNE(1.9999f) == 0x4000`, verified at startup by `RunSelfTests()`).
3. **Canonical Frame Input SHA-256 (`cases/cases.json`)**: Hashes `fixture_version`, frame index, `(rw, rh, out_w, out_h)`, `Reset`, `preset_hint`, `perf_quality`, `create_flags`, `guide_mask`, jitters, MV scales, `PreExposure`, `ExposureScale`, `world_to_view[16]`, `view_to_clip[16]`, and the tight active subrect bytes (`rw * bpp` per row) of every bound texture.
   - Verified byte-for-byte identical between `MSVC cl.exe /O2` and `x86_64-w64-mingw32-g++ -O2`.

---

## 2. NGX ABI, DLL Identity & Preset Routing

### 2.1 Clean-Room Header vs Official NGX SDK
- By default, `src/ngx_rr_bench.cpp` compiles standalone using `include/rr_ngx_abi.hpp` (parameter keys, enums, and defaults verified 1:1 against public [`NVIDIA/DLSS v310.9.1`](https://github.com/NVIDIA/DLSS/tree/v310.9.1/include) `nvsdk_ngx_defs.h`, `nvsdk_ngx_defs_dlssd.h`, and `nvsdk_ngx_helpers_dlssd_d3d.h`) and dynamically loads the driver's `_nvngx.dll` via `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore\NGXPath` (or DriverStore fallback / `--ngx-loader`).
- **Optional Static SDK Build**: If preferred, clone `NVIDIA/DLSS` (`v310.9.1`) locally and build with:
  ```powershell
  .\build.ps1 -NgxSdkDir "C:\path\to\DLSS"
  ```
  which defines `RR_USE_OFFICIAL_NGX_SDK=1` and links `nvsdk_ngx_s.lib`.

### 2.2 Whole-File DLL Hash vs Internal Slice Hash
`01_smoke/env_manifest.json` records **both** expected and actual in-process module metadata via `GetModuleHandleW(L"nvngx_dlssd.dll")`:
- **Whole PE file (`nvngx_dlssd.dll` `310.9.1.0`, `48,343,664` B)** SHA-256:
  `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80`
- *(Note for OpenRR cross-reference: `c7a2361f990391f3589601619d77f15ce92d46ad2a459235f9cfca5913fad05a` in OpenRR's `dlssd_layout_310_9_1_0.inc` is the hash of extracted `rr2` weight/immediate/LUT slices only, **not** the whole DLL file.)*
- If NVIDIA App / driver DRS override redirects `nvngx_dlssd.dll` to another file, `--smoke` catches the SHA-256 mismatch immediately.

### 2.3 Create & Evaluate Parameters
- `DLSS.Feature.Create.Flags = 0x4B` (`IsHDR | MVLowRes | DepthInverted | AutoExposure`)
- `DLSS.Denoise.Mode = 1` (`DLUnified`), `DLSS.Roughness.Mode = 1` (`Packed` in `Normals.w`), `DLSS.Use.HW.Depth = 1`
- `RayReconstruction.Hint.Render.Preset.{DLAA,UltraQuality,Quality,Balanced,Performance,UltraPerformance} = 6` (`Preset_F`, default `rr2` model in `v310.9.1`; `Preset_E = 5` and `Default = 0` tested in `05_probes`)
- `DLSS.Pre.Exposure = 1.0f`, `DLSS.Exposure.Scale = 1.0f`, `MV.Scale.X = 1.0f`, `MV.Scale.Y = 1.0f`
- Actual internal model routing is recorded honestly as `requested_preset_hint = 6` in `env_manifest.json`; CUDA kernel symbol evidence (e.g. `rr2_*`) is deferred to Nsight Systems if observable.

---

## 3. Scenes, DRS Semantics & Single-Variable Probes

### 3.1 Three Synthetic Scenes
| Scene | Purpose | DRS Behavior | Jitter & Motion |
|---|---|---|---|
| `s0_bench_parity` | 1:1 baseline anchor matching `openrr_bench` | Static `max_rw × max_rh` texture cropped at top-left `(0, 0 .. rw, rh)` | Constant jitter `(0.125, -0.25)`, constant `mv = (0.25, -0.15)` |
| `s1_temporal_halton` | Moving glossy sphere + occlusion/disocclusion edge + subpixel slanted bars + 1-spp MC noise | Re-synthesized at active `(rw, rh)` with scaled pixel velocity | 16-phase Halton `(2, 3)` in `[-0.5, +0.5]`, exact `current -> previous` pixel MVs |
| `s2_hdr_stress` | Linear HDR specular/emissive peaks up to `~16x` + `0.04` mirror floor + `0.017` dark shadow tiles | Steady resolution | 16-phase Halton `(2, 3)` |

### 3.2 DRS Schedule in Quality Passes (`80` frames)
- Frames `0..63`: Steady `(steady_rw, steady_rh)`, `Reset = 1` on `f000`, `Reset = 0` on `f001..f063`.
- Frames `64..71`: Alternates `85%` (`64..67`) and `100%` (`68..71`) **with `Reset = 0`** to test whether direct NGX `310.9.1.0` accepts dynamic subrect changes without reset. If NGX returns an error code, `ngx_result_hex` is logged and the runner falls back to feature recreate (`recreated = 1`) without aborting.
- Frames `72..79`: Alternates `85%` (`72..75`) and `100%` (`76..79`) **with `Reset = 1` on switch frames (`72`, `76`)**, matching `openrr_bench` and Streamline's resize+reset guidance.
- Raw pitch-stripped `.rgba16f` (`out_w * out_h * 8` bytes, little-endian `half4`) is saved only for key frames (`K10`: `0, 1, 2, 4, 8, 15, 31, 63, 67, 75`), while `frames_manifest.csv` logs `input_sha256`, `output_sha256`, `output_fnv1a64`, `non_finite`, and luminance stats for **every** frame.

### 3.3 Non-Blocking Single-Variable Probes (`05_probes`)
Each probe runs `16` frames (`0..15`) on `S1` (`1080p Quality`) changing **one** variable relative to `P00_baseline_s1_g0`, catching any `NVSDK_NGX_FAILED` code into `probes_summary.json` (`supported_active`, `supported_noop`, or `unsupported`):
- `P01_guide_disocc`: `DLSS.DisocclusionMask` (`R16_FLOAT`)
- `P02_guide_transp_rgba`: `DLSS.TransparencyLayer` (`RGBA16_FLOAT`) + `DLSS.TransparencyLayerOpacity` (`RGBA16_FLOAT`)
- `P03_guide_transp_tlmv`: `P02` + `DLSS.TransparencyLayerMvecs` (`RG16_FLOAT`, `out_w × out_h`)
- `P04_guide_sss_cbt`: `DLSSD.ScreenSpaceSubsurfaceScatteringGuide` (`R16_FLOAT`) + `DLSSD.ColorBeforeTransparency` (`RGBA16_FLOAT`)
- `P05_guide_spec_mvec`: `GBuffer.SpecularMvec` (`RG16_FLOAT`)
- `P06_guide_resp_r16f`: `DLSSD.ResponsivityMask` (`R16_FLOAT`, matching `v310.9.1` header requirement `R16F or R8 SNORM`)
- `P07_guide_dof_presetF`: `DLSSD.DepthOfFieldGuide` (`R16_FLOAT`) with `Preset = 6 (F)`
- `P08_guide_dof_presetE`: `DLSSD.DepthOfFieldGuide` (`R16_FLOAT`) with `Preset = 5 (E)` (per SDK header note: *"must use if DoF guide is needed"*)
- `P09_no_hit_with_spec_mv` & `P10_no_hit_plain`: Omit `DLSSD.SpecularHitDistance` with/without `SpecularMvec`
- `P11_odd_subrect_1281x721`: Odd render subrect `1281×721` inside `1288×728` allocation
- `P12_preset_default_hint0`: `Preset = 0 (Default)` vs `Preset = 6 (F)`

---

## 4. Clean GPU Timing & VRAM Protocol (`03_timing_core`)

1. **Process & Readback Isolation**: `03_timing_core` runs in a dedicated process with **zero `READBACK` heap copies** of output textures and uses static `S0 / g0_core7` so input uploads never occur during timed loops.
2. **Timestamp Scope**: Single `D3D12_COMMAND_LIST_TYPE_DIRECT` queue; `EndQuery(TIMESTAMP, 0)` immediately before `EvaluateFeature` and `EndQuery(TIMESTAMP, 1)` + `ResolveQueryData` immediately after in the same command list, followed by `ExecuteCommandLists` + fence wait (matching `openrr_bench`).
3. **Warmup & Rounds**: 3.0s GPU pre-soak -> `120` warmup frames -> `240` steady frames -> `64` DRS frames (`r1`, `r2` for `1080p Q`, `1440p Q`, `4K Q`, `4K Perf`; `r1` for `1080p DLAA`, `4K UltraPerf`), accompanied by background `nvidia-smi` clock/power/temp CSV logging. *(Locked-clock runs via `nvidia-smi -lgc` are optional, not required.)*
4. **VRAM Metrics**: Logs both:
   - **DXGI Process-Local `CurrentUsage`** (`IDXGIAdapter3::QueryVideoMemoryInfo` on `DXGI_MEMORY_SEGMENT_GROUP_LOCAL`) at `before_create`, `after_create`, `after_first_eval`, `steady_end`, `drs_end`.
   - **NGX Reported Feature Stats** (`DLSSDGetStatsCallback -> SizeInBytes` and `GetScratchBufferSize`).

---

## 5. Tiered Best-Effort Nsight Profiling (`06_nsight`)

Because NGX D3D12 dispatches internal CUDA/compute workloads through driver interop, profiler visibility depends on driver/tool version and admin permissions. Profiling is **strictly tiered and non-blocking**:

1. **Tier 1 — Nsight Systems Timeline (Primary)**:
   `run_suite.ps1 -RunNsight` (or manual CLI) runs `--profile-target 1080p_qual` and `4k_qual` (`40` frames, frame `32` bracketed by D3D12 marker `"RR_Contrast_Profile_Target_Evaluate"` and best-effort `cuProfilerStart/Stop`):
   ```powershell
   nsys profile --trace=dx12,cuda,nvtx --sample=none --cpuctxsw=none -o results\06_nsight\nsys_4k_qual -- .\bin\ngx_rr_bench.exe --profile-target 4k_qual --dlssd "C:\path\to\nvngx_dlssd.dll" --out results\06_nsight
   ```
   - Goal: Observe total per-frame GPU timeline and, if CUDA trace is visible, export per-kernel GPU durations (`cuda_gpu_kern_sum` CSV + `.nsys-rep`).
2. **Tier 2 — Nsight Compute / Nsight Graphics (Optional Follow-up Only If Visible)**:
   - **If** Tier 1 exposes individual CUDA kernels (e.g., top 3–5 hotspots in encoder/decoder/post/downsample), an optional targeted `ncu` summary capture on `--profile-target 1080p_qual` (with `--launch-skip` / `--kregex` on 2–3 representative kernels, default/light section set, **not** `--set full` across all kernels) is super helpful for Tensor/FP8 utilization insight.
   - **If** NGX hides CUDA symbols behind D3D12 metacommands/driver dispatches, **do not spend time fighting `ncu`** — either capture one Nsight Graphics GPU Trace frame if convenient, or simply deliver the Tier 1 `.nsys-rep` and note `cuda_kernels_not_exposed`.
