# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ToxMox / MoxRelay contributors
#
# bootstrap-deps.ps1 -- one-shot dependency bootstrap for a FRESH clone of MoxRelay.
#
# MoxRelay links an OUT-OF-TREE libobs build plus the Qt6 / FFmpeg binaries from the OBS
# prebuilt "obs-deps" bundle. None of that is committed to this repo. This script reproduces
# the exact pinned dependency tree and then writes the git-ignored CMakeUserPresets.json
# `local` preset that feeds the machine-specific absolute paths into the tracked msvc-x64
# configure preset (OBS_SRC / OBS_BUILD / MOXRELAY_OBS_RUNDIR / CMAKE_PREFIX_PATH).
#
# What it does, in order:
#   1. Clones obs-studio at the PINNED tag/commit (with submodules) into a predictable layout.
#   2. Configures + builds obs-studio (RelWithDebInfo, x64). OBS's own configure step
#      (cmake/windows/buildspec.cmake) downloads the PINNED obs-deps + Qt6 bundles into
#      <obs>/.deps and verifies them against the SHA-256 hashes pinned in obs-studio's
#      CMakePresets.json. The build populates libobs/obs.{lib,dll}, config/obsconfig.h, and
#      the rundir the deploy step copies from.
#   3. Verifies every artifact the MoxRelay build will reference, failing loudly on any miss.
#   4. Generates the git-ignored CMakeUserPresets.json `local` (+ `clangd`) preset.
#
# After this runs, build MoxRelay with:
#   cmake --preset local
#   cmake --build --preset msvc-x64
#
# Run from anywhere; paths are resolved relative to this script. A normal PowerShell shell is
# fine (the Visual Studio generator locates the v143 toolset itself); a "x64 Native Tools"
# developer shell also works and is required for the optional clangd Ninja sidecar.

#Requires -Version 5.1
[CmdletBinding()]
param(
    # Where the obs-studio clone + build tree lives. Default: a sibling folder next to the
    # MoxRelay clone, so the multi-GB OBS tree never lands inside this repo.
    [string] $DepsRoot,

    # Configure obs-studio with the browser source OFF. MoxRelay does not use the OBS browser
    # source, so this skips the large CEF download and shortens the OBS build. Default is OFF
    # here ($false) => the stock OBS browser-on preset is used, matching upstream's validated build.
    [switch] $NoBrowser,

    # Only (re)generate CMakeUserPresets.json against an obs-studio tree that is already built.
    [switch] $SkipObsBuild,

    # Overwrite an existing CMakeUserPresets.json (it is normally left untouched if present).
    [switch] $Force,

    # REPORT-ONLY. Query GitHub for obs-studio's latest stable release and print a nudge if it
    # is newer than the pinned tag. Does NOT change which version is fetched or built; a failed
    # or offline check is a warning, never an error.
    [switch] $CheckLatest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------------------------
# PINNED upstream revisions. These are the single source of truth for this script and MUST stay
# in lock-step with packaging/SOURCES.txt.in and obs-studio's own CMakePresets.json vendor block.
#
#   obs-studio : tag 32.1.2  (commit below) -- supplies libobs headers, obs.lib/obs.dll, plugins.
#   obs-deps   : 2025-08-23  -- the prebuilt FFmpeg/x264/curl/... bundle (obs-deps-2025-08-23-x64)
#                              AND the Qt6 bundle (obs-deps-qt6-2025-08-23-x64). Both are pinned
#                              (with SHA-256 hashes) inside obs-studio@32.1.2's CMakePresets.json,
#                              so checking out the pinned obs-studio commit pins obs-deps too.
# ---------------------------------------------------------------------------------------------
$ObsRepo       = 'https://github.com/obsproject/obs-studio.git'
$ObsTag        = '32.1.2'
$ObsCommit     = 'fb4d98bf88fae5fc85cb11fc57f7c5e309282194'
$ObsDepsDate   = '2025-08-23'
$ObsConfig     = 'RelWithDebInfo'

$QtDepsDir     = "obs-deps-qt6-$ObsDepsDate-x64"   # -> CMAKE_PREFIX_PATH (Qt6Config.cmake)
$FfmpegDepsDir = "obs-deps-$ObsDepsDate-x64"       # -> swresample.lib / avutil.lib (MoxRelay direct link)

function Write-Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail([string] $msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $WorkDir)
    Push-Location $WorkDir
    try {
        & $Exe @Arguments
        if ($LASTEXITCODE -ne 0) { Fail "'$Exe $($Arguments -join ' ')' failed with exit code $LASTEXITCODE (in $WorkDir)." }
    } finally {
        Pop-Location
    }
}

# ---------------------------------------------------------------------------------------------
# Resolve layout. scripts/ sits directly under the repo root.
# ---------------------------------------------------------------------------------------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $RepoRoot 'CMakePresets.json'))) {
    Fail "Could not find CMakePresets.json at the resolved repo root '$RepoRoot'. Run this script from the MoxRelay repo's scripts/ folder."
}

if (-not $DepsRoot) {
    $DepsRoot = Join-Path (Split-Path -Parent $RepoRoot) 'moxrelay-deps'
}
$DepsRoot = [System.IO.Path]::GetFullPath($DepsRoot)

$ObsSrc   = Join-Path $DepsRoot 'obs-studio'
$ObsBuild = Join-Path $ObsSrc   'build_x64'
$Rundir   = Join-Path $ObsBuild  "rundir/$ObsConfig"
$QtPrefix = Join-Path $ObsSrc    ".deps/$QtDepsDir"

Write-Step "MoxRelay dependency bootstrap"
Write-Host "  Repo root : $RepoRoot"
Write-Host "  Deps root : $DepsRoot"
Write-Host "  obs-studio: $ObsTag ($ObsCommit)"
Write-Host "  obs-deps  : $ObsDepsDate (Qt6 + FFmpeg bundles)"

# ---------------------------------------------------------------------------------------------
# Tool checks.
# ---------------------------------------------------------------------------------------------
foreach ($tool in 'git', 'cmake') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "'$tool' is not on PATH. Install it (CMake 3.28+ for the v6 presets) and re-run."
    }
}
$cmakeVer = (& cmake --version | Select-Object -First 1)
Write-Host "  cmake     : $cmakeVer"

# ---------------------------------------------------------------------------------------------
# Optional REPORT-ONLY upstream check. Read-only GitHub query for obs-studio's latest stable
# release; nudge if it is newer than the pin. This NEVER changes what is fetched or built, and a
# network/API failure is a warning -- the bootstrap continues with the pinned tag regardless.
# ---------------------------------------------------------------------------------------------
if ($CheckLatest) {
    Write-Step "Checking GitHub for a newer stable obs-studio (report-only)"
    try {
        $latest = Invoke-RestMethod -Uri 'https://api.github.com/repos/obsproject/obs-studio/releases/latest' `
            -Headers @{ 'User-Agent' = 'MoxRelay-bootstrap'; 'Accept' = 'application/vnd.github+json' } `
            -TimeoutSec 15 -ErrorAction Stop
        $latestTag = "$($latest.tag_name)".Trim()
        if (-not $latestTag) {
            Write-Host "  WARNING: GitHub returned no tag_name; skipping the check and continuing with the pin." -ForegroundColor Yellow
        } elseif ($latestTag -eq $ObsTag) {
            Write-Host "  Pinned obs-studio $ObsTag is the latest stable release." -ForegroundColor Green
        } else {
            Write-Host "  A newer stable obs-studio $latestTag is available; current pin is $ObsTag." -ForegroundColor Yellow
            Write-Host "  Bumping is a deliberate maintainer step: update the pin, re-run bootstrap, rebuild, run --selftest, re-ship." -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  WARNING: could not check for the latest obs-studio release ($($_.Exception.Message)); continuing with the pinned $ObsTag." -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------------------------
# 1. obs-studio source at the pinned commit (with submodules).
# ---------------------------------------------------------------------------------------------
if (-not $SkipObsBuild) {
    if (-not (Test-Path (Join-Path $ObsSrc '.git'))) {
        Write-Step "Cloning obs-studio into $ObsSrc"
        New-Item -ItemType Directory -Force -Path $DepsRoot | Out-Null
        Invoke-Native 'git' @('clone', $ObsRepo, $ObsSrc) $DepsRoot
    } else {
        Write-Step "Fetching obs-studio in $ObsSrc"
        Invoke-Native 'git' @('fetch', '--tags', 'origin') $ObsSrc
    }

    Write-Step "Checking out pinned commit $ObsCommit ($ObsTag)"
    Invoke-Native 'git' @('checkout', '--force', $ObsCommit) $ObsSrc
    Invoke-Native 'git' @('submodule', 'update', '--init', '--recursive', '--force') $ObsSrc

    $head = (& git -C $ObsSrc rev-parse HEAD).Trim()
    if ($head -ne $ObsCommit) { Fail "obs-studio HEAD is '$head', expected the pinned '$ObsCommit'." }

    # -----------------------------------------------------------------------------------------
    # 2. Configure + build obs-studio. The configure step auto-downloads the PINNED obs-deps +
    #    Qt6 bundles (2025-08-23) into <obs>/.deps and hash-verifies them; the build populates
    #    obs.{lib,dll}, config/obsconfig.h, and rundir/<cfg>/{bin,data,obs-plugins}.
    # -----------------------------------------------------------------------------------------
    $configureArgs = @('--preset', 'windows-x64')
    if ($NoBrowser) { $configureArgs += '-DENABLE_BROWSER=OFF' }

    Write-Step "Configuring obs-studio (cmake $($configureArgs -join ' ')) -- downloads pinned obs-deps $ObsDepsDate"
    Invoke-Native 'cmake' $configureArgs $ObsSrc

    Write-Step "Building obs-studio ($ObsConfig, x64) -- this is a long build"
    Invoke-Native 'cmake' @('--build', '--preset', 'windows-x64') $ObsSrc
} else {
    Write-Step "SkipObsBuild: assuming obs-studio is already built at $ObsSrc"
}

# ---------------------------------------------------------------------------------------------
# 3. Verify every artifact the MoxRelay build references. Fail loudly with the exact path.
# ---------------------------------------------------------------------------------------------
Write-Step "Verifying obs-studio build artifacts"
$required = @(
    @{ Path = (Join-Path $ObsBuild "libobs/$ObsConfig/obs.lib");        What = 'libobs import lib (OBS_BUILD)' },
    @{ Path = (Join-Path $ObsBuild "libobs/$ObsConfig/obs.dll");        What = 'libobs runtime DLL (OBS_BUILD)' },
    @{ Path = (Join-Path $ObsBuild 'config/obsconfig.h');              What = 'generated obsconfig.h (OBS_BUILD/config)' },
    @{ Path = (Join-Path $Rundir   'bin/64bit/obs.dll');               What = 'rundir obs.dll (MOXRELAY_OBS_RUNDIR)' },
    @{ Path = (Join-Path $Rundir   'data');                            What = 'rundir data tree (MOXRELAY_OBS_RUNDIR)' },
    @{ Path = (Join-Path $Rundir   'obs-plugins');                     What = 'rundir obs-plugins tree (MOXRELAY_OBS_RUNDIR)' },
    @{ Path = (Join-Path $QtPrefix 'lib/cmake/Qt6/Qt6Config.cmake');   What = "Qt6 from $QtDepsDir (CMAKE_PREFIX_PATH)" },
    @{ Path = (Join-Path $ObsSrc   ".deps/$FfmpegDepsDir/lib/swresample.lib"); What = "FFmpeg swresample from $FfmpegDepsDir" }
)
$missing = @()
foreach ($r in $required) {
    if (-not (Test-Path $r.Path)) { $missing += "  [missing] $($r.What): $($r.Path)" }
}
if ($missing.Count -gt 0) {
    Fail ("Required obs-studio artifacts are missing:`n" + ($missing -join "`n") +
          "`nIf obs-deps did not land as '$ObsDepsDate', the pinned obs-studio tag may have changed its dependency pin -- reconcile this script, packaging/SOURCES.txt.in, and obs-studio's CMakePresets.json before continuing.")
}
Write-Host "  All required artifacts present." -ForegroundColor Green

# ---------------------------------------------------------------------------------------------
# 4. Generate the git-ignored CMakeUserPresets.json `local` (+ `clangd`) preset.
# ---------------------------------------------------------------------------------------------
$UserPresets = Join-Path $RepoRoot 'CMakeUserPresets.json'

# CMake wants forward slashes in cache paths; normalize the absolute paths.
$ObsSrcF   = $ObsSrc   -replace '\\', '/'
$ObsBuildF = $ObsBuild -replace '\\', '/'
$RundirF   = $Rundir   -replace '\\', '/'
$QtPrefixF = $QtPrefix -replace '\\', '/'

if ((Test-Path $UserPresets) -and -not $Force) {
    Write-Step "CMakeUserPresets.json already exists -- leaving it untouched (pass -Force to regenerate)"
    Write-Host "  $UserPresets"
} else {
    Write-Step "Writing CMakeUserPresets.json"
    $json = @"
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 28, "patch": 0 },
  "configurePresets": [
    {
      "name": "local",
      "displayName": "Local machine (real absolute paths)",
      "description": "GIT-IGNORED. Generated by scripts/bootstrap-deps.ps1. Real local-machine paths to the libobs build and the Qt6 obs-deps bundle. Inherits msvc-x64 from CMakePresets.json.",
      "inherits": "msvc-x64",
      "cacheVariables": {
        "OBS_SRC": "$ObsSrcF",
        "OBS_BUILD": "$ObsBuildF",
        "MOXRELAY_OBS_RUNDIR": "$RundirF",
        "CMAKE_PREFIX_PATH": "$QtPrefixF"
      }
    },
    {
      "name": "clangd",
      "displayName": "clangd compilation database (local paths)",
      "description": "GIT-IGNORED. Binds the tracked clangd-base fragment to this machine's paths. Re-run 'cmake --preset clangd' from a VS dev shell after adding/removing source files.",
      "inherits": ["clangd-base", "local"]
    }
  ],
  "buildPresets": [
    {
      "name": "local",
      "displayName": "Build MoxRelay local (RelWithDebInfo)",
      "configurePreset": "local",
      "configuration": "RelWithDebInfo"
    }
  ]
}
"@
    Set-Content -Path $UserPresets -Value $json -Encoding UTF8
    Write-Host "  $UserPresets" -ForegroundColor Green

    # Soft check: this file MUST stay out of version control.
    & git -C $RepoRoot check-ignore -q 'CMakeUserPresets.json' 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  WARNING: CMakeUserPresets.json is NOT git-ignored in this checkout -- do not commit it." -ForegroundColor Yellow
    }
}

Write-Step "Done. Build MoxRelay with:"
Write-Host "    cmake --preset local"
Write-Host "    cmake --build --preset msvc-x64"
Write-Host "    cpack -G ZIP -C $ObsConfig --config build/CPackConfig.cmake   # optional portable ZIP"
exit 0
