cmake_minimum_required(VERSION 3.20)

# Allow the top-level script(s) to ship a vcpkg toolchain without the
# caller having to remember -DCMAKE_TOOLCHAIN_FILE=... A manifest at
# the repo root takes precedence (that is what configure.ps1 emits).
if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file" FORCE)
endif()

project(open-bambu-networking
    VERSION 0.1.0
    DESCRIPTION "Open-source replacement for Bambu Studio's bambu_networking plugin"
    LANGUAGES CXX
)

# Default install location: same tree Bambu Studio uses on Linux (~/.config/BambuStudio).
# On Windows it sits under %APPDATA%/BambuStudio. Override with
# -DCMAKE_INSTALL_PREFIX=... when needed.
if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
            set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.config/BambuStudio"
                CACHE PATH "Install prefix (plugins/ and ota/plugins/ under it)" FORCE)
        endif()
    endif()
elseif(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        if(DEFINED ENV{APPDATA} AND NOT "$ENV{APPDATA}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{APPDATA}/BambuStudio" _obn_default_prefix)
            set(CMAKE_INSTALL_PREFIX "${_obn_default_prefix}"
                CACHE PATH "Install prefix (plugins/ and ota/plugins/ under it)" FORCE)
        endif()
    endif()
endif()

# The plugin's reported version must have the same first-8-char prefix as
# Bambu Studio's SLIC3R_VERSION (AppImage v02.05.02.51 expects prefix
# "02.05.02"; source builds from the main branch currently expect
# "02.05.03"). We deliberately refuse to pick a default: a stale hardcode
# silently breaks Studio's compatibility gate on the user's machine,
# which is a much worse failure mode than failing at configure time.
# The top-level ./configure wrapper auto-detects this from the installed
# BambuStudio.conf; bare `cmake -S . -B build` callers need to pass
# -DOBN_VERSION=... by hand.
set(OBN_VERSION "" CACHE STRING "Plugin version reported via bambu_network_get_version()")
if(NOT OBN_VERSION)
    message(FATAL_ERROR
        "OBN_VERSION is not set. Run ./configure (it auto-detects from "
        "BambuStudio.conf) or pass -DOBN_VERSION=MAJOR.MINOR.PATCH.99 to cmake."
    )
endif()

# First three dotted components of OBN_VERSION (MM.mm.pp from MM.mm.pp.rr)
# as a single preprocessor integer 0xMMmmpp for #if ABI_VERSION in
# include/obn/bambu_networking.hpp (matches Studio's 8-char prefix gate).
if(NOT OBN_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "OBN_VERSION must look like MM.mm.pp.rr with decimal components (got '${OBN_VERSION}').")
endif()
function(_obn_dec_string_to_int s outvar)
    string(REGEX REPLACE "^0+" "" _t "${s}")
    if(_t STREQUAL "")
        set(_t "0")
    endif()
    math(EXPR _n "${_t}")
    set(${outvar} "${_n}" PARENT_SCOPE)
endfunction()
function(_obn_byte_to_hex2 b outvar)
    set(_digits "0123456789abcdef")
    math(EXPR _hi "${b} / 16")
    math(EXPR _lo "${b} % 16")
    string(SUBSTRING "${_digits}" ${_hi} 1 _h1)
    string(SUBSTRING "${_digits}" ${_lo} 1 _h2)
    set(${outvar} "${_h1}${_h2}" PARENT_SCOPE)
endfunction()
_obn_dec_string_to_int("${CMAKE_MATCH_1}" _obn_b0)
_obn_dec_string_to_int("${CMAKE_MATCH_2}" _obn_b1)
_obn_dec_string_to_int("${CMAKE_MATCH_3}" _obn_b2)
foreach(_obn_x IN ITEMS ${_obn_b0} ${_obn_b1} ${_obn_b2})
    if(_obn_x GREATER 255)
        message(FATAL_ERROR
            "OBN_VERSION component out of 0..255 range after parsing '${OBN_VERSION}'.")
    endif()
endforeach()
_obn_byte_to_hex2(${_obn_b0} _obn_h0)
_obn_byte_to_hex2(${_obn_b1} _obn_h1)
_obn_byte_to_hex2(${_obn_b2} _obn_h2)
set(OBN_ABI_VERSION_HEX "0x${_obn_h0}${_obn_h1}${_obn_h2}")
message(STATUS "obn: ABI_VERSION=${OBN_ABI_VERSION_HEX} (from OBN_VERSION=${OBN_VERSION})")

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

# When ON, 'cmake --install' (and therefore 'make install') will also
# edit ~/.config/BambuStudio/BambuStudio.conf so Studio trusts this
# plugin: sets "app.installed_networking"="1" and
# "app.update_network_plugin"="false". The patch only runs when the
# install prefix is the default BambuStudio dir - if you installed
# into a staging tree with --prefix=..., the per-user config is
# untouched regardless of this flag. A backup file (*.obn-bak) is
# written whenever the file actually changes.
option(OBN_PATCH_STUDIO_CONF
    "Edit ~/.config/BambuStudio/BambuStudio.conf after install" ON)

# When ON, pkg-config resolves libmosquitto with --static (Libs.private,
# transitive deps). Ignored when OBN_VENDOR_MOSQUITTO is ON.
option(OBN_STATIC_MOSQUITTO "Link libmosquitto via pkg-config --static" OFF)

# Fetch and build eclipse/mosquitto as a static PIC library — no libmosquitto-dev
# or libmosquitto.a on the host. Requires Git and network at configure time.
option(OBN_VENDOR_MOSQUITTO "Bundle libmosquitto via FetchContent (static PIC)" OFF)
set(OBN_MOSQUITTO_GIT_TAG "v2.1.2"
    CACHE STRING "Git tag for eclipse/mosquitto when OBN_VENDOR_MOSQUITTO is ON")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT WIN32)
    # Bambu Studio is built against libstdc++ new C++11 ABI; match it.
    # MSVC doesn't use libstdc++, so this define is Linux-only.
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=1)
endif()

if(MSVC)
    # /W4 is the closest MSVC analogue to -Wall -Wextra. Silence a few
    # warnings that would otherwise drown the build log without adding
    # any value:
    #   C4100: unreferenced formal parameter  (we pepper these in the
    #          ABI so Studio's function-pointer layout keeps matching).
    #   C4101: unreferenced local variable
    #   C4127: conditional expression is constant
    #   C4244/C4267: size_t/int narrowing noise in legacy zlib/jpeg APIs.
    #   C4996: "deprecated" POSIX/CRT shims (our platform layer abstracts
    #          them explicitly, we don't want MSVC scolding every call).
    add_compile_options(/W4 /wd4100 /wd4101 /wd4127 /wd4244 /wd4267 /wd4996)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    # UTF-8 source + runtime literals. MSVC defaults to the ACP which
    # mangles the non-ASCII log strings we sometimes echo from printers.
    add_compile_options(/utf-8)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT MSVC)
    # Hide every symbol by default, explicitly export only the ABI
    # functions via __attribute__((visibility("default"))). Keeps the
    # library slim and avoids name clashes with Studio's own symbols.
    # MSVC does the equivalent automatically via __declspec(dllexport).
    add_compile_options(
        -fvisibility=hidden
        $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
    )
endif()

# --------------------------------------------------------------------------
# libbambu_networking.so / bambu_networking.dll  (main plugin)
# --------------------------------------------------------------------------

# pkg-config is the standard way we resolve deps on Linux. On Windows we
# rely on vcpkg-provided find_package() scripts exclusively, since the
# MSVC toolchain has no pkg-config shipped with it.
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
endif()
find_package(OpenSSL REQUIRED)

if(OBN_VENDOR_MOSQUITTO AND OBN_STATIC_MOSQUITTO)
    message(WARNING
        "OBN_VENDOR_MOSQUITTO and OBN_STATIC_MOSQUITTO are both ON — "
        "using the bundled mosquitto build; system static libmosquitto (pkg-config) is ignored.")
endif()

if(WIN32 AND NOT OBN_VENDOR_MOSQUITTO)
    # libmosquitto has no first-class vcpkg port with static+TLS that we
    # can rely on across triplets, so on Windows we always vendor it via
    # FetchContent unless the user opts out explicitly by setting
    # OBN_VENDOR_MOSQUITTO=OFF (which then requires them to provide a
    # libmosquitto import library on their own).
    set(OBN_VENDOR_MOSQUITTO ON CACHE BOOL "Bundle libmosquitto via FetchContent" FORCE)
endif()

if(OBN_VENDOR_MOSQUITTO)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VendorMosquitto.cmake")
    obn_vendor_mosquitto_setup()
elseif(OBN_STATIC_MOSQUITTO)
    pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
    set(_obn_prev_lib_suffix "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(MOSQUITTO_STATIC_ARCHIVE NAMES mosquitto
        PATHS ${MOSQUITTO_LIBRARY_DIRS}
        NO_DEFAULT_PATH)
    if(NOT MOSQUITTO_STATIC_ARCHIVE)
        find_library(MOSQUITTO_STATIC_ARCHIVE NAMES mosquitto)
    endif()
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_obn_prev_lib_suffix}")
    if(NOT MOSQUITTO_STATIC_ARCHIVE OR NOT MOSQUITTO_STATIC_ARCHIVE MATCHES "\\.a$")
        message(FATAL_ERROR
            "OBN_STATIC_MOSQUITTO is ON but libmosquitto.a was not found "
            "(resolved '${MOSQUITTO_STATIC_ARCHIVE}'). Many distros ship only "
            "the shared library in libmosquitto-dev — build mosquitto with "
            "BUILD_SHARED_LIBS=OFF or pass -DOBN_STATIC_MOSQUITTO=OFF.")
    endif()
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --static --libs libmosquitto
        OUTPUT_VARIABLE _obn_mosq_ldflags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _obn_mosq_ld_rc)
    if(NOT _obn_mosq_ld_rc EQUAL 0)
        message(FATAL_ERROR "pkg-config --static --libs libmosquitto failed (code ${_obn_mosq_ld_rc})")
    endif()
    string(REGEX REPLACE "-lmosquitto(\\s|$)" "${MOSQUITTO_STATIC_ARCHIVE}\\1"
        _obn_mosq_ldflags "${_obn_mosq_ldflags}")
    separate_arguments(_obn_mosq_link UNIX_COMMAND "${_obn_mosq_ldflags}")
    message(STATUS "obn: linking libmosquitto statically (${MOSQUITTO_STATIC_ARCHIVE})")
else()
    pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
endif()

if(NOT OBN_VENDOR_MOSQUITTO)
    add_library(obn_mosquitto_iface INTERFACE)
    if(OBN_STATIC_MOSQUITTO)
        target_include_directories(obn_mosquitto_iface INTERFACE ${MOSQUITTO_INCLUDE_DIRS})
        target_link_libraries(obn_mosquitto_iface INTERFACE ${_obn_mosq_link})
    else()
        target_link_libraries(obn_mosquitto_iface INTERFACE PkgConfig::MOSQUITTO)
    endif()
endif()

if(WIN32)
    # vcpkg's CURL port ships a CMake config target; we don't need
    # pkg-config. Ask for HTTP, HTTPS, FTP, FTPS transports explicitly
    # because the default port only advertises HTTP.
    find_package(CURL REQUIRED)
else()
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
endif()

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
    src/bind_cloud.cpp
    src/cloud_presets.cpp
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
    src/platform.cpp
    src/print_job.cpp
    src/ssdp.cpp
    src/zip_reader.cpp
)

target_include_directories(bambu_networking PRIVATE include)
target_compile_definitions(bambu_networking PRIVATE
    OBN_VERSION_STRING="${OBN_VERSION}"
    ABI_VERSION=${OBN_ABI_VERSION_HEX}
    OBN_FT_FTPS_FASTPATH=$<BOOL:${OBN_FT_FTPS_FASTPATH}>
    OBN_ENABLE_WORKAROUNDS=$<BOOL:${OBN_ENABLE_WORKAROUNDS}>
)
if(WIN32)
    target_link_libraries(bambu_networking PRIVATE
        obn_mosquitto_iface
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        # Winsock, IPHelper (for SSDP multi-IF NICs), Crypt32 (cert store),
        # Bcrypt (OpenSSL's Schannel fallback + RAND_bytes on some configs).
        ws2_32
        iphlpapi
        crypt32
        bcrypt
    )
else()
    target_link_libraries(bambu_networking PRIVATE
        obn_mosquitto_iface
        PkgConfig::CURL
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
    )
endif()

# Output is "libbambu_networking.so" on Linux and "bambu_networking.dll"
# on Windows (no "lib" prefix, since Studio loads it by that name).
if(WIN32)
    set_target_properties(bambu_networking PROPERTIES
        OUTPUT_NAME "bambu_networking"
        PREFIX      ""
    )
    # mosquitto_common.dll is an imposed transitive dep of libmosquitto
    # (FetchContent-built separately above) and by default lands in
    # _deps/eclipse_mosquitto-build/libcommon/$<CONFIG>/. That is not on
    # the loader search path for our plugin, so copy it next to
    # bambu_networking.dll after each build.
    if(TARGET mosquitto_common)
        add_custom_command(TARGET bambu_networking POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:mosquitto_common>
                $<TARGET_FILE_DIR:bambu_networking>
            COMMENT "obn: staging mosquitto_common.dll next to bambu_networking.dll"
        )
    endif()
else()
    set_target_properties(bambu_networking PROPERTIES
        OUTPUT_NAME "bambu_networking"
        PREFIX      "lib"
    )
endif()

# --------------------------------------------------------------------------
# libBambuSource.so  (camera MJPG tunnel for P1/A1; consumed by Studio's
# gstbambusrc element).
# --------------------------------------------------------------------------

# Camera backend selection:
#   - Linux defaults to GStreamer (same deps Studio's own gstbambusrc pulls in).
#   - Windows defaults to ffmpeg/libav via vcpkg (no GStreamer dependency).
# The OBN_CAMERA_BACKEND cache variable lets a packager override the
# default on either platform.
if(WIN32)
    set(_obn_camera_default "ffmpeg")
else()
    set(_obn_camera_default "gstreamer")
endif()
set(OBN_CAMERA_BACKEND "${_obn_camera_default}"
    CACHE STRING "Camera backend in BambuSource (gstreamer|ffmpeg)")
set_property(CACHE OBN_CAMERA_BACKEND PROPERTY STRINGS gstreamer ffmpeg)

if(OBN_CAMERA_BACKEND STREQUAL "gstreamer")
    pkg_check_modules(GST     REQUIRED IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(GST_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
elseif(OBN_CAMERA_BACKEND STREQUAL "ffmpeg")
    # vcpkg's ffmpeg port exposes per-feature CMake packages. We need
    # the three pieces we use in stubs/BambuSource.cpp (muxers for
    # RTSP/RTP, the H.264 decoder + MJPEG encoder in libavcodec, and
    # swscale for the color-space conversion before MJPEG encode).
    find_package(FFMPEG CONFIG QUIET)
    if(NOT FFMPEG_FOUND)
        # vcpkg exposes per-component packages too; fall back to those.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
            pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
            pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)
            pkg_check_modules(SWSCALE  REQUIRED IMPORTED_TARGET libswscale)
        else()
            message(FATAL_ERROR
                "ffmpeg backend requested but neither find_package(FFMPEG) nor "
                "pkg-config was able to locate libav*. On Windows, run the "
                "top-level configure.ps1 so vcpkg fetches ffmpeg[core,avcodec,"
                "avformat,swscale].")
        endif()
    endif()
else()
    message(FATAL_ERROR "OBN_CAMERA_BACKEND must be 'gstreamer' or 'ffmpeg' (got '${OBN_CAMERA_BACKEND}')")
endif()

# zlib is pulled in for the Metadata/plate_*.png unpacker used by
# PrinterFileSystem to serve 3mf thumbnails directly from FTPS.
find_package(ZLIB REQUIRED)
# libpng is used to letterbox 3mf Metadata/plate_*.png and libjpeg to
# decode the /timelapse/thumbnail/*.jpg sidecars - both get padded to
# the aspect of the Studio tile they're shown in (see
# letterbox_image_to_aspect in stubs/BambuSource.cpp).
if(WIN32)
    find_package(PNG  REQUIRED)
    find_package(JPEG REQUIRED)
else()
    pkg_check_modules(PNG  REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
endif()

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
    src/platform.cpp
    src/zip_reader.cpp
)
target_include_directories(BambuSource PRIVATE include)
target_compile_definitions(BambuSource PRIVATE
    OBN_ENABLE_WORKAROUNDS=$<BOOL:${OBN_ENABLE_WORKAROUNDS}>
)
if(OBN_CAMERA_BACKEND STREQUAL "gstreamer")
    target_compile_definitions(BambuSource PRIVATE OBN_CAMERA_GSTREAMER=1)
else()
    target_compile_definitions(BambuSource PRIVATE OBN_CAMERA_FFMPEG=1)
endif()

if(WIN32)
    set_target_properties(BambuSource PROPERTIES OUTPUT_NAME "BambuSource" PREFIX "")
else()
    set_target_properties(BambuSource PROPERTIES OUTPUT_NAME "BambuSource" PREFIX "lib")
endif()

if(WIN32)
    target_link_libraries(BambuSource PRIVATE
        OpenSSL::SSL OpenSSL::Crypto
        ZLIB::ZLIB
        PNG::PNG
        JPEG::JPEG
        ws2_32 iphlpapi crypt32 bcrypt
    )
else()
    target_link_libraries(BambuSource PRIVATE
        OpenSSL::SSL OpenSSL::Crypto
        ZLIB::ZLIB
        PkgConfig::PNG
        PkgConfig::JPEG
    )
endif()

if(OBN_CAMERA_BACKEND STREQUAL "gstreamer")
    target_link_libraries(BambuSource PRIVATE PkgConfig::GST PkgConfig::GST_APP)
elseif(OBN_CAMERA_BACKEND STREQUAL "ffmpeg")
    if(FFMPEG_FOUND)
        target_include_directories(BambuSource PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(BambuSource PRIVATE ${FFMPEG_LIBRARIES})
    else()
        target_link_libraries(BambuSource PRIVATE
            PkgConfig::AVFORMAT
            PkgConfig::AVCODEC
            PkgConfig::AVUTIL
            PkgConfig::SWSCALE
        )
    endif()
endif()

# --------------------------------------------------------------------------
# live555.so / .dll  (stub; used by the real BambuSource internally)
# --------------------------------------------------------------------------

add_library(live555 SHARED stubs/live555.cpp)
if(WIN32)
    set_target_properties(live555 PROPERTIES OUTPUT_NAME "live555" PREFIX "")
else()
    set_target_properties(live555 PROPERTIES OUTPUT_NAME "live555" PREFIX "lib")
endif()

# --------------------------------------------------------------------------
# install (copies into <prefix>/plugins/)
# --------------------------------------------------------------------------

# A brand-new Bambu Studio profile may not have created plugins/ or
# ota/plugins/ yet (Studio often creates them only after the first
# official plugin download). cmake --install can then fail copying
# into a missing parent. Create the dirs explicitly before any
# install(TARGETS|FILES) into them. Use \${CMAKE_INSTALL_PREFIX} so the
# path is resolved at install time (honours cmake --install --prefix).
install(CODE "
    file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/plugins\")
    file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/ota/plugins\")
")

install(TARGETS bambu_networking BambuSource live555
    LIBRARY DESTINATION plugins
    RUNTIME DESTINATION plugins
)

# On Windows the plugin DLLs carry transitive references to vcpkg runtime
# DLLs (libcurl / openssl / zlib / libpng / libjpeg / ffmpeg / pthreadVC3)
# and to mosquitto_common.dll (FetchContent-built above). None of those
# ship with Bambu Studio, so stage them alongside the plugin. vcpkg's
# X_VCPKG_APPLOCAL_DEPS_INSTALL option handles the first group when the
# toolchain is active; for the FetchContent-built mosquitto_common we
# invoke install(FILES) explicitly.
if(WIN32)
    set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL "" FORCE)
    set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON CACHE BOOL "" FORCE)
    if(TARGET mosquitto_common)
        install(FILES $<TARGET_FILE:mosquitto_common>
            DESTINATION plugins)
    endif()
endif()

# Studio's PresetUpdater expects this JSON to declare the cached version in
# <data_dir>/ota/plugins/; we ship the same file so the updater trusts our
# installation.
configure_file(packaging/network_plugins.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/network_plugins.json @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/network_plugins.json
    DESTINATION ota/plugins)

# First-time Studio configuration: on a fresh profile Studio will try
# to redownload its own network plugin from the cloud unless the user
# flips two keys in BambuStudio.conf. We can do that automatically at
# install time, but only when installing to the default BambuStudio
# data dir - otherwise the prefix is clearly a staging tree and we
# should not touch the user's live config.
if(OBN_PATCH_STUDIO_CONF AND WIN32)
    install(CODE [[
        set(_appdata "$ENV{APPDATA}")
        if(_appdata STREQUAL "")
            message(STATUS "obn: %APPDATA% is not set, skipping BambuStudio.conf patch")
        else()
            file(TO_CMAKE_PATH "${_appdata}/BambuStudio" _expected)
            string(REPLACE "\\" "/" _prefix_norm "${CMAKE_INSTALL_PREFIX}")
            if(NOT _prefix_norm STREQUAL _expected)
                message(STATUS
                    "obn: install prefix (${CMAKE_INSTALL_PREFIX}) is not ${_expected}; "
                    "BambuStudio.conf is untouched. Edit it by hand if needed.")
            else()
                set(_conf "${_expected}/BambuStudio.conf")
                if(NOT EXISTS "${_conf}")
                    message(STATUS
                        "obn: ${_conf} does not exist yet -- launch Bambu Studio once "
                        "to create it, then re-run 'cmake --install'.")
                else()
                    file(READ "${_conf}" _orig)
                    # Studio appends an MD5 checksum line at the bottom
                    # (format "# <32-hex-digits>") which JSON parsers reject.
                    # Strip it before we parse; it will be regenerated by
                    # Studio on next startup.
                    string(REGEX REPLACE "\r?\n#[ \t]*[0-9a-fA-F]+[ \t\r\n]*$" "\n" _body "${_orig}")
                    string(JSON _app_type ERROR_VARIABLE _err TYPE "${_body}" app)
                    if(_err OR NOT _app_type STREQUAL "OBJECT")
                        message(WARNING
                            "obn: ${_conf} has no top-level \"app\" object; "
                            "skipping patch. Error: ${_err}")
                    else()
                        string(JSON _body SET "${_body}"
                            app installed_networking "\"1\"")
                        string(JSON _body SET "${_body}"
                            app update_network_plugin "\"false\"")
                        string(JSON _body SET "${_body}"
                            app ignore_module_cert "\"1\"")
                        if(NOT _body STREQUAL _orig)
                            file(WRITE "${_conf}.obn-bak" "${_orig}")
                            file(WRITE "${_conf}" "${_body}")
                            message(STATUS
                                "obn: patched ${_conf} "
                                "(backup saved to ${_conf}.obn-bak)")
                        else()
                            message(STATUS "obn: ${_conf} already patched")
                        endif()
                    endif()
                endif()
            endif()
        endif()
    ]])
endif()

if(OBN_PATCH_STUDIO_CONF AND UNIX AND NOT APPLE)
    install(CODE [[
        set(_home "$ENV{HOME}")
        if(_home STREQUAL "")
            message(STATUS "obn: $HOME is not set, skipping BambuStudio.conf patch")
        else()
            set(_expected "${_home}/.config/BambuStudio")
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL _expected)
                message(STATUS
                    "obn: install prefix (${CMAKE_INSTALL_PREFIX}) is not ${_expected}; "
                    "BambuStudio.conf is untouched. Edit it by hand if needed: "
                    "set \"installed_networking\":\"1\" and \"update_network_plugin\":\"false\" under \"app\".")
            else()
                set(_conf "${_expected}/BambuStudio.conf")
                if(NOT EXISTS "${_conf}")
                    message(STATUS
                        "obn: ${_conf} does not exist yet -- launch Bambu Studio once "
                        "to create it, then re-run 'make install' (or set "
                        "\"installed_networking\" / \"update_network_plugin\" by hand).")
                else()
                    file(READ "${_conf}" _orig)
                    set(_body "${_orig}")
                    string(JSON _app_type ERROR_VARIABLE _err TYPE "${_body}" app)
                    if(_err OR NOT _app_type STREQUAL "OBJECT")
                        message(WARNING
                            "obn: ${_conf} has no top-level \"app\" object; "
                            "skipping patch. Error: ${_err}")
                    else()
                        string(JSON _body SET "${_body}"
                            app installed_networking "\"1\"")
                        string(JSON _body SET "${_body}"
                            app update_network_plugin "\"false\"")
                        if(NOT _body STREQUAL _orig)
                            file(WRITE "${_conf}.obn-bak" "${_orig}")
                            file(WRITE "${_conf}" "${_body}")
                            message(STATUS
                                "obn: patched ${_conf} "
                                "(backup saved to ${_conf}.obn-bak)")
                        else()
                            message(STATUS
                                "obn: ${_conf} already has "
                                "installed_networking=1 and "
                                "update_network_plugin=false")
                        endif()
                    endif()
                endif()
            endif()
        endif()
    ]])
endif()

# --------------------------------------------------------------------------
# tests (compiled only if BUILD_TESTING is on)
# --------------------------------------------------------------------------

option(OBN_BUILD_TESTS "Build smoke tests" OFF)
if(OBN_BUILD_TESTS)
    enable_testing()
    add_executable(probe_plugin tests/probe_plugin.cpp)
    target_include_directories(probe_plugin PRIVATE include)
    if(NOT WIN32)
        target_link_libraries(probe_plugin PRIVATE dl)
    endif()
    add_test(NAME plugin_symbols
        COMMAND probe_plugin $<TARGET_FILE:bambu_networking>)

    # Live test: skipped unless OBN_PRINTER_IP etc. are exported when ctest
    # runs. Links against the same library as the plugin so it can exercise
    # obn::Agent directly.
    add_executable(ssdp_listener_test tests/ssdp_listener_test.cpp
        src/log.cpp
        src/platform.cpp
        src/ssdp.cpp)
    target_include_directories(ssdp_listener_test PRIVATE include)
    if(WIN32)
        target_link_libraries(ssdp_listener_test PRIVATE ws2_32 iphlpapi)
    endif()

    add_executable(ftps_live_test tests/ftps_live_test.cpp
        src/ftps.cpp
        src/log.cpp
        src/platform.cpp)
    target_include_directories(ftps_live_test PRIVATE include)
    target_link_libraries(ftps_live_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(ftps_live_test PRIVATE ws2_32 iphlpapi crypt32)
    endif()

    # Hermetic unit test for src/ftps_parse.hpp. Exercises the `ls -l`
    # parser that populates Entry::mtime -- a regression there causes
    # every file in Studio's storage tab to show up as 1970 (Bambu
    # firmware has no MLSD, so LIST is the only thing we have).
    add_executable(ftps_parse_test tests/ftps_parse_test.cpp
        src/platform.cpp)
    target_include_directories(ftps_parse_test PRIVATE include src)
    if(WIN32)
        target_link_libraries(ftps_parse_test PRIVATE ws2_32)
    endif()
    add_test(NAME ftps_parse COMMAND ftps_parse_test)

    add_executable(lan_live_test tests/lan_live_test.cpp
        src/agent.cpp
        src/auth.cpp
        src/cert_store.cpp
        src/cloud_auth.cpp
        src/cloud_presets.cpp
        src/cloud_session.cpp
        src/cover_cache.cpp
        src/cover_server.cpp
        src/ftps.cpp
        src/http_client.cpp
        src/json_lite.cpp
        src/lan_session.cpp
        src/log.cpp
        src/mqtt_client.cpp
        src/platform.cpp
        src/ssdp.cpp
        src/zip_reader.cpp)
    target_include_directories(lan_live_test PRIVATE include)
    target_compile_definitions(lan_live_test PRIVATE
        OBN_VERSION_STRING="${OBN_VERSION}"
        ABI_VERSION=${OBN_ABI_VERSION_HEX}
    )
    if(WIN32)
        target_link_libraries(lan_live_test PRIVATE
            obn_mosquitto_iface
            CURL::libcurl
            OpenSSL::SSL
            OpenSSL::Crypto
            ZLIB::ZLIB
            ws2_32 iphlpapi crypt32 bcrypt
        )
    else()
        target_link_libraries(lan_live_test PRIVATE
            obn_mosquitto_iface
            PkgConfig::CURL
            OpenSSL::SSL
            OpenSSL::Crypto
            ZLIB::ZLIB
        )
    endif()
    add_test(NAME lan_live COMMAND lan_live_test)
endif()
