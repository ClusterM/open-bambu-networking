#include "tls_socket.hpp"

#include "source_log.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace obn::tls {
namespace {

std::once_flag g_init_flag;
SSL_CTX*       g_ctx = nullptr;

void init_once()
{
    std::call_once(g_init_flag, []() {
        // OpenSSL 1.1 made these no-ops, but they remain harmless on
        // older libcrypto versions Studio sometimes pins.
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        g_ctx = SSL_CTX_new(TLS_client_method());
        if (!g_ctx) {
            obn::source::set_last_error("SSL_CTX_new failed");
            return;
        }
        // TLS 1.0 floor: Bambu firmware is happy with anything from
        // 1.0 upwards; pinning higher would lock out older A1 mini
        // firmware that still negotiates TLS 1.1.
        SSL_CTX_set_min_proto_version(g_ctx, TLS1_VERSION);
        // No verify: every printer ships its own self-signed cert
        // with no published CA chain. We have no anchor to verify
        // against, so the best we can do is rely on the auth packet /
        // RTSP credentials inside the encrypted tunnel.
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, nullptr);
    });
}

void store_openssl_error(const char* prefix)
{
    char errbuf[256];
    ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
    char composed[320];
    std::snprintf(composed, sizeof(composed), "%s: %s", prefix, errbuf);
    obn::source::set_last_error(composed);
}

} // namespace

SSL_CTX* shared_ctx()
{
    init_once();
    return g_ctx;
}

int dial(const std::string& host, int port, int timeout_ms)
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char port_s[16];
    std::snprintf(port_s, sizeof(port_s), "%d", port);
    int gai = ::getaddrinfo(host.c_str(), port_s, &hints, &res);
    if (gai != 0 || !res) {
        obn::source::set_last_error(gai_strerror(gai));
        return -1;
    }

    int  fd       = -1;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        // Same coarse send/recv timeout drives both the connect
        // attempt and downstream blocking reads; OpenSSL inherits it.
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    freeaddrinfo(res);
    if (fd < 0) obn::source::set_last_error("connect failed");
    return fd;
}

int dial_tls(const std::string& host, int port, int timeout_ms,
             int* out_fd, SSL** out_ssl)
{
    *out_fd  = -1;
    *out_ssl = nullptr;

    SSL_CTX* ctx = shared_ctx();
    if (!ctx) return -1;

    int fd = dial(host, port, timeout_ms);
    if (fd < 0) return -1;

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        store_openssl_error("SSL_new");
        ::close(fd);
        return -1;
    }
    SSL_set_fd(ssl, fd);
    // SNI is required by some Bambu firmware revisions even though they
    // never actually swap certs based on it. Setting it unconditionally
    // is harmless and matches what OrcaSlicer's RTSP code does.
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        store_openssl_error("SSL_connect");
        SSL_free(ssl);
        ::close(fd);
        return -1;
    }

    *out_fd  = fd;
    *out_ssl = ssl;
    return 0;
}

void close_tls(int* fd, SSL** ssl)
{
    if (ssl && *ssl) {
        // SSL_shutdown returns 0 on partial close, 1 on full close, -1
        // on error. We don't care about a graceful shutdown here -- the
        // peer is fine with a TCP RST.
        SSL_shutdown(*ssl);
        SSL_free(*ssl);
        *ssl = nullptr;
    }
    if (fd && *fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

int ssl_write_all(SSL* ssl, const void* buf, std::size_t len)
{
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, p + sent, static_cast<int>(len - sent));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            // WANT_READ during a write means the underlying handshake
            // wants to renegotiate; loop back and OpenSSL will drive
            // the read leg internally on the next SSL_write().
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            return -1;
        }
        sent += static_cast<std::size_t>(n);
    }
    return 0;
}

int ssl_read_full(SSL* ssl, void* buf, std::size_t len)
{
    auto*       p   = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < len) {
        int n = SSL_read(ssl, p + got, static_cast<int>(len - got));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            // ZERO_RETURN is a clean TLS close-notify; treat it as EOF
            // only when we have at least the previous bytes intact.
            if (err == SSL_ERROR_ZERO_RETURN) return 1;
            return -1;
        }
        got += static_cast<std::size_t>(n);
    }
    return 0;
}

int ssl_read_line(SSL* ssl, std::string* out, std::size_t max_len)
{
    out->clear();
    out->reserve(128);
    // Two-byte sliding window (last char + current char) so we can
    // detect CRLF without a second buffer or lookahead.
    char prev = '\0';
    while (out->size() < max_len) {
        char c = '\0';
        int  n = SSL_read(ssl, &c, 1);
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            if (err == SSL_ERROR_ZERO_RETURN) return 1;
            return -1;
        }
        if (prev == '\r' && c == '\n') {
            // Strip the trailing CR we already pushed.
            if (!out->empty()) out->pop_back();
            return 0;
        }
        out->push_back(c);
        prev = c;
    }
    obn::source::set_last_error("ssl_read_line: line exceeded max_len");
    return -1;
}

} // namespace obn::tls
