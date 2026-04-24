<#
.SYNOPSIS
    Windows equivalent of ./configure. Emits a ready-to-build CMake tree
    under .\build\ using MSVC + vcpkg.

.DESCRIPTION
    Mirrors the top-level POSIX ./configure script flag-for-flag as far
    as the Windows build supports. What it does:

      1. Locates Visual Studio (via vswhere) so we can hand CMake a
         matching generator; refuses to continue without VS 2019+ / MSBuild.
      2. Bootstraps a local vcpkg checkout under .\.vcpkg\ if the user
         has not already exported VCPKG_ROOT, and installs the manifest
         dependencies declared in vcpkg.json.
      3. Auto-detects the plugin version from Bambu Studio's config file
         (%APPDATA%\BambuStudio\BambuStudio.conf) exactly like ./configure
         reads ~/.config/BambuStudio/BambuStudio.conf on Linux.
      4. Writes a tiny config.ps1 so build.ps1 / install.ps1 know which
         build directory to operate on.

    Every switch below maps 1:1 to a CMake cache variable -- pass
    -CMakeArg extras for anything this script does not expose.

.PARAMETER Prefix
    Install prefix. Defaults to %APPDATA%\BambuStudio (same directory
    Bambu Studio uses for its config on Windows).

.PARAMETER BuildDir
    Build directory. Defaults to "build".

.PARAMETER BuildType
    CMake build type (Release|Debug|RelWithDebInfo|MinSizeRel).

.PARAMETER WithVersion
    Override the version string reported via bambu_network_get_version().
    When not set, configure reads BambuStudio.conf under the install
    prefix, grabs app.version and bumps the last component to 99.

.PARAMETER Generator
    CMake generator. Defaults to the newest Visual Studio found.

.PARAMETER Arch
    Target architecture. Only x64 is supported for now.

.PARAMETER CameraBackend
    gstreamer|ffmpeg. Defaults to ffmpeg on Windows (vcpkg installs
    ffmpeg; GStreamer is extremely painful to build from source on MSVC).

.PARAMETER DisableWorkarounds
    Strict drop-in replacement: disable OBN_ENABLE_WORKAROUNDS.

.PARAMETER DisableFtpsFastpath
    Disable OBN_FT_FTPS_FASTPATH.

.PARAMETER EnableTests
    Build smoke tests.

.PARAMETER NoConfPatch
    Skip the BambuStudio.conf patch at install time.

.PARAMETER NoBootstrapVcpkg
    Do not bootstrap vcpkg automatically. The user must point VCPKG_ROOT
    at an existing checkout.

.PARAMETER CMakeArg
    Extra flags passed verbatim to CMake (repeatable).
#>

[CmdletBinding()]
param(
    [string] $Prefix,
    [string] $BuildDir = "build",
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string] $BuildType = "Release",
    [string] $WithVersion,
    [string] $Generator,
    [ValidateSet("x64")]
    [string] $Arch = "x64",
    [ValidateSet("gstreamer", "ffmpeg")]
    [string] $CameraBackend = "ffmpeg",
    [switch] $DisableWorkarounds,
    [switch] $DisableFtpsFastpath,
    [switch] $EnableTests,
    [switch] $NoConfPatch,
    [switch] $NoBootstrapVcpkg,
    [string[]] $CMakeArg
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Section($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# Visual Studio detection (via vswhere, shipped with VS Installer since 2017).
# ---------------------------------------------------------------------------
function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at $vswhere. Install Visual Studio 2019 or newer (Community edition is fine)."
    }
    $json = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -format json | ConvertFrom-Json
    if (-not $json) {
        throw "vswhere did not find any Visual Studio installation with the MSVC C++ toolset. Install the 'Desktop development with C++' workload."
    }
    return $json[0]
}

# ---------------------------------------------------------------------------
# CMake: which "Visual Studio ..." generators exist (from cmake --help)
# ---------------------------------------------------------------------------
function Get-CMakeVersionLine {
    $v = & cmake --version 2>&1
    if ($LASTEXITCODE -ne 0) { return $null }
    return [string] ($v | Select-Object -First 1)
}

function Get-CMakeVisualStudioGenerators {
    $helpLines = & cmake --help 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --help failed (exit $LASTEXITCODE). Is CMake on PATH? Output: $helpLines"
    }
    $rows = [System.Collections.Generic.List[psobject]]::new()
    $seen = @{}
    foreach ($line in $helpLines) {
        if ($line -match '^\s{2}(Visual Studio \d+ \d{4})\s*=') {
            $name = $Matches[1]
            if ($seen[$name]) { continue }
            $seen[$name] = $true
            if ($name -match 'Visual Studio (\d+) (\d{4})') {
                $rows.Add([pscustomobject]@{
                    Name  = $name
                    Major = [int]$Matches[1]
                    Year  = [int]$Matches[2]
                })
            }
        }
    }
    if ($rows.Count -eq 0) { return @() }
    $rows | Sort-Object -Property Major, Year -Descending
}

function Test-IsVisualStudioCmakeGenerator([string] $g) {
    return [bool] ($g -and ($g -match '^Visual Studio \d+ \d{4}$'))
}

function Resolve-VisualStudioCmakeGenerator {
    param(
        [string] $Preferred,
        [bool] $UserPassedGenerator,
        [object[]] $Available
    )
    if ($Available.Count -eq 0) {
        $cv = Get-CMakeVersionLine
        throw @"
configure: ``cmake --help`` does not list any ``Visual Studio ...`` generator.
CMake reported: $cv
Install a recent CMake (or fix PATH so the right cmake is first).
"@
    }
    $names = $Available | ForEach-Object { $_.Name }
    if ($names -contains $Preferred) { return $Preferred }
    if ($UserPassedGenerator) {
        $cv = Get-CMakeVersionLine
        $list = $names -join ", "
        throw @"
configure: this CMake does not know the ``-G`` you requested:

  -Generator $Preferred

It is not listed under ``cmake --help``; available Visual Studio generators
here: $list

Your CMake: $cv

Update CMake, pass ``-Generator`` with one of the names above, or install
a Visual Studio / CMake pair that support the same -G.
"@
    }
    $best = $Available[0].Name
    $cv = Get-CMakeVersionLine
    Write-Host ""
    Write-Host "configure: NOTE: this CMake has no ""$Preferred"" -G (see cmake --help); using ""$best"" so configure can continue. ($cv)" -ForegroundColor Yellow
    Write-Host "configure:       Install a newer CMake for a native generator matching this Visual Studio, or set -Generator explicitly." -ForegroundColor Yellow
    return $best
}

# ---------------------------------------------------------------------------
# vcpkg bootstrap
# ---------------------------------------------------------------------------
function Ensure-Vcpkg {
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT "vcpkg.exe"))) {
        Write-Host "configure: using existing VCPKG_ROOT=$($env:VCPKG_ROOT)"
        return $env:VCPKG_ROOT
    }
    if ($NoBootstrapVcpkg) {
        throw "VCPKG_ROOT is not set and -NoBootstrapVcpkg was passed. Export VCPKG_ROOT or drop -NoBootstrapVcpkg."
    }
    $local = Join-Path $PSScriptRoot ".vcpkg"
    if (-not (Test-Path $local)) {
        Write-Section "bootstrapping vcpkg into $local"
        git clone --depth=1 https://github.com/microsoft/vcpkg $local
        if ($LASTEXITCODE -ne 0) { throw "git clone vcpkg failed" }
    }
    $vcpkgExe = Join-Path $local "vcpkg.exe"
    if (-not (Test-Path $vcpkgExe)) {
        $bootstrap = Join-Path $local "bootstrap-vcpkg.bat"
        # Redirect stdout+stderr to Information so bootstrap chatter
        # does not contaminate this function's return value.
        & $bootstrap -disableMetrics *>&1 | ForEach-Object { Write-Host $_ }
        # Defender / SmartScreen sometimes blocks the direct write from
        # bootstrap's downloader on project drives. Fall back to a
        # staged download + copy, which we have seen succeed on the
        # same drive.
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $vcpkgExe)) {
            Write-Host "configure: bootstrap-vcpkg failed, trying staged download fallback..."
            $meta = Join-Path $local "scripts\vcpkg-tool-metadata.txt"
            $tag  = $null
            if (Test-Path $meta) {
                $m = Select-String -Path $meta -Pattern '^VCPKG_TOOL_RELEASE_TAG=(.+)$' -ErrorAction SilentlyContinue
                if ($m) { $tag = $m.Matches[0].Groups[1].Value.Trim() }
            }
            if (-not $tag) { $tag = "2026-04-08" }
            $url  = "https://github.com/microsoft/vcpkg-tool/releases/download/$tag/vcpkg.exe"
            $temp = Join-Path $env:TEMP ("vcpkg-" + [guid]::NewGuid().ToString() + ".exe")
            Write-Host "configure: downloading $url -> $temp"
            Invoke-WebRequest -Uri $url -OutFile $temp -UseBasicParsing | Out-Null
            Unblock-File -Path $temp -ErrorAction SilentlyContinue
            Copy-Item -Path $temp -Destination $vcpkgExe -Force | Out-Null
            Remove-Item $temp -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path $vcpkgExe)) { throw "bootstrap-vcpkg failed to produce vcpkg.exe" }
    } else {
        Write-Host "configure: reusing existing vcpkg.exe at $vcpkgExe"
    }
    $env:VCPKG_ROOT = $local
    Write-Host "configure: VCPKG_ROOT=$local"
    # Return ONLY the path. Wrap in parens and cast to [string] so any
    # stray pipeline output from Write-Host (which normally doesn't
    # reach the pipeline but can in some hosts) cannot contaminate.
    return [string] $local
}

# ---------------------------------------------------------------------------
# Plugin version auto-detection from Bambu Studio config.
# ---------------------------------------------------------------------------
function Detect-StudioVersion([string] $confPath) {
    if (-not (Test-Path $confPath)) { return $null }
    # The file is JSON but Studio appends an MD5 checksum comment at
    # the bottom (#<32-hex-digits>) which trips ConvertFrom-Json. Strip it.
    $raw  = Get-Content -Raw -Path $confPath
    # Studio appends a trailing integrity comment such as:
    #   # MD5 checksum 57AA12917DB0CBBE43E3D4351015E07D
    # or just a bare "#<hex>" on older builds. Strip any line starting
    # with "#" to the end of file so ConvertFrom-Json doesn't choke.
    $body = [System.Text.RegularExpressions.Regex]::Replace($raw,
        '(?s)\r?\n#.*$', "`n")
    try {
        $obj = $body | ConvertFrom-Json
    } catch {
        return $null
    }
    if ($obj.PSObject.Properties.Name -notcontains "app") { return $null }
    $app = $obj.app
    if ($app.PSObject.Properties.Name -notcontains "version") { return $null }
    return [string] $app.version
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if (-not $Prefix) {
    if ($env:APPDATA) {
        $Prefix = Join-Path $env:APPDATA "BambuStudio"
    } else {
        throw "%APPDATA% is not set, pass -Prefix explicitly."
    }
}
$Prefix = [System.IO.Path]::GetFullPath($Prefix)

Write-Section "Visual Studio / MSBuild"
$vs = Find-VisualStudio
Write-Host ("configure: using {0} (installation path: {1})" -f $vs.displayName, $vs.installationPath)

if (-not $Generator) {
    # vswhere returns catalog.productLineVersion as a string matching the
    # Visual Studio release year ("2019", "2022", "2026", ...). The CMake
    # generator name uses the internal major version (16, 17, 18, ...).
    $plv = $null
    if ($vs.catalog -and $vs.catalog.productLineVersion) {
        $plv = [string] $vs.catalog.productLineVersion
    }
    # installationVersion starts with the internal major (e.g. "18.5...").
    $major = ($vs.installationVersion -split '\.')[0]
    switch ($major) {
        "18" { $Generator = "Visual Studio 18 2026" }
        "17" { $Generator = "Visual Studio 17 2022" }
        "16" { $Generator = "Visual Studio 16 2019" }
        default {
            # Fall back on productLineVersion when installationVersion is
            # unexpectedly formatted.
            switch -regex ($plv) {
                "2026" { $Generator = "Visual Studio 18 2026" }
                "2022" { $Generator = "Visual Studio 17 2022" }
                "2019" { $Generator = "Visual Studio 16 2019" }
                default { $Generator = "Visual Studio 17 2022" }
            }
        }
    }
}
$generatorUserPassed = $PSBoundParameters.ContainsKey('Generator')
if (Test-IsVisualStudioCmakeGenerator $Generator) {
    $cmakeVsGens = Get-CMakeVisualStudioGenerators
    $Generator = Resolve-VisualStudioCmakeGenerator -Preferred $Generator `
        -UserPassedGenerator $generatorUserPassed -Available $cmakeVsGens
}
Write-Host "configure: CMake generator = $Generator ($Arch)"

Write-Section "vcpkg"
$vcpkgRoot = Ensure-Vcpkg
$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"

Write-Section "plugin version"
$version = $WithVersion
if (-not $version) {
    $conf = Join-Path $Prefix "BambuStudio.conf"
    $detected = Detect-StudioVersion $conf
    if (-not $detected) {
        throw @"
configure: cannot determine the plugin version automatically.

  Tried to read ""version"" from: $conf

  Launch Bambu Studio at least once so it writes its config, or pass
  the version explicitly, for example:

      .\configure.ps1 -WithVersion 02.05.02.99

  Studio compares only the first 8 characters (""MAJOR.MINOR.PATCH""),
  so use X.Y.Z from the Studio you are installing next to, .99 last.
"@
    }
    # Bump the last dotted component to 99 so our plugin always looks
    # ""newer"" than the agent Studio shipped with itself.
    $version = ($detected -replace '\.[0-9]+$', '.99')
    Write-Host "configure: detected Bambu Studio $detected at $conf"
    Write-Host "configure: plugin version = $version (override with -WithVersion)"
} else {
    Write-Host "configure: plugin version (override) = $version"
}

Write-Section "cmake configure"

$cmakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $Generator,
    "-A", $Arch,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$Prefix",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DOBN_VERSION=$version",
    "-DOBN_CAMERA_BACKEND=$CameraBackend",
    "-DOBN_ENABLE_WORKAROUNDS=$(if ($DisableWorkarounds) { 'OFF' } else { 'ON' })",
    "-DOBN_FT_FTPS_FASTPATH=$(if ($DisableFtpsFastpath) { 'OFF' } else { 'ON' })",
    "-DOBN_BUILD_TESTS=$(if ($EnableTests) { 'ON' } else { 'OFF' })",
    "-DOBN_PATCH_STUDIO_CONF=$(if ($NoConfPatch) { 'OFF' } else { 'ON' })"
)
if ($CMakeArg) { $cmakeArgs += $CMakeArg }

Write-Host ("configure: cmake " + ($cmakeArgs -join ' '))
$cmakeLog = & cmake @cmakeArgs 2>&1
$cmakeText = if ($null -ne $cmakeLog) { ($cmakeLog | ForEach-Object { "$_" }) -join [Environment]::NewLine } else { "" }
$cmakeLog | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    $msg = "cmake configure failed (exit $LASTEXITCODE).`n"
    if ($cmakeText) { $msg += $cmakeText }
    if ($cmakeText -match 'Could not create named generator') {
        $cv = Get-CMakeVersionLine
        $msg += "`n`nHINT: That -G is not in your CMake. Your CMake: $cv`nRun ``cmake --help`` to see supported generators, upgrade CMake, or set -Generator."
    }
    throw $msg
}

# Drop a tiny config.ps1 so build.ps1 / install.ps1 know the build dir.
$configPs1 = Join-Path $PSScriptRoot "config.ps1"
Set-Content -Path $configPs1 -Encoding UTF8 -Value @"
# Generated by configure.ps1. Re-run it to refresh.
`$script:ObnBuildDir  = '$BuildDir'
`$script:ObnBuildType = '$BuildType'
`$script:ObnPrefix    = '$Prefix'
`$script:ObnVcpkgRoot = '$vcpkgRoot'
"@

Write-Host ""
Write-Host "configure: done." -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "  .\build.ps1             # cmake --build $BuildDir --config $BuildType"
Write-Host "  .\install.ps1           # cmake --install $BuildDir --config $BuildType"
Write-Host ""
Write-Host "Re-run '.\configure.ps1 -?' for the full list of options."
