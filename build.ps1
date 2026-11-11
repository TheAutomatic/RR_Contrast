# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [ValidateSet("auto", "msvc", "mingw")]
    [string]$Compiler = "auto",
    [string]$NgxSdkDir = "",
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BinDir = Join-Path $RepoRoot "bin"
$ObjDir = Join-Path $RepoRoot "obj"
New-Item -ItemType Directory -Force -Path $BinDir, $ObjDir | Out-Null

$OutExe = Join-Path $BinDir "ngx_rr_bench.exe"
$SrcFile = Join-Path $RepoRoot "src\ngx_rr_bench.cpp"
$IncDir = Join-Path $RepoRoot "include"

function Resolve-VsBuildCmd([string]$instRoot) {
    if (-not $instRoot -or -not (Test-Path $instRoot)) { return $null }
    $msvcRoot = Join-Path $instRoot "VC\Tools\MSVC"
    if (-not (Test-Path $msvcRoot)) { return $null }
    $verDir = Get-ChildItem -Path $msvcRoot -Directory -ErrorAction SilentlyContinue |
              Where-Object { Test-Path (Join-Path $_.FullName "bin\Hostx64\x64\cl.exe") } |
              Sort-Object Name -Descending | Select-Object -First 1
    if (-not $verDir) { return $null }
    $batAll = Join-Path $instRoot "VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $batAll) { return "`"$batAll`" x64 -vcvars_ver=$($verDir.Name)" }
    $bat64 = Join-Path $instRoot "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $bat64) { return "`"$bat64`" -vcvars_ver=$($verDir.Name)" }
    return $null
}

function Find-VsDevBatch {
    $vswhereCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    foreach ($vw in $vswhereCandidates) {
        if (Test-Path $vw) {
            $insts = & $vw -prerelease -products * -property installationPath 2>$null
            foreach ($inst in $insts) {
                $cmd = Resolve-VsBuildCmd $inst
                if ($cmd) { return $cmd }
            }
        }
    }
    $fallbackRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )
    foreach ($root in $fallbackRoots) {
        if (Test-Path $root) {
            $foundAll = Get-ChildItem -Path $root -Filter "vcvarsall.bat" -Recurse -ErrorAction SilentlyContinue |
                        Sort-Object FullName -Descending
            foreach ($f in $foundAll) {
                $instRoot = Split-Path (Split-Path (Split-Path $f.DirectoryName -Parent) -Parent) -Parent
                $cmd = Resolve-VsBuildCmd $instRoot
                if ($cmd) { return $cmd }
            }
        }
    }
    return $null
}

$useMsvc = $false
$vcvars = $null
if ($Compiler -eq "auto" -or $Compiler -eq "msvc") {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        $useMsvc = $true
    } else {
        $vcvars = Find-VsDevBatch
        if ($vcvars) { $useMsvc = $true }
        elseif ($Compiler -eq "msvc") { throw "MSVC cl.exe / vcvars64.bat not found." }
    }
}

if ($useMsvc) {
    Write-Host "[BUILD] Compiling with MSVC x64..." -ForegroundColor Cyan
    $sdkFlags = ""
    $sdkLibs = ""
    if ($NgxSdkDir -ne "") {
        $absSdk = (Resolve-Path $NgxSdkDir).Path
        $sdkFlags = "/DRR_USE_OFFICIAL_NGX_SDK=1 /I`"$absSdk\include`""
        $sdkLibs = "`"$absSdk\lib\Windows_x86_64\rel\nvsdk_ngx_s.lib`""
    }
    $clCmd = "cl.exe /nologo /std:c++20 /EHsc /O2 /fp:precise /W4 /utf-8 /I`"$IncDir`" $sdkFlags `"$SrcFile`" /Fo`"$ObjDir\ngx_rr_bench.obj`" /Fe`"$OutExe`" /link d3d12.lib dxgi.lib dxguid.lib version.lib advapi32.lib shell32.lib user32.lib $sdkLibs"
    $cmdFile = Join-Path $ObjDir "build_msvc.cmd"
    $lines = @("@echo off")
    if ($vcvars) {
        $lines += "call $vcvars >nul"
        $lines += "if errorlevel 1 exit /b %errorlevel%"
    }
    $lines += $clCmd
    Set-Content -Path $cmdFile -Value ($lines -join "`r`n") -Encoding ascii
    & cmd.exe /d /c "`"$cmdFile`""
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed with exit code $LASTEXITCODE" }
} else {
    Write-Host "[BUILD] Compiling with MinGW-w64 x86_64-w64-mingw32-g++..." -ForegroundColor Cyan
    $gxx = Get-Command x86_64-w64-mingw32-g++ -ErrorAction SilentlyContinue
    if (-not $gxx) { $gxx = Get-Command g++ -ErrorAction SilentlyContinue }
    if (-not $gxx) { throw "Neither MSVC nor MinGW g++ was found." }
    & $gxx.Source -std=c++20 -O2 -ffp-contract=off -Wall -Wextra -finput-charset=UTF-8 `
        -I"$IncDir" "$SrcFile" -o "$OutExe" `
        -ld3d12 -ldxgi -ldxguid -lversion -ladvapi32 -lshell32 -luser32 -static-libgcc -static-libstdc++
    if ($LASTEXITCODE -ne 0) { throw "MinGW build failed with exit code $LASTEXITCODE" }
}

Write-Host "[BUILD] Built $OutExe" -ForegroundColor Green
& $OutExe --self-test
if ($LASTEXITCODE -ne 0) { throw "Self-test failed!" }

$CasesJson = Join-Path $RepoRoot "cases\cases.json"
if ((-not $SkipVerify) -and (Test-Path $CasesJson)) {
    & $OutExe --verify-fixtures $CasesJson
    if ($LASTEXITCODE -ne 0) { throw "Fixture input hash verification failed!" }
}
