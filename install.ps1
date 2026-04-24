<#
.SYNOPSIS
    Windows equivalent of 'make install'. Delegates to 'cmake --install'.

.DESCRIPTION
    Reads the build directory and install prefix from config.ps1 emitted
    by configure.ps1. Run configure.ps1 first.

    On top of the plain cmake --install, the CMake install rules will
    patch %APPDATA%\BambuStudio\BambuStudio.conf to set
    installed_networking=1, update_network_plugin=false and
    ignore_module_cert=1 when installing into the default prefix.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$configPs1 = Join-Path $PSScriptRoot "config.ps1"
if (-not (Test-Path $configPs1)) {
    throw "config.ps1 not found. Run .\configure.ps1 first."
}
. $configPs1

$cmakeArgs = @("--install", $ObnBuildDir, "--config", $ObnBuildType)

Write-Host "install: cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with exit code $LASTEXITCODE" }

Write-Host ""
Write-Host "install: done. Bambu Studio should now load bambu_networking.dll from $ObnPrefix\plugins." -ForegroundColor Green
