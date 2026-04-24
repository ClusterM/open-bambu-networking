<#
.SYNOPSIS
    Windows equivalent of 'make'. Delegates to 'cmake --build <dir> --config <type>'.

.DESCRIPTION
    Reads the build directory and build type from the config.ps1 emitted
    by configure.ps1. Run configure.ps1 first.

.PARAMETER Target
    Optional CMake target name. Default builds everything.

.PARAMETER Verbose
    Pass --verbose to the underlying CMake build.
#>

[CmdletBinding()]
param(
    [string] $Target,
    [switch] $Parallel
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$configPs1 = Join-Path $PSScriptRoot "config.ps1"
if (-not (Test-Path $configPs1)) {
    throw "config.ps1 not found. Run .\configure.ps1 first."
}
. $configPs1

$cmakeArgs = @("--build", $ObnBuildDir, "--config", $ObnBuildType)
if ($Target) { $cmakeArgs += @("--target", $Target) }
if ($Parallel) { $cmakeArgs += "--parallel" }
if ($VerbosePreference -ne 'SilentlyContinue') { $cmakeArgs += "--verbose" }

Write-Host "build: cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }
