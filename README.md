# Open Bambu Networking

Open-source drop-in replacement for Bambu Studio's proprietary `bambu_networking`
plugin.

## Why this project exists

[Bambu Studio](https://github.com/bambulab/BambuStudio) is an excellent
open-source slicer, but every piece of code that actually talks to a
Bambu Lab printer — LAN discovery, MQTT telemetry, file transfer,
camera, OTA, cloud — lives in a closed-source shared library
(`libbambu_networking.so`, shipped as a "Network Plugin" Studio
downloads on first start). There's nothing inherently wrong with a
cloud product; the problem is the implementation:

- **The stock plugin ships an unaligned atomic that triggers the
  kernel's `split_lock` detector on every modern Intel CPU.** Startup
  stalls for 25-60 seconds while the kernel walks each trap; every
  Device-tab click hits it again. The workaround
  (`sysctl kernel.split_lock_mitigate=0`) degrades system-wide
  performance and still misbehaves in LAN-only mode. Reported to Bambu
  over a year ago and still open:
  [bambulab/BambuStudio#8605](https://github.com/bambulab/BambuStudio/issues/8605).
- **No ARM or non-x86_64 build.** The plugin is only published as
  Linux x86_64 (plus Windows and macOS). You can't use Studio or any
  third-party tool that reuses the plugin on a Raspberry Pi, an
  Ampere workstation, or any other aarch64 / riscv host, even though
  the rest of Studio builds cleanly.
- **Cloud chatter on every action, even in LAN-only mode.** Even when
  the printer is sitting on the same subnet as Studio in Developer
  Mode, the stock plugin keeps reaching out to
  `api.bambulab.com` / MakerWorld for things like bind status and
  task metadata. That's extra latency on every click, a hard
  dependency on the account infrastructure being up, and a
  surveillance footprint a lot of users didn't opt in to.

This project is a drop-in replacement for that library: same `dlsym`
ABI, same file name, same install location, but open source,
aligned-atomic-clean, cross-architecture-buildable, and strictly
LAN-first (the cloud is only ever contacted for sign-in, so that
Studio's preset sync works — the rest is straight to the printer).

Protocol knowledge comes from
[OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) and the
reference implementations in
[bambulabs_api](https://github.com/acse-ci223/bambulabs_api) and
[ha_bambu_lab](https://github.com/greghesp/ha-bambulab); everything
else is reverse-engineered from MITM captures of the stock plugin.

## Developer Mode requirement

Recent (2024+) printer firmware cryptographically verifies every MQTT command
it receives when the printer is paired with the Bambu cloud. The verification
uses a per-installation RSA key that the stock plugin ships as obfuscated data
and which we do not reproduce. Symptom on the printer screen when an unsigned
command arrives:

```
MQTT Command verification failed
err_code: 84033543
```

To use this plugin, put the printer in **Developer Mode**:

1. On the printer: Gear icon -> Settings -> LAN Only Mode -> enable "LAN Only", "LAN Only liveview" and "Developer mode".
2. In Bambu Studio: Device -> Connect via LAN with access code.

In this mode the printer skips MQTT verification and accepts plain LAN
commands. All LAN features of the plugin (discovery, telemetry, printing,
filename browsing, file transfer, camera) work normally.

Non-developer / hybrid / cloud-only modes are **only partially
implemented** and remain **severely limited**: the plugin can still
sign you into the cloud, fetch presets, and show telemetry, but **you
cannot reliably start a print** — the printer expects MQTT commands to
carry the stock plugin's RSA signature, which we do not ship. Without
Developer Mode every `print` / `project_file` / similar command is
rejected (`err_code: 84033543`). Fully replicating that signing chain
is out of scope for an open-source project. MakerWorld task history is
likewise out of scope.

## What works

The author only owns a **P2S**, so the "Tested" column in the table
below means "exercised with a real printer"; everything else is
reviewed against the stock plugin's disassembly, the
[OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) protocol
notes, and the
[ha_bambu_lab](https://github.com/greghesp/ha-bambulab) reference
implementation. Most features are protocol-level (same SSDP / LAN MQTT
/ FTPS on every Bambu printer) so hardware differences only matter for
a handful of rows — see "Applies to" for each row and the hardware
matrix further down.

**Community help is essential.** If you use another printer model or
CPU architecture, please try the plugin and open an issue on
[github.com/ClusterM/open-bambu-networking](https://github.com/ClusterM/open-bambu-networking)
with what works, what fails, and your firmware / OS / Studio version —
that is how we turn "not tested" into documented reality. Bug reports,
regressions, and small compatibility notes all belong in
[**Issues**](https://github.com/ClusterM/open-bambu-networking/issues).

Legend:

| Mark | Meaning |
| :--: | --- |
| ✅ | Implemented and working on the listed models. "Tested" column says where the author has physically verified. |
| ⚠️ | Partial / soft-fails / needs a prerequisite (see Notes). |
| ❌ | Not implemented (see "What is not implemented" for scope rationale). |
| ➖ | Not applicable to this model family. |

The **Impl** column distinguishes:

- **Native** — the plugin speaks the same wire protocol the stock
  `bambu_networking.so` does. Drop-in behaviour.
- **Workaround** — the plugin reaches the same user-visible outcome
  over a different transport. Listed individually in
  [Workaround reference](#workaround-reference) below; all of them
  can be compiled out together with `-DOBN_ENABLE_WORKAROUNDS=OFF`.
- **Passthrough** — the plugin just forwards MQTT / REST payloads;
  Studio does the work.

### Basics (model-independent)

| Feature | Status | Impl | Notes |
| --- | :-: | --- | --- |
| SSDP discovery (LAN) | ✅ | Native | Multicast listener on `239.255.255.250:1990` + Studio `on_server_connected_`. |
| LAN MQTT telemetry (Dev Mode) | ✅ | Native | TLS to `mqtts://<ip>:8883`, user `bblp`, pass = access code. |
| Cloud MQTT telemetry | ✅ | Native | TLS to `us.mqtt.bambulab.com:8883`. Runs in parallel with LAN when signed in; not exercised during LAN-focused testing. |
| Cloud login / ticket flow | ✅ | Native | Browser → `localhost` callback → `POST /user-service/user/ticket/<T>`. Session persisted to `obn.auth.json`. |
| User presets sync / profile / avatar | ✅ | Native | Driven by Studio reading `get_user_id()`; plugin just serves the login session. |
| MQTT command signing | ❌ | — | Stock plugin carries per-install RSA keys we don't reproduce. Workaround for the user: enable Developer Mode on the printer (all LAN commands bypass signing there). |

### Printing

| Feature | Status | Impl | Notes |
| --- | :-: | --- | --- |
| LAN print (FTPS + MQTT, Dev Mode) | ✅ (tested on P2S) | Native | `STOR /cache/<name>` then `{"print":{"command":"project_file",...}}` on LAN MQTT. |
| "Send to Printer" dialog (`ft_*`) | ✅ (tested on P2S) | Workaround | `ft_*` C ABI served over FTPS (`OBN_FT_FTPS_FASTPATH`, ON by default); stock uses a proprietary port-6000 tunnel. With `OFF` Studio falls back to its internal FTP path — same file, same place, less UI polish. |
| Cloud 3MF upload to S3 | ✅ | Native | 6-step upload sequence reversed from MITM of the stock plugin. |
| `create_task` (MakerWorld entry) | ⚠️ | ❌ | Soft-fails — logs 4xx, keeps going with `task_id="0"`. Printing works; MakerWorld history and timelapse-on-printer cloud flags don't. See [MakerWorld integration](#makerworld-integration). |
| "Print from Device" (`start_sdcard_print`) | ✅ (tested on P2S) | Workaround | Stock plugin: cloud REST endpoint we can't sign. Ours: publish `project_file` on LAN MQTT for a file already on the printer. |
| AMS telemetry / mapping | ✅ | Passthrough | Studio consumes `push_status` directly. |
| Nozzle mapping / multi-extruder | ✅ (not tested) | Passthrough | All MQTT passthrough — same code path as AMS. Applies only to dual-nozzle hardware (H2C, H2D, X2D). Author has no such printer to verify. |

### Camera liveview

Camera protocol differs by model (see the hardware matrix). Both paths
share the same `libBambuSource.so` tunnel API towards Studio; the
difference is what happens inside.

| Feature | Applies to | Status | Impl | Notes |
| --- | --- | :-: | --- | --- |
| MJPEG over TLS, port 6000 | A1, A1 mini | ✅ (not tested) | Native | Same TCP-over-TLS stream the stock plugin consumes. No A-series hardware for physical verification. |
| RTSPS → MJPEG transcode, port 322 | P1S, X1 (all), P2S, H-series, X2D | ✅ (tested on P2S) | Workaround | Stock plugin pipes raw H.264 off RTSPS to Studio's `gstbambusrc` element; we terminate RTSPS inside `libBambuSource.so` (`rtspsrc ! rtph264depay ! h264parse ! h264dec ! queue ! videoconvert ! videoscale ! jpegenc ! appsink`) and hand Studio MJPEG frames via the same `Bambu_ReadSample` API the MJPEG path uses for A-series. Heavier CPU, slightly more latency (~150 ms), identical UX. |
| Cloud camera (TUTK / Agora p2p) | any printer out of LAN | ❌ | ❌ | Proprietary libraries; out of scope — use LAN/Developer Mode instead. |

### File browser (Device → Files)

All four operations run in a worker thread inside `libBambuSource.so`,
serviced against FTPS (port 990) on the printer. Same transport for
every Bambu LAN firmware, so this row is identical across models
except for the storage-root layout (see hardware matrix).

| Feature | Status | Impl | Notes |
| --- | :-: | --- | --- |
| List files (LIST_INFO) | ✅ (tested on P2S) | Workaround | FTPS `MLSD` / `LIST`; stock port-6000 CTRL protocol not reimplemented. Dev Mode required on X1 / P1S / P2S (Studio gates the UI). |
| Download from storage | ✅ (tested on P2S) | Workaround | Streaming FTPS `RETR` with 256 KB `CONTINUE` chunks. |
| Delete from storage | ✅ (tested on P2S) | Workaround | FTPS `DELE` per path. |
| 3MF plate thumbnails (`Metadata/plate_*.png`) | ⚠️ | Workaround | Opens the archive via FTPS `RETR`, inflates the plate PNG with zlib. Flaky on very large archives — see [PrinterFileSystem](#printerfilesystem-sdusb-browser-ui-in-studio). |
| Timelapse / video thumbnails | ✅ (tested on P2S) | Workaround | Sidecar `<stem>.jpg` fetch. **P2S firmware doesn't write sidecars** — panel falls back to Studio's default icon. A-series and X1/P1S do produce them. |

### Status / Device tab

| Feature | Applies to | Status | Impl | Notes |
| --- | --- | :-: | --- | --- |
| Current-print thumbnail cover | All models | ✅ (tested on P2S) | Mixed | Covers both runtime paths: **(a)** LAN-only prints where `project_id=profile_id=subtask_id="0"` — the plugin rewrites those to synthetic `"lan-<fnv>"` ids on every `push_status` (workaround, pushes Studio off its SD-card placeholder branch); **(b)** cloud-initiated subtasks whose real ids are still attached after unbinding — here the plugin answers `get_subtask_info(<real_id>)` instead of routing through Bambu's CDN (regular stock replacement). Both paths share the same backend: localhost HTTP server pulls `/cache/<subtask>.gcode.3mf` over FTPS, unpacks `Metadata/plate_N.png`, serves it as `thumbnail.url = http://127.0.0.1:<port>/cover/…`. Same image HA uses. **Linux only: requires [`patches/bambustudio-statuspanel-thumbnail.patch`](patches/bambustudio-statuspanel-thumbnail.patch) applied to Studio** — `wxStaticBitmap` under wxGTK doesn't fire `wxEVT_PAINT` on `Bind()`'d handlers, so `PrintingTaskPanel::paint()` never runs and neither the LAN cover nor stock cloud covers ever show. |
| Firmware version panel (Device → Update) | All models | ✅ (tested on P2S) | Mixed | `bambu_network_get_printer_firmware` re-synthesises the "firmware list" Studio expects from the MQTT frames the printer already sends (`info.module[]` replies + `push_status.upgrade_state.new_ver_list`). That populates the Update tab with current per-module versions (OTA, AMS, AHB, …), stock-style. When the printer advertises a newer version the "current → new" arrow and the green "update available" badge appear. |
| Firmware Release Notes dialog | All models | ✅ (tested on P2S) | Workaround | Shown when a new version is advertised. Description text is synthesised locally with a link to the model-specific page on [bambulab.com/support/firmware-download](https://bambulab.com/en/support/firmware-download/all); we can't reach Bambu's cloud changelog API without login. If no new version is advertised the dialog is empty — which matches stock behaviour in the same situation. |
| Start firmware update (Update button) | All models | ✅ (tested on P2S) | Passthrough | Button publishes `{"upgrade":{"command":"upgrade_confirm"}}` on LAN MQTT. The printer already knows which OTA package it advertised and downloads it from Bambu's CDN itself — the plugin doesn't need to supply a URL. |
| Flash an arbitrary version (version picker) | — | ❌ | — | Stock plugin lets you pick any older/newer OTA from Bambu's cloud catalogue; that catalogue is auth-gated and we don't have cloud signing. Only the printer-advertised version is flashable. |

### Firmware / model-specific fix-ups

| Feature | Applies to | Status | Impl | Notes |
| --- | --- | :-: | --- | --- |
| `push_status` `home_flag` SDCARD-bits rewrite | **P2S only** (USB-only storage) | ✅ (tested on P2S) | Workaround | Only triggers when the printer actually reports `NO_SDCARD`; HAS/ABNORMAL/READONLY pass through untouched. Needed so Studio's Send-to-Printer UI doesn't grey out on a printer whose external storage is USB-only. H-series / X2D probably need this too, but no hardware available to confirm. |
| `push_status` `ipcam.file` block injection | P2S, A-series, older X1/P1 firmware | ✅ (tested on P2S) | Workaround | Only triggers when the block is absent; firmware that already advertises it is passed through. Makes Studio open the file-browser tunnel in the first place. |

### Platforms

| Target | Status | Notes |
| --- | :-: | --- |
| Linux x86_64 | ✅ | Primary development and test target. |
| Linux aarch64 | ✅ (not tested) | Cross-compile toolchain in `cmake/toolchains/`; not verified on-device yet. |
| Windows / macOS | ❌ | Architected for but not built — ABI uses `std::string`/`std::map`/`std::function` across the `dlsym` boundary, which needs MSVC STL on Windows and Xcode libc++ on macOS. Plus Studio enforces code-signing publisher matches on those OSes. |

### Hardware reality (what each model actually does)

Use this to figure out which rows above apply to your printer. The
plugin doesn't hard-code any of this — it auto-detects storage layout
and camera transport at runtime and speaks the right protocol per
printer. The table exists only to tell you *which* cell of the tables
above applies.

| Printer | Camera protocol | Storage | Nozzles | AMS | Verified here |
| --- | --- | --- | :-: | --- | :-: |
| A1 | MJPEG/TLS 6000 | microSD | 1 | AMS Lite (optional) | — |
| A1 mini | MJPEG/TLS 6000 | microSD | 1 | AMS Lite (optional) | — |
| P1P | — (no built-in camera; optional BL-P004 addon streams MJPEG) | microSD | 1 | AMS (optional) | — |
| P1S | RTSPS 322 | microSD | 1 | AMS (optional) | — |
| X1 / X1C / X1E | RTSPS 322 | microSD | 1 | AMS (optional) | — |
| **P2S** | RTSPS 322 | **USB only** (no SD slot) | 1 | AMS 2 Pro (optional) | ✅ |
| H2D | RTSPS 322 | USB | 2 | AMS 2 Pro (optional) | — |
| H2C | RTSPS 322 ¹ | USB ¹ | 2 | AMS 2 Pro (optional) ¹ | — |
| H2S ¹ | RTSPS 322 ¹ | USB ¹ | 1 ¹ | AMS 2 Pro (optional) ¹ | — |
| X2D ¹ | RTSPS 322 ¹ | USB ¹ | 2 ¹ | AMS 2 Pro (optional) ¹ | — |

¹ Specs for newer / not-yet-widely-available models are best-effort
from public product pages and Studio's built-in model enum; the
protocol parts (camera port, FTPS layout) are auto-detected at
runtime, so the plugin itself doesn't rely on the table being exact.
Corrections welcome from anyone with the actual hardware.

### Model specifics

Storage on the printer depends on the model: X1 / P1P / A1 (and A1
mini) use a microSD slot, while **P2S has a USB port instead** (no SD
slot). The plugin auto-detects which mount is available and uploads
to the right path:

- X1 / P1P / A1: FTPS exposes `/sdcard/` and (sometimes) `/usb/`; the
  `ft_*` fast-path probes both with `CWD` and reports whatever answers.
- P2S: FTPS has **no** `/sdcard` or `/usb` subdirectory — the FTPS root
  *is* the USB stick, so files sit right at `/`. The plugin detects
  this (both probes fail, root is reachable) and still advertises
  `"sdcard"` to Studio so the Send-to-Printer radio shows "External
  Storage"; uploads go to `/<dest_name>` (no prefix).

There's one more P2S-specific twist: the printer reports `home_flag`
with `NO_SDCARD` bits because, strictly speaking, it has no SD slot.
Studio's DeviceManager parses those bits into `DevStorage::SdcardState`,
which turns the Send button grey and shows a red "Please check the
network …" tile in the Device/Storage panel. Since we *can* write to
the USB over FTPS, the plugin rewrites bits 8-9 of `home_flag` from
`NO_SDCARD` (00) to `HAS_SDCARD_NORMAL` (01) on every `push_status`
frame. Real error states (`ABNORMAL`=10, `READONLY`=11) and printers
that already report `HAS_SDCARD` are passed through unchanged.

`⚠️ soft-fail` means the step logs a warning and continues with `task_id="0"`.
Printing works; the job does not show up in the cloud's job history and the
timelapse/record-on-printer cloud flags have no effect. Local print records
on the printer itself still work.

## Workaround reference

Everything marked **Workaround** in the table above is a deliberate
non-stock path chosen to get a Developer-Mode printer usable without
the proprietary secrets the stock plugin carries. All of them are
gated behind one CMake flag, `-DOBN_ENABLE_WORKAROUNDS` (default
`ON`). Build with `=OFF` if you want a strict drop-in that only
exposes the native paths from the "What works" table above — Studio
will transparently lose the affected features (Send greys out on P2S,
file browser stays empty on Dev-Mode printers, "Print from Device"
errors out with "start_sdcard_print disabled", RTSPS liveview refuses
to open, etc.), but nothing half-done runs at runtime.

| Workaround | What the stock plugin does | What we do | Why this is acceptable | Trade-offs |
| --- | --- | --- | --- | --- |
| `home_flag` SDCARD-bits rewrite | Printer reports its real `SdcardState` from the hardware. On P2S / A-series firmware without an SD slot the bits read `NO_SDCARD`. | On every LAN `push_status` we check bits [8:9]. If they are `00` (NO_SDCARD) and `ipcam` is present we flip them to `01` (HAS_SDCARD_NORMAL) in-place on the JSON text. `ABNORMAL` / `READONLY` and printers that already have the bit are passed through untouched. | We *can* read/write the USB stick over FTPS on P2S, so from Studio's workflow POV the storage exists and is healthy — the bit is lying. | If a real "SD card missing" ever reports as `NO_SDCARD` on P1/A1 the UI would mistakenly enable Send; in practice those printers have a real slot and use the ABNORMAL code path instead. |
| `ipcam.file` block injection | Firmware with a file browser advertises `ipcam.file = {"local":"local",…}`; Studio keys the MediaFilePanel off that. | When the printer omits the block we inject `{"local":"local","remote":"none","model_download":"enabled"}` into the ipcam object. Present blocks pass through unchanged. | Without this Studio short-circuits MediaFilePanel with "Browsing file in storage is not supported in current firmware" and never calls `Bambu_StartStreamEx(CTRL_TYPE)` on us. | The plugin advertises file-browser capability even for firmware revisions where it doesn't exist natively; the opposite side (our CTRL bridge) is all FTPS so it still works. |
| PrinterFileSystem CTRL bridge | Port-6000 proprietary CTRL protocol served by the stock `libBambuSource.so`: Bambu-framed binary JSON and per-command media blobs. | In our `libBambuSource.so` a worker thread per tunnel answers `LIST_INFO` / `SUB_FILE` / `FILE_DOWNLOAD` / `FILE_DEL` / `REQUEST_MEDIA_ABILITY` / `TASK_CANCEL` by translating each into FTPS (`MLSD`, `RETR`, `DELE`) on the same TLS endpoint we use for LAN print uploads. | FTPS is universal across Bambu LAN firmware and survives Developer Mode. The CTRL protocol isn't documented anywhere we've found. | `FILE_UPLOAD` is not implemented (Studio uses `ft_*` for uploads anyway). `SUB_FILE` re-downloads the whole `.3mf` for every thumbnail / metadata request: OK on LAN, wasteful vs. a native CTRL channel that could stream entries. |
| 3MF thumbnail extraction | Stock CTRL protocol likely streams just the requested entry out of the `.3mf` on the printer. | We fetch the whole archive into memory, parse the central directory, and `inflate` the target PNG (`Metadata/plate_1.png` / `plate_no_light_1.png`) or a requested entry with zlib raw-deflate. | No external ZIP dependency; archives fit in RAM for any reasonable print job. | Large models (~10+ MB) hit the wire once per thumbnail tile. Parser is minimal — compressed entries only via DEFLATE; we don't handle stored-with-encryption or ZIP64. |
| Camera liveview (RTSPS) | X1/P1S stock `libBambuSource.so` exposes the H.264 RTSPS stream directly; Studio's `gstbambusrc` element decodes it. | Our `libBambuSource.so` builds a GStreamer pipeline that terminates RTSPS internally (`rtspsrc ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! videoscale ! jpegenc ! appsink`) and hands Studio MJPEG frames via `Bambu_ReadSample`, the same wire format the port-6000 MJPEG path uses for P1/A1. | We can lean on the MJPEG reader Studio already ships instead of matching the stock H.264 frame metadata and GStreamer plumbing byte-for-byte. | Extra decode + re-encode on the host CPU; 720p @ 30 fps consumes ~15 % of a mid-tier desktop. Higher end-to-end latency (~100–300 ms more) than passing H.264 through untouched. |
| `start_sdcard_print` over LAN MQTT | Stock plugin hits a cloud REST endpoint (`start_sdcard_print`) that signs and relays the command via the cloud tunnel. | We publish `{"print":{"command":"project_file", "url":"ftp://<path>", …}}` directly on the active LAN MQTT session. | The cloud endpoint requires MakerWorld signing we don't reproduce, and Developer Mode printers have no cloud route to receive it on anyway. LAN MQTT is what the firmware actually consumes. | No cloud task record. `task_id` / `subtask_id` / `project_id` are all `0`, which a very old firmware could refuse if it ever validates them (none observed). |
| Firmware info synthesis (`get_printer_firmware`) | Stock plugin serves `GET /v1/iot-service/api/slicer/resource/printer/firmware?dev_id=…` against the Bambu cloud, returning a JSON envelope (current/new version per module + release notes + binary URL) that Studio's `UpgradePanel` and `ReleaseNoteDialog` render. | The plugin doesn't call the cloud at all. Instead the agent maintains a `DeviceFw` cache keyed by `dev_id` that harvests versions from every MQTT frame the printer already sends: `info.command=get_version` replies (module array with `name`/`sw_ver`/`sn`/`loader_ver`) and `push_status.upgrade_state.new_ver_list`. `render_firmware_json(dev_id)` then emits the same envelope Studio expects — current versions populate the Update panel, advertised newer versions light up the "update available" banner, the description links to `bambulab.com/en/support/firmware-download/all`. The actual flash is a passthrough: Studio's "Update" button publishes `{"upgrade":{"command":"upgrade_confirm"}}` on LAN MQTT, and the printer fetches the binary from Bambu's CDN itself. | The cloud firmware catalogue requires a signed request with an `accessToken` for a linked account and answers with HTML behind Cloudflare from an untrusted client (no stable public JSON). What we need (current version, advertised new version, a way to flash the advertised one) is already on the wire in plaintext via MQTT, so we re-synthesise the envelope locally. | No cross-version history — you can only flash what the printer is currently advertising, not an older or a beta OTA from the cloud catalogue. Release Notes text is a short auto-generated stub with an external link, not the full changelog. No effect when no new version is advertised (Release Notes dialog empty — same as stock). |
| `ft_*` FTPS fastpath (`OBN_FT_FTPS_FASTPATH`, also default ON) | Port-6000 proprietary tunnel for the `FileTransfer` ABI (Send-to-Printer dialog + eMMC pre-flight + media-ability probes). | FTPS-backed `STOR` for uploads + `CWD /sdcard` / `CWD /usb` probe for media-ability answers. TUTK/Agora URLs still return `FT_EIO`. | Same tunnel the LAN print path already uses; gives the richer dialog sequence (ability probe → storage picker → percent progress) without a new transport. | `emmc` is never advertised (Bambu firmware reserves it for system files). Fall-back path via Studio's internal FTP upload still puts the file in the same place if you turn this OFF. See [FileTransfer module](#filetransfer-module-ft_-c-abi). |
| Current-print thumbnail cover | Stock plugin gets `project_id`/`profile_id`/`subtask_id` back from the cloud task it created, and `get_subtask_info` returns a `thumbnail.url` served out of Bambu's CDN. | Two runtime paths share the same backend: **(a)** LAN-only prints whose ids come through as `"0"` — `MachineObject::is_sdcard_printing()` would otherwise route the Device-panel thumbnail to a static SD-card placeholder, so we rewrite the zeros to synthetic `"lan-<fnv>"` ids on every `push_status` (this part is the workaround). **(b)** Cloud-initiated subtasks that still carry their real numeric ids (e.g. after unbinding a cloud-paired printer) — Studio calls `get_subtask_info(<real_id>)` exactly like it would against stock; we just serve that ourselves instead of the CDN. Both paths land in the same backend: a single-thread HTTP server on `127.0.0.1:<random>`, FTPS-download of `/cache/<subtask>.gcode.3mf`, unpack `Metadata/plate_N.png` via zlib raw-inflate and serve as `image/png` behind the URL we return from `get_subtask_info`. On Linux this additionally requires [`patches/bambustudio-statuspanel-thumbnail.patch`](patches/bambustudio-statuspanel-thumbnail.patch) against Studio: `PrintingTaskPanel::set_thumbnail_img` relies on a custom `wxEVT_PAINT` handler which `wxStaticBitmap` doesn't dispatch under wxGTK (native `GtkImage` wrapper), so without the patch no cover — LAN or cloud — ever becomes visible on screen. | wxWebRequest on libsoup3 / Linux rejects `file://` URLs; a loopback HTTP server is the minimum that still uses the same code path cloud thumbnails take. File is cached under `temp_directory_path()/obn-covers/` so reprint requests don't re-download the archive. `/cache/` is the drop-zone for both `start_local_print_with_record` and the printer's own cloud-download, so one lookup covers both origins. | Extra 2-20 MB FTPS `RETR` once per print, a socket listener on loopback for the lifetime of the plugin, and a 15 s wait budget inside the HTTP server while the FTPS worker is still fetching. If the .3mf is gone from `/cache/` the server returns 503 → Studio falls back to its broken-image icon (clickable to retry). |

All of these except the last are gated by `OBN_ENABLE_WORKAROUNDS`.
The `ft_*` fastpath has its own flag because stock-compatible builds
may want to keep Send-to-Printer's UI progress working while turning
everything else off.

## Cloud sign-in

Even though every print path is local, the plugin still implements
Bambu's cloud account login. The reason is that Studio's UI and preset
machinery are heavily wired to a logged-in `user_id`: without a session
a number of features quietly degrade. With a session they behave as
they did under the stock plugin.

**What login gives you:**

After sign-in, Studio may show a **Synchronization** dialog asking
whether to pull personal data from Bambu Cloud. The stock UI lists
exactly three categories — the same ones the account machinery is built
around:

1. **Process presets** (print / process profiles)
2. **Filament presets**
3. **Printer presets** (machine profiles)

- **User presets sync.** On startup Studio calls
  `preset_bundle->load_user_presets(agent->get_user_id(), ...)`. The
  three kinds above (plus AMS material mappings and anything else tied
  to the account) are loaded from `<config_dir>/user/<uid>/` and pushed
  to the cloud (`/v1/user-service/my/preferences`) when you change
  them. Without a session Studio falls back to `DEFAULT_USER_FOLDER_NAME`
  and your custom profiles are invisible.
- **Avatar and nickname in the sidebar.** The "Sign in" button turns
  into your profile widget.
- **Cloud device list / bind status.** Studio queries
  `/v1/iot-service/api/user/bind` with your `accessToken` to show which
  printers are paired with your account. Used for the device picker
  and the "Bind to cloud" dialog; LAN discovery still works without it.
- **Filament CDN metadata.** Some OEM filament entries pull extra data
  (colour swatches, recommended profiles) from authenticated cloud
  endpoints. Missing these shows up as a generic filament name.

**What login does _not_ give you:**

- It does **not** enable MQTT command signing — the printer still
  refuses unsigned commands outside Developer Mode regardless of who
  you are signed in as (see
  [MQTT command signing](#mqtt-command-signing-err_code-84033543)).
- It does **not** register your prints on MakerWorld. `create_task`
  soft-fails (see the table below).
- It does **not** unlock the TUTK/Agora cloud-p2p transports. Those
  need a separate proprietary library.

**How the flow works:**

1. Studio opens the account portal in the system browser or its
   embedded wxWebView. The URL Studio uses is composed from
   `web_host(region)` (which the plugin reports) and a `callback`
   query string pointing at `http://localhost:<port>/`.
2. After the user authenticates, the portal redirects the browser to
   `http://localhost:<port>/?ticket=<T>&redirect_url=...`. Studio's
   in-process HTTP server picks up the ticket.
3. Studio calls into the plugin: `bambu_network_get_my_token(ticket)`
   runs `POST /v1/user-service/user/ticket/<T>`; we get back
   `{accessToken, refreshToken, expiresIn, ...}`.
4. Studio then calls `bambu_network_get_my_profile` → we run
   `GET /v1/user-service/my/profile` with `Authorization: Bearer …`.
5. Studio assembles `{"data":{"token":"…","refresh_token":"…",
   "expires_in":"…","user":{"uid":"…","name":"…", …}}}` and feeds it
   back via `bambu_network_change_user`. `Agent::apply_login_info`
   accepts both that envelope and the raw API shape.
6. The session is persisted to `<config_dir>/obn.auth.json` with
   mode 0600. On next Studio start the plugin loads it and reports
   `is_user_login() == true` without prompting again. The plugin
   rotates the access token via
   `POST /v1/user-service/user/refreshtoken` as the expiry approaches.
7. Logout (Studio menu → "Log Out") calls `bambu_network_user_logout`,
   which clears `obn.auth.json` and the in-memory session.

**Running without sign-in** is fully supported: go straight to 
"Device → Connect via LAN with access code",
and LAN printing / camera / discovery / FTPS all work. You'll just
lose the Studio features in the second list above.

## What is **not** implemented

These are deliberate scope choices, not bugs. Each one is either out of
reach without cryptographic secrets we don't have, or hidden behind a
proprietary transport we haven't reversed:

### MQTT command signing (`err_code: 84033543`)

Newer firmware requires every MQTT command sent to the printer to carry a
per-installation RSA signature. The stock plugin ships the keys as
obfuscated data. Without those, the printer rejects commands with
"MQTT Command verification failed" on screen.
**Workaround:** put the printer in Developer Mode (above), which disables
verification.

### MakerWorld integration

`POST /v1/user-service/my/task` (create cloud task record) and the
related tracking endpoints all require the signing chain above plus
client-side certification headers (`x-bbl-app-certification-id`,
`x-bbl-device-security-sign`) that the stock plugin computes in native
code. We log the 40x and continue, so the print itself completes.
**Scope:** out of scope permanently — this is a closed network service.

### FileTransfer module (`ft_*` C ABI)

Used by Studio for the "Send to Printer" dialog (upload without
printing), the eMMC pre-flight check in the regular Print job, and a
handful of media-ability queries. The proprietary plugin serves these
over a port-6000 TLS tunnel with a JSON command framer we haven't
reimplemented.

We have two modes, toggled at configure time with
`-DOBN_FT_FTPS_FASTPATH=ON|OFF` (default **ON**):

**`OBN_FT_FTPS_FASTPATH=ON` (default).**
For `bambu:///local/*` URLs the plugin serves the ABI over the same
FTPS connection the print job uses:

- `cmd_type=7` (media ability) is answered by probing `CWD /sdcard`
  and `CWD /usb` on the printer. X1/P1P/A1 and first-gen A1 mini have
  the SD-card mount; P2S has a USB port instead. Printers with both
  will get both back. If *neither* path answers but the FTPS login
  itself succeeded (the P2S case), the fastpath treats the FTPS root
  as the storage mount and reports `["sdcard"]` so Studio's picker
  lights up. `emmc` is never reported — on Bambu firmware that storage
  is for system files, not user uploads.
- `cmd_type=5` (upload) runs `STOR /<dest_storage>/<dest_name>` with
  byte-level progress forwarded to Studio as `{"progress":N}` via
  `msg_cb` (skipping 99, which Studio reserves as a timeout tripwire).
  When the tunnel is in root-is-storage mode the `STOR` target is just
  `/<dest_name>`.
- TUTK/Agora URLs still return `FT_EIO` — we don't speak the cloud
  p2p transport.

Net effect: the "Send to Printer" dialog walks through the same UI
states as with the stock plugin (Reading storage → picker → sending
with real % progress), the eMMC pre-flight in the regular Print job
gets a clean `["sdcard"]` / `["usb"]` answer and Studio stops logging
a fallback every time.

**`OBN_FT_FTPS_FASTPATH=OFF`.**
Every `ft_*` entry point is a polite-failure stub that fires its
callback synchronously with `FT_EIO` and a "fall back to FTP" message.
Studio's internal fallback kicks in and the 3mf is uploaded through
`bambu_network_start_send_gcode_to_sdcard`, which also uses FTPS.
Same file lands in the same place — the UI just skips the media-
ability step and the per-percent progress comes from the fallback
instead.

**Scope:** the fastpath is a deliberate shortcut, not a clean
reimplementation of the proprietary port-6000 protocol. If you need
MakerWorld-style project metadata, eMMC-as-primary-storage, or the
"Send to multiple machines" batch UI to work end-to-end, you'll still
be limited to what the fallback path supports.

### PrinterFileSystem (SD/USB browser UI in Studio)

Studio's MediaFilePanel opens a port-6000 tunnel through `libBambuSource.so`
and then switches it to a CTRL channel via `Bambu_StartStreamEx(CTRL_TYPE)`.
From there it sends JSON request/response messages (`LIST_INFO`, `SUB_FILE`,
`FILE_DOWNLOAD`, `FILE_DEL`, `REQUEST_MEDIA_ABILITY`, `TASK_CANCEL`) that
Studio renders as the Device → Files panel (timelapses, camera recordings,
printed models with thumbnails).

`libBambuSource.so` does **not** speak the proprietary CTRL wire protocol;
instead it runs a worker thread per tunnel that services those JSON requests
over **FTPS** (the same service we use for "Print via LAN" uploads). That
gives Studio the full file browser on every printer we can reach over TLS
on port 990, regardless of whether the firmware implements the CTRL protocol
itself.

Supported operations:

- `REQUEST_MEDIA_ABILITY` — probe `/sdcard` and `/usb`; if neither exists we
  treat the FTPS root as the storage mount (P2S-style USB-only) and report
  it as `"sdcard"` to Studio.
- `LIST_INFO` — `MLSD` (falling back to `LIST`) on:
  `<prefix>/timelapse/` for the timelapse tab,
  `<prefix>/ipcam/` for the manual video tab,
  `<prefix>/` for the model tab.
  Files are filtered by extension (`.mp4`, `.3mf`, `.gcode.3mf`).
- `SUB_FILE` thumbnails — for timelapses we fetch the sidecar `.jpg`; for
  `.3mf` files we download the archive, parse the central directory, and
  `inflate` `Metadata/plate_1.png` (or `plate_no_light_1.png`) using zlib's
  raw deflate — no external ZIP dependency.
- `FILE_DOWNLOAD` — streaming `RETR` with 256 KB chunks, each chunk
  delivered as a separate `CONTINUE` reply so Studio's progress bar
  updates smoothly. The final reply is `SUCCESS`.
- `FILE_DEL` — `DELE` per path (modern form `{"paths":[…]}` only; the
  legacy `{"delete":[…]}` form isn't used by current Studio releases).
- `TASK_CANCEL` — marks a sequence number; the worker aborts the relevant
  multi-chunk response cleanly at the next check point.

`FILE_UPLOAD` is intentionally not implemented here: Studio's
"Send to Printer" dialog uses the separate `ft_*` ABI (see above), which
already has an FTPS fastpath.

For printers whose firmware **does** send `ipcam.file` in its MQTT
`push_status` (X1 / P1S class), the plugin passes it through untouched —
those printers speak the real CTRL protocol to their stock BambuSource,
and we don't want to clobber a capability we don't match exactly.

For printers that **don't** advertise `ipcam.file` (P2S, A-series, some
firmware revisions) the plugin injects
`"file":{"local":"local","remote":"none","model_download":"enabled"}`
into every LAN `ipcam` block. That's what makes Studio open the
port-6000 tunnel in the first place; without it, MediaFilePanel would
short-circuit with "Browsing file in storage is not supported in current
firmware." and never give us a CTRL channel to service.

**Storage root layout:**

| Printer | FTPS layout                                  | Our prefix    |
| ------- | -------------------------------------------- | ------------- |
| X1 / P1 | `/sdcard/{,timelapse,ipcam,…}`               | `/sdcard`     |
| A1      | `/sdcard/…` (microSD)                        | `/sdcard`     |
| P2S     | `/timelapse`, `/ipcam`, `/*.gcode.3mf` on /  | `""` (root)   |

The P2S variant uses the "FTPS root == USB stick" convention from our
`ft_*` implementation. We re-advertise it as `"sdcard"` because Studio's
storage dropdown treats anything that isn't `"emmc"` as removable media.

**Limitations:** Studio still refuses to *open* the browser in LAN Only
Mode for X1/P1S/P2S ("Browsing file in storage is not supported in LAN
Only Mode") — that check lives inside Studio itself, ahead of the
plugin. Developer Mode (which is what we target anyway) bypasses it.

### TUTK / Agora cloud p2p transports

Used by Studio as a fallback when LAN is unavailable and for the X1 family
in non-LAN mode. Closed-source proprietary libraries (`libBambuTUTK` /
`libBambuAgora`) implement them. We don't wrap either.
**Scope:** out of scope — use LAN/Developer Mode instead.

### Windows / macOS builds

ABI uses `std::string`/`std::map`/`std::function` across the `dlsym`
boundary, which means matching MSVC STL on Windows and Xcode libc++ on
macOS. Studio also enforces code-signing publisher matches there.
**Scope:** architected for but not built; Linux x86_64 and aarch64 ship.

## Supported platforms

- Linux x86_64 (primary target, gcc 13+/15+, libstdc++ new C++11 ABI).
- Linux aarch64 (cross-compile-friendly, see `cmake/toolchains/`).

Windows and macOS are architected for but not yet built: the ABI uses
`std::string`/`std::map`/`std::function` across the boundary, which means we
would need to match MSVC's STL on Windows and Xcode/libc++ on macOS. Studio
also enforces a matching code-signing publisher on those OSes unless the user
sets `ignore_module_cert = 1`.

## Version compatibility

Studio compares the plugin's `bambu_network_get_version()` against its own
`SLIC3R_VERSION` using the first 8 characters (`MAJOR.MINOR.PATCH`).

Configure the plugin version at CMake time with `-DOBN_VERSION=...`. Default is
tracked to match the latest Bambu Studio release. Example:

```sh
cmake -S . -B build -DOBN_VERSION=02.05.02.99
cmake --build build -j
```

The currently shipped AppImage `v02.05.02.51` expects prefix `02.05.02`. A
source build of BambuStudio from the main branch (as of writing) expects
`02.05.03`.

## Build options

| Option | Default | What it does |
| --- | --- | --- |
| `OBN_VERSION` | tracked to latest Bambu Studio release | The version string `bambu_network_get_version()` returns. Studio checks only the first 8 chars (`MAJOR.MINOR.PATCH`). |
| `OBN_ENABLE_WORKAROUNDS` | `ON` | Master switch for every non-stock code path (see [Workaround reference](#workaround-reference)): `home_flag` / `ipcam.file` rewrites, PrinterFileSystem FTPS bridge in `libBambuSource.so`, RTSPS→MJPEG transcode, `start_sdcard_print` over LAN MQTT. With `OFF` the plugin is a strict drop-in: same wire protocols the stock plugin uses and nothing else. Studio will transparently lose every workaround-backed feature (file browser stays empty, Send greys out on P2S, etc.) but nothing half-done runs at runtime. |
| `OBN_FT_FTPS_FASTPATH` | `ON` | Serve the `ft_*` C ABI over FTPS for LAN URLs (media-ability probe + STOR-based upload with live progress). Turn `OFF` to keep every `ft_*` call as a polite-failure stub; Studio will fall back to its internal FTP send path. Both modes land the file in the same place on the printer — see [FileTransfer module](#filetransfer-module-ft_-c-abi) for the trade-offs. Orthogonal to `OBN_ENABLE_WORKAROUNDS`. |

## Install

```sh
cmake --install build --prefix ~/.config/BambuStudio
```

This copies:

- `libbambu_networking.so` to `~/.config/BambuStudio/plugins/`
- `libBambuSource.so` stub to `~/.config/BambuStudio/plugins/`
- `liblive555.so` stub to `~/.config/BambuStudio/plugins/`
- `ota/plugins/network_plugins.json` metadata so Studio's updater leaves it
  alone.

Then edit `~/.config/BambuStudio/BambuStudio.conf`, section `"app"`:

```json
"installed_networking": "1",
"update_network_plugin": "false"
```

## Logging

The plugin writes a printf-style log of every ABI call and every
MQTT / FTPS / HTTP action it takes. This is the primary
troubleshooting tool — when something misbehaves, check the log first.

**Where it goes.** By default into Studio's log directory next to
Studio's own logs, under the name `obn.log`:

```
~/.config/BambuStudio/log/obn.log
```

If Studio doesn't pass a log directory (rare), the plugin falls back
to `/tmp/obn.log`.

**Log levels.** `trace < debug < info < warn < error`. The default
is `debug`, which is chatty enough to diagnose most issues without
being unreadable. `trace` adds per-MQTT-frame and per-FTPS-command
dumps.

**Configuration via environment variables** (read once, on first log
call — so export them *before* launching Studio):

| Variable | Default | Effect |
| --- | --- | --- |
| `OBN_LOG_FILE` | `<studio-log-dir>/obn.log` | Absolute path to the log file. Set explicitly to redirect; set to `/dev/null` to silence the file sink. |
| `OBN_LOG_LEVEL` | `debug` | Threshold. One of `trace`, `debug`, `info`, `warn`, `error`, `off`. |
| `OBN_LOG_STDERR` | `1` | When `1`, also echo every line to Studio's stderr (visible in the terminal you launched Studio from). Set to `0` if you just want the file. |

Example — quietest useful setup, file only:

```sh
OBN_LOG_LEVEL=info OBN_LOG_STDERR=0 bambu-studio
```

Example — full wire-level trace to a specific file:

```sh
OBN_LOG_LEVEL=trace OBN_LOG_FILE=/tmp/obn-session.log bambu-studio
```

**Line format:**

```
[LVL] [TID] file.cpp:line func_name: message
```

where `TID` is the last 6 digits of the OS thread id — useful to
correlate MQTT background-thread activity (usually one stable TID per
printer) with Studio main-thread ABI calls.

**Secrets — be careful before sharing a log.** The plugin does *not*
currently auto-redact secrets. At `debug` and `trace` levels the log
can contain: printer access codes (MQTT password), session bearer /
refresh tokens from `obn.auth.json`, raw MQTT `push_status` payloads
(which include serial numbers and filament metadata), FTPS file
paths, and device IPs. Before pasting a log into a bug report, grep
out `access_code`, `Bearer`, `accessToken`, `refreshToken`,
`password`, and your printer's serial / WAN IP. (Tightening this up
into a proper redacting logger is on the TODO list.)

## License

MIT — see [LICENSE](LICENSE).
