// Stub for liblive555.so. Bambu Studio itself does not dlsym anything from
// this library; it only checks that the file exists in the OTA cache so the
// "update available" notification stays off. See PresetUpdater.cpp:1137-1160.

#if defined(_WIN32)
#    define OBN_EXPORT extern "C" __declspec(dllexport)
#else
#    define OBN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

OBN_EXPORT int live555_is_stub()
{
    return 1;
}
