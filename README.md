# open-bambu-networking

Open-source drop-in replacement for Bambu Studio's proprietary `bambu_networking`
plugin, based on reverse-engineered protocols from
[OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) and reference
implementations from
[bambulabs_api](https://github.com/acse-ci223/bambulabs_api) and
[ha_bambu_lab](https://github.com/greghesp/ha-bambulab).

> **Status:** Phase 1 (ABI scaffolding). Exports all ~128 symbols Bambu Studio
> looks up. All functions are currently no-op stubs. Studio loads the library
> successfully; nothing works yet.

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
