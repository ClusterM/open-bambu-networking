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
    src/ftps.cpp
    src/http_client.cpp
    src/json_lite.cpp
    src/lan_session.cpp
    src/log.cpp
    src/mqtt_client.cpp
    src/print_job.cpp
    src/ssdp.cpp
)

target_include_directories(bambu_networking PRIVATE include)
target_compile_definitions(bambu_networking PRIVATE
    OBN_VERSION_STRING="${OBN_VERSION}"
)
target_link_libraries(bambu_networking PRIVATE
    PkgConfig::MOSQUITTO
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
)

# No "lib" prefix rename: output is literally "libbambu_networking.so"
set_target_properties(bambu_networking PROPERTIES
    OUTPUT_NAME "bambu_networking"
    PREFIX      "lib"
)

# --------------------------------------------------------------------------
# libBambuSource.so  (stub for camera; only needs to load)
# --------------------------------------------------------------------------

add_library(BambuSource SHARED stubs/BambuSource.cpp)
set_target_properties(BambuSource PROPERTIES OUTPUT_NAME "BambuSource" PREFIX "lib")

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

# --------------------------------------------------------------------------
# obn-login (CLI helper for offline sign-in with email/password)
# --------------------------------------------------------------------------

add_executable(obn_login
    tools/obn_login.cpp
    src/auth.cpp
    src/cloud_auth.cpp
    src/http_client.cpp
    src/json_lite.cpp
    src/log.cpp
)
target_include_directories(obn_login PRIVATE include)
target_compile_definitions(obn_login PRIVATE
    OBN_VERSION_STRING="${OBN_VERSION}"
)
target_link_libraries(obn_login PRIVATE
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
)
set_target_properties(obn_login PROPERTIES OUTPUT_NAME "obn-login")
install(TARGETS obn_login RUNTIME DESTINATION bin)

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
