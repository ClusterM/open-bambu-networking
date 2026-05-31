# Open Bambu Networking — interactive installer for Windows.
# Ships inside the distribution archive next to lib\vXX.XX.XX\ directories.
# Detects the slicer, matches the ABI version, copies binaries, patches
# the slicer conf, registers the DirectShow filter, and drops a default
# obn.conf if absent.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Write-Info  { param([string]$msg) Write-Host "  [info]  $msg" -ForegroundColor Green }
function Write-Warn  { param([string]$msg) Write-Host "  [warn]  $msg" -ForegroundColor Yellow }
function Write-Err   { param([string]$msg) Write-Host "  [error] $msg" -ForegroundColor Red }

# ── Client selection ─────────────────────────────────────────────────────

Write-Host ""
Write-Host "Open Bambu Networking - Installer" -ForegroundColor White
Write-Host ""
Write-Host "Select your slicer:"
Write-Host "  1) Bambu Studio"
Write-Host "  2) Orca Slicer"
Write-Host ""
$choice = Read-Host "Choice [1]"
if ($choice -eq "2") {
    $Client      = "orca_slicer"
    $ClientLabel = "Orca Slicer"
    $ConfName    = "OrcaSlicer.conf"
    $VersionKey  = "network_plugin_version"
    $ClientDir   = Join-Path $env:APPDATA "OrcaSlicer"
    $DisplayName = "OrcaSlicer"
} else {
    $Client      = "bambu_studio"
    $ClientLabel = "Bambu Studio"
    $ConfName    = "BambuStudio.conf"
    $VersionKey  = "version"
    $ClientDir   = Join-Path $env:APPDATA "BambuStudio"
    $DisplayName = "Bambu Studio"
}
Write-Host ""

$Prefix = $ClientDir

# ── ABI version detection ────────────────────────────────────────────────

function Detect-VersionFromConf {
    param([string]$ConfPath, [string]$Key)
    if (-not (Test-Path $ConfPath)) { return "" }
    $line = Select-String -Path $ConfPath `
        -Pattern "`"$Key`"\s*:\s*`"([0-9][0-9.]*)`"" `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($line -and $line.Matches.Count -gt 0 -and $line.Matches[0].Groups.Count -ge 2) {
        return $line.Matches[0].Groups[1].Value
    }
    return ""
}

function Detect-VersionFromExe {
    param([string]$Name)
    $regPaths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    $entries = Get-ItemProperty -Path $regPaths -ErrorAction SilentlyContinue |
               Where-Object { $_.DisplayName -eq $Name }
    foreach ($e in $entries) {
        $exe = $null
        if ($e.DisplayIcon) {
            $candidate = ($e.DisplayIcon -split ',')[0].Trim('"')
            if (Test-Path $candidate) { $exe = $candidate }
        }
        if (-not $exe -and $e.InstallLocation) {
            $base = ([System.IO.Path]::GetFileNameWithoutExtension($Name).ToLower() -replace ' ','-')
            $candidate = Join-Path $e.InstallLocation ($base + '.exe')
            if (Test-Path $candidate) { $exe = $candidate }
        }
        if ($exe) {
            $fv = (Get-Item $exe).VersionInfo.FileVersion
            if ($fv -match '^\d+(\.\d+){2,3}$') {
                return $fv
            }
        }
    }
    return ""
}

$ConfPath = Join-Path $Prefix $ConfName

$confVer = Detect-VersionFromConf -ConfPath $ConfPath -Key $VersionKey
$exeVer  = Detect-VersionFromExe -Name $DisplayName

$detected = ""
if (-not [string]::IsNullOrEmpty($exeVer)) {
    $detected = $exeVer
} elseif (-not [string]::IsNullOrEmpty($confVer)) {
    $detected = $confVer
} elseif ($Client -eq "orca_slicer") {
    $detected = "02.03.00"
    Write-Warn "No $VersionKey found — defaulting to $detected"
}

if ([string]::IsNullOrEmpty($detected)) {
    Write-Err "Cannot determine ABI version."
    Write-Err "Launch $ClientLabel at least once, then re-run this installer."
    exit 1
}

# Extract major.minor.patch
if ($detected -match '^(\d+\.\d+\.\d+)') {
    $AbiPrefix = $Matches[1]
} else {
    Write-Err "Cannot parse version: $detected"
    exit 1
}
$PluginVer = "$AbiPrefix.99"

# ── Match available ABI directory ────────────────────────────────────────

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LibDir = Join-Path $ScriptDir "lib"

if (-not (Test-Path $LibDir)) {
    Write-Err "lib\ directory not found next to this script"
    exit 1
}

$MatchedDir = $null
$candidate = Join-Path $LibDir "v$AbiPrefix"
if (Test-Path $candidate) {
    $MatchedDir = $candidate
}

if (-not $MatchedDir) {
    $dirs = Get-ChildItem -Path $LibDir -Directory -Filter "v*" |
            Sort-Object Name
    foreach ($d in $dirs) {
        $v = $d.Name -replace '^v', ''
        if ($v -le $AbiPrefix) { $MatchedDir = $d.FullName }
    }
}

if (-not $MatchedDir) {
    $dirs = Get-ChildItem -Path $LibDir -Directory -Filter "v*" | Sort-Object Name
    if ($dirs.Count -gt 0) { $MatchedDir = $dirs[-1].FullName }
}

if (-not $MatchedDir -or -not (Test-Path $MatchedDir)) {
    Write-Err "No matching ABI version found in $LibDir"
    exit 1
}

$MatchedVer = (Split-Path -Leaf $MatchedDir) -replace '^v', ''
$PluginVer = "$MatchedVer.99"

# ── Confirmation ─────────────────────────────────────────────────────────

$DestDir = Join-Path $Prefix "plugins"

Write-Host "Installation summary:" -ForegroundColor White
Write-Host "  Slicer:       $ClientLabel"
Write-Host "  Config dir:   $Prefix"
Write-Host "  ABI version:  $MatchedVer (detected $detected)"
Write-Host "  Install to:   $DestDir"
Write-Host ""
$confirm = Read-Host "Proceed? [Y/n]"
if ($confirm -match '^[Nn]') {
    Write-Host "Aborted."
    exit 0
}
Write-Host ""

# ── Install binaries ─────────────────────────────────────────────────────

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

if ($Client -eq "bambu_studio") {
    $PluginDestName = "bambu_networking.dll"
} else {
    $PluginDestName = "bambu_networking_$PluginVer.dll"
}

$srcPlugin = Join-Path $MatchedDir "bambu_networking.dll"
if (-not (Test-Path $srcPlugin)) {
    Write-Err "Plugin binary not found in $MatchedDir"
    exit 1
}
Copy-Item -Path $srcPlugin -Destination (Join-Path $DestDir $PluginDestName) -Force
Write-Info "Installed $PluginDestName"

$srcSource = Join-Path $MatchedDir "BambuSource.dll"
if (Test-Path $srcSource) {
    Copy-Item -Path $srcSource -Destination (Join-Path $DestDir "BambuSource.dll") -Force
    Write-Info "Installed BambuSource.dll"
}

$srcLive = Join-Path $MatchedDir "live555.dll"
$dstLive = Join-Path $DestDir "live555.dll"
if (Test-Path $srcLive) {
    $installLive = $true
    if (Test-Path $dstLive) {
        $existingSize = (Get-Item $dstLive).Length
        if ($existingSize -gt 65536) {
            $installLive = $false
            Write-Info "Keeping existing live555.dll ($existingSize bytes, looks like vendor build)"
        }
    }
    if ($installLive) {
        Copy-Item -Path $srcLive -Destination $dstLive -Force
        Write-Info "Installed live555.dll"
    }
}

# OTA manifest (Bambu Studio only)
if ($Client -eq "bambu_studio") {
    $srcJson = Join-Path $MatchedDir "network_plugins.json"
    if (Test-Path $srcJson) {
        $otaDir = Join-Path $Prefix "ota\plugins"
        New-Item -ItemType Directory -Force -Path $otaDir | Out-Null
        Copy-Item -Path $srcJson -Destination (Join-Path $otaDir "network_plugins.json") -Force
        Write-Info "Installed ota\plugins\network_plugins.json"
    }
}

# ── Patch slicer conf ────────────────────────────────────────────────────

if (Test-Path $ConfPath) {
    try {
        $raw = Get-Content -Path $ConfPath -Raw
        $jsonBody = $raw -replace '[\r\n]+# MD5 checksum[^\r\n]*[\r\n]*$', ''
        $conf = $jsonBody | ConvertFrom-Json
        $changed = $false

        if ($null -ne $conf.app) {
            if ($Client -eq "bambu_studio") {
                if ($conf.app.installed_networking -ne "1") {
                    $conf.app.installed_networking = "1"; $changed = $true
                }
                if ($conf.app.update_network_plugin -ne "false") {
                    $conf.app.update_network_plugin = "false"; $changed = $true
                }
                if ($conf.app.ignore_module_cert -ne "1") {
                    $conf.app.ignore_module_cert = "1"; $changed = $true
                }
            } else {
                $orcaPatches = @{
                    installed_networking = "true"
                    network_plugin_version = $PluginVer
                    network_plugin_remind_later = "true"
                    ignore_module_cert = "1"
                }
                foreach ($k in $orcaPatches.Keys) {
                    $current = $conf.app | Select-Object -ExpandProperty $k -ErrorAction SilentlyContinue
                    if ($current -ne $orcaPatches[$k]) {
                        if ($null -eq ($conf.app | Get-Member -Name $k -MemberType NoteProperty)) {
                            $conf.app | Add-Member -NotePropertyName $k -NotePropertyValue $orcaPatches[$k]
                        } else {
                            $conf.app.$k = $orcaPatches[$k]
                        }
                        $changed = $true
                    }
                }
                $skipped = $conf.app | Select-Object -ExpandProperty network_plugin_skipped_versions -ErrorAction SilentlyContinue
                if ($skipped -and $skipped.Contains($PluginVer)) {
                    $parts = ($skipped -split ';') | Where-Object { $_ -and $_ -ne $PluginVer }
                    $newSkipped = $parts -join ';'
                    $conf.app.network_plugin_skipped_versions = $newSkipped
                    $changed = $true
                }
            }

            if ($changed) {
                Copy-Item -Path $ConfPath -Destination "$ConfPath.obn-bak" -Force
                $newJson = $conf | ConvertTo-Json -Depth 20
                $newJson + "`n# MD5 checksum 00000000000000000000000000000000`n" |
                    Set-Content -Path $ConfPath -NoNewline
                Write-Info "Patched $ConfName (backup: $ConfName.obn-bak)"
            } else {
                Write-Info "Slicer conf already patched, no changes needed"
            }
        } else {
            Write-Warn "$ConfName has no 'app' object, skipping patch"
        }
    } catch {
        Write-Warn "Could not parse $ConfName, skipping patch: $_"
    }
} else {
    Write-Warn "$ConfName not found — launch $ClientLabel once, then re-run"
}

# ── Register DirectShow filter ───────────────────────────────────────────

$bsDll = Join-Path $DestDir "BambuSource.dll"
if (Test-Path $bsDll) {
    Write-Host ""
    $regConfirm = Read-Host "Register BambuSource DirectShow filter for live camera? [Y/n]"
    if ($regConfirm -notmatch '^[Nn]') {
        $rc = (Start-Process regsvr32.exe -ArgumentList "/s `"$bsDll`"" -Wait -PassThru).ExitCode
        if ($rc -eq 0) {
            Write-Info "Registered BambuSource DirectShow filter (HKCU)"
        } else {
            Write-Warn "regsvr32 failed (rc=$rc); register manually: regsvr32 /s `"$bsDll`""
        }
    }
}

# ── Create default obn.conf if absent ────────────────────────────────────

$ObnConf = Join-Path $Prefix "obn.conf"
if (-not (Test-Path $ObnConf)) {
    @"
## Open Bambu Networking — user settings
##
## Lines starting with ## are comments.
## Lines starting with a single # are disabled settings;
## remove the # to enable them.
##
## Log settings can also be set via OBN_LOG_* environment variables;
## when set, they take priority over this file.

## ─── Logging ────────────────────────────────────────────────

log_level = info
log_stderr = 1
log_to_file = 0
# log_file = C:\path\to\obn.log

## ─── Cloud endpoints ──────────────────────────────────────
##
## Leave empty for production US/CN by country_code.
##   Global:  https://api.bambulab.com / https://bambulab.com / us.mqtt.bambulab.com
##   CN:      https://api.bambulab.cn  / https://bambulab.cn  / cn.mqtt.bambulab.com
##   Dev/QA:  https://api-dev.bambulab.net / https://api-qa.bambulab.net / ...

# cloud_api_host = https://api.bambulab.com
# cloud_web_host = https://bambulab.com
# cloud_mqtt_host = us.mqtt.bambulab.com
# cloud_mqtt_port = 8883

## ─── Cloud access ────────────────────────────────────────
##
## Block background cloud MQTT/REST connections.
## Auth, preset sync, and bind/unbind are still allowed.

# block_cloud = 1

## ─── LAN TLS ─────────────────────────────────────────────
##
## Skip TLS certificate verification for LAN MQTT/FTPS connections.

# lan_tls_skip_verify = 0

## ─── Print behavior ──────────────────────────────────────
##
## Always save timelapse to external storage (USB/SD), ignoring the
## Internal/External toggle in the print dialog.  Studio defaults
## that toggle to internal storage, so this avoids switching it every time.

# force_timelapse_external = 0

## ─── BambuSource logging ──────────────────────────────────
##
## Separate log level/file for the BambuSource (video/CTRL) library.
## Only read if BambuSource is loaded; file is never created by it.

# bambusource_log_level = info
# bambusource_log_file = C:\path\to\bambusource.log
"@ | Set-Content -Path $ObnConf -Encoding UTF8
    Write-Info "Created default obn.conf"
}

# ── Summary ──────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "Installation complete!" -ForegroundColor Green
Write-Host ""
Write-Host "  Plugin:     $(Join-Path $DestDir $PluginDestName)"
Write-Host "  Config:     $ObnConf"
Write-Host "  Slicer:     $ClientLabel ($Prefix)"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Close $ClientLabel if it is running"
Write-Host "  2. Launch $ClientLabel - it should load the open-bambu-networking plugin"
Write-Host "  3. Edit $ObnConf to customize plugin behavior"
Write-Host ""
Write-Host "GitHub: https://github.com/ClusterM/open-bambu-networking" -ForegroundColor Cyan
Write-Host ""

if ($Host.Name -eq "ConsoleHost") {
    Write-Host "Press any key to exit..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}
