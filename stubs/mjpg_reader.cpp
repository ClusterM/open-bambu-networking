#include "mjpg_reader.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace obn::mjpg {

namespace {

using obn::source::log_at;
using obn::source::log_fmt;
using obn::source::Logger;
using obn::source::LL_DEBUG;
using obn::source::set_last_error;

// Reasonable upper bound for a single 1280x720 JPEG frame. Stock
// camera tops out around 60 KB; 1 MB is generous future-proofing.
constexpr std::size_t kMaxFrameSize = 1u << 20;

std::once_flag g_ssl_init_flag;
SSL_CTX*       g_ssl_ctx = nullptr;

void ssl_init_once()
{
    std::call_once(g_ssl_init_flag, []() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        g_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (g_ssl_ctx) {
            SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_VERSION);
            SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        }
    });
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
        set_last_error(gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
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
    if (fd < 0) set_last_error("connect failed");
    return fd;
}

int ssl_write_all(SSL* ssl, const void* buf, std::size_t len)
{
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, p + sent, static_cast<int>(len - sent));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        sent += static_cast<std::size_t>(n);
    }
    return 0;
}

void build_auth_packet(const Config& cfg, std::uint8_t out[80])
{
    std::memset(out, 0, 80);
    auto put_u32_le = [&](std::size_t off, std::uint32_t v) {
        out[off + 0] = static_cast<std::uint8_t>( v        & 0xff);
        out[off + 1] = static_cast<std::uint8_t>((v >> 8)  & 0xff);
        out[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
        out[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    };
    put_u32_le(0,  0x40);   // payload size (always 0x40 for auth)
    put_u32_le(4,  0x3000); // packet type (auth)
    put_u32_le(8,  0);      // flags
    put_u32_le(12, 0);
    std::memcpy(out + 16, cfg.user.data(),
                std::min<std::size_t>(cfg.user.size(), 32));
    std::memcpy(out + 48, cfg.passwd.data(),
                std::min<std::size_t>(cfg.passwd.size(), 32));
}

class MjpgReaderImpl final : public Reader {
public:
    MjpgReaderImpl(Logger logger, void* log_ctx)
        : logger_(logger ? logger : obn::source::noop_logger),
          log_ctx_(log_ctx) {}

    ~MjpgReaderImpl() override { stop(); }

    int start(const Config& cfg) override
    {
        ssl_init_once();
        if (!g_ssl_ctx) {
            set_last_error("SSL_CTX not ready");
            return -1;
        }

        log_fmt(logger_, log_ctx_,
                "mjpg: dialing %s:%d", cfg.host.c_str(), cfg.port);
        fd_ = dial(cfg.host, cfg.port, cfg.connect_timeout_ms);
        if (fd_ < 0) return -1;

        ssl_ = SSL_new(g_ssl_ctx);
        if (!ssl_) {
            set_last_error("SSL_new failed");
            close_socket();
            return -1;
        }
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, cfg.host.c_str());
        if (SSL_connect(ssl_) != 1) {
            char errbuf[256];
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            log_fmt(logger_, log_ctx_, "mjpg: TLS handshake failed: %s", errbuf);
            set_last_error(errbuf);
            close_socket();
            return -1;
        }
        log_fmt(logger_, log_ctx_, "mjpg: TLS established (cipher=%s)",
                SSL_get_cipher(ssl_));

        std::uint8_t auth[80];
        build_auth_packet(cfg, auth);
        if (ssl_write_all(ssl_, auth, sizeof(auth)) != 0) {
            log_fmt(logger_, log_ctx_, "mjpg: auth write failed");
            set_last_error("auth write failed");
            close_socket();
            return -1;
        }
        log_fmt(logger_, log_ctx_,
                "mjpg: sent auth (user=%s pw_len=%zu)",
                cfg.user.c_str(), cfg.passwd.size());

        t0_     = std::chrono::steady_clock::now();
        worker_ = std::thread(&MjpgReaderImpl::worker_main, this);
        return 0;
    }

    PullResult try_pull(Frame* out) override
    {
        std::lock_guard<std::mutex> lk(mailbox_mu_);
        if (!ready_) {
            if (worker_done_.load(std::memory_order_acquire)) {
                if (!eos_reported_) {
                    eos_reported_ = true;
                    return Pull_StreamEnd;
                }
                return Pull_Error;
            }
            return Pull_WouldBlock;
        }
        pulled_buf_.swap(mailbox_);
        ready_         = false;
        out->jpeg      = pulled_buf_.data();
        out->size      = pulled_buf_.size();
        out->dt_100ns  = mailbox_dt_;
        out->itrack    = mailbox_itrack_;
        out->flags     = mailbox_flags_;
        return Pull_Ok;
    }

    void stop() override
    {
        stop_flag_.store(true, std::memory_order_release);
        // Shut the socket down before grabbing the io mutex so any
        // in-flight SSL_read returns promptly. Same trick the C-ABI
        // tunnel_close() in stubs/BambuSource.cpp uses to avoid
        // freeing SSL out from under a blocked reader.
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(io_mu_);
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        close_socket();
    }

private:
    void close_socket()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int ssl_read_all(void* buf, std::size_t len)
    {
        auto* p = static_cast<std::uint8_t*>(buf);
        std::size_t got = 0;
        while (got < len) {
            if (stop_flag_.load(std::memory_order_acquire)) return -1;
            int n;
            int err = SSL_ERROR_NONE;
            {
                std::lock_guard<std::mutex> lk(io_mu_);
                if (!ssl_ || stop_flag_.load(std::memory_order_acquire))
                    return -1;
                n = SSL_read(ssl_, p + got, static_cast<int>(len - got));
                if (n <= 0) err = SSL_get_error(ssl_, n);
            }
            if (n <= 0) {
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                if (err == SSL_ERROR_ZERO_RETURN) return 1;
                return -1;
            }
            got += static_cast<std::size_t>(n);
        }
        return 0;
    }

    void worker_main()
    {
        std::uint64_t frame_count = 0;
        std::vector<std::uint8_t> frame;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            std::uint8_t hdr[16];
            int rc = ssl_read_all(hdr, sizeof(hdr));
            if (rc != 0) {
                log_at(LL_DEBUG, logger_, log_ctx_,
                       "mjpg: header read end (rc=%d)", rc);
                break;
            }
            auto u32 = [&](std::size_t off) -> std::uint32_t {
                return  (static_cast<std::uint32_t>(hdr[off + 0]))
                      | (static_cast<std::uint32_t>(hdr[off + 1]) << 8)
                      | (static_cast<std::uint32_t>(hdr[off + 2]) << 16)
                      | (static_cast<std::uint32_t>(hdr[off + 3]) << 24);
            };
            std::uint32_t payload_size = u32(0);
            std::uint32_t itrack       = u32(4);
            std::uint32_t flags        = u32(8);
            if (payload_size == 0 || payload_size > kMaxFrameSize) {
                log_fmt(logger_, log_ctx_,
                        "mjpg: bogus payload size %u", payload_size);
                break;
            }

            frame.resize(payload_size);
            rc = ssl_read_all(frame.data(), payload_size);
            if (rc != 0) {
                log_at(LL_DEBUG, logger_, log_ctx_,
                       "mjpg: payload read end (rc=%d)", rc);
                break;
            }

            // JPEG magic check; resync on mismatch by tearing the
            // worker down (the C-ABI caller will reconnect).
            if (payload_size < 4 ||
                frame[0] != 0xFF || frame[1] != 0xD8 ||
                frame[payload_size - 2] != 0xFF ||
                frame[payload_size - 1] != 0xD9) {
                log_fmt(logger_, log_ctx_,
                        "mjpg: JPEG magic mismatch size=%u", payload_size);
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - t0_).count();
            std::uint64_t dt_100ns = static_cast<std::uint64_t>(ns / 100);

            {
                std::lock_guard<std::mutex> lk(mailbox_mu_);
                mailbox_         = frame; // copy; producer keeps `frame` alive for next iter
                mailbox_dt_      = dt_100ns;
                mailbox_itrack_  = static_cast<int>(itrack);
                mailbox_flags_   = static_cast<int>(flags);
                ready_           = true;
            }

            if (++frame_count == 1 || (frame_count % 60) == 0) {
                log_fmt(logger_, log_ctx_,
                        "mjpg: frame #%llu size=%u",
                        static_cast<unsigned long long>(frame_count),
                        payload_size);
            }
        }
        worker_done_.store(true, std::memory_order_release);
        log_at(LL_DEBUG, logger_, log_ctx_, "mjpg: worker exited");
    }

    Logger      logger_;
    void*       log_ctx_;
    int         fd_  = -1;
    SSL*        ssl_ = nullptr;
    std::chrono::steady_clock::time_point t0_{};

    std::thread       worker_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> worker_done_{false};

    // Serialises SSL_read on the worker thread against the
    // SSL_shutdown / SSL_free in stop().
    std::mutex io_mu_;

    // Mailbox: producer overwrites a stale frame, consumer swaps the
    // freshest one out on try_pull().
    std::mutex                mailbox_mu_;
    std::vector<std::uint8_t> mailbox_;
    std::vector<std::uint8_t> pulled_buf_;
    std::uint64_t             mailbox_dt_     = 0;
    int                       mailbox_itrack_ = 0;
    int                       mailbox_flags_  = 0;
    bool                      ready_          = false;
    bool                      eos_reported_   = false;
};

} // namespace

std::unique_ptr<Reader> make_reader(
    obn::source::Logger logger, void* log_ctx)
{
    return std::make_unique<MjpgReaderImpl>(logger, log_ctx);
}

} // namespace obn::mjpg
