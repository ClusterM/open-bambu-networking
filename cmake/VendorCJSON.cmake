# Static PIC cJSON via FetchContent for embedding into vendored mosquitto's
# libmosquitto_common without a host libcjson.so NEEDED on libbambu_networking.

function(obn_vendor_cjson_setup)
    include(FetchContent)

    set(_obn_cjson_override_dir "${CMAKE_CURRENT_BINARY_DIR}/_obn_cmake_overrides")
    file(MAKE_DIRECTORY "${_obn_cjson_override_dir}")

    list(PREPEND CMAKE_MODULE_PATH "${_obn_cjson_override_dir}")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)

    # CACHE+FORCE so cJSON option() calls cannot reset these when combined with
    # mosquitto in the same CMake run.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG ${OBN_CJSON_GIT_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(cjson)

    if(NOT TARGET cjson)
        message(FATAL_ERROR "obn: FetchContent cJSON did not produce target cjson")
    endif()

    # Mosquitto includes <cjson/cJSON.h>; upstream tarball uses cJSON.h at root.
    set(_obn_cjson_iface_root "${CMAKE_CURRENT_BINARY_DIR}/_obn_cjson_include")
    file(MAKE_DIRECTORY "${_obn_cjson_iface_root}/cjson")
    file(COPY "${cjson_SOURCE_DIR}/cJSON.h" DESTINATION "${_obn_cjson_iface_root}/cjson")

    target_include_directories(cjson PUBLIC "$<BUILD_INTERFACE:${_obn_cjson_iface_root}>")
    set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(cJSON ALIAS cjson)

    file(WRITE "${_obn_cjson_override_dir}/FindcJSON.cmake"
        "include_guard(GLOBAL)\n"
        "if(NOT TARGET cJSON)\n"
        "  message(FATAL_ERROR \"obn: FindcJSON: vendored cJSON target missing\")\n"
        "endif()\n"
        "set(CJSON_FOUND TRUE)\n"
        "set(cJSON_FOUND TRUE)\n"
        "set(CJSON_INCLUDE_DIRS \"${_obn_cjson_iface_root}\")\n"
    )

    message(STATUS "obn: using vendored cJSON static (${OBN_CJSON_GIT_TAG})")
endfunction()
