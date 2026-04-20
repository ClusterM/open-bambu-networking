#pragma once

// Ultra-light HTTP server used solely to hand print cover PNGs back to
// Studio's wxWebRequest (which, on Linux, runs on libsoup and therefore
// doesn't accept file:// URLs).
//
// Lifetime is managed by Agent: one singleton per process, started lazily
// the first time a synthetic subtask id needs a URL and torn down when
// the Agent is destroyed. Listens on 127.0.0.1:<random> so nothing from
// the network side can reach it.
//
// The server is a single-threaded accept loop spawning a short-lived
// handler thread per connection - we're serving at most a handful of
// requests over the lifetime of a print, so a thread pool would be
// overkill.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace obn::cover_server {

class Server {
public:
    Server();
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Binds to 127.0.0.1:0 and starts the accept loop. Idempotent -
    // further calls are no-ops while the server is up. Returns 0 on
    // success, errno on failure.
    int start();

    void stop();

    // 0 if not running yet.
    int port() const { return port_.load(); }

    // http://127.0.0.1:<port>/cover/cover-XXXXXXXX-pN.png - matches the
    // basename produced by cover_cache::path_for.
    std::string url_for(const std::string& subtask_name, int plate_idx) const;

private:
    void accept_loop();

    int              listen_fd_ = -1;
    std::atomic<int> port_{0};
    std::atomic<bool> running_{false};
    std::thread      accept_thread_;
};

} // namespace obn::cover_server
