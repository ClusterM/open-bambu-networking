#include "obn/cert_store.hpp"

#include "obn/log.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace obn::cert_store {

namespace {

// OpenSSL 1.1+ initializes itself, but we still guard the per-process setup
// so we can be sure SSL_library_init/ERR_load_crypto_strings-style state is
// idempotent across repeat Agent lifecycles.
std::once_flag g_ssl_init;

void init_openssl_once()
{
    std::call_once(g_ssl_init, []() {
        ::SSL_load_error_strings();
        ::OpenSSL_add_ssl_algorithms();
    });
}

int set_nonblocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Waits for `fd` to become readable or writable (depending on `want_write`)
// with a timeout expressed in milliseconds. Returns 1 on ready, 0 on timeout,
// -1 on error.
int wait_fd(int fd, bool want_write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(fd + 1,
                    want_write ? nullptr : &set,
                    want_write ? &set : nullptr,
                    nullptr, &tv);
}

int connect_with_timeout(const std::string& host, int port, int timeout_ms)
{
    // getaddrinfo handles both IPv4 literals and hostnames. Printers are
    // typically reached by IPv4 literal, but we accept hostnames for parity
    // with the rest of the plugin.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%d", port);
    int gai = ::getaddrinfo(host.c_str(), port_buf, &hints, &res);
    if (gai != 0 || !res) {
        OBN_WARN("cert_store: getaddrinfo(%s) failed: %s", host.c_str(),
                 ::gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (set_nonblocking(fd) < 0) { ::close(fd); fd = -1; continue; }
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno != EINPROGRESS) { ::close(fd); fd = -1; continue; }
        int w = wait_fd(fd, /*want_write=*/true, timeout_ms);
        if (w <= 0) { ::close(fd); fd = -1; continue; }
        int       so_err = 0;
        socklen_t so_len = sizeof(so_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0 || so_err != 0) {
            ::close(fd); fd = -1; continue;
        }
        break;
    }
    ::freeaddrinfo(res);
    return fd;
}

bool drive_ssl_connect(SSL* ssl, int fd, int timeout_ms)
{
    for (;;) {
        int rc = ::SSL_connect(ssl);
        if (rc == 1) return true;
        int err = ::SSL_get_error(ssl, rc);
        if (err == SSL_ERROR_WANT_READ) {
            if (wait_fd(fd, /*want_write=*/false, timeout_ms) <= 0) return false;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (wait_fd(fd, /*want_write=*/true, timeout_ms) <= 0) return false;
        } else {
            return false;
        }
    }
}

} // namespace

std::string device_cert_path(const std::string& config_dir, const std::string& dev_id)
{
    std::string base = config_dir;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/certs/" + dev_id + ".pem";
}

bool ensure_parent_dir(const std::string& file_path)
{
    auto slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return true;
    std::string dir = file_path.substr(0, slash);
    // Walk the path and mkdir each segment; ignores EEXIST.
    std::string acc;
    acc.reserve(dir.size());
    if (!dir.empty() && dir.front() == '/') acc.push_back('/');
    size_t i = (acc.empty() ? 0 : 1);
    while (i <= dir.size()) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty() && acc != "/") {
                if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
                    OBN_WARN("cert_store: mkdir(%s) failed: %s",
                             acc.c_str(), std::strerror(errno));
                    return false;
                }
            }
            if (i == dir.size()) break;
            acc.push_back('/');
        } else {
            acc.push_back(dir[i]);
        }
        ++i;
    }
    return true;
}

bool capture_peer_cert_pem(const std::string& host,
                           int                port,
                           int                timeout_ms,
                           const std::string& out_pem_path)
{
    init_openssl_once();

    int fd = connect_with_timeout(host, port, timeout_ms);
    if (fd < 0) {
        OBN_WARN("cert_store: TCP connect %s:%d failed", host.c_str(), port);
        return false;
    }

    SSL_CTX* ctx = ::SSL_CTX_new(::TLS_client_method());
    if (!ctx) {
        OBN_ERROR("cert_store: SSL_CTX_new failed");
        ::close(fd);
        return false;
    }
    // We deliberately do not set VERIFY_PEER here: the whole point is to
    // capture the self-signed cert even though it won't validate against any
    // CA we know yet.
    ::SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    SSL* ssl = ::SSL_new(ctx);
    if (!ssl) {
        ::SSL_CTX_free(ctx);
        ::close(fd);
        return false;
    }
    // Set SNI so that servers multiplexing on IP route correctly. Printers
    // ignore it but Studio's own plugin sends the dev_ip as SNI, so we match.
    ::SSL_set_tlsext_host_name(ssl, host.c_str());
    ::SSL_set_fd(ssl, fd);

    bool ok = drive_ssl_connect(ssl, fd, timeout_ms);
    if (!ok) {
        unsigned long ecode = ::ERR_peek_last_error();
        char ebuf[256];
        ::ERR_error_string_n(ecode, ebuf, sizeof(ebuf));
        OBN_WARN("cert_store: SSL_connect %s:%d failed (%s)",
                 host.c_str(), port, ebuf);
        ::SSL_shutdown(ssl);
        ::SSL_free(ssl);
        ::SSL_CTX_free(ctx);
        ::close(fd);
        return false;
    }

    // OpenSSL 3.0+ returns a reffed cert, which the caller must free. The 1.x
    // equivalent (SSL_get_peer_certificate) is a macro for the same thing in
    // modern headers.
    X509* cert = ::SSL_get1_peer_certificate(ssl);
    bool  wrote = false;
    if (cert) {
        if (!ensure_parent_dir(out_pem_path)) {
            ::X509_free(cert);
        } else {
            FILE* f = std::fopen(out_pem_path.c_str(), "wb");
            if (!f) {
                OBN_WARN("cert_store: open(%s) failed: %s",
                         out_pem_path.c_str(), std::strerror(errno));
            } else {
                if (::PEM_write_X509(f, cert) == 1) wrote = true;
                else OBN_WARN("cert_store: PEM_write_X509 failed for %s",
                              out_pem_path.c_str());
                std::fclose(f);
            }
            ::X509_free(cert);
        }
    } else {
        OBN_WARN("cert_store: SSL_get1_peer_certificate returned null");
    }

    ::SSL_shutdown(ssl);
    ::SSL_free(ssl);
    ::SSL_CTX_free(ctx);
    ::close(fd);
    return wrote;
}

} // namespace obn::cert_store
