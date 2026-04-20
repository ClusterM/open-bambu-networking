#include "obn/agent.hpp"

#include <sys/stat.h>

#include <utility>

#include "obn/bambu_networking.hpp"
#include "obn/cert_store.hpp"
#include "obn/cloud_auth.hpp"
#include "obn/cloud_session.hpp"
#include "obn/json_lite.hpp"
#include "obn/log.hpp"
#include "obn/ssdp.hpp"

namespace obn {

Agent::Agent(std::string log_dir) : log_dir_(std::move(log_dir)) {}
Agent::~Agent()
{
    if (discovery_) discovery_->stop();
    if (cloud_session_) cloud_session_->stop();
}

int Agent::connect_printer(std::string dev_id,
                           std::string dev_ip,
                           std::string username,
                           std::string password,
                           bool        use_ssl)
{
    // Studio calls connect_printer() again when the user switches to a
    // different printer or re-enters the access code. Tear down any prior
    // session cleanly so we don't leak MQTT threads.
    {
        std::unique_ptr<LanSession> prev;
        {
            std::lock_guard<std::mutex> lk(mu_);
            prev = std::move(lan_session_);
        }
        // prev.reset() happens outside the lock; destructor joins the MQTT
        // loop thread which may call back into notify_local_connected under
        // mu_.
    }

    // Clearing the certified-devices cache so that if Studio re-binds to the
    // same printer (e.g. after a firmware reboot or an access-code change)
    // we snapshot its cert again on the very next install_device_cert() tick.
    {
        std::lock_guard<std::mutex> lk(mu_);
        certified_devs_.clear();
    }

    std::string ca_file = bambu_ca_bundle_path();
    auto session = std::make_unique<LanSession>(std::move(dev_id),
                                                std::move(dev_ip),
                                                std::move(username),
                                                std::move(password),
                                                use_ssl,
                                                std::move(ca_file));

    std::string sess_dev_id = session->dev_id();

    int rc = session->start(
        [this, sess_dev_id](int status, std::string msg) {
            notify_local_connected(status, sess_dev_id, msg);
        },
        [this](std::string d, std::string json) {
            notify_local_message(d, json);
        });

    if (rc == BAMBU_NETWORK_SUCCESS) {
        std::lock_guard<std::mutex> lk(mu_);
        lan_session_ = std::move(session);
    }
    return rc;
}

int Agent::disconnect_printer()
{
    std::unique_ptr<LanSession> session;
    {
        std::lock_guard<std::mutex> lk(mu_);
        session = std::move(lan_session_);
    }
    if (session) session->disconnect();
    return BAMBU_NETWORK_SUCCESS;
}

int Agent::send_message_to_printer(const std::string& dev_id,
                                   const std::string& json_str,
                                   int                qos)
{
    LanSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lan_session_ && lan_session_->dev_id() == dev_id)
            session = lan_session_.get();
    }
    if (!session) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return session->publish_json(json_str, qos);
}

void Agent::notify_local_connected(int status, const std::string& dev_id, const std::string& msg)
{
    BBL::OnLocalConnectedFn cb;
    BBL::QueueOnMainFn      queue;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb    = on_local_connect_;
        queue = queue_on_main_;
    }
    OBN_DEBUG("notify_local_connected status=%d dev=%s msg=%s cb=%d queued=%d",
              status, dev_id.c_str(), msg.c_str(), cb ? 1 : 0, queue ? 1 : 0);
    if (!cb) return;
    auto invoke = [cb, status, dev_id, msg]() { cb(status, dev_id, msg); };
    if (queue) queue(invoke);
    else       invoke();
}

void Agent::notify_local_message(const std::string& dev_id, const std::string& json)
{
    BBL::OnMessageFn cb;
    {
        // Per Studio's NetworkAgent wiring, local MQTT report messages go to
        // on_local_message_. We intentionally do not marshal through
        // queue_on_main_ here: DeviceManager.cpp does its own thread hop
        // based on the JSON content (some update paths are fast-path).
        std::lock_guard<std::mutex> lk(mu_);
        cb = on_local_message_;
    }
    OBN_DEBUG("notify_local_message dev=%s bytes=%zu cb=%d",
              dev_id.c_str(), json.size(), cb ? 1 : 0);
    if (cb) cb(dev_id, json);
}

void Agent::set_config_dir(std::string dir)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        config_dir_ = std::move(dir);
    }
    // Swap the auth store to a real on-disk file as soon as Studio
    // tells us where to keep it. Studio calls set_config_dir() exactly
    // once during plugin init, before any user-facing ABI.
    std::string cfg = config_dir();
    if (!cfg.empty()) {
        auth_store_ = std::make_unique<obn::auth::Store>(cfg + "/obn.auth.json");
        auth_store_->load();
        hydrate_session();
    }
}

void Agent::set_cert_file(std::string folder, std::string filename)
{
    std::lock_guard<std::mutex> lk(mu_);
    cert_folder_   = std::move(folder);
    cert_filename_ = std::move(filename);
}

void Agent::set_country_code(std::string code)
{
    std::lock_guard<std::mutex> lk(mu_);
    country_code_ = std::move(code);
}

void Agent::set_extra_http_headers(std::map<std::string, std::string> headers)
{
    std::lock_guard<std::mutex> lk(mu_);
    extra_http_headers_ = std::move(headers);
}

void Agent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    user_selected_machine_ = std::move(dev_id);
}

std::string Agent::country_code() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return country_code_;
}

std::string Agent::config_dir() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return config_dir_;
}

std::string Agent::cert_folder() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return cert_folder_;
}

std::string Agent::cert_filename() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return cert_filename_;
}

std::string Agent::bambu_ca_bundle_path() const
{
    std::string folder;
    {
        std::lock_guard<std::mutex> lk(mu_);
        folder = cert_folder_;
    }
    if (folder.empty()) return {};
    if (folder.back() == '/') folder.pop_back();
    // Studio ships two cert files in resources/cert/:
    //   slicer_base64.cer  - RapidSSL leaf for *.bambulab.com (cloud only)
    //   printer.cer        - BBL Root/Intermediate CA bundle that signs the
    //                        printer's device cert (CN=<serial>, issuer=
    //                        BBL Device CA N7-V2). This one is what we want.
    std::string path = folder + "/printer.cer";
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG)) return path;
    return {};
}

std::string Agent::user_selected_machine() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return user_selected_machine_;
}

bool Agent::start_discovery(bool enable, bool sending)
{
    OBN_INFO("start_discovery enable=%d sending=%d", enable, sending);

    // The existing Discovery is held under mu_; we capture the callback
    // outside the lock to avoid holding it across the socket syscall.
    if (!enable) {
        std::unique_ptr<ssdp::Discovery> d;
        {
            std::lock_guard<std::mutex> lk(mu_);
            d = std::move(discovery_);
        }
        if (d) d->stop();
        return false;
    }

    BBL::OnMsgArrivedFn cb;
    BBL::QueueOnMainFn  queue;
    ssdp::Discovery*    d_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!discovery_) discovery_ = std::make_unique<ssdp::Discovery>();
        d_ptr = discovery_.get();
        cb    = on_ssdp_msg_;
        queue = queue_on_main_;
    }

    // All SSDP messages are trampolined through queue_on_main_ so that
    // Studio's DeviceManager::on_machine_alive() mutates the UI-owned
    // machine list only on the main thread. Without this we eventually
    // race on_machine_alive against wx's own rendering and crash under
    // high packet rates.
    auto on_msg = [this](std::string json) {
        BBL::OnMsgArrivedFn local_cb;
        BBL::QueueOnMainFn  local_queue;
        {
            std::lock_guard<std::mutex> lk(mu_);
            local_cb    = on_ssdp_msg_;
            local_queue = queue_on_main_;
        }
        if (!local_cb) return;
        auto invoke = [local_cb, json = std::move(json)]() mutable {
            local_cb(std::move(json));
        };
        if (local_queue) local_queue(invoke);
        else             invoke();
    };

    return d_ptr->start(2021, std::move(on_msg));
}

void Agent::install_device_cert(const std::string& dev_id, bool lan_only)
{
    // Studio calls this ~1 Hz from DeviceManagerRefresher::on_timer in
    // addition to once right after on_printer_connected_fn. We deduplicate so
    // that the printer only sees one extra TLS handshake per session.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!certified_devs_.insert(dev_id).second) {
            // Already captured earlier this session - nothing more to do.
            return;
        }
    }

    if (!lan_only) {
        // Cloud / hybrid mode: Bambu's own plugin fetches the device-
        // specific MQTT tunnel cert from MakerWorld here. We don't have
        // cloud auth plumbed in yet (phases 4-5), so log and bail cleanly.
        OBN_DEBUG("install_device_cert dev=%s lan_only=0, cloud cert fetch deferred to phase 4", dev_id.c_str());
        return;
    }

    // Grab the IP of the currently-open LAN session (if it matches dev_id).
    // install_device_cert can technically be called for a device we haven't
    // opened an MQTT connection to yet; in that case there's nothing to
    // snapshot.
    std::string ip;
    std::string cfg_dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lan_session_ && lan_session_->dev_id() == dev_id)
            ip = lan_session_->dev_ip();
        cfg_dir = config_dir_;
    }
    if (ip.empty()) {
        OBN_DEBUG("install_device_cert dev=%s: no active LAN session, skipping", dev_id.c_str());
        // Put it back so we retry on the next tick.
        std::lock_guard<std::mutex> lk(mu_);
        certified_devs_.erase(dev_id);
        return;
    }
    if (cfg_dir.empty()) {
        OBN_WARN("install_device_cert dev=%s: config_dir not set", dev_id.c_str());
        return;
    }

    std::string out_path = cert_store::device_cert_path(cfg_dir, dev_id);
    OBN_INFO("install_device_cert dev=%s ip=%s: snapshotting to %s",
             dev_id.c_str(), ip.c_str(), out_path.c_str());
    bool ok = cert_store::capture_peer_cert_pem(ip, 8883, /*timeout_ms=*/3000, out_path);
    if (!ok) {
        OBN_WARN("install_device_cert dev=%s: snapshot failed", dev_id.c_str());
        // Don't poison the cache - allow a retry on the next tick.
        std::lock_guard<std::mutex> lk(mu_);
        certified_devs_.erase(dev_id);
    }
}

#define OBN_SETTER(method, field, type)                 \
    void Agent::method(type fn)                         \
    {                                                   \
        std::lock_guard<std::mutex> lk(mu_);            \
        field = std::move(fn);                          \
    }

OBN_SETTER(set_on_ssdp_msg_fn,          on_ssdp_msg_,          BBL::OnMsgArrivedFn)
OBN_SETTER(set_on_user_login_fn,        on_user_login_,        BBL::OnUserLoginFn)
OBN_SETTER(set_on_printer_connected_fn, on_printer_connected_, BBL::OnPrinterConnectedFn)
OBN_SETTER(set_on_server_connected_fn,  on_server_connected_,  BBL::OnServerConnectedFn)
OBN_SETTER(set_on_http_error_fn,        on_http_error_,        BBL::OnHttpErrorFn)
OBN_SETTER(set_get_country_code_fn,     get_country_code_,     BBL::GetCountryCodeFn)
OBN_SETTER(set_on_subscribe_failure_fn, on_subscribe_failure_, BBL::GetSubscribeFailureFn)
OBN_SETTER(set_on_message_fn,           on_message_,           BBL::OnMessageFn)
OBN_SETTER(set_on_user_message_fn,      on_user_message_,      BBL::OnMessageFn)
OBN_SETTER(set_on_local_connect_fn,     on_local_connect_,     BBL::OnLocalConnectedFn)
OBN_SETTER(set_on_local_message_fn,     on_local_message_,     BBL::OnMessageFn)
OBN_SETTER(set_queue_on_main_fn,        queue_on_main_,        BBL::QueueOnMainFn)
OBN_SETTER(set_server_callback,         server_err_,           BBL::OnServerErrFn)

#undef OBN_SETTER

// --------------------------------------------------------------------------
// Cloud user session.
// --------------------------------------------------------------------------

std::string Agent::cloud_region() const
{
    std::string cc = country_code();
    return cc == "CN" ? "CN" : "GLOBAL";
}

bool Agent::user_logged_in() const
{
    return auth_store_ && auth_store_->snapshot().logged_in();
}

int Agent::apply_login_info(const std::string& login_info_json)
{
    if (!auth_store_) {
        OBN_WARN("apply_login_info: config_dir not set yet; dropping");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    std::string perr;
    auto root = obn::json::parse(login_info_json, &perr);
    if (!root) {
        OBN_WARN("apply_login_info: bad JSON: %s", perr.c_str());
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    // change_user() gets called with two very different shapes and we have
    // to tolerate both:
    //
    //  A. Raw API response to /v1/user-service/user/ticket/<T> (from our
    //     own login_with_ticket, via the login WebView's window.postMessage
    //     path). camelCase fields right at the root:
    //       {"accessToken":"...","refreshToken":"...","expiresIn":N,
    //        "refreshExpiresIn":N,"tfaKey":"","accessMethod":"ticket",...}
    //
    //  B. Studio-built envelope out of HttpServer.cpp:538-546 (the
    //     third-party-login flow that lands on localhost:<port>). Nested,
    //     snake_case, and expires_in is stringified:
    //       {"data":{"token":"...","refresh_token":"...",
    //                "expires_in":"31536000","refresh_expires_in":"...",
    //                "user":{"uid":"...","name":"...",
    //                        "account":"...","avatar":"..."}}}
    //
    // We probe B first (data.token takes precedence) because if both are
    // present the envelope is the canonical one: it also carries the
    // already-fetched profile, which saves us a second /my/profile HTTP.

    auto read_int_or_str = [](const obn::json::Value& v) -> std::int64_t {
        if (v.is_number()) return v.as_int(0);
        const auto& s = v.as_string();
        if (s.empty()) return 0;
        try { return std::stoll(s); } catch (...) { return 0; }
    };

    std::string access_token  = root->find("data.token").as_string();
    std::string refresh_token = root->find("data.refresh_token").as_string();
    std::int64_t expires_in   = read_int_or_str(root->find("data.expires_in"));
    std::string account       = root->find("data.user.account").as_string();
    std::string user_id       = root->find("data.user.uid").as_string();
    std::string user_name     = root->find("data.user.name").as_string();
    std::string nick_name     = root->find("data.user.nickname").as_string();
    std::string avatar        = root->find("data.user.avatar").as_string();

    bool have_profile_inline = !user_id.empty() || !user_name.empty() ||
                               !avatar.empty() || !account.empty();

    if (access_token.empty()) {
        // Shape A: raw API response. Profile is *not* included here.
        access_token  = root->find("accessToken").as_string();
        refresh_token = root->find("refreshToken").as_string();
        expires_in    = read_int_or_str(root->find("expiresIn"));
        account       = root->find("account").as_string();
    }

    if (access_token.empty()) {
        OBN_WARN("apply_login_info: no access token in payload (json head: %.200s)",
                 login_info_json.c_str());
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    obn::auth::Session s = auth_store_->snapshot();
    s.region        = cloud_region();
    s.access_token  = access_token;
    if (!refresh_token.empty()) s.refresh_token = refresh_token;
    if (!account.empty())       s.account       = account;
    s.expires_at    = std::chrono::system_clock::now() +
                      std::chrono::seconds(expires_in > 0 ? expires_in
                                                          : 3 * 30 * 24 * 3600);
    auth_store_->set(s);

    // Skip the /my/profile round-trip when Studio already put the profile
    // into the envelope; that's the common case and halves login latency.
    // Fall back to the network fetch otherwise (shape A).
    if (have_profile_inline) {
        auth_store_->update_profile(user_id, user_name, nick_name, avatar);
        OBN_INFO("change_user: hello %s (uid=%s, inline profile)",
                 user_name.empty() ? nick_name.c_str() : user_name.c_str(),
                 user_id.c_str());
    } else {
        auto prof = obn::cloud::get_profile(s.region, s.access_token);
        if (prof.ok) {
            auth_store_->update_profile(prof.user_id, prof.user_name,
                                        prof.nick_name, prof.avatar);
            OBN_INFO("change_user: hello %s (uid=%s)",
                     prof.user_name.empty() ? prof.nick_name.c_str() : prof.user_name.c_str(),
                     prof.user_id.c_str());
        } else {
            OBN_WARN("change_user: profile fetch failed: %s", prof.error_message.c_str());
        }
    }

    if (auto cb = [this]() { std::lock_guard<std::mutex> lk(mu_); return on_user_login_; }())
        cb(0, "ok");
    return BAMBU_NETWORK_SUCCESS;
}

void Agent::clear_session()
{
    if (auth_store_) auth_store_->clear();
    if (auto cb = [this]() { std::lock_guard<std::mutex> lk(mu_); return on_user_login_; }())
        cb(1, "logout");
}

// --------------------------------------------------------------------------
// Cloud MQTT plumbing.
// --------------------------------------------------------------------------

int Agent::connect_cloud()
{
    auth::Session s;
    BBL::OnServerConnectedFn on_server;
    BBL::OnMessageFn         on_msg;
    BBL::GetSubscribeFailureFn on_sub_fail;
    BBL::QueueOnMainFn       queue;
    BBL::OnPrinterConnectedFn on_printer_connected;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cloud_session_) cloud_session_ = std::make_unique<CloudSession>();
        on_server            = on_server_connected_;
        on_msg               = on_message_;
        on_sub_fail          = on_subscribe_failure_;
        queue                = queue_on_main_;
        on_printer_connected = on_printer_connected_;
    }

    if (!auth_store_) {
        OBN_WARN("connect_cloud: no auth store (config_dir not set yet)");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    s = auth_store_->snapshot();
    if (!s.logged_in()) {
        OBN_WARN("connect_cloud: not logged in, skipping");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    cloud_session_->configure(cloud_region(), s.user_id, s.access_token,
                              /*ca_file=*/{});

    // Trampoline callbacks onto Studio's UI thread where one is
    // registered. The message callback is intentionally NOT queued:
    // DeviceManager::on_push_message() is thread-aware and has its own
    // fast-path handling.
    auto on_connected_cb = [this, on_server, queue, on_printer_connected]
        (int status, int reason, std::string /*msg*/)
    {
        OBN_INFO("cloud: server_connected status=%d reason=%d", status, reason);
        if (on_server) {
            auto invoke = [on_server, status, reason]() {
                on_server(status, reason);
            };
            if (queue) queue(invoke); else invoke();
        }
        // On successful CONNACK, if we already know the user's device
        // list (passed via add_subscribe earlier), fire
        // on_printer_connected with a "tunnel/" prefix for each of
        // them so Studio marks them cloud-online and requests pushall.
        if (status == 0 && on_printer_connected) {
            std::vector<std::string> devs;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (cloud_session_) {
                    // CloudSession exposes is_connected() only; mirror
                    // its subscribed set via our own copy -> we don't
                    // duplicate the state here. Instead: we rely on
                    // Studio calling add_subscribe right after
                    // connect_server, which will then call this path
                    // via the sub-success logic below.
                }
            }
            (void)devs;
        }
    };

    auto on_msg_cb = [this, on_msg, on_printer_connected]
        (std::string dev_id, std::string json)
    {
        // Mirror Bambu's plugin: the FIRST cloud report we receive
        // for a device kicks off an on_printer_connected("tunnel/<id>")
        // notification so Studio moves the device from "subscribing"
        // to "online" in its UI.
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            first = cloud_connected_devs_.insert(dev_id).second;
        }
        if (first && on_printer_connected) {
            BBL::OnPrinterConnectedFn cb = on_printer_connected;
            BBL::QueueOnMainFn        q;
            {
                std::lock_guard<std::mutex> lk(mu_);
                q = queue_on_main_;
            }
            auto invoke = [cb, dev_id]() { cb("tunnel/" + dev_id); };
            if (q) q(invoke); else invoke();
        }
        if (on_msg) on_msg(std::move(dev_id), std::move(json));
    };

    auto on_sub_fail_cb = [this, on_sub_fail](std::string dev_id) {
        if (on_sub_fail) {
            BBL::QueueOnMainFn q;
            {
                std::lock_guard<std::mutex> lk(mu_);
                q = queue_on_main_;
            }
            auto invoke = [on_sub_fail, dev_id]() { on_sub_fail(dev_id); };
            if (q) q(invoke); else invoke();
        }
    };

    return cloud_session_->start(on_connected_cb, on_msg_cb, on_sub_fail_cb);
}

int Agent::disconnect_cloud()
{
    std::unique_ptr<CloudSession> sess;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sess = std::move(cloud_session_);
        cloud_connected_devs_.clear();
    }
    if (sess) sess->stop();
    return BAMBU_NETWORK_SUCCESS;
}

bool Agent::cloud_connected() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return cloud_session_ && cloud_session_->is_connected();
}

int Agent::cloud_refresh()
{
    // Studio's DeviceManagerRefresher calls refresh_connection() on a
    // 1-second wx timer as a keep-alive / "reconnect if dropped" probe
    // (see DevManager.cpp DeviceManagerRefresher::on_timer). Doing a
    // hard disconnect+connect here produces a tight loop where every
    // tick tears down a healthy session, which Studio reports back to
    // the user as "failed to connect".
    //
    // Policy:
    //   * if we already have a live MQTT session -> no-op
    //   * otherwise -> (re)connect with the current credentials
    if (cloud_connected()) {
        return BAMBU_NETWORK_SUCCESS;
    }
    disconnect_cloud();
    return connect_cloud();
}

int Agent::cloud_add_subscribe(const std::vector<std::string>& dev_ids)
{
    CloudSession* sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sess = cloud_session_.get();
    }
    if (!sess) {
        OBN_WARN("cloud_add_subscribe: no active cloud session");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    return sess->add_subscribe(dev_ids);
}

int Agent::cloud_del_subscribe(const std::vector<std::string>& dev_ids)
{
    CloudSession* sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sess = cloud_session_.get();
        for (const auto& d : dev_ids) cloud_connected_devs_.erase(d);
    }
    if (!sess) return BAMBU_NETWORK_SUCCESS;
    return sess->del_subscribe(dev_ids);
}

int Agent::cloud_send_message(const std::string& dev_id,
                              const std::string& json_str,
                              int qos)
{
    CloudSession* sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sess = cloud_session_.get();
    }
    if (!sess) {
        OBN_WARN("cloud_send_message: no active cloud session for %s",
                 dev_id.c_str());
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    return sess->publish(dev_id, json_str, qos);
}

void Agent::hydrate_session()
{
    if (!auth_store_) return;
    auto s = auth_store_->snapshot();
    if (!s.logged_in()) return;
    if (!auth_store_->needs_refresh()) {
        OBN_INFO("cloud: session for %s still fresh", s.account.c_str());
        return;
    }
    if (s.refresh_token.empty()) {
        OBN_WARN("cloud: stored session expired and no refresh_token; ignore it");
        return;
    }
    auto r = obn::cloud::refresh_token(s.region, s.refresh_token);
    if (!r.ok) {
        OBN_WARN("cloud: refresh failed: %s", r.error_message.c_str());
        return;
    }
    auth_store_->update_tokens(r.access_token,
                               r.refresh_token.empty() ? s.refresh_token : r.refresh_token,
                               std::chrono::seconds(r.expires_in > 0 ? r.expires_in : 3 * 30 * 24 * 3600));
    OBN_INFO("cloud: access_token refreshed for %s", s.account.c_str());
}

} // namespace obn
