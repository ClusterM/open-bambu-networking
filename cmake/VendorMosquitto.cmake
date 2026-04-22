# Bundled libmosquitto (static + PIC) via FetchContent for embedding into
# libbambu_networking.so without a system libmosquitto.a.

function(obn_vendor_mosquitto_setup)
    find_package(Threads REQUIRED)
    include(FetchContent)

    FetchContent_Declare(eclipse_mosquitto
        GIT_REPOSITORY https://github.com/eclipse/mosquitto.git
        GIT_TAG ${OBN_MOSQUITTO_GIT_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )

    # CACHE+FORCE so mosquitto's option() calls (CMP0077 OLD) cannot reset these.
    set(WITH_BROKER OFF CACHE BOOL "" FORCE)
    set(WITH_APPS OFF CACHE BOOL "" FORCE)
    set(WITH_CLIENTS OFF CACHE BOOL "" FORCE)
    set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
    set(WITH_DOCS OFF CACHE BOOL "" FORCE)
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)
    set(WITH_PIC ON CACHE BOOL "" FORCE)
    set(WITH_LIB_CPP OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(eclipse_mosquitto)

    if(NOT TARGET libmosquitto_static)
        message(FATAL_ERROR
            "obn: bundled mosquitto did not produce target libmosquitto_static")
    endif()
    # Upstream sets full include paths for libmosquitto (shared), but the
    # static target misses libcommon/common includes while still compiling
    # sources that include property_common.h.
    target_include_directories(libmosquitto_static PRIVATE
        "${eclipse_mosquitto_SOURCE_DIR}/common"
        "${eclipse_mosquitto_SOURCE_DIR}/deps/picohttpparser"
        "${eclipse_mosquitto_SOURCE_DIR}/libcommon"
    )

    add_library(obn_mosquitto_iface INTERFACE)
    target_include_directories(obn_mosquitto_iface INTERFACE
        "${eclipse_mosquitto_SOURCE_DIR}/include"
    )
    target_link_libraries(obn_mosquitto_iface INTERFACE
        libmosquitto_static
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        find_library(_obn_vendor_librt NAMES rt)
        if(_obn_vendor_librt)
            target_link_libraries(obn_mosquitto_iface INTERFACE "${_obn_vendor_librt}")
        endif()
    endif()

    message(STATUS "obn: using vendored libmosquitto_static (${OBN_MOSQUITTO_GIT_TAG})")
endfunction()
