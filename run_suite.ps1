# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DlssdPath,
    [string]$OutDir = "results",
    [string]$NgxLoaderPath = "",
    [switch]$AllowDllMismatch,
    [switch]$ProcessExitTeardown,
    [switch]$SkipExt,
    [switch]$SkipProbes,
    [switch]$RunNsight
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExePath = Join-Path $RepoRoot "bin\ngx_rr_bench.exe"
$CasesJson = Join-Path $RepoRoot "cases\cases.json"

if (-not (Test-Path $ExePath)) {
    & (Join-Path $RepoRoot "build.ps1")
}

$AbsDlssd = (Resolve-Path $DlssdPath).Path
$AbsOut = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir))
New-Item -ItemType Directory -Force -Path $AbsOut | Out-Null

Write-Host "=== [0/6] Verifying canonical input SHA-256 fixtures ===" -ForegroundColor Cyan
& $ExePath --verify-fixtures $CasesJson
if ($LASTEXITCODE -ne 0) { throw "Canonical input fixture verification failed. Stop before GPU run." }

# Record git commit & clean/patched state (including staged & untracked files) for 1:1 reproducibility
$MetaDir = Join-Path $AbsOut "00_meta"
New-Item -ItemType Directory -Force -Path $MetaDir | Out-Null
if (Get-Command git -ErrorAction SilentlyContinue) {
    git -C $RepoRoot rev-parse HEAD 2>$null | Out-File -Encoding utf8 (Join-Path $MetaDir "git_head.txt")
    git -C $RepoRoot log -1 --stat 2>$null | Out-File -Encoding utf8 (Join-Path $MetaDir "git_log_1.txt")
    git -C $RepoRoot status --porcelain=v1 2>$null | Out-File -Encoding utf8 (Join-Path $MetaDir "git_status.txt")
    git -C $RepoRoot diff HEAD 2>$null | Out-File -Encoding utf8 (Join-Path $MetaDir "git_diff.patch")
    $Untracked = git -C $RepoRoot ls-files --others --exclude-standard 2>$null
    if ($Untracked) {
        $UntrackedRoot = Join-Path $MetaDir "untracked_files"
        foreach ($relPath in $Untracked) {
            $srcFile = Join-Path $RepoRoot $relPath
            if (Test-Path -LiteralPath $srcFile -PathType Leaf) {
                $dstFile = Join-Path $UntrackedRoot $relPath
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dstFile) | Out-Null
                Copy-Item -LiteralPath $srcFile -Destination $dstFile -Force
            }
        }
    }
}
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    nvidia-smi -q | Out-File -Encoding utf8 (Join-Path $MetaDir "nvidia_smi_q.txt")
    nvidia-smi --query-gpu=timestamp,name,driver_version,pstate,clocks.gr,clocks.mem,power.draw,temperature.gpu,memory.used,memory.total --format=csv |
        Out-File -Encoding utf8 (Join-Path $MetaDir "nvidia_smi_snapshot.csv")
}

$CommonArgs = @("--dlssd", $AbsDlssd)
if ($ProcessExitTeardown) { $CommonArgs += "--process-exit-teardown" }
if ($NgxLoaderPath -ne "") {
    $CommonArgs += @("--ngx-loader", (Resolve-Path $NgxLoaderPath).Path)
}
if ($AllowDllMismatch) {
    $CommonArgs += "--allow-dll-mismatch"
}

Write-Host "=== [1/6] Stage 1: Smoke Test & DLL Identity Check ===" -ForegroundColor Cyan
$SmokeArgs = @("--smoke") + $CommonArgs + @("--out", (Join-Path $AbsOut "01_smoke"))
& $ExePath @SmokeArgs
if ($LASTEXITCODE -ne 0) { throw "Stage 1 Smoke test failed. Inspect 01_smoke/env_manifest.json before continuing." }

Write-Host "=== [2/6] Stage 2: Quality Core (S0 / g0_core7, key-frame readback) ===" -ForegroundColor Cyan
& $ExePath --pass quality-core @CommonArgs --out (Join-Path $AbsOut "02_quality_core")
if ($LASTEXITCODE -ne 0) { throw "Stage 2 Quality Core failed." }

Write-Host "=== [3/6] Stage 3: Timing Core (S0 / g0_core7, separate clean process, ZERO readback) ===" -ForegroundColor Cyan
$TimingDir = Join-Path $AbsOut "03_timing_core"
New-Item -ItemType Directory -Force -Path $TimingDir | Out-Null
$SmiProc = $null
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    $TelCsv = Join-Path $TimingDir "gpu_clock_power_telemetry.csv"
    $SmiProc = Start-Process -FilePath "nvidia-smi" -ArgumentList @(
        "--query-gpu=timestamp,pstate,clocks.current.graphics,clocks.current.memory,power.draw,temperature.gpu,utilization.gpu",
        "--format=csv", "-lms", "250"
    ) -RedirectStandardOutput $TelCsv -NoNewWindow -PassThru
}
try {
    & $ExePath --pass timing-core @CommonArgs --out $TimingDir
    if ($LASTEXITCODE -ne 0) { throw "Stage 3 Timing Core failed." }
} finally {
    if ($SmiProc -and -not $SmiProc.HasExited) {
        Stop-Process -Id $SmiProc.Id -Force -ErrorAction SilentlyContinue
    }
}

if (-not $SkipExt) {
    Write-Host "=== [4/6] Stage 4: Quality Extended (S1 Halton Temporal + S2 HDR Stress) ===" -ForegroundColor Cyan
    & $ExePath --pass quality-ext @CommonArgs --out (Join-Path $AbsOut "04_quality_ext")
    if ($LASTEXITCODE -ne 0) { throw "Stage 4 Quality Extended failed." }
}

if (-not $SkipProbes) {
    Write-Host "=== [5/6] Stage 5: Non-blocking Single-Variable Capability & Guide Probes ===" -ForegroundColor Cyan
    & $ExePath --pass probes @CommonArgs --out (Join-Path $AbsOut "05_probes")
    if ($LASTEXITCODE -ne 0) { throw "Stage 5 Probes failed." }
}

if ($RunNsight) {
    Write-Host "=== [6/6] Stage 6: Best-effort Nsight Systems Trace ===" -ForegroundColor Cyan
    $NsysDir = Join-Path $AbsOut "06_nsight"
    New-Item -ItemType Directory -Force -Path $NsysDir | Out-Null
    $nsysCmd = Get-Command nsys.exe -ErrorAction SilentlyContinue
    if (-not $nsysCmd) {
        $nsysFound = Get-ChildItem -Path "${env:ProgramFiles}\NVIDIA Corporation" -Filter "nsys.exe" -Recurse -ErrorAction SilentlyContinue |
                     Sort-Object FullName -Descending | Select-Object -First 1
        if ($nsysFound) { $nsysCmd = @{ Source = $nsysFound.FullName } }
    }
    if ($nsysCmd) {
        foreach ($tgt in @("1080p_qual", "4k_qual")) {
            $repBase = Join-Path $NsysDir "nsys_$tgt"
            & $nsysCmd.Source profile --trace=dx12,cuda,nvtx --sample=none --cpuctxsw=none --force-overwrite=true `
                -o $repBase -- $ExePath --profile-target $tgt @CommonArgs --out $NsysDir
            if ($LASTEXITCODE -eq 0 -and (Test-Path "$repBase.nsys-rep")) {
                & $nsysCmd.Source stats --report cuda_gpu_kern_sum,dx12_gpu_marker_sum --format csv `
                    --output "$repBase" "$repBase.nsys-rep" 2>$null
            } else {
                Write-Warning "nsys profile returned non-zero for $tgt (continuing — profiler capture is non-blocking)."
            }
        }
    } else {
        Write-Warning "nsys.exe not found; skipping automated Nsight Systems capture (see docs/SPEC.md for manual Nsight steps)."
    }
}

Write-Host "=== Generating SHA256SUMS.txt ===" -ForegroundColor Cyan
$SumLines = Get-ChildItem -Path $AbsOut -File -Recurse | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($AbsOut.Length).TrimStart('\', '/').Replace('\', '/')
    $h = (Get-FileHash -Algorithm SHA256 -Path $_.FullName).Hash.ToLowerInvariant()
    "$h  $rel"
}
$SumLines | Out-File -Encoding ascii (Join-Path $AbsOut "SHA256SUMS.txt")
Write-Host "[COMPLETE] All requested passes finished in $AbsOut" -ForegroundColor Green
