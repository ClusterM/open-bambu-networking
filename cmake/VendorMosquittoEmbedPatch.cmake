# Embed vendored mosquitto fully into bambu_networking (no extra DLLs).
# Upstream builds libmosquitto_common as SHARED on WIN32, which makes
# libmosquitto_static depend on mosquitto_common.dll at load time.

function(obn_patch_mosquitto_common_object _src_root)
    if(NOT IS_DIRECTORY "${_src_root}/libcommon")
        message(FATAL_ERROR "obn: mosquitto embed patch: missing libcommon/ under '${_src_root}'")
    endif()

    set(_mark "obn: libmosquitto_common OBJECT embed")
    set(_f "${_src_root}/libcommon/CMakeLists.txt")
    file(READ "${_f}" _body)
    string(FIND "${_body}" "${_mark}" _pos)
    if(_pos LESS 0)
        string(REPLACE
            "if(WIN32)
	add_library(libmosquitto_common SHARED
		\${C_SRC}
	)
else()
	add_library(libmosquitto_common OBJECT
		\${C_SRC}
	)
endif()"
            "# ${_mark}
add_library(libmosquitto_common OBJECT
	\${C_SRC}
)"
            _body "${_body}")
        file(WRITE "${_f}" "${_body}")
    endif()

    set(_hmark "obn: LIBMOSQUITTO_STATIC libcommon")
    set(_h "${_src_root}/include/mosquitto/libcommon.h")
    file(READ "${_h}" _hbody)
    string(FIND "${_hbody}" "${_hmark}" _hpos)
    if(_hpos LESS 0)
        string(REPLACE
            "#ifdef WIN32
#  ifdef libmosquitto_common_EXPORTS
#    define libmosqcommon_EXPORT __declspec(dllexport)
#  else
#    define libmosqcommon_EXPORT  __declspec(dllimport)
#  endif
#else
#  define libmosqcommon_EXPORT
#endif"
            "#ifdef WIN32
#  ifndef LIBMOSQUITTO_STATIC /* ${_hmark} */
#    ifdef libmosquitto_common_EXPORTS
#      define libmosqcommon_EXPORT __declspec(dllexport)
#    else
#      define libmosqcommon_EXPORT __declspec(dllimport)
#    endif
#  else
#    define libmosqcommon_EXPORT
#  endif
#else
#  define libmosqcommon_EXPORT
#endif"
            _hbody "${_hbody}")
        file(WRITE "${_h}" "${_hbody}")
    endif()
endfunction()
