#include "obn/auth.hpp"

#include "obn/json_lite.hpp"
#include "obn/log.hpp"

#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace obn::auth {

namespace {

using clock = std::chrono::system_clock;

std::string to_iso8601(clock::time_point tp)
{
    auto t = clock::to_time_t(tp);
    char buf[80];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

clock::time_point parse_iso8601(const std::string& s)
{
    if (s.empty()) return {};
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ",
                    &y, &mo, &d, &h, &mi, &se) != 6)
        return {};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    // timegm is Linux-specific; we're targeting Linux/glibc.
    auto t = timegm(&tm);
    return clock::from_time_t(t);
}

} // namespace

Store::Store(std::string path) : path_(std::move(path)) {}

void Store::load()
{
    std::lock_guard<std::mutex> lk(mu_);
    load_locked();
}

void Store::load_locked()
{
    s_ = {};
    if (path_.empty()) return;
    std::ifstream in(path_);
    if (!in.good()) return;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return;
    std::string err;
    auto root = obn::json::parse(text, &err);
    if (!root) {
        OBN_WARN("auth: failed to parse %s: %s", path_.c_str(), err.c_str());
        return;
    }
    s_.region        = root->find("region").as_string();
    if (s_.region.empty()) s_.region = "GLOBAL";
    s_.account       = root->find("account").as_string();
    s_.access_token  = root->find("access_token").as_string();
    s_.refresh_token = root->find("refresh_token").as_string();
    s_.expires_at    = parse_iso8601(root->find("expires_at").as_string());
    s_.user_id       = root->find("user_id").as_string();
    s_.user_name     = root->find("user_name").as_string();
    s_.nick_name     = root->find("nick_name").as_string();
    s_.avatar        = root->find("avatar").as_string();
    OBN_INFO("auth: loaded session for %s (user_id=%s, expires=%s)",
             s_.account.c_str(), s_.user_id.c_str(),
             to_iso8601(s_.expires_at).c_str());
}

void Store::persist_locked() const
{
    if (path_.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    auto dir = fs::path(path_).parent_path();
    if (!dir.empty()) fs::create_directories(dir, ec);

    std::string tmp = path_ + ".tmp";
    {
        // Write with umask-style 0600 permissions.
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            OBN_ERROR("auth: open(%s) failed: %s", tmp.c_str(), std::strerror(errno));
            return;
        }
        std::string body;
        body += "{\n";
        auto add = [&](const char* k, const std::string& v, bool last = false) {
            body += "  \"";
            body += k;
            body += "\": ";
            body += obn::json::escape(v);
            if (!last) body += ",";
            body += "\n";
        };
        add("region",        s_.region);
        add("account",       s_.account);
        add("access_token",  s_.access_token);
        add("refresh_token", s_.refresh_token);
        add("expires_at",    to_iso8601(s_.expires_at));
        add("user_id",       s_.user_id);
        add("user_name",     s_.user_name);
        add("nick_name",     s_.nick_name);
        add("avatar",        s_.avatar, /*last=*/true);
        body += "}\n";
        ssize_t written = ::write(fd, body.data(), body.size());
        ::fsync(fd);
        ::close(fd);
        if (written < static_cast<ssize_t>(body.size())) {
            OBN_ERROR("auth: partial write on %s", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        OBN_ERROR("auth: rename(%s -> %s) failed: %s",
                  tmp.c_str(), path_.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
    }
}

void Store::set(Session s)
{
    std::lock_guard<std::mutex> lk(mu_);
    s_ = std::move(s);
    persist_locked();
}

void Store::update_tokens(const std::string& access,
                          const std::string& refresh,
                          std::chrono::seconds lifetime)
{
    std::lock_guard<std::mutex> lk(mu_);
    s_.access_token  = access;
    if (!refresh.empty()) s_.refresh_token = refresh;
    s_.expires_at = clock::now() + lifetime;
    persist_locked();
}

void Store::update_profile(const std::string& user_id,
                           const std::string& user_name,
                           const std::string& nick_name,
                           const std::string& avatar)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!user_id.empty())   s_.user_id   = user_id;
    if (!user_name.empty()) s_.user_name = user_name;
    if (!nick_name.empty()) s_.nick_name = nick_name;
    if (!avatar.empty())    s_.avatar    = avatar;
    persist_locked();
}

Session Store::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return s_;
}

void Store::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    s_ = {};
    if (!path_.empty()) ::unlink(path_.c_str());
}

bool Store::needs_refresh(std::chrono::seconds margin) const
{
    std::lock_guard<std::mutex> lk(mu_);
    if (s_.access_token.empty()) return true;
    if (s_.expires_at.time_since_epoch().count() == 0) return false; // unknown
    return clock::now() + margin >= s_.expires_at;
}

} // namespace obn::auth
