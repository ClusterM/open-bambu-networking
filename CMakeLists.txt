cmake_minimum_required(VERSION 3.20)
project(open-bambu-networking
    VERSION 0.1.0
    DESCRIPTION "Open-source replacement for Bambu Studio's bambu_networking plugin"
    LANGUAGES CXX
)

# The plugin's reported version must have the same first-8-char prefix as
# Bambu Studio's SLIC3R_VERSION. AppImage v02.05.02.51 expects prefix
# "02.05.02"; source builds from the main branch currently expect "02.05.03".
set(OBN_VERSION "02.05.02.99" CACHE STRING "Plugin version reported via bambu_network_get_version()")

# When ON, the ft_* C ABI used by Studio's "Send to Printer" dialog and
# the Print job's eMMC pre-flight speaks a real transport instead of
# reporting an immediate I/O error. The transport is FTPS (same one we
# use for LAN print uploads), not the proprietary port-6000 tunnel, so
# only `bambu:///local/*` URLs are served; TUTK/Agora URLs still get
# FT_EIO. With this OFF, every ft_* entry point is a polite-failure
# stub and Studio falls back to its internal FTP path (our
# start_send_gcode_to_sdcard / start_local_print entry points). Both
# modes produce the same file in the same place on the printer; the
# FTPS fastpath gives the more native dialog sequence (ability probe
# / storage picker / percent progress).
option(OBN_FT_FTPS_FASTPATH "Serve ft_* ABI calls over FTPS for LAN URLs" ON)

# Toggles every non-stock behaviour in the plugin. With this ON the plugin
# makes choices the original bambu_networking.so never would in order to
# get Developer-Mode LAN printers (most notably the P2S, which has a USB
# stick in place of an SD card) fully functional in Studio. With this
# OFF the plugin stays a strict drop-in: the same MQTT/HTTP/FTPS/MJPEG
# paths the stock plugin uses, and nothing else. Features that only exist
# via a workaround disappear cleanly (no partial UI, no runtime errors):
#   - push_status `home_flag` SDCARD-bits rewrite (P2S reports NO_SDCARD)
#   - push_status `ipcam.file` block injection (unblocks MediaFilePanel on P2S)
#   - `start_sdcard_print` routed over LAN MQTT instead of the cloud API
#   - RTSPS camera transcode to MJPEG inside libBambuSource
#     (stock pipes H.264 straight to Studio's gstbambusrc)
#   - PrinterFileSystem CTRL bridge served from a worker thread over FTPS
#     (stock speaks a proprietary port-6000 CTRL protocol)
# See "What works" and "Workaround reference" in README.md.
option(OBN_ENABLE_WORKAROUNDS "Enable non-stock workaround paths (see README)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Bambu Studio is built against libstdc++ new C++11 ABI; match it.
add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=1)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Hide every symbol by default, explicitly export only the ABI functions via
# __attribute__((visibility("default"))). Keeps the library slim and avoids
# name clashes with Studio's own symbols.
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)

# --------------------------------------------------------------------------
# libbambu_networking.so  (main plugin)
# --------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
pkg_check_modules(CURL     REQUIRED IMPORTED_TARGET libcurl)
find_package(OpenSSL REQUIRED)
# zlib is shared between bambu_networking (cover_cache: unpacks
# Metadata/plate_N.png out of /cache/<name>.3mf to show it as the
# printing-task thumbnail) and BambuSource (same unpack, but for the
# file browser).
find_package(ZLIB REQUIRED)

add_library(bambu_networking SHARED
    src/abi_meta.cpp
    src/abi_lifecycle.cpp
    src/abi_callbacks.cpp
    src/abi_cloud.cpp
    src/abi_lan.cpp
    src/abi_user.cpp
    src/abi_bind.cpp
    src/abi_print.cpp
    src/abi_presets.cpp
    src/abi_http.cpp
    src/abi_camera.cpp
    src/abi_makerworld.cpp
    src/abi_track.cpp
    src/abi_ft.cpp
    src/agent.cpp
    src/auth.cpp
    src/cert_store.cpp
    src/cloud_auth.cpp
    src/cloud_print.cpp
    src/cloud_session.cpp
    src/cover_cache.cpp
    src/cover_server.cpp
    src/ftps.cpp
    src/http_client.cpp
    src/json_lite.cpp
    src/lan_session.cpp
    src/log.cpp
    src/mqtt_client.cpp
    src/print_job.cpp
    src/ssdp.cpp
    src/zip_reader.cpp
)

target_include_directories(bambu_networking PRIVATE include)
target_compile_definitions(bambu_networking PRIVATE
    OBN_VERSION_STRING="${OBN_VERSION}"
    OBN_FT_FTPS_FASTPATH=$<BOOL:${OBN_FT_FTPS_FASTPATH}>
    OBN_ENABLE_WORKAROUNDS=$<BOOL:${OBN_ENABLE_WORKAROUNDS}>
)
target_link_libraries(bambu_networking PRIVATE
    PkgConfig::MOSQUITTO
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

# No "lib" prefix rename: output is literally "libbambu_networking.so"
set_target_properties(bambu_networking PROPERTIES
    OUTPUT_NAME "bambu_networking"
    PREFIX      "lib"
)

# --------------------------------------------------------------------------
# libBambuSource.so  (camera MJPG tunnel for P1/A1; consumed by Studio's
# gstbambusrc element).
# --------------------------------------------------------------------------

pkg_check_modules(GST      REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GST_APP  REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
# zlib is pulled in for the Metadata/plate_*.png unpacker used by
# PrinterFileSystem to serve 3mf thumbnails directly from FTPS.
find_package(ZLIB REQUIRED)
# libpng is used to letterbox 3mf Metadata/plate_*.png and libjpeg to
# decode the /timelapse/thumbnail/*.jpg sidecars - both get padded to
# the aspect of the Studio tile they're shown in (see
# letterbox_image_to_aspect in stubs/BambuSource.cpp).
pkg_check_modules(PNG      REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(JPEG     REQUIRED IMPORTED_TARGET libjpeg)

# BambuSource ships the PrinterFileSystem CTRL bridge (FTPS-backed file
# browser for X1/P1S/P2S/A-series in LAN mode), so it needs ftps + json +
# log from the core tree. Compile them in directly instead of linking
# against libbambu_networking to keep the shared-object self-contained
# (Studio dlopens BambuSource *before* it loads the network plugin).
add_library(BambuSource SHARED
    stubs/BambuSource.cpp
    src/ftps.cpp
    src/json_lite.cpp
    src/log.cpp
    src/zip_reader.cpp
)
target_include_directories(BambuSource PRIVATE include)
target_compile_definitions(BambuSource PRIVATE
    OBN_ENABLE_WORKAROUNDS=$<BOOL:${OBN_ENABLE_WORKAROUNDS}>
)
set_target_properties(BambuSource PROPERTIES OUTPUT_NAME "BambuSource" PREFIX "lib")
target_link_libraries(BambuSource PRIVATE
    OpenSSL::SSL OpenSSL::Crypto
    PkgConfig::GST PkgConfig::GST_APP
    ZLIB::ZLIB
    PkgConfig::PNG
    PkgConfig::JPEG
)

# --------------------------------------------------------------------------
# liblive555.so  (stub; used by the real BambuSource internally)
# --------------------------------------------------------------------------

add_library(live555 SHARED stubs/live555.cpp)
set_target_properties(live555 PROPERTIES OUTPUT_NAME "live555" PREFIX "lib")

# --------------------------------------------------------------------------
# install (copies into <prefix>/plugins/)
# --------------------------------------------------------------------------

install(TARGETS bambu_networking BambuSource live555
    LIBRARY DESTINATION plugins
)

# Studio's PresetUpdater expects this JSON to declare the cached version in
# <data_dir>/ota/plugins/; we ship the same file so the updater trusts our
# installation.
configure_file(packaging/network_plugins.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/network_plugins.json @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/network_plugins.json
    DESTINATION ota/plugins)

# --------------------------------------------------------------------------
# tests (compiled only if BUILD_TESTING is on)
# --------------------------------------------------------------------------

option(OBN_BUILD_TESTS "Build smoke tests" ON)
if(OBN_BUILD_TESTS)
    enable_testing()
    add_executable(probe_plugin tests/probe_plugin.cpp)
    target_include_directories(probe_plugin PRIVATE include)
    target_link_libraries(probe_plugin PRIVATE dl)
    add_test(NAME plugin_symbols
        COMMAND probe_plugin $<TARGET_FILE:bambu_networking>)

    # Live test: skipped unless OBN_PRINTER_IP etc. are exported when ctest
    # runs. Links against the same library as the plugin so it can exercise
    # obn::Agent directly.
    add_executable(ssdp_listener_test tests/ssdp_listener_test.cpp
        src/log.cpp
        src/ssdp.cpp)
    target_include_directories(ssdp_listener_test PRIVATE include)

    add_executable(ftps_live_test tests/ftps_live_test.cpp
        src/ftps.cpp
        src/log.cpp)
    target_include_directories(ftps_live_test PRIVATE include)
    target_link_libraries(ftps_live_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    add_executable(lan_live_test tests/lan_live_test.cpp
        src/agent.cpp
        src/auth.cpp
        src/cert_store.cpp
        src/cloud_auth.cpp
        src/cloud_session.cpp
        src/http_client.cpp
        src/json_lite.cpp
        src/lan_session.cpp
        src/log.cpp
        src/mqtt_client.cpp
        src/ssdp.cpp)
    target_include_directories(lan_live_test PRIVATE include)
    target_compile_definitions(lan_live_test PRIVATE
        OBN_VERSION_STRING="${OBN_VERSION}"
    )
    target_link_libraries(lan_live_test PRIVATE
        PkgConfig::MOSQUITTO
        PkgConfig::CURL
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME lan_live COMMAND lan_live_test)
endif()
