#pragma once

// Mark a symbol as exported by the plugin. With -fvisibility=hidden the
// default ELF visibility is local; OBN_ABI exports the function under the
// plain C-linkage name (no C++ mangling) so Bambu Studio's dlsym() finds it.
#if defined(_WIN32)
#    define OBN_ABI extern "C" __declspec(dllexport)
#else
#    define OBN_ABI extern "C" __attribute__((visibility("default")))
#endif
