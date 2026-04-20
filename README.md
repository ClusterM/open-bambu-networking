# open-bambu-networking

Open-source drop-in replacement for Bambu Studio's proprietary `bambu_networking`
plugin, based on reverse-engineered protocols from
[OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) and reference
implementations from
[bambulabs_api](https://github.com/acse-ci223/bambulabs_api) and
[ha_bambu_lab](https://github.com/greghesp/ha-bambulab).

> **Status:** Phase 8 (LAN printing + camera). SSDP discovery, live
> telemetry, LAN printing, cloud ticket sign-in (for user presets /
> profile) and camera (MJPEG for P1/A1, RTSPS→MJPEG transcode for
> X1/P1S/P2S) all work. Cloud print upload to S3 works but the final
> `create_task` step cannot be signed, so MakerWorld task history is
> not recorded. The printer itself must be in **Developer Mode** (aka
> LAN-only mode) for LAN commands to succeed — see
> [Developer Mode requirement](#developer-mode-requirement) below.

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

| Feature | P1 / A1 | X1 / P1S / P2S |
| --- | --- | --- |
| SSDP discovery (LAN) | ✅ | ✅ |
| MQTT telemetry (LAN, Dev Mode) | ✅ | ✅ |
| MQTT telemetry (cloud) | ✅ | ✅ |
| LAN print (FTPS + MQTT) | ✅ | ✅ (Dev Mode only) |
| Cloud project upload (S3) | ✅ | ✅ |
| `create_task` (MakerWorld entry) | ⚠️ soft-fail | ⚠️ soft-fail |
| Camera liveview (MJPEG) | ✅ | n/a |
| Camera liveview (RTSPS → MJPEG transcode) | n/a | ✅ |
| Cloud login / user info | ✅ | ✅ |
| AMS telemetry / mapping | ✅ | ✅ |

`⚠️ soft-fail` means the step logs a warning and continues with `task_id="0"`.
Printing works; the job does not show up in the cloud's job history and the
timelapse/record-on-printer cloud flags have no effect. Local print records
on the printer itself still work.

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

Used by Studio for:

- eMMC pre-flight check in Print job (determines whether the file goes
  to eMMC vs. SD card);
- the separate "Send to Printer" dialog (upload without printing);
- some media-ability queries.

Our implementation returns `FT_EIO` immediately. On P1/A1 this means the
print always lands on the SD card (not eMMC) and the Send-to-Printer
dialog is unavailable; the regular Print button works and copies via FTP.
On X1/P1S/P2S the proprietary transport (TUTK/cloud-p2p) is used by Studio
and is out of scope here — printing falls through to FTPS, which works.
**Scope:** could be added for P1/A1 if there is demand. Transport is
documented in `OpenBambuAPI/video.md` (port 6000, TLS, 80-byte auth).

### PrinterFileSystem (SD-card browser UI in Studio)

For P1/A1 this multiplexes a CTRL channel (`Bambu_StartStreamEx(CTRL_TYPE)`,
JSON request/response) over the same port-6000 tunnel used by the camera.
For X1/P1S/P2S in LAN-only mode **Studio itself refuses to open the
browser** ("Browsing file in storage is not supported in LAN Only Mode"),
so this is not a limitation we can fix from the plugin side.
**Scope:** deferred with the FileTransfer module.

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
