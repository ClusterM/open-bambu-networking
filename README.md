# open-bambu-networking

Open-source drop-in replacement for Bambu Studio's proprietary `bambu_networking`
plugin, based on reverse-engineered protocols from
[OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) and reference
implementations from
[bambulabs_api](https://github.com/acse-ci223/bambulabs_api) and
[ha_bambu_lab](https://github.com/greghesp/ha-bambulab).

> **Status:** Phase 9 (file browser + Print-from-Device). SSDP
> discovery, live telemetry, LAN printing, cloud ticket sign-in (for
> user presets / profile), camera (MJPEG for P1/A1, RTSPS→MJPEG
> transcode for X1/P1S/P2S), Device→Files browser with downloads and
> deletes, and "Print from Device" over LAN MQTT all work on a P2S
> in Developer Mode. Cloud print upload to S3 works but the final
> `create_task` step cannot be signed, so MakerWorld task history is
> not recorded. Everything that isn't a stock-compatible wire path
> (ipcam/home_flag rewrites, PrinterFileSystem FTPS bridge, RTSPS
> transcode, Print-from-Device-over-LAN) is gated behind the
> `OBN_ENABLE_WORKAROUNDS` CMake flag — see [Workaround
> reference](#workaround-reference). The printer itself must be in
> **Developer Mode** (aka LAN-only mode) for LAN commands to
> succeed — see [Developer Mode requirement](#developer-mode-requirement)
> below.

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

1. On the printer: Settings -> General -> LAN Only Mode -> enable.
2. In Bambu Studio: Device -> Connect via LAN with access code.

In this mode the printer skips MQTT verification and accepts plain LAN
commands. All LAN features of the plugin (discovery, telemetry, printing,
filename browsing, file transfer, camera) work normally.

Non-developer / hybrid / cloud-only modes are **not supported** and will
not be supported: replicating the proprietary signature chain is out of
scope for an open-source plugin. MakerWorld task history is likewise out
of scope.

## Cloud sign-in

Even though every print path is local, the plugin still implements
Bambu's cloud account login. The reason is that Studio's UI and preset
machinery are heavily wired to a logged-in `user_id`: without a session
a number of features quietly degrade. With a session they behave as
they did under the stock plugin.

**What login gives you:**

- **User presets sync.** On startup Studio calls
  `preset_bundle->load_user_presets(agent->get_user_id(), ...)`. Your
  per-account filament / print / machine presets, custom profiles,
  AMS material mappings, and anything else stored under the account
  are loaded from `<config_dir>/user/<uid>/` and pushed to the cloud
  (`/v1/user-service/my/preferences`) when you change them. Without a
  session Studio falls back to `DEFAULT_USER_FOLDER_NAME` and your
  custom profiles are invisible.
- **Avatar and nickname in the sidebar.** The "Sign in" button turns
  into your profile widget.
- **Cloud device list / bind status.** Studio queries
  `/v1/iot-service/api/user/bind` with your `accessToken` to show which
  printers are paired with your account. Used for the device picker
  and the "Bind to cloud" dialog; LAN discovery still works without it.
- **Filament CDN metadata.** Some OEM filament entries pull extra data
  (colour swatches, recommended profiles) from authenticated cloud
  endpoints. Missing these shows up as a generic filament name.
- **Print history / account email** shown in "About".
- **Refresh-token rotation.** The plugin keeps the session alive in
  the background so Studio doesn't kick you back to the login screen
  after ~24h.

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

**Running without sign-in** is fully supported: close the login
dialog, go straight to "Device → Connect via LAN with access code",
and LAN printing / camera / discovery / FTPS all work. You'll just
lose the Studio features in the second list above.

## What works

Legend for the per-model columns:

| Mark | Meaning |
| :--: | --- |
| ✅ | Implemented **and exercised** against a real printer of that family. |
| ✅ (not tested) | Implemented; should work in theory but **not tested** on that model. The author only has a P2S, so everything else relies on code inspection and reference implementations. |
| ⚠️ | Partially works / soft-fails (see Notes). |
| ❌ | Not implemented (see "What is not implemented" for scope rationale). |
| ➖ | Not applicable to this model (e.g. MJPEG liveview on a printer that only serves RTSPS). |

The **Impl** column distinguishes three kinds of feature:

- **Native** — the plugin speaks the same wire protocol the stock
  `bambu_networking.so` does. Drop-in behaviour.
- **Workaround** — the plugin reaches the same user-visible outcome
  over a different transport. Listed individually in
  [Workaround reference](#workaround-reference) below; all of them
  can be compiled out together with `-DOBN_ENABLE_WORKAROUNDS=OFF`.
- **Passthrough** — the plugin just forwards MQTT / REST payloads;
  Studio does the work.

| Feature | P1 / A1 | X1 / P1S | P2S | Impl | Notes |
| --- | :-: | :-: | :-: | --- | --- |
| SSDP discovery (LAN) | ✅ (not tested) | ✅ (not tested) | ✅ | Native | Multicast listener on `239.255.255.250:1990` + Studio `on_server_connected_`. |
| LAN MQTT telemetry (Dev Mode) | ✅ (not tested) | ✅ (not tested) | ✅ | Native | TLS to `mqtts://<ip>:8883`, user `bblp`, pass = access code. |
| Cloud MQTT telemetry | ✅ (not tested) | ✅ (not tested) | ✅ (not tested) | Native | TLS to `us.mqtt.bambulab.com:8883`. Runs in parallel with LAN if signed in. Not exercised during LAN-focused testing. |
| LAN print (FTPS + MQTT) | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Native | `STOR /cache/<name>` then `{"print":{"command":"project_file",...}}` on LAN MQTT. |
| "Send to Printer" dialog (`ft_*`) | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Workaround | `ft_*` C ABI served over FTPS (`OBN_FT_FTPS_FASTPATH`, ON by default); stock uses a proprietary port-6000 tunnel. With `OFF` Studio falls back to its internal FTP path — same file, same place, less UI polish. |
| Cloud 3MF upload to S3 | ✅ (not tested) | ✅ (not tested) | ✅ (not tested) | Native | 6-step upload sequence from MITM of the stock plugin. |
| `create_task` (MakerWorld entry) | ⚠️ | ⚠️ | ⚠️ | ❌ | Soft-fails — logs 4xx, keeps going with `task_id="0"`. Printing works; MakerWorld history and timelapse-on-printer cloud flags don't. See [MakerWorld integration](#makerworld-integration). |
| MQTT command signing | ❌ | ❌ | ❌ | ❌ | Stock plugin carries per-install RSA keys we don't reproduce. Workaround for the user: enable Developer Mode on the printer. |
| Camera liveview (MJPEG, port 6000) | ✅ (not tested) | ➖ | ➖ | Native | Same TCP-over-TLS stream the stock plugin consumes. The author has no P1/A1 hardware to test on. |
| Camera liveview (RTSPS → MJPEG) | ➖ | ✅ (not tested) | ✅ | Workaround | Stock plugin pipes raw H.264 off RTSPS to Studio's `gstbambusrc` element directly; we run `rtspsrc ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! videoscale ! jpegenc ! appsink` inside `libBambuSource.so` so Studio still sees MJPEG. Heavier CPU, slightly more latency, identical UX. |
| File browser list (LIST_INFO) | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Workaround | FTPS `MLSD`/`LIST` from a worker thread inside `libBambuSource.so`; stock protocol over port 6000 not reimplemented. |
| File download from storage | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Workaround | Streaming FTPS `RETR` with 256 KB `CONTINUE` reply chunks. Same workaround envelope. |
| File delete from storage | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Workaround | FTPS `DELE` per path. |
| 3MF thumbnails in list (`Metadata/plate_*.png`) | ✅ (not tested) | ✅ (not tested) | ⚠️ | Workaround | Opens the archive via FTPS `RETR`, extracts the plate PNG in-process with zlib raw-inflate. Flaky on large archives today — see the open note under [PrinterFileSystem](#printerfilesystem-sdusb-browser-ui-in-studio). |
| Timelapse / video thumbnails | ✅ (not tested) | ✅ (not tested) | ✅ | Workaround | Sidecar `<stem>.jpg` fetch. P2S doesn't write sidecars — panel falls back to Studio's default icon. |
| "Print from Device" (`start_sdcard_print`) | ✅ (not tested) | ✅ (not tested, Dev Mode) | ✅ (Dev Mode) | Workaround | Stock plugin: cloud REST endpoint we can't sign. Ours: publish `project_file` on LAN MQTT for a file already on the printer. |
| `push_status` `home_flag` SDCARD-bits rewrite | ➖ | ➖ | ✅ | Workaround | Only triggers when the printer actually reports `NO_SDCARD`; HAS/ABNORMAL/READONLY pass through untouched. Needed so Studio's Send-to-Printer UI doesn't grey out on printers whose external storage is USB-only. |
| `push_status` `ipcam.file` block injection | ➖ | ➖ | ✅ | Workaround | Only triggers when the block is absent; firmware that already advertises it is passed through. Makes Studio open the file-browser tunnel in the first place. |
| Current-print thumbnail (Device tab) | ➖ | ✅ (not tested) | ✅ * | Mixed | Covers both paths Studio can take at runtime: (a) LAN-only prints where `project_id=profile_id=subtask_id="0"` — the plugin rewrites those to synthetic "lan-&lt;fnv&gt;" ids (workaround, pushes Studio off its SD-card placeholder branch); (b) cloud-initiated subtasks whose real ids are still attached after unbinding — here the plugin simply answers `get_subtask_info(<real_id>)` instead of routing through Bambu's CDN (this part is a regular stock replacement, not a hack). Both paths then share the same backend: localhost HTTP server that pulls `/cache/<subtask>.gcode.3mf` over FTPS, unpacks `Metadata/plate_N.png` and serves it as `thumbnail.url = http://127.0.0.1:<port>/cover/…`. Same image HA uses. **Linux only: requires [`patches/bambustudio-statuspanel-thumbnail.patch`](patches/bambustudio-statuspanel-thumbnail.patch) applied to Studio** — `wxStaticBitmap` under wxGTK doesn't fire `wxEVT_PAINT` on `Bind()`'d handlers, so `PrintingTaskPanel::paint()` never runs and neither the LAN cover nor stock cloud covers ever show. |
| Cloud login / ticket flow | ✅ (not tested) | ✅ (not tested) | ✅ | Native | Browser → `localhost` callback → `POST /user-service/user/ticket/<T>`. Session persisted to `obn.auth.json`. |
| User presets sync / profile / avatar | ✅ (not tested) | ✅ (not tested) | ✅ | Native | Driven by Studio reading `get_user_id()`; plugin just serves the login session. |
| AMS telemetry / mapping | ✅ (not tested) | ✅ (not tested) | ✅ | Passthrough | Studio consumes `push_status` directly. |
| Nozzle mapping / multi-extruder | ✅ (not tested) | ✅ (not tested) | ✅ (not tested) | Passthrough | Same as AMS; P2S is single-extruder so not exercised. |
| TUTK / Agora cloud-p2p transports | ❌ | ❌ | ❌ | ❌ | Proprietary libraries; out of scope — use LAN/Developer Mode instead. |
| Windows / macOS builds | ❌ | ❌ | ❌ | ❌ | Architected for but Linux x86_64 / aarch64 only today. |

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
| `ft_*` FTPS fastpath (`OBN_FT_FTPS_FASTPATH`, also default ON) | Port-6000 proprietary tunnel for the `FileTransfer` ABI (Send-to-Printer dialog + eMMC pre-flight + media-ability probes). | FTPS-backed `STOR` for uploads + `CWD /sdcard` / `CWD /usb` probe for media-ability answers. TUTK/Agora URLs still return `FT_EIO`. | Same tunnel the LAN print path already uses; gives the richer dialog sequence (ability probe → storage picker → percent progress) without a new transport. | `emmc` is never advertised (Bambu firmware reserves it for system files). Fall-back path via Studio's internal FTP upload still puts the file in the same place if you turn this OFF. See [FileTransfer module](#filetransfer-module-ft_-c-abi). |
| Current-print thumbnail cover | Stock plugin gets `project_id`/`profile_id`/`subtask_id` back from the cloud task it created, and `get_subtask_info` returns a `thumbnail.url` served out of Bambu's CDN. | Two runtime paths share the same backend: **(a)** LAN-only prints whose ids come through as `"0"` — `MachineObject::is_sdcard_printing()` would otherwise route the Device-panel thumbnail to a static SD-card placeholder, so we rewrite the zeros to synthetic `"lan-<fnv>"` ids on every `push_status` (this part is the workaround). **(b)** Cloud-initiated subtasks that still carry their real numeric ids (e.g. after unbinding a cloud-paired printer) — Studio calls `get_subtask_info(<real_id>)` exactly like it would against stock; we just serve that ourselves instead of the CDN. Both paths land in the same backend: a single-thread HTTP server on `127.0.0.1:<random>`, FTPS-download of `/cache/<subtask>.gcode.3mf`, unpack `Metadata/plate_N.png` via zlib raw-inflate and serve as `image/png` behind the URL we return from `get_subtask_info`. On Linux this additionally requires [`patches/bambustudio-statuspanel-thumbnail.patch`](patches/bambustudio-statuspanel-thumbnail.patch) against Studio: `PrintingTaskPanel::set_thumbnail_img` relies on a custom `wxEVT_PAINT` handler which `wxStaticBitmap` doesn't dispatch under wxGTK (native `GtkImage` wrapper), so without the patch no cover — LAN or cloud — ever becomes visible on screen. | wxWebRequest on libsoup3 / Linux rejects `file://` URLs; a loopback HTTP server is the minimum that still uses the same code path cloud thumbnails take. File is cached under `temp_directory_path()/obn-covers/` so reprint requests don't re-download the archive. `/cache/` is the drop-zone for both `start_local_print_with_record` and the printer's own cloud-download, so one lookup covers both origins. | Extra 2-20 MB FTPS `RETR` once per print, a socket listener on loopback for the lifetime of the plugin, and a 15 s wait budget inside the HTTP server while the FTPS worker is still fetching. If the .3mf is gone from `/cache/` the server returns 503 → Studio falls back to its broken-image icon (clickable to retry). |

All of these except the last are gated by `OBN_ENABLE_WORKAROUNDS`.
The `ft_*` fastpath has its own flag because stock-compatible builds
may want to keep Send-to-Printer's UI progress working while turning
everything else off.

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

## What it is

A native shared library (`libbambu_networking.so`) binary-compatible with the
`dlsym`-based ABI Bambu Studio uses to call into the proprietary plugin. Drops
into `~/.config/BambuStudio/plugins/` in place of the original.

See [`NETWORK_PLUGIN.md`](../BambuStudio/%20NETWORK_PLUGIN.md) in the sibling
BambuStudio checkout for a full ABI reference, download flow, signature logic
and version-check rules.

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

## License

MIT — see [LICENSE](LICENSE).
