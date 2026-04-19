#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "obn/abi_export.hpp"

// FileTransfer module: a pure C ABI, distinct from the std::string-based
// bambu_network_* ABI. See src/slic3r/Utils/FileTransferUtils.hpp for the
// types. Phase 1 implements only ft_abi_version honestly; everything else
// is a sentinel returning FT_EINVAL so Studio refuses to route transfers
// through us until Phase 8 wires up real tunnels.

extern "C" {

// Mirror the types Studio uses. We can't include Studio's header directly
// here because it depends on Boost for logging; redefine the pieces needed
// for exporting.

struct ft_job_result {
    int         ec;
    int         resp_ec;
    const char* json;
    const void* bin;
    uint32_t    bin_size;
};

struct ft_job_msg {
    int         kind;
    const char* json;
};

struct FT_TunnelHandle;
struct FT_JobHandle;

typedef enum {
    FT_OK         =   0,
    FT_EINVAL     =  -1,
    FT_ESTATE     =  -2,
    FT_EIO        =  -3,
    FT_ETIMEOUT   =  -4,
    FT_ECANCELLED =  -5,
    FT_EXCEPTION  =  -6,
    FT_EUNKNOWN   = -128
} ft_err;

using ft_tunnel_connect_cb = void (*)(void* user, int ok, int err, const char* msg);
using ft_tunnel_status_cb  = void (*)(void* user, int old_status, int new_status, int err, const char* msg);
using ft_job_result_cb     = void (*)(void* user, ft_job_result result);
using ft_job_msg_cb        = void (*)(void* user, ft_job_msg msg);

} // extern "C"

OBN_ABI int ft_abi_version()
{
    return 1;
}

OBN_ABI void ft_free(void* /*p*/) {}

OBN_ABI void ft_job_result_destroy(ft_job_result* /*r*/) {}
OBN_ABI void ft_job_msg_destroy(ft_job_msg* /*m*/) {}

OBN_ABI ft_err ft_tunnel_create(const char* /*url*/, FT_TunnelHandle** out)
{
    if (out) *out = nullptr;
    return FT_EINVAL;
}
OBN_ABI void   ft_tunnel_retain(FT_TunnelHandle* /*h*/) {}
OBN_ABI void   ft_tunnel_release(FT_TunnelHandle* /*h*/) {}
OBN_ABI ft_err ft_tunnel_start_connect(FT_TunnelHandle* /*h*/, ft_tunnel_connect_cb /*cb*/, void* /*user*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_tunnel_sync_connect(FT_TunnelHandle* /*h*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_tunnel_set_status_cb(FT_TunnelHandle* /*h*/, ft_tunnel_status_cb /*cb*/, void* /*user*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_tunnel_shutdown(FT_TunnelHandle* /*h*/) { return FT_OK; }

OBN_ABI ft_err ft_job_create(const char* /*params_json*/, FT_JobHandle** out)
{
    if (out) *out = nullptr;
    return FT_EINVAL;
}
OBN_ABI void   ft_job_retain(FT_JobHandle* /*h*/) {}
OBN_ABI void   ft_job_release(FT_JobHandle* /*h*/) {}
OBN_ABI ft_err ft_job_set_result_cb(FT_JobHandle* /*h*/, ft_job_result_cb /*cb*/, void* /*user*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_job_get_result(FT_JobHandle* /*h*/, uint32_t /*timeout_ms*/, ft_job_result* out)
{
    if (out) std::memset(out, 0, sizeof(*out));
    return FT_EINVAL;
}
OBN_ABI ft_err ft_tunnel_start_job(FT_TunnelHandle* /*h*/, FT_JobHandle* /*job*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_job_cancel(FT_JobHandle* /*h*/) { return FT_OK; }
OBN_ABI ft_err ft_job_set_msg_cb(FT_JobHandle* /*h*/, ft_job_msg_cb /*cb*/, void* /*user*/) { return FT_EINVAL; }
OBN_ABI ft_err ft_job_try_get_msg(FT_JobHandle* /*h*/, ft_job_msg* out)
{
    if (out) std::memset(out, 0, sizeof(*out));
    return FT_EINVAL;
}
OBN_ABI ft_err ft_job_get_msg(FT_JobHandle* /*h*/, uint32_t /*timeout_ms*/, ft_job_msg* out)
{
    if (out) std::memset(out, 0, sizeof(*out));
    return FT_EINVAL;
}
