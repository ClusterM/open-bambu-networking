// File-Transfer module stubs.
//
// Studio uses this C ABI (see src/slic3r/Utils/FileTransferUtils.hpp) as a
// fast-path for two things:
//
//   1. "Is there eMMC on this printer?" pre-flight in the Print job
//      (PrintJob.cpp). URL is always bambu:///local/<ip>?port=6000 — the
//      port that only P1/A1-family printers listen on. X1/P2S just don't
//      open it, so FT is structurally a no-op on those.
//   2. The Send-to-Printer dialog (SendToPrinter.cpp), which tries tcp
//      (port=6000) -> tutk (cloud p2p) -> ftp in that order.
//
// Neither path is required for printing: Studio always has an FTP fallback
// when FT fails. Our *only* job here is to fail *immediately and cleanly*
// so the UI doesn't sit on its thumbs waiting for a callback that will
// never come. In particular:
//
//   * ft_tunnel_create must return OK with a real handle, otherwise Studio
//     passes a nullptr into set_status_cb and later APIs which would then
//     need to special-case it. Easier to give it a handle.
//   * ft_tunnel_start_connect MUST fire the saved connection callback
//     synchronously. Studio's tramp is
//       `(*pcb)(ok == 0, ec, std::string(msg?msg:""));`
//     i.e. ok=0 means success, any other value means failure. We pass
//     ok=1 to say "failed" and FT_EIO as the error code.
//   * ft_tunnel_sync_connect returns FT_EIO for the eMMC pre-flight.
//   * ft_tunnel_start_job must fire the saved result callback with
//     ec=FT_EIO so Studio logs the failure and falls back.
//
// A full implementation would wrap the TLS/JPEG-style tunnel on
// port 6000 (see OpenBambuAPI/video.md) and speak the proprietary
// JSON command protocol on top of it. Out of scope for now — see
// README's "Not implemented" section.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "obn/abi_export.hpp"
#include "obn/log.hpp"

extern "C" {

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

namespace {

constexpr const char* kUnsupportedMsg =
    "FileTransfer over TCP is not implemented by the open-source plugin. "
    "Studio will fall back to FTP (see README)";

struct FT_Tunnel {
    std::atomic<int>     refcount{1};
    ft_tunnel_connect_cb conn_cb{nullptr};
    void*                conn_user{nullptr};
    ft_tunnel_status_cb  status_cb{nullptr};
    void*                status_user{nullptr};
    bool                 shut_down{false};
};

struct FT_Job {
    std::atomic<int>  refcount{1};
    ft_job_result_cb  result_cb{nullptr};
    void*             result_user{nullptr};
    ft_job_msg_cb     msg_cb{nullptr};
    void*             msg_user{nullptr};
    bool              delivered{false};
};

void retain(FT_Tunnel* t) { if (t) t->refcount.fetch_add(1, std::memory_order_relaxed); }
void release(FT_Tunnel* t)
{
    if (t && t->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete t;
}
void retain(FT_Job* j) { if (j) j->refcount.fetch_add(1, std::memory_order_relaxed); }
void release(FT_Job* j)
{
    if (j && j->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete j;
}

} // namespace

extern "C" {
struct FT_TunnelHandle;
struct FT_JobHandle;
} // extern "C"

OBN_ABI int ft_abi_version() { return 1; }

// ft_free: Studio uses this only if the result_destroy/msg_destroy helpers
// are missing. Our results/messages contain no heap-owned pointers, so
// this is a true no-op.
OBN_ABI void ft_free(void* /*p*/) {}

OBN_ABI void ft_job_result_destroy(ft_job_result* /*r*/) {}
OBN_ABI void ft_job_msg_destroy(ft_job_msg* /*m*/) {}

OBN_ABI ft_err ft_tunnel_create(const char* url, FT_TunnelHandle** out)
{
    if (!out) return FT_EINVAL;
    auto* t = new FT_Tunnel();
    *out = reinterpret_cast<FT_TunnelHandle*>(t);
    OBN_INFO("ft_tunnel_create url=%s (stub)", url ? url : "(null)");
    return FT_OK;
}

OBN_ABI void ft_tunnel_retain(FT_TunnelHandle* h)
{
    retain(reinterpret_cast<FT_Tunnel*>(h));
}

OBN_ABI void ft_tunnel_release(FT_TunnelHandle* h)
{
    release(reinterpret_cast<FT_Tunnel*>(h));
}

OBN_ABI ft_err ft_tunnel_set_status_cb(FT_TunnelHandle* h, ft_tunnel_status_cb cb, void* user)
{
    auto* t = reinterpret_cast<FT_Tunnel*>(h);
    if (!t) return FT_EINVAL;
    t->status_cb   = cb;
    t->status_user = user;
    return FT_OK;
}

OBN_ABI ft_err ft_tunnel_start_connect(FT_TunnelHandle* h, ft_tunnel_connect_cb cb, void* user)
{
    auto* t = reinterpret_cast<FT_Tunnel*>(h);
    if (!t) return FT_EINVAL;
    t->conn_cb   = cb;
    t->conn_user = user;

    // Fire the callback *synchronously* with ok=1 (failure). Studio's
    // tramp decodes `is_success = (ok == 0)`, so any nonzero value
    // means "no". Use FT_EIO so the log says "I/O error" rather than
    // the more alarming "invalid argument".
    OBN_INFO("ft_tunnel_start_connect: reporting synthetic failure (stub)");
    if (cb) cb(user, /*ok=*/1, /*err=*/FT_EIO, kUnsupportedMsg);
    // Also notify the status callback so any UI listening on it sees
    // a transition to a terminal state.
    if (t->status_cb) t->status_cb(t->status_user, 0, /*new_status=*/-1, FT_EIO, kUnsupportedMsg);
    return FT_OK;
}

OBN_ABI ft_err ft_tunnel_sync_connect(FT_TunnelHandle* h)
{
    if (!h) return FT_EINVAL;
    OBN_INFO("ft_tunnel_sync_connect: returning FT_EIO (stub)");
    return FT_EIO;
}

OBN_ABI ft_err ft_tunnel_shutdown(FT_TunnelHandle* h)
{
    auto* t = reinterpret_cast<FT_Tunnel*>(h);
    if (!t) return FT_EINVAL;
    t->shut_down = true;
    return FT_OK;
}

OBN_ABI ft_err ft_job_create(const char* params_json, FT_JobHandle** out)
{
    if (!out) return FT_EINVAL;
    auto* j = new FT_Job();
    *out = reinterpret_cast<FT_JobHandle*>(j);
    OBN_INFO("ft_job_create params=%.200s (stub)",
                   params_json ? params_json : "(null)");
    return FT_OK;
}

OBN_ABI void ft_job_retain(FT_JobHandle* h)
{
    retain(reinterpret_cast<FT_Job*>(h));
}

OBN_ABI void ft_job_release(FT_JobHandle* h)
{
    release(reinterpret_cast<FT_Job*>(h));
}

OBN_ABI ft_err ft_job_set_result_cb(FT_JobHandle* h, ft_job_result_cb cb, void* user)
{
    auto* j = reinterpret_cast<FT_Job*>(h);
    if (!j) return FT_EINVAL;
    j->result_cb   = cb;
    j->result_user = user;
    return FT_OK;
}

OBN_ABI ft_err ft_job_set_msg_cb(FT_JobHandle* h, ft_job_msg_cb cb, void* user)
{
    auto* j = reinterpret_cast<FT_Job*>(h);
    if (!j) return FT_EINVAL;
    j->msg_cb   = cb;
    j->msg_user = user;
    return FT_OK;
}

OBN_ABI ft_err ft_tunnel_start_job(FT_TunnelHandle* th, FT_JobHandle* jh)
{
    auto* j = reinterpret_cast<FT_Job*>(jh);
    if (!th || !j) return FT_EINVAL;

    // Synthesize an immediate failure result. All pointer members stay
    // null -- Studio copies them into a std::string / std::vector, which
    // handle null gracefully, and then calls ft_job_result_destroy (no-op
    // for us). No heap leak.
    OBN_INFO("ft_tunnel_start_job: delivering FT_EIO result (stub)");
    j->delivered = true;
    if (j->result_cb) {
        ft_job_result r{};
        r.ec       = FT_EIO;
        r.resp_ec  = 0;
        r.json     = nullptr;
        r.bin      = nullptr;
        r.bin_size = 0;
        j->result_cb(j->result_user, r);
    }
    return FT_OK;
}

OBN_ABI ft_err ft_job_get_result(FT_JobHandle* h, uint32_t /*timeout_ms*/, ft_job_result* out)
{
    auto* j = reinterpret_cast<FT_Job*>(h);
    if (!out) return FT_EINVAL;
    std::memset(out, 0, sizeof(*out));
    out->ec = FT_EIO;
    if (j) j->delivered = true;
    return FT_OK;
}

OBN_ABI ft_err ft_job_cancel(FT_JobHandle* /*h*/) { return FT_OK; }

OBN_ABI ft_err ft_job_try_get_msg(FT_JobHandle* h, ft_job_msg* out)
{
    if (out) std::memset(out, 0, sizeof(*out));
    if (!h) return FT_EINVAL;
    // Nothing to deliver; Studio will spin on try_get_msg and then move on.
    return FT_EIO;
}

OBN_ABI ft_err ft_job_get_msg(FT_JobHandle* h, uint32_t /*timeout_ms*/, ft_job_msg* out)
{
    if (out) std::memset(out, 0, sizeof(*out));
    if (!h) return FT_EINVAL;
    return FT_EIO;
}
