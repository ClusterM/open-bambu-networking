#include <string>

#include "obn/abi_export.hpp"

#ifndef OBN_VERSION_STRING
#    define OBN_VERSION_STRING "02.05.02.99"
#endif

// Returned to Studio's NetworkAgent::get_version(). Studio validates the first
// 8 characters against its own SLIC3R_VERSION in check_networking_version()
// (see src/slic3r/GUI/GUI_App.cpp). The default value is chosen to match the
// shipped AppImage; pass -DOBN_VERSION=... at configure time for source
// builds.
OBN_ABI std::string bambu_network_get_version()
{
    return std::string(OBN_VERSION_STRING);
}

// Studio passes `true` for debug builds and `false` for release. A plugin
// compiled for the other mode should return false; we always claim
// consistency because a single release-mode .so must work with both Studio
// builds.
OBN_ABI bool bambu_network_check_debug_consistent(bool /*is_debug*/)
{
    return true;
}
