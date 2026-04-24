#include "obn/platform.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  include <errno.h>
#endif

namespace obn::plat {

// -----------------------------------------------------------------------
// Winsock bootstrap. WSAStartup is idempotent but we guard it anyway so
// shared libraries that are loaded/unloaded can tolerate repeated calls
// without leaking a reference count.
// -----------------------------------------------------------------------

#if defined(_WIN32)
namespace {
std::once_flag g_wsa_once;
void do_wsa_init()
{
    WSADATA d;
    (void)::WSAStartup(MAKEWORD(2, 2), &d);
    // We intentionally don't call WSACleanup(). The plugin DLL is loaded
    // for the full lifetime of the Bambu Studio process and WSAStartup
    // refcounts cleanly; WSACleanup at DLL unload would race against any
    // still-running network threads.
}
} // namespace
#endif

void ensure_sockets_initialised()
{
#if defined(_WIN32)
    std::call_once(g_wsa_once, do_wsa_init);
#endif
}

// -----------------------------------------------------------------------
// Nonblocking / error code helpers
// -----------------------------------------------------------------------

int set_nonblocking(socket_t s)
{
#if defined(_WIN32)
    u_long mode = 1;
    return (::ioctlsocket(s, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int f = ::fcntl(s, F_GETFL, 0);
    if (f < 0) return -1;
    return ::fcntl(s, F_SETFL, f | O_NONBLOCK);
#endif
}

int socket_last_error()
{
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

std::string socket_strerror(int err)
{
#if defined(_WIN32)
    char* buf = nullptr;
    DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    if (n == 0 || !buf) {
        char fb[64];
        std::snprintf(fb, sizeof(fb), "winsock error %d", err);
        if (buf) ::LocalFree(buf);
        return fb;
    }
    std::string r(buf, n);
    ::LocalFree(buf);
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n' || r.back() == ' '))
        r.pop_back();
    return r;
#else
    return std::strerror(err);
#endif
}

// -----------------------------------------------------------------------
// poll wrapper
// -----------------------------------------------------------------------

int wait_socket(socket_t s, short events, int timeout_ms)
{
    pollfd_t p{};
    p.fd     = s;
    p.events = events;
    return poll_sockets(&p, 1, timeout_ms);
}

// -----------------------------------------------------------------------
// Thread id for logs
// -----------------------------------------------------------------------

long current_thread_id()
{
#if defined(_WIN32)
    return static_cast<long>(::GetCurrentThreadId());
#elif defined(SYS_gettid)
    return static_cast<long>(::syscall(SYS_gettid));
#else
    // Fallback (macOS / BSD without SYS_gettid): hash std::this_thread::get_id().
    // Not reached on our supported platforms but keeps things compiling.
    return 0;
#endif
}

// -----------------------------------------------------------------------
// Time helpers
// -----------------------------------------------------------------------

void gmtime_safe(std::time_t t, std::tm* out)
{
    if (!out) return;
#if defined(_WIN32)
    ::gmtime_s(out, &t);
#else
    ::gmtime_r(&t, out);
#endif
}

void localtime_safe(std::time_t t, std::tm* out)
{
    if (!out) return;
#if defined(_WIN32)
    ::localtime_s(out, &t);
#else
    ::localtime_r(&t, out);
#endif
}

std::time_t timegm_portable(std::tm* tm)
{
    if (!tm) return static_cast<std::time_t>(-1);
#if defined(_WIN32)
    return ::_mkgmtime(tm);
#else
    return ::timegm(tm);
#endif
}

// -----------------------------------------------------------------------
// File helpers
// -----------------------------------------------------------------------

int open_secure_write(const char* path)
{
    if (!path) return -1;
#if defined(_WIN32)
    // _O_NOINHERIT avoids leaking the handle to child processes spawned
    // by Studio's printer-update mechanism. ACLs are inherited from the
    // user's AppData directory which is already locked down per-user.
    int fd = -1;
    errno_t e = ::_sopen_s(&fd, path,
                           _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
                           _SH_DENYWR,
                           _S_IREAD | _S_IWRITE);
    if (e != 0) return -1;
    return fd;
#else
    return ::open(path,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  S_IRUSR | S_IWUSR);
#endif
}

int write_all(int fd, const void* data, std::size_t n)
{
    const char* p     = static_cast<const char*>(data);
    std::size_t total = 0;
    while (total < n) {
#if defined(_WIN32)
        int w = ::_write(fd, p + total,
                         static_cast<unsigned int>(n - total));
#else
        ssize_t w = ::write(fd, p + total, n - total);
#endif
        if (w <= 0) {
#if !defined(_WIN32)
            if (w < 0 && errno == EINTR) continue;
#endif
            return -1;
        }
        total += static_cast<std::size_t>(w);
    }
    return static_cast<int>(total);
}

int fsync_fd(int fd)
{
#if defined(_WIN32)
    return ::_commit(fd);
#else
    return ::fsync(fd);
#endif
}

int close_fd(int fd)
{
#if defined(_WIN32)
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

int unlink_path(const char* path)
{
    if (!path) return -1;
#if defined(_WIN32)
    return ::_unlink(path);
#else
    return ::unlink(path);
#endif
}

} // namespace obn::plat
