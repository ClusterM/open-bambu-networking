# Building on Windows

This document describes how to build the network plugin for **Bambu Studio on Windows (x64)** with **MSVC** and **vcpkg** (manifest mode). General behaviour, ABI notes, and Linux instructions remain in [README.md](README.md).

## What you are building

The Windows build produces the same three DLLs Studio loads as the stock “Network Plugin”:

| Output | Role |
| --- | --- |
| `bambu_networking.dll` | Main plugin (LAN/cloud MQTT, FTPS, etc.) |
| `BambuSource.dll` | Camera and Device → Files tunnel (FFmpeg on Windows) |
| `live555.dll` | RTSP stack used by `BambuSource` |

`libmosquitto` is **vendored** (CMake `FetchContent`); OpenSSL, libcurl, zlib, libpng, libjpeg-turbo, FFmpeg, pthreads, cJSON, etc. come from **vcpkg** per [`vcpkg.json`](vcpkg.json).

**Camera path:** On Windows the camera pipeline uses **FFmpeg** (`OBN_CAMERA_FFMPEG`), not GStreamer, because the official Bambu Studio install does not ship GStreamer.

## Prerequisites

- **Windows 10 or 11**, **x64** (the CMake preset uses `x64-windows`).
- **Visual Studio 2019, 2022, or 2025/2026** (Community is fine) with:
  - Workload: **Desktop development with C++**
  - **MSVC v14x** toolset, **Windows 10/11 SDK**
- **CMake 3.20+** (the one bundled with Visual Studio is enough:  
  e.g. `…\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`).
- **Git** (to clone the repo; also used to bootstrap vcpkg into `.vcpkg` if you do not set `VCPKG_ROOT`).
- **PowerShell 5.1+** or **PowerShell 7+** (default terminal for the scripts in this repo).

You do **not** need a separate “Developer Command Prompt”: the **Visual Studio** CMake generator sets up the toolchain when you run `configure.ps1`.

## Quick start

From a normal PowerShell window, in the repository root:

```powershell
.\configure.ps1
.\build.ps1
.\install.ps1
```

- **`configure.ps1`** — finds Visual Studio (via `vswhere`), sets up vcpkg (unless `VCPKG_ROOT` is already set), runs `cmake` with the vcpkg toolchain, and writes `config.ps1` for the next steps.
- **`build.ps1`** — runs `cmake --build <BuildDir> --config <type>` (default: `build`, `Release`).
- **`install.ps1`** — runs `cmake --install` into your install prefix (default: `%APPDATA%\BambuStudio`).

**Tip:** If `cmake` is not on your `PATH`, use “Developer PowerShell for VS” *or* prepend the full path to CMake to `PATH` for that session, then run the same three commands.

## Install layout and Studio config

Default install prefix is **`%APPDATA%\BambuStudio`**, matching a typical Bambu Studio data directory.

After `install.ps1` you should have (among other files):

- `%APPDATA%\BambuStudio\plugins\bambu_networking.dll`
- `%APPDATA%\BambuStudio\plugins\BambuSource.dll`
- `%APPDATA%\BambuStudio\plugins\live555.dll`
- `%APPDATA%\BambuStudio\ota\plugins\network_plugins.json`

The install step can patch **`%APPDATA%\BambuStudio\BambuStudio.conf`** (backup: `BambuStudio.conf.obn-bak`):

- `"installed_networking": "1"`
- `"update_network_plugin": "false"`
- `"ignore_module_cert": "1"`

Studio normally requires the plugin’s Authenticode signature to match its own publisher. **`ignore_module_cert`** turns that check off for development builds. To avoid it, you would need to sign the DLLs with a cert that matches Bambu’s publisher (out of scope here).

To skip the config edit, use **`-NoConfPatch`** on `configure.ps1` (see below).

## `configure.ps1` in detail

### Plugin version string

Studio only compares the **first 8 characters** of the version (e.g. `02.06.00`).  
By default, `configure.ps1` reads `app.version` from:

`%APPDATA%\BambuStudio\BambuStudio.conf`

…then sets the last dotted component to **`.99`** so the built plugin usually appears “newer” than the in-box agent.

**Launch Bambu Studio at least once** so this file exists, **or** pass an explicit version:

```powershell
.\configure.ps1 -WithVersion 02.06.00.99
```

The file is JSON with an optional **trailing comment** (e.g. `# MD5 checksum …`). The script strips that before parsing; if you edit the file by hand, keep a valid `app` object.

### vcpkg location

- If **`VCPKG_ROOT`** is set and points at a tree containing **`vcpkg.exe`**, that instance is used (no clone into the repo).
- Otherwise the script bootstraps **`.\.vcpkg`** under the project root (`git clone` + `bootstrap-vcpkg`).

**Recommended for reliability:** put vcpkg (and often the build tree) on a **local NTFS** drive, not a **VirtualBox/VMware shared folder** and not an exotic remote FS: vcpkg’s MSYS2 bootstrap uses archives with **hardlinks**; some shared folders break extraction (`Invalid argument` / `gawk.exe` errors).

**Example — use a fixed vcpkg on `D:\`:**

```powershell
$env:VCPKG_ROOT = "D:\vp"
.\configure.ps1 -BuildDir "D:\b\obn"
```

- **`-BuildDir PATH`** — CMake build directory (default: `build`). Use a short path on a local disk if the repo lives on a problematic drive.

- **`-NoBootstrapVcpkg`** — fail if `VCPKG_ROOT` is not set; use when you only want your own vcpkg.

If `bootstrap-vcpkg` fails (e.g. antivirus blocking writes), the script may fall back to downloading **`vcpkg.exe`** to `%TEMP%` and copying it into the vcpkg root. You can also unblock/exclude the folder in Defender.

### Other useful switches

| Switch | Default | Description |
| --- | --- | --- |
| `-Prefix` | `%APPDATA%\BambuStudio` | CMake install prefix. |
| `-BuildDir` | `build` | CMake `-B` directory. |
| `-BuildType` | `Release` | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel`. |
| `-WithVersion` | *(from conf)* | Override `OBN_VERSION` / reported plugin version. |
| `-Generator` | auto (VS 16/17/18) | CMake `-G` (override only if you know you need to). |
| `-Arch` | `x64` | Only **x64** is supported in this port. |
| `-CameraBackend` | `ffmpeg` | Use **`ffmpeg`** on Windows. `gstreamer` is not the supported path for official Studio. |
| `-DisableWorkarounds` | off | `OBN_ENABLE_WORKAROUNDS=OFF` (stricter, fewer features). |
| `-DisableFtpsFastpath` | off | `OBN_FT_FTPS_FASTPATH=OFF`. |
| `-EnableTests` | off | Build `probe_plugin.exe` and other tests. |
| `-NoConfPatch` | off | Do not patch `BambuStudio.conf` on install. |
| `-NoBootstrapVcpkg` | off | Require pre-set `VCPKG_ROOT`. |
| `-CMakeArg` | — | Extra CMake flags; repeatable. |

Full parameter help:

```powershell
Get-Help .\configure.ps1 -Full
# or
.\configure.ps1 -?
```

`configure.ps1` writes **`config.ps1`** next to itself so **`build.ps1`** and **`install.ps1`** know `ObnBuildDir`, `ObnBuildType`, and `ObnPrefix`. Re-run `configure.ps1` after changing prefix, generator, or vcpkg root.

## `build.ps1` and `install.ps1`

**Build** (uses `config.ps1` from the last `configure.ps1` run):

```powershell
.\build.ps1
# optional: single target, verbose log
.\build.ps1 -Target bambu_networking -Verbose
```

**Install:**

```powershell
.\install.ps1
```

## Environment and PATH gotchas

- **`cmd` / `link.exe` from MSYS2 or Git “Unix tools”** ahead of the real Windows tools can break CMake, Ninja, or vcpkg. If **invoking `cmd` opens an “Open with” dialog** or builds fail in odd ways, ensure **`C:\Windows\System32`** is **before** MSYS2/WSL shims in `PATH`, or use **Developer PowerShell for Visual Studio** with a clean environment.
- **LONG PATHS:** if builds fail with very deep paths, enable long paths in Windows or move the repo and `VCPKG_ROOT` to a shorter path (e.g. `D:\b\obn`).
- **Stale CMake cache:** if the project was ever configured for **Linux** (or another machine path) inside the same `BuildDir`, delete the build directory and run **`.\configure.ps1`** again. Mixed host/path caches are not supported.

## After build: quick DLL check (optional)

With **`-EnableTests`**, the build includes **`probe_plugin.exe`**. From the build output directory (e.g. `build\Release` or your `-BuildDir\Release`):

```powershell
.\probe_plugin.exe ".\bambu_networking.dll"
```

The plugin also needs runtime DLLs next to it or on `PATH` (vcpkg **applocal** / install rules copy many of them; **`mosquitto_common.dll`** is staged next to `bambu_networking.dll` by the project’s CMake post-build / install when applicable).

## CI

The GitHub Actions workflow **`.github/workflows/build.yml`** includes a **Windows** job; it mirrors the same vcpkg manifest + CMake flow (paths may differ slightly from a local tree).

## See also

- [README.md — Build and install (Linux)](README.md#build-and-install) — `./configure` / `make` and Linux dependencies.
- [NETWORK_PLUGIN.md](NETWORK_PLUGIN.md) — how Studio loads the plugin and ABI expectations.
- [STATUS.md](STATUS.md) — implementation status per symbol.
