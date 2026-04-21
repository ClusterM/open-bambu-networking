# Bambu Studio Network Plugin — full reference

This document describes how Bambu Studio integrates with its proprietary **Network Plugin** (`bambu_networking`) — where it is downloaded from, where it is installed, how it is validated, and the exact C ABI contract it must implement. The goal is to document how the plugin is integrated, loaded, validated and invoked, based purely on the Bambu Studio source code.

The reference is derived from a read-through of the upstream [bambulab/BambuStudio](https://github.com/bambulab/BambuStudio) tree; no binary disassembly is involved. Every claim in this document is backed by a concrete file and line range in Studio's sources. Behaviour that lives strictly inside the closed-source `bambu_networking` binary is out of scope here.

All source references point at the current BambuStudio tree.

---

## 1. Architecture overview

Bambu Studio is a wxWidgets/C++ application. All networking code (Bambu Lab cloud, MQTT/SSDP to printers, print/upload jobs, authentication, OSS, tracking, and so on) lives in a separate **dynamically-loaded library** (`.dll` / `.so` / `.dylib`). Studio talks to it through a single C ABI whose symbols all start with `bambu_network_…`.

Key players:

| Role | Source |
|------|--------|
| C ABI declarations (`dlsym` typedefs) | `src/slic3r/Utils/NetworkAgent.hpp` |
| Symbol resolver and method wrappers | `src/slic3r/Utils/NetworkAgent.cpp` |
| Shared protocol structures / constants | `src/slic3r/Utils/bambu_networking.hpp` |
| `ft_*` File Transfer ABI | `src/slic3r/Utils/FileTransferUtils.{hpp,cpp}` |
| Module signature verification | `src/slic3r/Utils/CertificateVerify.{hpp,cpp}` |
| Lifecycle (URL, download, install, version) | `src/slic3r/GUI/GUI_App.cpp` |
| OTA synchronization | `src/slic3r/Utils/PresetUpdater.cpp` |
| UI job "download & install" | `src/slic3r/GUI/Jobs/UpgradeNetworkJob.{hpp,cpp}` |

> Note: the code occasionally refers to two further libraries, **`BambuSource`** and **`live555`**. These are the camera/player and the RTSP stack; they are fetched and installed through the exact same mechanism and live next to the main library, but the "Network Plugin" contract proper is `bambu_networking`.

The current Studio version pinned in sources (tag `v02.06.00.51`) is `SLIC3R_VERSION = "02.06.00.51"` (`version.inc`); the expected agent version is `BAMBU_NETWORK_AGENT_VERSION = "02.06.00.50"` (`src/slic3r/Utils/bambu_networking.hpp:100`).

---

## 2. Where the plugin is downloaded from

### 2.1. Base API

The URL is built by `GUI_App::get_http_url` based on the `country_code` stored in `app_config`:

```1469:1505:src/slic3r/GUI/GUI_App.cpp
std::string GUI_App::get_http_url(std::string country_code, std::string path)
{
    std::string url;
    if (country_code == "US") {
        url = "https://api.bambulab.com/";
    }
    else if (country_code == "CN") {
        url = "https://api.bambulab.cn/";
    }
    // ENV_CN_DEV  -> https://api-dev.bambu-lab.com/
    // ENV_CN_QA   -> https://api-qa.bambu-lab.com/
    // ENV_CN_PRE  -> https://api-pre.bambu-lab.com/
    // NEW_ENV_DEV_HOST -> https://api-dev.bambulab.net/
    // NEW_ENV_QAT_HOST -> https://api-qa.bambulab.net/
    // NEW_ENV_PRE_HOST -> https://api-pre.bambulab.net/
    else {
        url = "https://api.bambulab.com/";
    }
    url += path.empty() ? "v1/iot-service/api/slicer/resource" : path;
    return url;
}
```

The resulting base is `https://api.bambulab.com/v1/iot-service/api/slicer/resource` (or its regional equivalent).

### 2.2. Manifest request

`GUI_App::get_plugin_url` assembles the query parameter `slicer/plugins/cloud=<ver>`:

```1545:1556:src/slic3r/GUI/GUI_App.cpp
std::string GUI_App::get_plugin_url(std::string name, std::string country_code)
{
    std::string url = get_http_url(country_code);
    std::string curr_version = SLIC3R_VERSION;
    std::string using_version = curr_version.substr(0, 9) + "00";
    if (name == "cameratools")
        using_version = curr_version.substr(0, 6) + "00.00";
    url += (boost::format("?slicer/%1%/cloud=%2%") % name % using_version).str();
    return url;
}
```

For the networking plugin the helper is called with `name == "plugins"`. For `SLIC3R_VERSION = "02.06.00.51"` the request becomes:

```
GET https://api.bambulab.com/v1/iot-service/api/slicer/resource?slicer/plugins/cloud=02.06.00.00
```

### 2.3. Response format (JSON manifest)

The response is parsed in `GUI_App::download_plugin` (see `src/slic3r/GUI/GUI_App.cpp` around lines 1617–1649). The expected shape:

```json
{
  "message": "success",
  "resources": [
    {
      "type": "slicer/plugins/cloud",
      "version": "02.05.03.xx",
      "description": "…changelog…",
      "url": "https://<cdn>/<path>/plugin.zip",
      "force_update": false
    }
  ]
}
```

Studio consumes only `version`, `description`, `url` and `force_update`. `url` points at a ZIP archive that is fetched next.

### 2.4. Special HTTP headers

- **`X-BBL-OS-Type`** is temporarily set to `"windows_arm"` when downloading the plugin on Windows ARM64 and restored to `"windows"` after the request: `src/slic3r/GUI/GUI_App.cpp` 1597–1605, 1665–1672 and `src/slic3r/Utils/PresetUpdater.cpp` 1209–1237.
- All other "sticky" headers (User-Agent etc.) are registered through `Slic3r::Http::set_extra_headers` and forwarded into the plugin via `bambu_network_set_extra_http_header`.

### 2.5. Background synchronization (OTA)

`PresetUpdater::priv::sync_plugins` hits the same HTTP API, but its purpose is to populate the OTA cache rather than install the plugin immediately:

```1165:1253:src/slic3r/Utils/PresetUpdater.cpp
void PresetUpdater::priv::sync_plugins(std::string http_url, std::string plugin_version)
{
    ...
    std::string using_version = curr_version.substr(0, 9) + "00";
    auto cache_plugin_folder = cache_path / PLUGINS_SUBPATH;        // data_dir/ota/plugins
    ...
    std::map<std::string, Resource> resources {
        {"slicer/plugins/cloud", { using_version, "", "", "", false, cache_plugin_folder.string()}}
    };
    sync_resources(http_url, resources, true, plugin_version, "network_plugins.json");
    ...
    if (result) {
        if (force_upgrade) {
            app_config->set("update_network_plugin", "true");
        } else {
            // push notification BBLPluginUpdateAvailable
        }
    }
}
```

`sync_resources` builds the final URL like this:

```581:583:src/slic3r/Utils/PresetUpdater.cpp
    std::string url = http_url;
    url += query_params;
    Slic3r::Http http = Slic3r::Http::get(url);
```

i.e. identically to `get_plugin_url`.

### 2.6. Download entry points

- **Background**: `GUI_App::on_init` → `CallAfter` → `preset_updater->sync(http_url, lang, network_ver, ...)` (`src/slic3r/GUI/GUI_App.cpp` 1333–1340).
- **"Download Bambu Network Plug-in" dialog**: `GUI_App::updating_bambu_networking()` (line 1975) → `DownloadProgressDialog` → `UpgradeNetworkJob::process()` (`src/slic3r/GUI/Jobs/UpgradeNetworkJob.cpp` 48–130).
- **Manual trigger from the WebView**: event `begin_network_plugin_download` (`src/slic3r/GUI/GUI_App.cpp` ~4078–4090) and `ShowDownNetPluginDlg`.
- User-facing wiki article shown on failure: `https://wiki.bambulab.com/en/software/bambu-studio/failed-to-get-network-plugin` (`src/slic3r/GUI/DownloadProgressDialog.cpp` 32–33).

---

## 3. Where it is stored and how it is installed

### 3.1. Working directory (active plugin)

Studio loads the binary from **`<data_dir>/plugins/`**. The file name varies by OS:

| Platform | Path |
|----------|------|
| Windows  | `<data_dir>\plugins\bambu_networking.dll` |
| Windows  | `<data_dir>\plugins\BambuSource.dll` (optional, camera) |
| Windows  | `<data_dir>\plugins\live555.dll` (RTSP/media) |
| macOS    | `<data_dir>/plugins/libbambu_networking.dylib` |
| macOS    | `<data_dir>/plugins/libBambuSource.dylib` |
| macOS    | `<data_dir>/plugins/liblive555.dylib` |
| Linux    | `<data_dir>/plugins/libbambu_networking.so` |
| Linux    | `<data_dir>/plugins/libBambuSource.so` |
| Linux    | `<data_dir>/plugins/liblive555.so` |

On Linux `<data_dir>` is usually `~/.config/BambuStudio/` (wxWidgets XDG path), on macOS `~/Library/Application Support/BambuStudio/`, on Windows `%AppData%\BambuStudio\`.

The path is computed in `NetworkAgent::initialize_network_module`:

```183:245:src/slic3r/Utils/NetworkAgent.cpp
    auto plugin_folder = data_dir_path / "plugins";
    if (using_backup) plugin_folder = plugin_folder/"backup";
    ...
#if defined(_MSC_VER) || defined(_WIN32)
    library = plugin_folder.string() + "\\" + std::string(BAMBU_NETWORK_LIBRARY) + ".dll";
    ...
    networking_module = LoadLibrary(lib_wstr);
#else
    #if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib";
    #else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so";
    #endif
    networking_module = dlopen(library.c_str(), RTLD_LAZY);
#endif
```

The constant `BAMBU_NETWORK_LIBRARY = "bambu_networking"` lives in `src/slic3r/Utils/bambu_networking.hpp:97`.

### 3.2. Backup copy

After a successful unpack `install_plugin` copies every top-level file from `<data_dir>/plugins/` into **`<data_dir>/plugins/backup/`**. If at startup the primary plugin fails to load or is version-incompatible, Studio makes a second attempt with `using_backup=true` — the path then becomes `<data_dir>/plugins/backup/`:

```1874:1905:src/slic3r/GUI/GUI_App.cpp
    fs::path dir_path(plugin_folder);
    if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
        ...
        for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
            if (it->path().string() == backup_folder) continue;
            auto dest_path = backup_folder.string() + "/" + it->path().filename().string();
            if (fs::is_regular_file(it->status())) {
                ... CopyFileResult cfr = copy_file(it->path().string(), dest_path, error_message, false);
            } else {
                copy_framework(it->path().string(), dest_path);
            }
        }
    }
```

The retry logic is in `GUI_App::on_init_network` (`src/slic3r/GUI/GUI_App.cpp` 3421–3459).

### 3.3. OTA cache (staging)

All background downloads land in **`<data_dir>/ota/plugins/`** (the constant `PLUGINS_SUBPATH` defined at `PresetUpdater.cpp:57`). That folder is expected to contain **all three** libraries plus a JSON manifest:

```1137:1160:src/slic3r/Utils/PresetUpdater.cpp
    network_library = cache_folder.string() + "/bambu_networking.dll";      // or .dylib / .so
    player_library  = cache_folder.string() + "/BambuSource.dll";
    live555_library = cache_folder.string() + "/live555.dll";
    std::string changelog_file = cache_folder.string() + "/network_plugins.json";
    if (fs::exists(network_library)
        && fs::exists(player_library)
        && fs::exists(live555_library)
        && fs::exists(changelog_file))
    {
        has_plugins = true;
        parse_ota_files(changelog_file, cached_version, force, description);
    }
```

If any of the files is missing, the cache is considered incomplete.

### 3.4. `network_plugins.json` format

The JSON is produced by `sync_resources` after unpacking the archive:

```712:723:src/slic3r/Utils/PresetUpdater.cpp
    json j;
    j["version"]     = resource_update->second.version;
    j["description"] = resource_update->second.description;
    j["force"]       = resource_update->second.force;
    boost::nowide::ofstream c;
    c.open(changelog_file, std::ios::out | std::ios::trunc);
    c << std::setw(4) << j << std::endl;
```

Minimal valid file:

```json
{
  "version": "02.06.00.50",
  "description": "…",
  "force": false
}
```

### 3.5. The "download -> install" flow

1. `UpgradeNetworkJob` (with `name="plugins"` and `package_name="networking_plugins.zip"`, `src/slic3r/GUI/Jobs/UpgradeNetworkJob.cpp:19-20`) calls:
   - `GUI_App::download_plugin("plugins", "networking_plugins.zip", ...)` — drops the ZIP into `temp_directory_path()/networking_plugins.zip` (a parallel branch in `WebDownPluginDlg` / `GuideFrame` uses the name `network_plugin.zip`).
   - `GUI_App::install_plugin("plugins", "networking_plugins.zip", ...)` — extracts the archive into **`<data_dir>/plugins/`** while preserving its internal directory hierarchy.
2. On success a flag is written: `app_config["app"]["installed_networking"] = "1"` (`src/slic3r/GUI/GUI_App.cpp` 1906–1909).
3. `restart_networking()` (`src/slic3r/GUI/GUI_App.cpp` 1914–1957) restarts the agent: it calls `on_init_network(try_backup=true)`, resets `StaticBambuLib`, re-registers callbacks and kicks off discovery.

### 3.6. Applying OTA at startup

If `update_network_plugin == "true"`, on the next launch — **before** network initialization — Studio copies the freshly downloaded libraries in:

```3359:3418:src/slic3r/GUI/GUI_App.cpp
void GUI_App::copy_network_if_available()
{
    if (app_config->get("update_network_plugin") != "true") return;
    auto plugin_folder = data_dir_path / "plugins";
    auto cache_folder  = data_dir_path / "ota" / "plugins";
#if defined(_MSC_VER) || defined(_WIN32)
    const char* library_ext = ".dll";
#elif defined(__WXMAC__)
    const char* library_ext = ".dylib";
#else
    const char* library_ext = ".so";
#endif
    for (auto& dir_entry : boost::filesystem::directory_iterator(cache_folder)) {
        if (boost::algorithm::iends_with(file_path, library_ext)) {
            copy_file(file_path, (plugin_folder / file_name).string(), error_message, false);
            fs::permissions(dest_path, fs::owner_read|fs::owner_write|fs::group_read|fs::others_read);
        }
    }
    fs::remove_all(cache_folder);
    app_config->set("update_network_plugin", "false");
}
```

Note: only **top-level files whose extension matches the library extension** are copied. Subdirectories and auxiliary files (e.g. certificates) are ignored. The shipped plugin must therefore be "flat" — just the library binary (`bambu_networking.{dll|so|dylib}`) plus, optionally, `BambuSource` and `live555`.

### 3.7. Removal

`GUI_App::remove_old_networking_plugins` wipes the **whole** `<data_dir>/plugins/` tree:

```1959:1973:src/slic3r/GUI/GUI_App.cpp
void GUI_App::remove_old_networking_plugins()
{
    auto plugin_folder = data_dir_path / "plugins";
    if (boost::filesystem::exists(plugin_folder)) {
        fs::remove_all(plugin_folder);
    }
}
```

---

## 4. What the plugin is, physically

It is a plain native dynamic library with C exports. The calling convention is `cdecl` on Windows (`FT_CALL __cdecl` in `FileTransferUtils.hpp:15`) and the standard System V AMD64 ABI on Linux/macOS.

- The main module is **`bambu_networking`** — it implements the entire networking API (`bambu_network_*`) and the file-transfer ABI (`ft_*`). **Both symbol sets live in the same library**: immediately after loading, `NetworkAgent::initialize_network_module` calls `InitFTModule(networking_module)` (`src/slic3r/Utils/NetworkAgent.cpp:276`).
- Optional companion modules Studio knows how to pick up:
  - `BambuSource` — the wrapper for the printer camera stream. Loaded separately through `NetworkAgent::get_bambu_source_entry()` (`src/slic3r/Utils/NetworkAgent.cpp:511-562`); if it fails to load, `m_networking_compatible = false` is set and the user sees "please update the plugin" (`src/slic3r/GUI/GUI_App.cpp:3430-3437`).
  - `live555` — the classic RTSP library used internally by `BambuSource`. Studio never calls it directly but requires it to be present in the OTA cache (see § 3.3).

The ZIP is usually a few MiB. Studio imposes no formal size limit; `install_plugin` simply extracts every file through `miniz` (`mz_zip_…`).

No `plugins.json`/`manifest.xml` inside the archive is required. After extraction Studio only reads:
- the library itself — via `LoadLibrary`/`dlopen`;
- `network_plugins.json` **in the OTA cache** (not in the installed folder);
- the symbol `bambu_network_get_version` to determine the version.

---

## 5. Validation

### 5.1. Studio <-> plugin version compatibility

The main check is that the first **8 characters** of the version string match, i.e. `MAJOR.MINOR.PATCH` without the build suffix:

```1982:1998:src/slic3r/GUI/GUI_App.cpp
bool GUI_App::check_networking_version()
{
    std::string network_ver = Slic3r::NetworkAgent::get_version();
    std::string studio_ver = SLIC3R_VERSION;   // "02.06.00.51"
    if (network_ver.length() >= 8) {
        if (network_ver.substr(0,8) == studio_ver.substr(0,8)) {  // "02.06.00"
            m_networking_compatible = true;
            return true;
        }
    }
    m_networking_compatible = false;
    return false;
}
```

For `SLIC3R_VERSION = "02.06.00.51"` the plugin must return **a string starting with `"02.06.00"`** (e.g. `"02.06.00.50"`). Otherwise Studio marks it incompatible, sets `m_networking_need_update=true` and pops up the update dialog.

> Observation: on Linux this version check is effectively the **only** formal compatibility gate — see § 5.2, where the signature check is a no-op on that platform.

The plugin exposes its version through the symbol `bambu_network_get_version` (`func_get_version` typed as `std::string(*)(void)`). See `NetworkAgent::get_version`:

```583:603:src/slic3r/Utils/NetworkAgent.cpp
std::string NetworkAgent::get_version()
{
    bool consistent = true;
    if (check_debug_consistent_ptr) {
#if defined(NDEBUG)
        consistent = check_debug_consistent_ptr(false);
#else
        consistent = check_debug_consistent_ptr(true);
#endif
    }
    if (!consistent) return "00.00.00.00";
    if (get_version_ptr) return get_version_ptr();
    return "00.00.00.00";
}
```

A separate consistency check is `bambu_network_check_debug_consistent(bool is_debug)` — it lets the plugin reject a mismatched debug/release build. If it returns `false`, Studio treats the version as `"00.00.00.00"` and refuses to proceed.

### 5.2. Binary signature

Before calling `LoadLibrary`/`dlopen` Studio compares the module's publisher with Studio's own publisher:

```190:267:src/slic3r/Utils/NetworkAgent.cpp
    std::optional<SignerSummary> self_cert_summary, module_cert_summary;
    if (validate_cert) self_cert_summary = SummarizeSelf();
    ...
    if (self_cert_summary) {
        module_cert_summary = SummarizeModule(library);
        if (module_cert_summary) {
            if (IsSamePublisher(*self_cert_summary, *module_cert_summary))
                networking_module = LoadLibrary(lib_wstr);   // (or dlopen)
            else
                BOOST_LOG_TRIVIAL(info) << "module is from another publisher...";
        }
    } else {
        networking_module = LoadLibrary(lib_wstr);           // self cert unknown -> load as is
    }
```

`IsSamePublisher`:

```294:300:src/slic3r/Utils/CertificateVerify.cpp
bool IsSamePublisher(const SignerSummary& a, const SignerSummary& b)
{
    if (!a.team_id.empty() && a.team_id == b.team_id) return true;   // macOS TeamID
    if (a.spki_sha256 == b.spki_sha256) return true;                 // same SPKI
    if (a.cert_sha256 == b.cert_sha256) return true;                 // same certificate
    return false;
}
```

- **Windows**: the Authenticode signature of the main `bambu-studio.exe` and of `bambu_networking.dll` must share either an SPKI or a certificate. If the plugin is unsigned, `SummarizeModule` returns `nullopt`, the "error" branch is logged, `networking_module` stays `nullptr`, and the module **will not be loaded**.
- **macOS**: the comparison uses the `team_id` (Developer ID).
- **Linux**: `SummarizeSelf` / `SummarizeModule` **always return `std::nullopt`** — see:

```289:291:src/slic3r/Utils/CertificateVerify.cpp
#else
    std::optional<SignerSummary> SummarizeSelf() { return std::nullopt; }
    std::optional<SignerSummary> SummarizeModule(const std::string&) { return std::nullopt; }
#endif
```

Therefore on Linux `if (self_cert_summary)` is false and Studio takes the "load as is" branch — **the signature is effectively not verified on Linux**.

### 5.3. Bypassing the signature check

`AppConfig` exposes a flag **`ignore_module_cert`**, which is forwarded to the `validate_cert` parameter:

```3423:3423:src/slic3r/GUI/GUI_App.cpp
    int load_agent_dll = Slic3r::NetworkAgent::initialize_network_module(false, !app_config->get_bool("ignore_module_cert"));
```

Setting `ignore_module_cert = 1` in `BambuStudio.conf` disables the publisher check on Windows/macOS entirely.

### 5.4. What "plugin installed" looks like to Studio

- A boolean **`installed_networking`** key in `app_config` (section `app`) — set to `"1"` after a successful `install_plugin` (`src/slic3r/GUI/GUI_App.cpp:1906-1909`). This flag drives the "show install/update dialog" logic.
- The actual "the plugin works" check is this chain:
  1. `LoadLibrary`/`dlopen` returns non-null;
  2. `bambu_network_check_debug_consistent` returns `true` for the appropriate build flavor;
  3. `bambu_network_get_version` returns a string at least 8 chars long with the right version prefix;
  4. `BambuSource` also loaded successfully.

### 5.5. Archive integrity (MD5/SHA)

**Not checked.** There is no hash verification of the ZIP anywhere in `download_plugin` / `install_plugin` / `sync_resources` (`src/slic3r/GUI/GUI_App.cpp`, `src/slic3r/Utils/PresetUpdater.cpp`). The only defense-in-depth measure is the binary's own signature.

Error codes of the form `BAMBU_NETWORK_ERR_CHECK_MD5_FAILED` (see `src/slic3r/Utils/bambu_networking.hpp:29, 54, 70`) belong to MD5 checks **inside the plugin** during print-job uploads, not to verification of the plugin itself.

---

## 6. The full C ABI contract

All symbols are resolved through `GetProcAddress` (Windows) / `dlsym` (Linux, macOS) in `NetworkAgent::get_network_function`:

```564:581:src/slic3r/Utils/NetworkAgent.cpp
void* NetworkAgent::get_network_function(const char* name)
{
    if (!networking_module) return nullptr;
#if defined(_MSC_VER) || defined(_WIN32)
    return GetProcAddress(networking_module, name);
#else
    return dlsym(networking_module, name);
#endif
}
```

Symbol names are not mangled — every function must be declared `extern "C"`.

> ABI note: even though this is a C-style interface, the signatures use C++ types (`std::string`, `std::vector`, `std::map`, `std::function`, and custom structs `PrintParams`/`BBLModelTask`/…). The plugin must therefore be built with the same compiler and libstdc++/libc++ standard-library ABI as Bambu Studio itself. It is **not** a pure C ABI — mixing compilers/linkers (e.g. GCC vs. MSVC) is not safe.

### 6.1. Initialization and lifecycle

| Symbol | Typedef | Description |
|--------|---------|-------------|
| `bambu_network_check_debug_consistent` | `bool(*)(bool is_debug)` | Returns `true` if the plugin build matches Studio's build flavor (debug/release). Called before `get_version`. |
| `bambu_network_get_version` | `std::string(*)(void)` | Returns the version formatted as `NN.NN.NN.NN`. The first 8 characters must match `SLIC3R_VERSION`. |
| `bambu_network_create_agent` | `void*(*)(std::string log_dir)` | Creates an agent instance and returns an opaque handle (`void* agent`). |
| `bambu_network_destroy_agent` | `int(*)(void* agent)` | Destroys the agent. |
| `bambu_network_init_log` | `int(*)(void* agent)` | Initializes the internal log. |
| `bambu_network_set_config_dir` | `int(*)(void*, std::string)` | Configures directory (equal to `data_dir()`). |
| `bambu_network_set_cert_file` | `int(*)(void*, std::string folder, std::string filename)` | Studio passes `resources_dir()/cert` and `slicer_base64.cer`. |
| `bambu_network_set_country_code` | `int(*)(void*, std::string)` | `"US"`, `"CN"`, … |
| `bambu_network_start` | `int(*)(void*)` | Starts the agent's event loop / worker threads. |

#### Initialization sequence

The Studio-side call order after `create_agent` is deterministic and lives in `GUI_App::on_init_network` (`src/slic3r/GUI/GUI_App.cpp:3461-3510`):

1. `set_config_dir(data_dir())`
2. `init_log()`
3. `set_cert_file(resources_dir()+"/cert", "slicer_base64.cer")`
4. `init_http_extra_header` → `set_extra_http_header(...)`
5. the full `set_on_*_fn(...)` battery (see § 6.2)
6. `set_country_code(country_code)`
7. `start()`
8. `start_discovery(true, false)`

The plugin must tolerate this exact order (in particular, no networking work should happen before `start()`).

### 6.2. Callbacks (registration)

All take a `void* agent` and an `std::function<…>`:

| Symbol | Callback type (from `bambu_networking.hpp`) |
|--------|---------------------------------------------|
| `bambu_network_set_on_ssdp_msg_fn` | `OnMsgArrivedFn = std::function<void(std::string dev_info_json_str)>` |
| `bambu_network_set_on_user_login_fn` | `OnUserLoginFn = std::function<void(int online_login, bool login)>` |
| `bambu_network_set_on_printer_connected_fn` | `OnPrinterConnectedFn = std::function<void(std::string topic_str)>` |
| `bambu_network_set_on_server_connected_fn` | `OnServerConnectedFn = std::function<void(int return_code, int reason_code)>` |
| `bambu_network_set_on_http_error_fn` | `OnHttpErrorFn = std::function<void(unsigned http_code, std::string http_body)>` |
| `bambu_network_set_get_country_code_fn` | `GetCountryCodeFn = std::function<std::string()>` |
| `bambu_network_set_on_subscribe_failure_fn` | `GetSubscribeFailureFn = std::function<void(std::string topic)>` |
| `bambu_network_set_on_message_fn` | `OnMessageFn = std::function<void(std::string dev_id, std::string msg)>` |
| `bambu_network_set_on_user_message_fn` | `OnMessageFn` |
| `bambu_network_set_on_local_connect_fn` | `OnLocalConnectedFn = std::function<void(int status, std::string dev_id, std::string msg)>` |
| `bambu_network_set_on_local_message_fn` | `OnMessageFn` |
| `bambu_network_set_queue_on_main_fn` | `QueueOnMainFn = std::function<void(std::function<void()>)>` — "run this lambda on the GUI thread" |
| `bambu_network_set_server_callback` | `OnServerErrFn = std::function<void(std::string url, int status)>` |

### 6.3. Cloud — connection and subscriptions

| Symbol | Signature |
|--------|-----------|
| `bambu_network_connect_server` | `int(void*)` |
| `bambu_network_is_server_connected` | `bool(void*)` |
| `bambu_network_refresh_connection` | `int(void*)` |
| `bambu_network_start_subscribe` | `int(void*, std::string module)` |
| `bambu_network_stop_subscribe` | `int(void*, std::string module)` |
| `bambu_network_add_subscribe` | `int(void*, std::vector<std::string> dev_list)` |
| `bambu_network_del_subscribe` | `int(void*, std::vector<std::string> dev_list)` |
| `bambu_network_enable_multi_machine` | `void(void*, bool)` |
| `bambu_network_send_message` | `int(void*, std::string dev_id, std::string json_str, int qos, int flag)` — MQTT-style call |

### 6.4. Local printer connection (LAN)

| Symbol | Signature |
|--------|-----------|
| `bambu_network_connect_printer` | `int(void*, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)` |
| `bambu_network_disconnect_printer` | `int(void*)` |
| `bambu_network_send_message_to_printer` | `int(void*, std::string dev_id, std::string json_str, int qos, int flag)` |
| `bambu_network_update_cert` | `int(void* agent)` — `func_check_cert`; refreshes certificates at runtime |
| `bambu_network_install_device_cert` | `void(void*, std::string dev_id, bool lan_only)` |
| `bambu_network_start_discovery` | `bool(void*, bool start, bool sending)` — SSDP |

### 6.5. Authentication and user

| Symbol | Signature |
|--------|-----------|
| `bambu_network_change_user` | `int(void*, std::string user_info)` |
| `bambu_network_is_user_login` | `bool(void*)` |
| `bambu_network_user_logout` | `int(void*, bool request)` |
| `bambu_network_get_user_id` | `std::string(void*)` |
| `bambu_network_get_user_name` | `std::string(void*)` |
| `bambu_network_get_user_avatar` | `std::string(void*)` |
| `bambu_network_get_user_nickanme` | `std::string(void*)` *(the "nickanme" typo is part of the actual ABI!)* |
| `bambu_network_build_login_cmd` | `std::string(void*)` |
| `bambu_network_build_logout_cmd` | `std::string(void*)` |
| `bambu_network_build_login_info` | `std::string(void*)` |
| `bambu_network_get_my_profile` | `int(void*, std::string token, unsigned int* http_code, std::string* http_body)` |
| `bambu_network_get_my_token`   | `int(void*, std::string ticket, unsigned int* http_code, std::string* http_body)` |
| `bambu_network_get_user_info`  | `int(void*, int* identifier)` |

> Known Studio bug (`src/slic3r/Utils/NetworkAgent.cpp:368`): the `get_my_token_ptr` pointer is mistakenly resolved via the string `"bambu_network_get_my_profile"` instead of `"bambu_network_get_my_token"`. Studio still tries to read the `bambu_network_get_my_token` symbol as well, so a compatible plugin must export **both**. Through that pointer Studio will in practice execute the `get_my_profile` body — the two functions must therefore share identical signatures, and any real token-fetching logic ends up running from `get_my_profile`.

### 6.6. Binding / bind

| Symbol | Signature |
|--------|-----------|
| `bambu_network_ping_bind` | `int(void*, std::string ping_code)` |
| `bambu_network_bind_detect` | `int(void*, std::string dev_ip, std::string sec_link, detectResult& detect)` |
| `bambu_network_bind` | `int(void*, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)` |
| `bambu_network_unbind` | `int(void*, std::string dev_id)` |
| `bambu_network_request_bind_ticket` | `int(void*, std::string* ticket)` |
| `bambu_network_query_bind_status` | `int(void*, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)` |

The `detectResult` struct (`src/slic3r/Utils/bambu_networking.hpp:180-189`):

```cpp
struct detectResult {
    std::string result_msg, command, dev_id, model_id, dev_name, version, bind_state, connect_type;
};
```

### 6.7. Printer selection and metadata

| Symbol | Signature |
|--------|-----------|
| `bambu_network_get_bambulab_host` | `std::string(void*)` |
| `bambu_network_get_user_selected_machine` | `std::string(void*)` |
| `bambu_network_set_user_selected_machine` | `int(void*, std::string dev_id)` |
| `bambu_network_modify_printer_name` | `int(void*, std::string dev_id, std::string dev_name)` |
| `bambu_network_get_printer_firmware` | `int(void*, std::string dev_id, unsigned* http_code, std::string* http_body)` |

`get_printer_firmware` is invoked from `MachineObject::get_firmware_info` (`src/slic3r/GUI/DeviceManager.cpp:3764`) on a background thread when the user opens **Device → Update**. A return value `< 0` makes Studio silently hide the firmware list (`m_firmware_valid = false`). Otherwise `http_body` is parsed as JSON with the following schema:

```json
{
  "devices": [{
    "dev_id": "<printer serial>",
    "firmware": [
      {
        "version": "01.08.02.00",
        "url": "https://public-cdn.bblmw.com/upgrade/.../ota.zip",
        "description": "optional release notes text (plain/markdown)"
      }
    ],
    "ams": [{
      "firmware": [
        { "version": "00.00.07.89", "url": "https://.../ams.bin", "description": "..." }
      ]
    }]
  }]
}
```

Studio creates a `FirmwareInfo item` per entry in `firmware[]` / `ams[].firmware[]` and derives the file name from the tail of `url` (`item.name = url.substr(url.find_last_of('/') + 1)`). If the name cannot be extracted, the entry is skipped. The `description` field is the text displayed in the **Release Notes** dialog.

Important: Studio does **not** read the currently installed version from this response — that arrives separately, through the MQTT `info.command=get_version` payload (array `info.module[]`, field `sw_ver`) and `push_status.upgrade_state.new_ver_list`. This ABI call answers only "what can be flashed" (plus, optionally, release notes for those versions). The **Update** button ultimately publishes `{"upgrade":{"command":"upgrade_confirm"}}` over LAN MQTT — the printer itself downloads the firmware from the CDN, and Studio uses the URL in `firmware[].url` only for the displayed file name.

When `devices[0].firmware[]` is empty (the currently installed firmware is already the newest one known to the printer), the Release Notes dialog opens empty — this is normal stock behaviour, not a bug.

### 6.8. Submitting a print job

Types:
- `OnUpdateStatusFn = std::function<void(int status, int code, std::string msg)>`
- `WasCancelledFn   = std::function<bool()>`
- `OnWaitFn         = std::function<bool(int status, std::string job_info)>`

The `PrintParams` struct (`src/slic3r/Utils/bambu_networking.hpp:192-241`) carries these fields: `dev_id`, `task_name`, `project_name`, `preset_name`, `filename`, `config_filename`, `plate_index`, `ftp_folder`, `ftp_file`, `ftp_file_md5`, `nozzle_mapping`, `ams_mapping`, `ams_mapping2`, `ams_mapping_info`, `nozzles_info`, `connection_type`, `comments`, `origin_profile_id`, `stl_design_id`, `origin_model_id`, `print_type`, `dst_file`, `dev_name`, `dev_ip`, `use_ssl_for_ftp`, `use_ssl_for_mqtt`, `username`, `password`, `task_bed_leveling`, `task_flow_cali`, `task_vibration_cali`, `task_layer_inspect`, `task_record_timelapse`, `task_timelapse_use_internal`, `task_use_ams`, `task_bed_type`, `extra_options`, `auto_bed_leveling`, `auto_flow_cali`, `auto_offset_cali`, `extruder_cali_manual_mode`, `task_ext_change_assist`, `try_emmc_print`.

| Symbol | Signature |
|--------|-----------|
| `bambu_network_start_print` | `int(void*, PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)` — cloud |
| `bambu_network_start_local_print_with_record` | `int(void*, PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)` — LAN + metadata upload |
| `bambu_network_start_send_gcode_to_sdcard` | `int(void*, PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)` |
| `bambu_network_start_local_print` | `int(void*, PrintParams, OnUpdateStatusFn, WasCancelledFn)` — LAN only |
| `bambu_network_start_sdcard_print` | `int(void*, PrintParams, OnUpdateStatusFn, WasCancelledFn)` |

Print-job stages — the `SendingPrintJobStage` enum (`bambu_networking.hpp:146-156`): `Create=0, Upload=1, Waiting=2, Sending=3, Record=4, WaitPrinter=5, Finished=6, ERROR=7, Limit=8`.

### 6.9. User presets

| Symbol | Signature |
|--------|-----------|
| `bambu_network_get_user_presets` | `int(void*, std::map<std::string, std::map<std::string, std::string>>* user_presets)` |
| `bambu_network_request_setting_id` | `std::string(void*, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)` |
| `bambu_network_put_setting` | `int(void*, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)` |
| `bambu_network_get_setting_list` | `int(void*, std::string bundle_version, ProgressFn, WasCancelledFn)` |
| `bambu_network_get_setting_list2` | `int(void*, std::string bundle_version, CheckFn, ProgressFn, WasCancelledFn)` |
| `bambu_network_delete_setting` | `int(void*, std::string setting_id)` |

`CheckFn = std::function<bool(std::map<std::string,std::string>)>`, `ProgressFn = std::function<void(int)>`.

All six entry points are thin wrappers over one REST resource —

```
<method> /v1/iot-service/api/slicer/setting[/<setting_id>]?version=<bundle>&public=false
```

— on the cloud API host; see §6.10.1 for base URL, headers and the common response envelope that apply here and to every other HTTP endpoint the plugin touches. Preset IDs are prefixed by type (observed in live responses):

| Type | ID prefix | Public counterpart |
|------|-----------|---------------------|
| `print` (process) | `PPUS…` | `GP…` |
| `filament` | `PFUS…` | `GFS…` / `GFL…` |
| `printer` (machine) | `PMUS…` | `GM…` |

#### 6.9.1. Per-method schema

**`GET /slicer/setting?public=false&version=<bundle>` — list metadata** (called from `get_setting_list` / `get_setting_list2`):

```json
{
  "message": "success", "code": null, "error": null,
  "print":    { "private": [ /* Meta, … */ ], "public": [] },
  "printer":  { "private": [ /* Meta, … */ ], "public": [] },
  "filament": { "private": [ /* Meta, … */ ], "public": [] },
  "settings": []
}
```

Every entry in `private[]` is metadata only — no `setting` payload:

```json
{
  "setting_id": "PFUS7bf6d4b8df15d8",
  "name": "Bambu PLA Tough @BBL P1P 0.2 nozzle",
  "version": "0.0.0.0",
  "update_time": "2026-04-06 19:03:50",
  "base_id": null,
  "filament_id": null,
  "filament_vendor": null,
  "filament_type": null,
  "filament_is_support": null,
  "nozzle_temperature": null,
  "nozzle_hrc": null,
  "inherits": null,
  "nickname": null
}
```

`update_time` is rendered as `"YYYY-MM-DD HH:MM:SS"` in UTC; `load_user_preset()` expects unix seconds, so the plugin converts.

**`GET /slicer/setting/<setting_id>` — full preset** (observed only by direct probe; the stock plugin does **not** call it):

```json
{
  "message": "success", "code": null, "error": null,
  "setting_id": "PFUS7bf6d4b8df15d8",
  "name": "Bambu PLA Tough @BBL P1P 0.2 nozzle",
  "type": "filament",
  "version": "0.0.0.0",
  "base_id": null, "filament_id": null, "nickname": null,
  "update_time": "2026-04-06 19:03:50",
  "public": false,
  "setting": {
    "activate_air_filtration": 0,
    "compatible_printers": "\"Bambu Lab P1P 0.2 nozzle\"",
    "filament_type": "\"PLA\"",
    "...": "..."
  }
}
```

Values inside `setting` are already in the `ConfigOption::serialize()` form Studio's loader expects (quoted scalars, semicolon-separated lists, etc.). Some keys are echoed as native JSON numbers instead of strings; callers coerce.

The `user_id` of the owner is **not** returned by either endpoint — `PresetCollection::load_user_preset()` requires it, so callers must inject their own from the authenticated session.

**`POST /slicer/setting` — create** (called from `request_setting_id`). Request:

```json
{
  "name": "<preset name>",
  "type": "filament|print|printer",
  "version": "<bundle version>",
  "base_id": "<parent system preset id or empty>",
  "filament_id": "<filament id or empty>",
  "setting": { "<option>": "<serialized value>", "...": "..." }
}
```

Response on success:

```json
{ "message": "success", "code": null, "error": null,
  "setting_id": "PFUSdce8291f0b44ab",
  "update_time": "2026-04-21 17:56:43" }
```

Missing mandatory fields return `HTTP 400` with a plain-text error (e.g. `field "version" is not set`); `type` outside `{print,filament,printer}` returns `HTTP 422` with `{"detail":"Invalid input parameters"}`.

**`PATCH /slicer/setting/<setting_id>` — update** (called from `put_setting`): same body shape as `POST`; same response. `PATCH` against a non-existent id returns `HTTP 422`.

**`DELETE /slicer/setting/<setting_id>` — remove** (called from `delete_setting`): `{"message":"success","code":null,"error":null}`. Idempotent: `DELETE` of a missing id still answers `200`.

#### 6.9.2. `values_map` keys the loader expects

`PresetCollection::load_user_preset(name, values_map, ...)` rejects a preset unless `values_map` contains, at minimum:

| Key | Source | Notes |
|-----|--------|-------|
| `version` | response `version` | Must be parseable by `Semver::parse`; preset is skipped if cloud major > Studio major. |
| `setting_id` | response `setting_id` | Used as the stable identifier Studio writes back into the preset file. |
| `updated_time` | response `update_time` | **Unix seconds as a decimal string**, not the ISO string the server returns. |
| `user_id` | authenticated session | Server does not include it; caller must inject. |
| `base_id` | response `base_id` | Empty string when the preset is a custom root. |
| `type` | response `type` | `print` / `filament` / `printer`; top-level collection key in list response. |
| `filament_id` | response `filament_id` | Only mandatory when `type == "filament"` and `base_id` is empty. |
| `inherits` | inside `setting` | Pass-pass from cloud; parent lookup during load. |
| (all other preset options) | inside `setting` | Merged into `DynamicPrintConfig` via `load_string_map`. |

On a fresh machine Studio's local preset cache is empty, so the stock plugin's metadata-only list walk produces no visible presets — the loader has a `setting_id` but no `setting` map to merge in. Cross-device sync therefore only works if the plugin *also* issues `GET /slicer/setting/<id>` per entry and builds the full `values_map` itself.

#### 6.9.3. Call sequence

`GUI_App::start_sync_user_preset()` drives the whole thing on a worker thread (`src/slic3r/GUI/GUI_App.cpp`):

1. One-shot catalogue walk:
   1. `m_agent->get_setting_list2(bundle_version, check_fn, progress_fn, cancel_fn)` — enumerates all user presets. For each catalogue entry the plugin invokes `check_fn` with `{type, name, setting_id, updated_time}`; the closure returns `true` when the local `PresetCollection::need_sync()` says this row is newer than the on-disk copy. Progress 0-100 drives a modal `ProgressDialog`.
   2. On success Studio calls `reload_settings()`, which calls `m_agent->get_user_presets(&map)` and feeds the map into `preset_bundle->load_user_presets(app_config, map, ...)`.
2. Continuous background loop, 100 ms tick, every 20 ticks:
   1. For each of `print` / `filament` / `printer` collections, `PresetCollection::get_user_presets(&result_presets)` produces the dirty local presets.
   2. Each dirty preset is handed to `sync_preset(preset)`, which calls `get_differed_values_to_update` to produce a `values_map`, then:
      - `preset->sync_info == "create"` or empty → `request_setting_id(name, &values_map, &http_code)` (POST).
      - `preset->sync_info == "update"` → `put_setting(setting_id, name, &values_map, &http_code)` (PATCH).
   3. `delete_cache_presets` list (presets removed locally) → `delete_setting(id)` one-by-one.

The sync loop checks `values_map["code"] == "14"` to detect the server's "preset quota exceeded" response and shows a `BBLUserPresetExceedLimit` notification without retrying further creates for that preset type.

### 6.10. HTTP / cloud service

| Symbol | Signature |
|--------|-----------|
| `bambu_network_get_studio_info_url` | `std::string(void*)` |
| `bambu_network_set_extra_http_header` | `int(void*, std::map<std::string, std::string>)` |
| `bambu_network_get_my_message` | `int(void*, int type, int after, int limit, unsigned int* http_code, std::string* http_body)` |
| `bambu_network_check_user_task_report` | `int(void*, int* task_id, bool* printable)` |
| `bambu_network_get_user_print_info` | `int(void*, unsigned int* http_code, std::string* http_body)` |
| `bambu_network_get_user_tasks` | `int(void*, TaskQueryParams, std::string* http_body)` |
| `bambu_network_get_task_plate_index` | `int(void*, std::string task_id, int* plate_index)` |
| `bambu_network_get_subtask_info` | `int(void*, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)` |
| `bambu_network_get_slice_info` | `int(void*, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)` |
| `bambu_network_report_consent` | `int(void*, std::string expand)` |

`TaskQueryParams` (`bambu_networking.hpp:243-249`): `dev_id`, `status`, `offset`, `limit`.

#### 6.10.1. Common cloud transport

Every REST call the plugin makes — authentication, bind, print-job orchestration, preset sync, device firmware, MakerWorld — lands on the same regional API host, chosen by the user's `country_code` in `app_config` (the same switch `GUI_App::get_http_url` uses for the plugin manifest, see §2.1):

| Region | API host | Web host |
|--------|----------|----------|
| `US` / default | `https://api.bambulab.com` | `https://bambulab.com` |
| `CN` | `https://api.bambulab.cn` | `https://bambulab.cn` |

All authenticated endpoints require exactly one mandatory header:

```
Authorization: Bearer <access_token>
```

MITM dumps of the stock plugin show it also sending the full Studio fingerprint on every request — `User-Agent: bambu_network_agent/<ver>`, plus `X-BBL-Client-ID`, `X-BBL-Client-Name`, `X-BBL-Client-Type`, `X-BBL-Client-Version`, `X-BBL-Device-ID`, `X-BBL-Language`, `X-BBL-OS-Type`, `X-BBL-OS-Version`, `X-BBL-Agent-Version`, `X-BBL-Executable-info`, `X-BBL-Agent-OS-Type`, and anything Studio injects through `bambu_network_set_extra_http_header`. Direct probes against the production server confirm that **none** of the `X-BBL-*` headers, nor even the custom `User-Agent`, are required for the API to accept the call. They influence analytics only.

Most JSON responses share a common envelope:

```json
{
  "message": "success" | "<human message>",
  "code":    null      | <integer error>,
  "error":   null      | "<string>",
  "...endpoint-specific fields..."
}
```

`code` is the "business" error code the GUI inspects (for example `14` for preset quota exceeded, `2` for missing resources). Transport-level failures surface as non-2xx HTTP codes — typically `400` for malformed bodies, `401` for a missing/expired bearer, `422` for invalid-input (e.g. `PATCH` against an unknown ID), `5xx` for server-side failures.

For endpoints that return a plain-text error (notably `POST /slicer/setting` with a missing mandatory field) the body is a bare string — the envelope is absent.

#### 6.10.2. What each ABI call does behind the curtain

All paths below are relative to the regional API host from §6.10.1. The "evidence" column states how firm the mapping is — either `MITM` (seen in a live dump of the stock plugin), `probe` (issued by hand with `curl` against production), `source` (read out of Studio's own code) or `stub` (the plugin never hits the network and Studio is happy with a canned response).

- **`get_studio_info_url`** — string accessor, no HTTP call. The stock plugin returns a URL for the "news / banner" side panel (usually a MakerWorld page); an empty string disables the panel. `open-bambu-networking` returns empty. *Evidence: source, stub.*
- **`set_extra_http_header`** — pure state update. Studio calls it during startup and on region/language switches to attach fingerprint headers to every subsequent request. The plugin stores the map and folds it into outgoing header sets; the server ignores the contents. *Evidence: source.*
- **`get_my_message`** — the Message Centre bell polls this for `(type, after, limit)`. Studio parses `http_body` as JSON and expects an envelope with a `messages[]` array. The exact URL was not captured in our MITM dumps (the stock plugin only emits it when there is something in the cloud inbox for the user); the most likely candidate from community traces is `GET /v1/user-service/my/messages?type=<t>&after=<unix>&limit=<n>`. The plugin currently returns an empty body with `http_code = 0` — Studio's parser treats that as "no messages" and the bell stays clear. *Evidence: source + stub; URL unconfirmed.*
- **`check_user_task_report`** — polled after every print to decide whether to show the "rate this print" prompt. The output contract is `*task_id` (zero means "nothing to report") and `*printable`. Stock endpoint was not captured; `open-bambu-networking` returns `0 / false` unconditionally, which is the documented way to suppress the popup. *Evidence: source + stub; URL unconfirmed.*
- **`get_user_print_info`** — `GET /v1/iot-service/api/user/bind`. This is the single source for the cloud side of the Devices tab. Response shape (from MITM plus our own probes): `{"devices":[{ "dev_id", "name", "online", "print_status", "dev_model_name", "dev_product_name", "dev_access_code", ... }]}`. Studio's `DeviceManager::parse_user_print_info` reads slightly different field names — `dev_name`, `dev_online`, `task_status` — so the plugin remaps on the way out (see `src/abi_http.cpp::remap_bind_payload`). *Evidence: MITM + probe.*
- **`get_user_tasks`** — the Cloud Task / History grid. Studio passes the whole `http_body` through to its JSON parser. The stock endpoint is not captured in our dumps. Plugin currently returns an empty body, which leaves the grid empty. *Evidence: source + stub; URL unconfirmed.*
- **`get_task_plate_index`** — looks up which plate a given cloud `task_id` ran on. Studio falls back to plate `0` on failure. Plugin returns `plate_index = -1`. *Evidence: source + stub; URL unconfirmed.*
- **`get_subtask_info`** — MakerWorld subtask detail fetch; Studio pulls the printer-card hero image from `context.plates[<plate_idx>].thumbnail.url` in the response. `content` is a JSON *string* holding an inner `{info:{plate_idx}}` envelope — both shapes are in `DeviceManager.cpp`. The stock cloud URL is unconfirmed; under `OBN_ENABLE_WORKAROUNDS` the plugin synthesises a minimal response whenever the subtask id looks like `lan-<fnv>` (emitted by our own LAN push-status rewrite) and points `url` at the local `cover_server` serving the PNG extracted from `/cache/<name>.3mf`. See `src/abi_http.cpp::bambu_network_get_subtask_info`. *Evidence: source + workaround; cloud URL unconfirmed.*
- **`get_slice_info`** — slice summary (time / weight / material cost / layer thumbnails) for a cloud task. Plugin returns empty. *Evidence: source + stub; URL unconfirmed.*
- **`report_consent`** — one-shot "I accepted the privacy / telemetry dialog" notification, body `{"expand":"<flag>"}`. Studio ignores the return value. Plugin returns `0` without hitting the network. *Evidence: source + stub; URL unconfirmed.*

The plugin's other HTTP-heavy surfaces follow the same transport and envelope rules but live in their own sections because of their size. The endpoints below are all verified against real traffic unless marked:

| Concern | Endpoint(s) | Section | Evidence |
|---------|-------------|---------|----------|
| Bearer-token login / refresh / profile | `POST /v1/user-service/user/ticket/<T>`, `POST /v1/user-service/user/refreshtoken`, `GET /v1/user-service/my/profile` | §6.5 | MITM + probe |
| Device bind / unbind / rename | `POST /v1/iot-service/api/user/bind`, `GET /v1/iot-service/api/user/bind`, `PATCH /v1/iot-service/api/user/device/info`, `DELETE /v1/iot-service/api/user/bind?dev_id=<id>` | §6.6 | MITM + probe |
| Printer firmware catalogue | stock: unknown cloud catalogue call; ours: synthesised from MQTT state | §6.7 | source only (stock) |
| Cloud print-job pipeline | `POST /v1/iot-service/api/user/project`, `PUT <presigned>`, `PUT /v1/iot-service/api/user/notification`, `GET /v1/iot-service/api/user/notification?action=upload&ticket=<t>`, `PATCH /v1/iot-service/api/user/project/<pid>`, `GET /v1/iot-service/api/user/upload?models=<mid>_<plate>.3mf`, `POST /v1/user-service/my/task` | §6.8 | MITM |
| User presets sync | `<m> /v1/iot-service/api/slicer/setting[/<id>]?public=false&version=<bundle>` | §6.9 | MITM + probe |
| MakerWorld / Mall, OSS upload | various `design-service` / `iot-service` / OSS paths | §6.12 | not captured |
| Camera / live view / HMS snapshot | not captured | §6.11 | — |
| Analytics / telemetry | not captured | §6.13 | — |

### 6.11. Camera

| Symbol | Signature |
|--------|-----------|
| `bambu_network_get_camera_url` | `int(void*, std::string dev_id, std::function<void(std::string)>)` |
| `bambu_network_get_camera_url_for_golive` | `int(void*, std::string dev_id, std::string sdev_id, std::function<void(std::string)>)` |
| `bambu_network_get_hms_snapshot` | `int(void*, std::string& dev_id, std::string& file_name, std::function<void(std::string, int)>)` |

### 6.12. MakerWorld / Mall

| Symbol | Signature |
|--------|-----------|
| `bambu_network_get_design_staffpick` | `int(void*, int offset, int limit, std::function<void(std::string)>)` |
| `bambu_network_start_publish` | `int(void*, PublishParams, OnUpdateStatusFn, WasCancelledFn, std::string* out)` |
| `bambu_network_get_model_publish_url` | `int(void*, std::string* url)` |
| `bambu_network_get_subtask` | `int(void*, BBLModelTask* task, OnGetSubTaskFn)` |
| `bambu_network_get_model_mall_home_url` | `int(void*, std::string* url)` |
| `bambu_network_get_model_mall_detail_url` | `int(void*, std::string* url, std::string id)` |
| `bambu_network_put_model_mall_rating` | `int(void*, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)` |
| `bambu_network_get_oss_config` | `int(void*, std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)` |
| `bambu_network_put_rating_picture_oss` | `int(void*, std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)` |
| `bambu_network_get_model_mall_rating` | `int(void*, int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)` |
| `bambu_network_get_mw_user_preference` | `int(void*, std::function<void(std::string)>)` |
| `bambu_network_get_mw_user_4ulist` | `int(void*, int seed, int limit, std::function<void(std::string)>)` |

`PublishParams` (`bambu_networking.hpp:251-258`): `project_name`, `project_3mf_file`, `preset_name`, `project_model_id`, `design_id`, `config_filename`.

### 6.13. Tracking / telemetry

| Symbol | Signature |
|--------|-----------|
| `bambu_network_track_enable` | `int(void*, bool enable)` |
| `bambu_network_track_remove_files` | `int(void*)` |
| `bambu_network_track_event` | `int(void*, std::string evt_key, std::string content)` |
| `bambu_network_track_header` | `int(void*, std::string header)` |
| `bambu_network_track_update_property` | `int(void*, std::string name, std::string value, std::string type)` |
| `bambu_network_track_get_property` | `int(void*, std::string name, std::string& value, std::string type)` |

These are used only for analytics — a plugin that simply returns `0` from all of them is functionally indistinguishable for Studio's own code paths.

### 6.14. File Transfer ABI (`ft_*`)

This subsystem is initialized right after `bambu_networking` loads, via `InitFTModule(networking_module)`, and resolves its symbols from the same module (`src/slic3r/Utils/FileTransferUtils.hpp`, `FileTransferUtils.cpp`):

```71:95:src/slic3r/Utils/FileTransferUtils.hpp
using fn_ft_abi_version        = int(FT_CALL *)();
using fn_ft_free               = void(FT_CALL *)(void *);
using fn_ft_job_result_destroy = void(FT_CALL *)(ft_job_result *);
using fn_ft_job_msg_destroy    = void(FT_CALL *)(ft_job_msg *);

using fn_ft_tunnel_create        = ft_err(FT_CALL *)(const char *url, FT_TunnelHandle **out);
using fn_ft_tunnel_retain        = void(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_release       = void(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_start_connect = ft_err(FT_CALL *)(FT_TunnelHandle *, void(FT_CALL *)(void *user, int ok, int err, const char *msg), void *user);
using fn_ft_tunnel_sync_connect  = ft_err(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_set_status_cb = ft_err(FT_CALL *)(FT_TunnelHandle *, void(FT_CALL *)(void *user, int old_status, int new_status, int err, const char *msg), void *user);
using fn_ft_tunnel_shutdown      = ft_err(FT_CALL *)(FT_TunnelHandle *);

using fn_ft_job_create        = ft_err(FT_CALL *)(const char *params_json, FT_JobHandle **out);
using fn_ft_job_retain        = void(FT_CALL *)(FT_JobHandle *);
using fn_ft_job_release       = void(FT_CALL *)(FT_JobHandle *);
using fn_ft_job_set_result_cb = ft_err(FT_CALL *)(FT_JobHandle *, void(FT_CALL *)(void *user, ft_job_result result), void *user);
using fn_ft_job_get_result    = ft_err(FT_CALL *)(FT_JobHandle *, uint32_t timeout_ms, ft_job_result *out_result);
using fn_ft_tunnel_start_job  = ft_err(FT_CALL *)(FT_TunnelHandle *, FT_JobHandle *);
using fn_ft_job_cancel        = ft_err(FT_CALL *)(FT_JobHandle *);
using fn_ft_job_set_msg_cb    = ft_err(FT_CALL *)(FT_JobHandle *, void(FT_CALL *)(void *user, ft_job_msg msg), void *user);
using fn_ft_job_try_get_msg   = ft_err(FT_CALL *)(FT_JobHandle *, ft_job_msg *out_msg);
using fn_ft_job_get_msg       = ft_err(FT_CALL *)(FT_JobHandle *, uint32_t timeout_ms, ft_job_msg *out_msg);
```

Unlike `bambu_network_*`, this is a **pure C ABI**. Calling convention: `__cdecl` on Windows.

`ft_err`:
```cpp
typedef enum { FT_OK = 0, FT_EINVAL = -1, FT_ESTATE = -2, FT_EIO = -3,
               FT_ETIMEOUT = -4, FT_ECANCELLED = -5, FT_EXCEPTION = -6,
               FT_EUNKNOWN = -128 } ft_err;
```

Result / message structs:

```27:40:src/slic3r/Utils/FileTransferUtils.hpp
struct ft_job_result { int ec; int resp_ec; const char *json; const void *bin; uint32_t bin_size; };
struct ft_job_msg    { int kind; const char *json; };
```

Studio expects `ft_abi_version() == 1` (the default `abi_required` in `InitFTModule`).

Semantically, this ABI describes a "tunnel + job" bus: open a connection to the printer (`ft_tunnel_create` from a `url`), start jobs on it, listen for results and messages.

### 6.15. Error codes

The complete list of error values the plugin is expected to return through `int` lives in `src/slic3r/Utils/bambu_networking.hpp:13-94` (general, bind, `start_local_print_with_record`, `start_print`, `start_local_print`, `start_send_gcode_to_sdcard`, connection).

---

## 7. Additional notes

1. **Sanity entry point for debugging**: immediately after `create_agent` Studio makes the exact sequence of calls documented in § 6.1 ("Initialization sequence"). Observing those in order is the shortest way to confirm that the ABI is wired correctly.
2. `QueueOnMainFn` is critical: nearly every UI-touching callback must be dispatched through this lambda — wxWidgets is not thread-safe, and direct calls from the plugin's worker threads will race.
3. **Client certificates**: the file `<resources>/cert/slicer_base64.cer` is the root CA bundle Bambu uses for TLS/MQTT. It is handed to the plugin via `bambu_network_set_cert_file`.
4. **ABI/STL compatibility** is the single biggest foot-gun of this contract: the plugin has to be built with the exact same toolchain that built Bambu Studio (matching MSVC runtime on Windows, matching libstdc++ ABI on Linux, matching Xcode/libc++ on macOS). Any mismatch is undefined behaviour the moment a `std::string` / `std::map` crosses the library boundary.

---

## 8. Map of key source locations

| Topic | File:lines |
|-------|------------|
| Resolution of all 100+ symbols | `src/slic3r/Utils/NetworkAgent.cpp:279-382` |
| API typedefs | `src/slic3r/Utils/NetworkAgent.hpp:10-115` |
| Name constants | `src/slic3r/Utils/bambu_networking.hpp:97-100` |
| Error codes | `src/slic3r/Utils/bambu_networking.hpp:13-94` |
| Data structures | `src/slic3r/Utils/bambu_networking.hpp:180-275` |
| `InitFTModule` / `UnloadFTModule` | `src/slic3r/Utils/FileTransferUtils.hpp:239-253` |
| `ft_*` symbol resolution | `src/slic3r/Utils/FileTransferUtils.cpp:12-37` |
| Signature verification | `src/slic3r/Utils/CertificateVerify.cpp:289-300` |
| Signature bypass | `app_config → ignore_module_cert`; `src/slic3r/GUI/GUI_App.cpp:3423` |
| Request URL | `src/slic3r/GUI/GUI_App.cpp:1469-1556` |
| Plugin download | `src/slic3r/GUI/GUI_App.cpp:1573-1761` |
| Extraction / installation | `src/slic3r/GUI/GUI_App.cpp:1763-1912` |
| Version check | `src/slic3r/GUI/GUI_App.cpp:1982-1998` |
| Restart networking | `src/slic3r/GUI/GUI_App.cpp:1914-1957` |
| Removal | `src/slic3r/GUI/GUI_App.cpp:1959-1973` |
| OTA copy-in | `src/slic3r/GUI/GUI_App.cpp:3359-3419` |
| Agent initialization | `src/slic3r/GUI/GUI_App.cpp:3421-3519` |
| OTA `sync_plugins` | `src/slic3r/Utils/PresetUpdater.cpp:1165-1253` |
| `sync_resources` (shared engine) | `src/slic3r/Utils/PresetUpdater.cpp:561-737` |
| OTA cache validation | `src/slic3r/Utils/PresetUpdater.cpp:1131-1163` |
| UI install job | `src/slic3r/GUI/Jobs/UpgradeNetworkJob.cpp:16-146` |
| "Downloading Bambu Network Plug-in" dialog | `src/slic3r/GUI/DownloadProgressDialog.cpp` |

---

## Summary

Key facts about the stock Bambu Network Plugin, distilled from the sections above:

- **Download source**: `https://api.bambulab.com/v1/iot-service/api/slicer/resource?slicer/plugins/cloud=<MAJOR.MINOR.PATCH.00>` (or the regional `.cn` / dev / QA endpoints), which returns a JSON manifest pointing at a ZIP.
- **Install layout**: the binary ends up at `<data_dir>/plugins/{bambu_networking,BambuSource,live555}.{dll|so|dylib}`; OTA staging in `<data_dir>/ota/plugins/` must hold all three libraries plus `network_plugins.json` or the cache is treated as incomplete.
- **Version gate**: Studio compares only the first 8 characters of `bambu_network_get_version()` against `SLIC3R_VERSION`; everything beyond that is build metadata.
- **Signature gate**: Authenticode publisher match on Windows, Developer Team ID match on macOS; on Linux the check is a no-op. `ignore_module_cert` in `AppConfig` disables it on Windows/macOS.
- **ABI surface**: roughly 100 `bambu_network_*` entry points using C linkage but `std::string` / `std::vector` / `std::map` / `std::function` at the boundary — tightly coupled to Studio's libstdc++/libc++ ABI — plus a separate, pure-C `ft_*` tunnel/job bus (`ft_abi_version() == 1`) that ships in the same `.so`/`.dll`.
- **Initialization contract**: a deterministic call sequence `create_agent → set_config_dir → init_log → set_cert_file → set_extra_http_header → set_on_*_fn(…) → set_country_code → start → start_discovery` (`GUI_App::on_init_network`), with `QueueOnMainFn` as the only safe way back to the GUI thread.
- **Notable Studio quirks observed during reverse engineering**: the `bambu_network_get_user_nickanme` symbol name is misspelled in the real ABI, and Studio mistakenly resolves `get_my_token` through the string `"bambu_network_get_my_profile"` — a compatible plugin must export both, with matching signatures.
