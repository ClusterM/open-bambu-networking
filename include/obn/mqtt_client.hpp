#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

struct mosquitto;
struct mosquitto_message;

namespace obn::mqtt {

struct ConnectConfig {
    std::string host;
    int         port         = 8883;
    std::string username;
    std::string password;
    bool        use_tls      = true;
    // Bambu printers ship self-signed certs; skip chain/hostname verification
    // the same way Bambu's own plugin and the Python paho-based reference
    // clients do.
    bool        tls_insecure = true;
    int         keepalive_s  = 60;
    std::string client_id;
};

struct Message {
    std::string topic;
    std::string payload;
    int         qos    = 0;
    bool        retain = false;
};

// Thin wrapper around libmosquitto. Owns one background network thread per
// instance (via mosquitto_loop_start). All three callbacks may be invoked
// from that network thread, never concurrently for the same Client.
class Client {
public:
    using OnConnectCb    = std::function<void(int rc)>;
    using OnDisconnectCb = std::function<void(int rc)>;
    using OnMessageCb    = std::function<void(const Message&)>;

    explicit Client(std::string client_id);
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    void set_on_connect(OnConnectCb cb);
    void set_on_disconnect(OnDisconnectCb cb);
    void set_on_message(OnMessageCb cb);

    // Configures TLS (if enabled) and calls mosquitto_connect_async followed
    // by mosquitto_loop_start. Returns 0 on success, a libmosquitto MOSQ_ERR_*
    // code otherwise.
    int connect(const ConnectConfig& cfg);

    int subscribe(const std::string& topic, int qos = 0);
    int unsubscribe(const std::string& topic);
    int publish(const std::string& topic, const std::string& payload, int qos = 0, bool retain = false);

    void disconnect();

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    // Maps a libmosquitto return code to a human-readable string.
    static const char* err_str(int rc);

private:
    static void s_on_connect(::mosquitto* m, void* obj, int rc);
    static void s_on_disconnect(::mosquitto* m, void* obj, int rc);
    static void s_on_message(::mosquitto* m, void* obj, const ::mosquitto_message* msg);

    ::mosquitto*      mosq_{};
    std::string       client_id_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> loop_started_{false};

    mutable std::mutex mu_;
    OnConnectCb        on_connect_;
    OnDisconnectCb     on_disconnect_;
    OnMessageCb        on_message_;
};

// Refcounted mosquitto_lib_init/cleanup. Safe to call any number of times.
void global_init();
void global_cleanup();

} // namespace obn::mqtt
