#include "obn/mqtt_client.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include <mosquitto.h>

#include "obn/log.hpp"

namespace obn::mqtt {

namespace {

std::mutex       g_init_mu;
int              g_init_refcount = 0;

} // namespace

void global_init()
{
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (g_init_refcount++ == 0) {
        ::mosquitto_lib_init();
    }
}

void global_cleanup()
{
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (--g_init_refcount == 0) {
        ::mosquitto_lib_cleanup();
    }
}

const char* Client::err_str(int rc)
{
    return ::mosquitto_strerror(rc);
}

Client::Client(std::string client_id)
    : client_id_(std::move(client_id))
{
    global_init();
    mosq_ = ::mosquitto_new(client_id_.empty() ? nullptr : client_id_.c_str(),
                            /*clean_session=*/true,
                            /*obj=*/this);
    if (!mosq_) {
        global_cleanup();
        throw std::runtime_error("mosquitto_new failed");
    }

    ::mosquitto_connect_callback_set(mosq_, &Client::s_on_connect);
    ::mosquitto_disconnect_callback_set(mosq_, &Client::s_on_disconnect);
    ::mosquitto_message_callback_set(mosq_, &Client::s_on_message);
}

Client::~Client()
{
    if (mosq_) {
        if (loop_started_.load(std::memory_order_acquire)) {
            ::mosquitto_disconnect(mosq_);
            ::mosquitto_loop_stop(mosq_, /*force=*/true);
        }
        ::mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    global_cleanup();
}

void Client::set_on_connect(OnConnectCb cb)
{
    std::lock_guard<std::mutex> lk(mu_);
    on_connect_ = std::move(cb);
}

void Client::set_on_disconnect(OnDisconnectCb cb)
{
    std::lock_guard<std::mutex> lk(mu_);
    on_disconnect_ = std::move(cb);
}

void Client::set_on_message(OnMessageCb cb)
{
    std::lock_guard<std::mutex> lk(mu_);
    on_message_ = std::move(cb);
}

int Client::connect(const ConnectConfig& cfg)
{
    if (!mosq_) return MOSQ_ERR_INVAL;

    OBN_DEBUG("mqtt connect host=%s port=%d tls=%d insecure=%d client_id=%s user=%s",
              cfg.host.c_str(), cfg.port, cfg.use_tls, cfg.tls_insecure,
              client_id_.c_str(), cfg.username.c_str());

    if (!cfg.username.empty() || !cfg.password.empty()) {
        int rc = ::mosquitto_username_pw_set(mosq_,
            cfg.username.empty() ? nullptr : cfg.username.c_str(),
            cfg.password.empty() ? nullptr : cfg.password.c_str());
        if (rc != MOSQ_ERR_SUCCESS) {
            OBN_ERROR("mqtt username_pw_set rc=%d (%s)", rc, err_str(rc));
            return rc;
        }
    }

    if (cfg.use_tls) {
        // Three TLS modes, decided by what the caller has supplied:
        //   1. ca_file provided -> chain verification against that bundle;
        //      hostname check still skipped because the printer's cert CN is
        //      its serial number, not the IP we connect by.
        //   2. no ca_file, tls_insecure=true -> accept anything; fall back to
        //      the distro trust store just because libmosquitto requires us
        //      to hand it *some* CA path.
        //   3. no ca_file, tls_insecure=false -> distro trust store with full
        //      verification. Unlikely to work for LAN but left as an option
        //      for future cloud use.
        const char* cafile = nullptr;
        const char* capath = nullptr;
        bool        verify_peer = false;
        if (!cfg.ca_file.empty()) {
            cafile      = cfg.ca_file.c_str();
            verify_peer = true;
            OBN_DEBUG("mqtt using BBL CA bundle for verification: %s", cafile);
        } else {
            static const char* kCaCandidates[] = {
                "/etc/ssl/certs/ca-certificates.crt",        // Debian, Ubuntu
                "/etc/pki/tls/certs/ca-bundle.crt",          // Fedora, RHEL
                "/etc/ssl/ca-bundle.pem",                    // openSUSE
                "/etc/ssl/cert.pem",                         // Alpine, macOS
            };
            for (const char* p : kCaCandidates) {
                FILE* f = std::fopen(p, "rb");
                if (f) { std::fclose(f); cafile = p; break; }
            }
            capath      = cafile ? nullptr : "/etc/ssl/certs";
            verify_peer = !cfg.tls_insecure;
        }
        int rc = ::mosquitto_tls_set(mosq_, cafile, capath, nullptr, nullptr, nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            OBN_ERROR("mqtt tls_set rc=%d (%s) cafile=%s capath=%s",
                      rc, err_str(rc),
                      cafile ? cafile : "(null)",
                      capath ? capath : "(null)");
            return rc;
        }
        // SSL_VERIFY_PEER (1) vs SSL_VERIFY_NONE (0).
        rc = ::mosquitto_tls_opts_set(mosq_, verify_peer ? 1 : 0, nullptr, nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            OBN_ERROR("mqtt tls_opts_set rc=%d (%s)", rc, err_str(rc));
            return rc;
        }
        if (cfg.tls_insecure) {
            rc = ::mosquitto_tls_insecure_set(mosq_, true);
            if (rc != MOSQ_ERR_SUCCESS) {
                OBN_ERROR("mqtt tls_insecure_set rc=%d (%s)", rc, err_str(rc));
                return rc;
            }
        }
    }

    int rc = ::mosquitto_connect_async(mosq_,
                                       cfg.host.c_str(),
                                       cfg.port,
                                       cfg.keepalive_s);
    if (rc != MOSQ_ERR_SUCCESS) {
        OBN_ERROR("mqtt connect_async rc=%d (%s)", rc, err_str(rc));
        return rc;
    }

    rc = ::mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        OBN_ERROR("mqtt loop_start rc=%d (%s)", rc, err_str(rc));
        return rc;
    }
    loop_started_.store(true, std::memory_order_release);
    return MOSQ_ERR_SUCCESS;
}

int Client::subscribe(const std::string& topic, int qos)
{
    if (!mosq_) return MOSQ_ERR_INVAL;
    return ::mosquitto_subscribe(mosq_, nullptr, topic.c_str(), qos);
}

int Client::unsubscribe(const std::string& topic)
{
    if (!mosq_) return MOSQ_ERR_INVAL;
    return ::mosquitto_unsubscribe(mosq_, nullptr, topic.c_str());
}

int Client::publish(const std::string& topic, const std::string& payload, int qos, bool retain)
{
    if (!mosq_) return MOSQ_ERR_INVAL;
    return ::mosquitto_publish(mosq_,
                               nullptr,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.data(),
                               qos,
                               retain);
}

void Client::disconnect()
{
    if (!mosq_) return;
    ::mosquitto_disconnect(mosq_);
    if (loop_started_.exchange(false, std::memory_order_acq_rel)) {
        ::mosquitto_loop_stop(mosq_, /*force=*/false);
    }
}

void Client::s_on_connect(::mosquitto* /*m*/, void* obj, int rc)
{
    auto* self = static_cast<Client*>(obj);
    if (!self) return;
    self->connected_.store(rc == 0, std::memory_order_release);
    OBN_INFO("mqtt connect callback rc=%d (%s)", rc, err_str(rc));
    OnConnectCb cb;
    {
        std::lock_guard<std::mutex> lk(self->mu_);
        cb = self->on_connect_;
    }
    if (cb) cb(rc);
}

void Client::s_on_disconnect(::mosquitto* /*m*/, void* obj, int rc)
{
    auto* self = static_cast<Client*>(obj);
    if (!self) return;
    self->connected_.store(false, std::memory_order_release);
    OBN_INFO("mqtt disconnect callback rc=%d (%s)", rc, err_str(rc));
    OnDisconnectCb cb;
    {
        std::lock_guard<std::mutex> lk(self->mu_);
        cb = self->on_disconnect_;
    }
    if (cb) cb(rc);
}

void Client::s_on_message(::mosquitto* /*m*/, void* obj, const ::mosquitto_message* msg)
{
    auto* self = static_cast<Client*>(obj);
    if (!self || !msg) return;
    OBN_DEBUG("mqtt msg topic=%s bytes=%d qos=%d", msg->topic, msg->payloadlen, msg->qos);
    if (msg->payload && msg->payloadlen > 0) {
        // Truncate at 2 KiB to keep the log usable; printer reports can
        // be huge (full device state dumps) and most of that isn't
        // relevant for debugging a single command exchange.
        int dump = msg->payloadlen < 2048 ? msg->payloadlen : 2048;
        OBN_DEBUG("mqtt msg payload=%.*s%s",
                  dump, static_cast<const char*>(msg->payload),
                  msg->payloadlen > dump ? "...(truncated)" : "");
    }
    OnMessageCb cb;
    {
        std::lock_guard<std::mutex> lk(self->mu_);
        cb = self->on_message_;
    }
    if (!cb) return;
    Message m;
    m.topic  = msg->topic ? msg->topic : std::string{};
    if (msg->payload && msg->payloadlen > 0) {
        m.payload.assign(static_cast<const char*>(msg->payload),
                         static_cast<std::size_t>(msg->payloadlen));
    }
    m.qos    = msg->qos;
    m.retain = msg->retain;
    cb(m);
}

} // namespace obn::mqtt
