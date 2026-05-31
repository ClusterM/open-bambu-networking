#pragma once

// Compile-time load banner text.
// OBN_RELEASE_BUILD / OBN_PROJECT_VERSION_STRING come from CMake when building a release.
// OBN_GIT_COMMIT / OBN_GIT_DIRTY come from CMake when git is available (interim builds).

#ifdef OBN_RELEASE_BUILD
#  define OBN_PLUGIN_LOAD_BANNER_MSG \
       "Loaded Open Bamboo Networking plugin, version " OBN_PROJECT_VERSION_STRING
#else
#  ifdef OBN_GIT_COMMIT
#    ifdef OBN_GIT_DIRTY
#      define OBN_PLUGIN_LOAD_BANNER_MSG \
           "Loaded Open Bamboo Networking plugin, interim build, commit #" OBN_GIT_COMMIT " (dirty)"
#    else
#      define OBN_PLUGIN_LOAD_BANNER_MSG \
           "Loaded Open Bamboo Networking plugin, interim build, commit #" OBN_GIT_COMMIT
#    endif
#  else
#    define OBN_PLUGIN_LOAD_BANNER_MSG \
         "Loaded Open Bamboo Networking plugin, interim build"
#  endif
#endif
