// Minimal stub for libBambuSource.so. Bambu Studio loads this DLL via
// NetworkAgent::get_bambu_source_entry(). If the load fails, Studio sets
// m_networking_compatible=false and nags the user to reinstall the plugin.
// Shipping an empty .so here is enough to keep the compatibility check happy;
// the camera preview and golive features won't work, but nothing else breaks.

#if defined(_WIN32)
#    define OBN_EXPORT extern "C" __declspec(dllexport)
#else
#    define OBN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Some Studio builds probe for a specific entry symbol. Provide a harmless
// one so `dlsym` returns non-NULL and downstream code sees "BambuSource is
// present".
OBN_EXPORT int bambu_source_is_stub()
{
    return 1;
}
