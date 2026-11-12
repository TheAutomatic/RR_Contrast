# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DlssdPath,
    [string]$OutDir = "results_v2_motion",
    [string]$NgxLoaderPath = "",
    [switch]$AllowDllMismatch,
    [switch]$ProcessExitTeardown,
    [switch]$SkipExt,
    [switch]$SkipProbes
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExePath = Join-Path $RepoRoot "bin\ngx_rr_bench.exe"
$CasesV2Json = Join-Path $RepoRoot "cases\cases_v2_motion.json"

if (-not (Test-Path $ExePath)) {
    & (Join-Path $RepoRoot "build.ps1")
}

$AbsDlssd = (Resolve-Path $DlssdPath).Path
$AbsOut = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir))
New-Item -ItemType Directory -Force -Path $AbsOut | Out-Null

Write-Host "=== [0/4] Verifying v2_motion canonical input SHA-256 fixtures ===" -ForegroundColor Cyan
& $ExePath --suite v2_motion --verify-fixtures $CasesV2Json
if ($LASTEXITCODE -ne 0) { throw "v2_motion canonical input fixture verification failed. Stop before GPU run." }

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

$CommonArgs = @("--suite", "v2_motion", "--dlssd", $AbsDlssd)
if ($ProcessExitTeardown) { $CommonArgs += "--process-exit-teardown" }
if ($NgxLoaderPath -ne "") {
    $CommonArgs += @("--ngx-loader", (Resolve-Path $NgxLoaderPath).Path)
}
if ($AllowDllMismatch) {
    $CommonArgs += "--allow-dll-mismatch"
}

Write-Host "=== [1/4] Stage 1: v2_motion Smoke Test & DLL Identity Check ===" -ForegroundColor Cyan
$SmokeArgs = @("--smoke") + $CommonArgs + @("--out", (Join-Path $AbsOut "01_smoke"))
& $ExePath @SmokeArgs
if ($LASTEXITCODE -ne 0) { throw "Stage 1 Smoke test failed. Inspect 01_smoke/env_manifest.json before continuing." }

if (-not $SkipExt) {
    Write-Host "=== [2/4] Stage 2: v2_motion Quality Extended (S1 Halton Temporal + S2 HDR Stress, active pixel MVs & guides) ===" -ForegroundColor Cyan
    & $ExePath --pass quality-ext @CommonArgs --out (Join-Path $AbsOut "04_quality_ext")
    if ($LASTEXITCODE -ne 0) { throw "Stage 2 Quality Extended (v2_motion) failed." }
}

if (-not $SkipProbes) {
    Write-Host "=== [3/4] Stage 3: v2_motion Single-Variable Guide & ABI Micro-Probes (P00..P17) ===" -ForegroundColor Cyan
    & $ExePath --pass probes @CommonArgs --out (Join-Path $AbsOut "05_probes")
    if ($LASTEXITCODE -ne 0) { throw "Stage 3 Probes (v2_motion) failed." }
}

Write-Host "=== [4/4] Generating SHA256SUMS.txt ===" -ForegroundColor Cyan
$SumLines = Get-ChildItem -Path $AbsOut -File -Recurse | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($AbsOut.Length).TrimStart('\', '/').Replace('\', '/')
    $h = (Get-FileHash -Algorithm SHA256 -Path $_.FullName).Hash.ToLowerInvariant()
    "$h  $rel"
}
$SumLines | Out-File -Encoding ascii (Join-Path $AbsOut "SHA256SUMS.txt")
Write-Host "[COMPLETE] All v2_motion passes finished in $AbsOut" -ForegroundColor Green
