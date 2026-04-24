#pragma once

// Platform portability shim. Pulls in the right socket / filesystem / time
// headers for the target OS and exposes a small set of helpers that actually
// differ between POSIX and Windows (closing a socket, making it nonblocking,
// OS thread id, timegm, gmtime_r/localtime_r, secure 0600-style file open,
// fsync-style flush, multiline error strings). The rest of the POSIX-ish API
// (socket, bind, connect, recvfrom, setsockopt, htons/ntohs, getaddrinfo,
// inet_ntop, etc.) is identical on both sides and is simply reachable after
// this header has been included.

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#if defined(_WIN32)

// winsock2.h must come before windows.h; defining WIN32_LEAN_AND_MEAN
// keeps the rest of <windows.h> slim (gdi.h / winuser.h drag in heavy
// headers we never use here).
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mswsock.h>
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  include <fcntl.h>
#  include <sys/stat.h>

#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#  pragma comment(lib, "crypt32.lib")

#else // POSIX

#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <errno.h>

#endif

namespace obn::plat {

// -----------------------------------------------------------------------
// Socket handle and error types
// -----------------------------------------------------------------------

#if defined(_WIN32)
using socket_t                = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline bool is_valid_socket(socket_t s) { return s != INVALID_SOCKET; }
#else
using socket_t                = int;
constexpr socket_t kInvalidSocket = -1;
inline bool is_valid_socket(socket_t s) { return s >= 0; }
#endif

// -----------------------------------------------------------------------
// Winsock initialisation (no-op on POSIX)
// -----------------------------------------------------------------------

// Idempotent, thread-safe. Must be called before any socket API use on
// Windows; harmless everywhere else. Called lazily by close_socket,
// connect_ex, and the init helper embedded in log/ssdp/ftps.
void ensure_sockets_initialised();

// -----------------------------------------------------------------------
// Socket helpers that actually differ between platforms
// -----------------------------------------------------------------------

// Close a socket. close() on POSIX, closesocket() on Windows.
inline int close_socket(socket_t s)
{
#if defined(_WIN32)
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

// Put a socket into nonblocking mode.
int set_nonblocking(socket_t s);

// Last socket error code, normalised to a POSIX-like value where possible.
// Callers should check against EINPROGRESS / EWOULDBLOCK / EINTR using the
// constants below rather than the raw WSAE* values.
int socket_last_error();

// Human-readable description for a socket error (errno or WSA error).
std::string socket_strerror(int err);

// POSIX error constants re-mapped to the Windows equivalents when they
// differ. On POSIX these simply forward to the real macros.
#if defined(_WIN32)
constexpr int kEINPROGRESS = WSAEWOULDBLOCK; // Windows reports WOULDBLOCK
constexpr int kEWOULDBLOCK = WSAEWOULDBLOCK;
constexpr int kEAGAIN      = WSAEWOULDBLOCK;
constexpr int kEINTR       = WSAEINTR;
constexpr int kEBADF       = WSAENOTSOCK;
constexpr int kECONNRESET  = WSAECONNRESET;
#else
constexpr int kEINPROGRESS = EINPROGRESS;
constexpr int kEWOULDBLOCK = EWOULDBLOCK;
constexpr int kEAGAIN      = EAGAIN;
constexpr int kEINTR       = EINTR;
constexpr int kEBADF       = EBADF;
constexpr int kECONNRESET  = ECONNRESET;
#endif

// -----------------------------------------------------------------------
// poll() wrapper
// -----------------------------------------------------------------------

// WSAPoll (Vista+) is close enough to POSIX poll() for our needs. The
// pollfd layout matches, so we just typedef and forward. POLLIN / POLLOUT
// are defined in both headers.
#if defined(_WIN32)
using pollfd_t = WSAPOLLFD;
inline int poll_sockets(pollfd_t* fds, unsigned int n, int timeout_ms)
{
    return ::WSAPoll(fds, static_cast<ULONG>(n), timeout_ms);
}
#else
using pollfd_t = struct pollfd;
inline int poll_sockets(pollfd_t* fds, unsigned int n, int timeout_ms)
{
    return ::poll(fds, n, timeout_ms);
}
#endif

// Wait for a single socket to become readable or writable. events should
// be POLLIN or POLLOUT (OR'd if both). Returns 1 on ready, 0 on timeout,
// -1 on error.
int wait_socket(socket_t s, short events, int timeout_ms);

// -----------------------------------------------------------------------
// OS thread id for logging
// -----------------------------------------------------------------------

long current_thread_id();

// -----------------------------------------------------------------------
// Time helpers
// -----------------------------------------------------------------------

// Thread-safe gmtime / localtime.
void gmtime_safe(std::time_t t, std::tm* out);
void localtime_safe(std::time_t t, std::tm* out);

// Inverse of gmtime: struct tm (UTC) -> time_t. glibc exposes timegm();
// Windows exposes _mkgmtime(); we wrap them.
std::time_t timegm_portable(std::tm* tm);

// -----------------------------------------------------------------------
// File helpers
// -----------------------------------------------------------------------

// Open a file for writing with exclusive ACL when possible (POSIX: mode
// 0600 via open(); Windows: relies on user profile ACL inheritance — the
// parent dir is already locked to the user). Returns a raw fd suitable
// for write()/close() on POSIX, or _write()/_close() on Windows.
// Returns -1 on failure; sets err if non-null.
int open_secure_write(const char* path);

// Write a full buffer; returns bytes written or -1 on error.
int write_all(int fd, const void* data, std::size_t n);

// fsync-style flush. _commit() on Windows.
int fsync_fd(int fd);

// Close an fd opened by open_secure_write.
int close_fd(int fd);

// Delete a file. unlink() on POSIX, _unlink() on Windows. std::filesystem
// would work too but we want the low-level path for auth.cpp's atomic
// write-and-rename code which already has a C style.
int unlink_path(const char* path);

} // namespace obn::plat
