# RR_Contrast

Standalone D3D12 reference benchmark, output-capture, and capability-probe harness for official NVIDIA DLSS Ray Reconstruction (`nvngx_dlssd.dll` `310.9.1.0`, Preset F / `rr2`).

Built for 1:1 cross-vendor image quality, GPU timing, VRAM, and optional-guide parity evaluation against AMD RDNA4 implementations under **bit-identical canonical inputs** (`fixture_version = "rr_contrast_v1"`).

## Quick Start (Windows x64 + RTX 5090)

### 1. Prerequisites
- Windows 11 x64 (HAGS state recorded automatically; default enabled recommended)
- NVIDIA RTX 5090 with current Game Ready / Studio driver
- Visual Studio 2022 / 2026 C++ Build Tools (`cl.exe` x64)
- Official `nvngx_dlssd.dll` version **`310.9.1.0`**:
  - Expected file size: `48,343,664` bytes
  - Expected whole-file SHA-256: `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80`
  - *(Do **not** commit `nvngx_dlssd.dll` into git; pass its local path via `-DlssdPath`.)*

### 2. Build & Verify Input Fixtures (CPU-only, ~5s)
```powershell
.\build.ps1
```
`build.ps1` compiles `bin\ngx_rr_bench.exe` with zero external SDK dependencies, runs IEEE-754 RNE `FloatToHalfRNE` + FIPS 180-4 SHA-256 self-tests, and verifies all 29 canonical suite cases against the pre-committed AMD-side input hashes in `cases\cases.json`.

### 3. Run Smoke Test First (~5s)
```powershell
.\bin\ngx_rr_bench.exe --smoke --dlssd "C:\path\to\nvngx_dlssd.dll" --out results\01_smoke
```
Confirms:
1. `_nvngx.dll` initializes `NVSDK_NGX_Feature_RayReconstruction` (`13`) on the RTX 5090.
2. The `nvngx_dlssd.dll` actually loaded in-process matches the requested `310.9.1.0` SHA-256 (detecting any driver/NVIDIA App OTA override).
3. `EvaluateFeature` produces finite, non-blank `RGBA16F` output and active temporal history (`f015_normal != f015_forced_reset`).

### 4. Run Full Suite (~5–8 min, ~3.7 GB raw / ~2.0 GB compressed)
```powershell
.\run_suite.ps1 -DlssdPath "C:\path\to\nvngx_dlssd.dll" -OutDir results -RunNsight
```
*(Omit `-RunNsight` if running Nsight Systems / Compute manually; profiler capture is strictly non-blocking.)*

### 5. Package Deliverable Archive
Compress `results\` with `zstd` or `7-Zip` (`results_rtx5090_rr310910.7z` or `.tar.zst`, ~1.8–2.2 GB) and share via cloud drive together with any local `git diff` if adjustments were made.

See [`docs/SPEC.md`](docs/SPEC.md) for full protocol details, parameter/format tables, DRS & probe semantics, and Nsight instructions.
