#include "obn/cloud_auth.hpp"

#include "obn/http_client.hpp"
#include "obn/json_lite.hpp"
#include "obn/log.hpp"

#include <sstream>

namespace obn::cloud {

namespace {

// Build the account-login JSON body. The server accepts either
// {account, password} or {account, code} (for verify-code flow).
std::string password_body(const std::string& email, const std::string& password)
{
    std::ostringstream os;
    os << '{'
       << "\"account\":"  << obn::json::escape(email)    << ','
       << "\"password\":" << obn::json::escape(password) << ','
       << "\"apiError\":\"\""
       << '}';
    return os.str();
}

std::string code_body(const std::string& email, const std::string& code)
{
    std::ostringstream os;
    os << '{'
       << "\"account\":" << obn::json::escape(email) << ','
       << "\"code\":"    << obn::json::escape(code)
       << '}';
    return os.str();
}

std::string send_email_body(const std::string& email)
{
    std::ostringstream os;
    os << '{'
       << "\"email\":" << obn::json::escape(email) << ','
       << "\"type\":\"codeLogin\""
       << '}';
    return os.str();
}

std::string refresh_body(const std::string& refresh)
{
    std::ostringstream os;
    os << '{'
       << "\"refreshToken\":" << obn::json::escape(refresh)
       << '}';
    return os.str();
}

// Extract the common "accessToken" shape. Fields that are absent stay
// empty; callers check `ok` first.
void fill_auth_fields(const obn::json::Value& root, AuthResult& r)
{
    r.access_token  = root.find("accessToken").as_string();
    r.refresh_token = root.find("refreshToken").as_string();
    r.expires_in    = root.find("expiresIn").as_int(0);
    r.login_type    = root.find("loginType").as_string();
    // tfaKey is sometimes called "tfa_key"; check both.
    auto tfa1 = root.find("tfaKey").as_string();
    auto tfa2 = root.find("tfa_key").as_string();
    r.tfa_key = !tfa1.empty() ? tfa1 : tfa2;
}

std::string api_error(const obn::json::Value& root, long status)
{
    auto msg = root.find("message").as_string();
    if (msg.empty()) msg = root.find("error").as_string();
    if (msg.empty()) msg = "http " + std::to_string(status);
    return msg;
}

} // namespace

std::string api_host(const std::string& region)
{
    if (region == "CN" || region == "cn") return "https://api.bambulab.cn";
    return "https://api.bambulab.com";
}

std::string web_host(const std::string& region)
{
    // No trailing slash: Studio appends "/sign-in" (WebUserLoginDialog)
    // and "api/sign-in/ticket?..." (bind flow) to this value.
    if (region == "CN" || region == "cn") return "https://bambulab.cn";
    return "https://bambulab.com";
}

AuthResult login_with_password(const std::string& region,
                               const std::string& email,
                               const std::string& password)
{
    AuthResult r;
    auto resp = obn::http::post_json(api_host(region) + "/v1/user-service/user/login",
                                     password_body(email, password));
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        OBN_WARN("login: transport error: %s", resp.error.c_str());
        return r;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        OBN_WARN("login: http %ld body=%.200s", resp.status_code, resp.body.c_str());
    }
    std::string perr;
    auto root = obn::json::parse(resp.body, &perr);
    if (!root) {
        r.error_message = "bad JSON: " + perr;
        return r;
    }
    fill_auth_fields(*root, r);
    if (!r.access_token.empty()) {
        r.ok = true;
        OBN_INFO("login: ok account=%s expires_in=%lds",
                 email.c_str(), static_cast<long>(r.expires_in));
    } else if (!r.login_type.empty()) {
        // Not a failure per se - the cloud just wants a second step.
        // We report this as ok=false with a descriptive message; callers
        // look at login_type to decide what to do next.
        r.error_message = "second step required: " + r.login_type;
        OBN_INFO("login: two-step required account=%s type=%s",
                 email.c_str(), r.login_type.c_str());
    } else {
        r.error_message = api_error(*root, resp.status_code);
        OBN_WARN("login: failed account=%s: %s", email.c_str(), r.error_message.c_str());
    }
    return r;
}

AuthResult login_with_code(const std::string& region,
                           const std::string& email,
                           const std::string& code)
{
    AuthResult r;
    auto resp = obn::http::post_json(api_host(region) + "/v1/user-service/user/login",
                                     code_body(email, code));
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        return r;
    }
    std::string perr;
    auto root = obn::json::parse(resp.body, &perr);
    if (!root) { r.error_message = "bad JSON: " + perr; return r; }
    fill_auth_fields(*root, r);
    r.ok = !r.access_token.empty();
    if (!r.ok) r.error_message = api_error(*root, resp.status_code);
    return r;
}

AuthResult send_email_code(const std::string& region, const std::string& email)
{
    AuthResult r;
    auto resp = obn::http::post_json(api_host(region) + "/v1/user-service/user/sendemail/code",
                                     send_email_body(email));
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        return r;
    }
    if (resp.status_code != 200) {
        std::string perr;
        auto root = obn::json::parse(resp.body, &perr);
        r.error_message = root ? api_error(*root, resp.status_code)
                               : ("http " + std::to_string(resp.status_code));
        return r;
    }
    r.ok = true;
    return r;
}

AuthResult login_with_ticket(const std::string& region,
                             const std::string& ticket)
{
    AuthResult r;
    if (ticket.empty()) {
        r.error_message = "empty ticket";
        return r;
    }
    // Endpoint confirmed from the original plugin's traffic:
    //   POST https://api.bambulab.com/v1/user-service/user/ticket/<TICKET>
    //   body: {"ticket":"<TICKET>"}
    // Response on success (HTTP 200):
    //   {"accessToken":"...","refreshToken":"...","expiresIn":31536000,
    //    "refreshExpiresIn":...,"tfaKey":"","accessMethod":"ticket",
    //    "loginType":"","firstAppLogin":false}
    // The ticket is single-use and short-lived; any failure here means
    // Studio will re-open the login dialog.
    std::string url  = api_host(region) + "/v1/user-service/user/ticket/" + ticket;
    std::string body = std::string("{\"ticket\":") + obn::json::escape(ticket) + "}";
    auto resp = obn::http::post_json(url, body);
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        return r;
    }
    std::string perr;
    auto root = obn::json::parse(resp.body, &perr);
    if (!root) {
        r.error_message = "bad JSON: " + perr;
        return r;
    }
    fill_auth_fields(*root, r);
    r.ok = !r.access_token.empty();
    if (!r.ok) r.error_message = api_error(*root, resp.status_code);
    return r;
}

AuthResult refresh_token(const std::string& region,
                         const std::string& refresh)
{
    AuthResult r;
    // Note: the endpoint name varies between Studio versions and the HA
    // community docs (`/v1/user-service/user/refreshtoken` or
    // `/v1/user-service/user/refresh-token`). We try the more common
    // dash-less form; if it 404s we'll iterate later.
    auto resp = obn::http::post_json(api_host(region) + "/v1/user-service/user/refreshtoken",
                                     refresh_body(refresh));
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        return r;
    }
    std::string perr;
    auto root = obn::json::parse(resp.body, &perr);
    if (!root) {
        r.error_message = "bad JSON: " + perr;
        return r;
    }
    fill_auth_fields(*root, r);
    r.ok = !r.access_token.empty();
    if (!r.ok) r.error_message = api_error(*root, resp.status_code);
    return r;
}

ProfileResult get_profile(const std::string& region,
                          const std::string& access_token)
{
    ProfileResult r;
    std::map<std::string, std::string> hdrs{
        {"Authorization", "Bearer " + access_token},
    };
    auto resp = obn::http::get_json(api_host(region) + "/v1/user-service/my/profile", hdrs);
    r.http_status = resp.status_code;
    r.raw_body    = resp.body;
    if (!resp.error.empty()) {
        r.error_message = resp.error;
        return r;
    }
    if (resp.status_code != 200) {
        r.error_message = "http " + std::to_string(resp.status_code);
        return r;
    }
    std::string perr;
    auto root = obn::json::parse(resp.body, &perr);
    if (!root) { r.error_message = "bad JSON: " + perr; return r; }
    // The profile response looks like:
    //   {"uidStr":"...","name":"...","avatar":"...","account":"...","nickname":"..."}
    // (field names vary slightly across account regions; we accept a few
    // common spellings).
    r.user_id   = root->find("uidStr").as_string();
    if (r.user_id.empty()) {
        auto uid = root->find("uid").as_int(0);
        if (uid != 0) r.user_id = std::to_string(uid);
    }
    r.user_name = root->find("name").as_string();
    r.nick_name = root->find("nickname").as_string();
    if (r.nick_name.empty()) r.nick_name = root->find("nickName").as_string();
    r.avatar    = root->find("avatar").as_string();
    r.account   = root->find("account").as_string();
    r.ok = true;
    return r;
}

} // namespace obn::cloud
