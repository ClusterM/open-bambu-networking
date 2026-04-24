// Smoke test: ensures every symbol Bambu Studio resolves via dlsym() /
// GetProcAddress() is present in the built .so/.dll, and that
// bambu_network_get_version() returns a non-empty,
// SLIC3R_VERSION-compatible string.

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

#if defined(_WIN32)
using module_handle_t = HMODULE;
module_handle_t obn_dlopen(const char* path)
{
    return ::LoadLibraryA(path);
}
const char* obn_dlerror_str(char* buf, size_t n)
{
    DWORD e = ::GetLastError();
    DWORD r = ::FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, buf, static_cast<DWORD>(n), nullptr);
    if (!r) std::snprintf(buf, n, "LoadLibrary failed (GetLastError=%lu)", (unsigned long)e);
    return buf;
}
void* obn_dlsym(module_handle_t h, const char* name)
{
    return reinterpret_cast<void*>(::GetProcAddress(h, name));
}
void obn_dlclose(module_handle_t h) { ::FreeLibrary(h); }
#else
using module_handle_t = void*;
module_handle_t obn_dlopen(const char* path) { return ::dlopen(path, RTLD_LAZY); }
const char* obn_dlerror_str(char* /*buf*/, size_t /*n*/) { return ::dlerror(); }
void* obn_dlsym(module_handle_t h, const char* name) { return ::dlsym(h, name); }
void obn_dlclose(module_handle_t h) { ::dlclose(h); }
#endif

// Canonical list copied from BambuStudio's NetworkAgent.cpp resolver and the
// real plugin's ELF exports. Keep this list in sync when Studio changes.
const char* kBambuNetworkSymbols[] = {
    "bambu_network_check_debug_consistent",
    "bambu_network_get_version",
    "bambu_network_create_agent",
    "bambu_network_destroy_agent",
    "bambu_network_init_log",
    "bambu_network_set_config_dir",
    "bambu_network_set_cert_file",
    "bambu_network_set_country_code",
    "bambu_network_start",
    "bambu_network_set_on_ssdp_msg_fn",
    "bambu_network_set_on_user_login_fn",
    "bambu_network_set_on_printer_connected_fn",
    "bambu_network_set_on_server_connected_fn",
    "bambu_network_set_on_http_error_fn",
    "bambu_network_set_get_country_code_fn",
    "bambu_network_set_on_subscribe_failure_fn",
    "bambu_network_set_on_message_fn",
    "bambu_network_set_on_user_message_fn",
    "bambu_network_set_on_local_connect_fn",
    "bambu_network_set_on_local_message_fn",
    "bambu_network_set_queue_on_main_fn",
    "bambu_network_set_server_callback",
    "bambu_network_connect_server",
    "bambu_network_is_server_connected",
    "bambu_network_refresh_connection",
    "bambu_network_start_subscribe",
    "bambu_network_stop_subscribe",
    "bambu_network_add_subscribe",
    "bambu_network_del_subscribe",
    "bambu_network_enable_multi_machine",
    "bambu_network_send_message",
    "bambu_network_connect_printer",
    "bambu_network_disconnect_printer",
    "bambu_network_send_message_to_printer",
    "bambu_network_update_cert",
    "bambu_network_install_device_cert",
    "bambu_network_start_discovery",
    "bambu_network_change_user",
    "bambu_network_is_user_login",
    "bambu_network_user_logout",
    "bambu_network_get_user_id",
    "bambu_network_get_user_name",
    "bambu_network_get_user_avatar",
    "bambu_network_get_user_nickanme",
    "bambu_network_build_login_cmd",
    "bambu_network_build_logout_cmd",
    "bambu_network_build_login_info",
    "bambu_network_get_my_profile",
    "bambu_network_get_my_token",
    "bambu_network_get_user_info",
    "bambu_network_ping_bind",
    "bambu_network_bind_detect",
    "bambu_network_bind",
    "bambu_network_unbind",
    "bambu_network_request_bind_ticket",
    "bambu_network_query_bind_status",
    "bambu_network_modify_printer_name",
    "bambu_network_report_consent",
    "bambu_network_get_bambulab_host",
    "bambu_network_get_user_selected_machine",
    "bambu_network_set_user_selected_machine",
    "bambu_network_start_print",
    "bambu_network_start_local_print_with_record",
    "bambu_network_start_send_gcode_to_sdcard",
    "bambu_network_start_local_print",
    "bambu_network_start_sdcard_print",
    "bambu_network_get_user_presets",
    "bambu_network_request_setting_id",
    "bambu_network_put_setting",
    "bambu_network_get_setting_list",
    "bambu_network_get_setting_list2",
    "bambu_network_delete_setting",
    "bambu_network_get_studio_info_url",
    "bambu_network_set_extra_http_header",
    "bambu_network_get_my_message",
    "bambu_network_check_user_task_report",
    "bambu_network_get_user_print_info",
    "bambu_network_get_user_tasks",
    "bambu_network_get_printer_firmware",
    "bambu_network_get_task_plate_index",
    "bambu_network_get_subtask_info",
    "bambu_network_get_slice_info",
    "bambu_network_get_camera_url",
    "bambu_network_get_camera_url_for_golive",
    "bambu_network_get_hms_snapshot",
    "bambu_network_get_design_staffpick",
    "bambu_network_start_publish",
    "bambu_network_get_model_publish_url",
    "bambu_network_get_subtask",
    "bambu_network_get_model_mall_home_url",
    "bambu_network_get_model_mall_detail_url",
    "bambu_network_put_model_mall_rating",
    "bambu_network_get_oss_config",
    "bambu_network_put_rating_picture_oss",
    "bambu_network_get_model_mall_rating",
    "bambu_network_get_mw_user_preference",
    "bambu_network_get_mw_user_4ulist",
    "bambu_network_track_enable",
    "bambu_network_track_remove_files",
    "bambu_network_track_event",
    "bambu_network_track_header",
    "bambu_network_track_update_property",
    "bambu_network_track_get_property",
};

const char* kFtSymbols[] = {
    "ft_abi_version",
    "ft_free",
    "ft_job_result_destroy",
    "ft_job_msg_destroy",
    "ft_tunnel_create",
    "ft_tunnel_retain",
    "ft_tunnel_release",
    "ft_tunnel_start_connect",
    "ft_tunnel_sync_connect",
    "ft_tunnel_set_status_cb",
    "ft_tunnel_shutdown",
    "ft_job_create",
    "ft_job_retain",
    "ft_job_release",
    "ft_job_set_result_cb",
    "ft_job_get_result",
    "ft_tunnel_start_job",
    "ft_job_cancel",
    "ft_job_set_msg_cb",
    "ft_job_try_get_msg",
    "ft_job_get_msg",
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <path-to-libbambu_networking.so>\n", argv[0]);
        return 2;
    }

    module_handle_t h = obn_dlopen(argv[1]);
    if (!h) {
        char ebuf[256];
        std::fprintf(stderr, "LoadLibrary/dlopen failed: %s\n",
                     obn_dlerror_str(ebuf, sizeof(ebuf)));
        return 1;
    }

    int missing = 0;
    for (const char* name : kBambuNetworkSymbols) {
        if (!obn_dlsym(h, name)) {
            std::fprintf(stderr, "MISSING: %s\n", name);
            ++missing;
        }
    }
    for (const char* name : kFtSymbols) {
        if (!obn_dlsym(h, name)) {
            std::fprintf(stderr, "MISSING: %s\n", name);
            ++missing;
        }
    }

    using fn_ver = std::string (*)();
    using fn_dbg = bool (*)(bool);
    auto ver = reinterpret_cast<fn_ver>(obn_dlsym(h, "bambu_network_get_version"));
    auto dbg = reinterpret_cast<fn_dbg>(obn_dlsym(h, "bambu_network_check_debug_consistent"));
    if (ver) {
        std::string v = ver();
        std::printf("version: %s\n", v.c_str());
        if (v.size() < 8) {
            std::fprintf(stderr, "VERSION TOO SHORT (need >=8 chars)\n");
            ++missing;
        }
    }
    if (dbg) {
        std::printf("debug_consistent(false)=%d true=%d\n", dbg(false), dbg(true));
    }

    // ft_abi_version must be 1 per FileTransferUtils.hpp.
    using fn_ft = int (*)();
    auto ft_ver = reinterpret_cast<fn_ft>(obn_dlsym(h, "ft_abi_version"));
    if (ft_ver) {
        int v = ft_ver();
        std::printf("ft_abi_version: %d\n", v);
        if (v != 1) {
            std::fprintf(stderr, "ft_abi_version must be 1 (got %d)\n", v);
            ++missing;
        }
    }

    obn_dlclose(h);
    if (missing) {
        std::fprintf(stderr, "%d problem(s) found\n", missing);
        return 1;
    }
    std::printf("OK (all %zu + %zu symbols present)\n",
                sizeof(kBambuNetworkSymbols) / sizeof(*kBambuNetworkSymbols),
                sizeof(kFtSymbols) / sizeof(*kFtSymbols));
    return 0;
}
