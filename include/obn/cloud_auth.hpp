#pragma once

// High-level wrappers for Bambu's cloud auth endpoints.
//
// We currently implement the email/password path against the global
// `api.bambulab.com` host (with CN mirror at `api.bambulab.cn`). The
// ticket-exchange path (used by Studio's "system browser login") still
// needs to be reverse-engineered from the original plugin; until then
// `login_with_ticket` returns Unimplemented.

#include <cstdint>
#include <string>
#include <vector>

namespace obn::cloud {

struct AuthResult {
    bool        ok           = false;
    long        http_status  = 0;
    std::string raw_body;       // server response body (verbatim); we keep it so
                                // Studio-side code that expects the JSON shape
                                // of "login_info" gets the real thing.
    std::string access_token;
    std::string refresh_token;
    long        expires_in   = 0; // seconds until access_token dies, as reported
    std::string login_type;      // "", "verifyCode", "tfa"
    std::string tfa_key;         // present when login_type == "tfa"
    std::string error_message;   // human-readable; populated on !ok
};

struct ProfileResult {
    bool        ok           = false;
    long        http_status  = 0;
    std::string raw_body;
    std::string user_id;
    std::string user_name;
    std::string nick_name;
    std::string avatar;
    std::string account;    // email
    std::string error_message;
};

// Endpoint host helpers. `region` is "CN" or anything else (global).
//   api_host  -> REST API base, e.g. "https://api.bambulab.com" (no slash).
//   web_host  -> portal base Studio injects into wxWebView, e.g.
//                "https://bambulab.com/" (WITH trailing slash). Studio
//                concatenates different suffixes onto this base
//                ("/sign-in", "api/sign-in/ticket?..." without a leading
//                slash, "/<lang>/sign-in"). The trailing slash on our side
//                is mandatory - otherwise "host + api/..." produces
//                "bambulab.comapi/..." and DNS fails.
std::string api_host(const std::string& region);
std::string web_host(const std::string& region);

AuthResult login_with_password(const std::string& region,
                               const std::string& email,
                               const std::string& password);

// Finish the verifyCode flow started by a login_with_password() where
// `login_type == "verifyCode"`: the user typed in the 6-digit code the
// cloud emailed them and we post it back. Returns a fully-populated
// session on success.
AuthResult login_with_code(const std::string& region,
                           const std::string& email,
                           const std::string& code);

// Request an email verification code for `email`. Returns ok=true only
// on HTTP 200; otherwise error_message is filled in.
AuthResult send_email_code(const std::string& region,
                           const std::string& email);

// Ticket-exchange: not yet implemented. Returns ok=false with
// error_message set.
AuthResult login_with_ticket(const std::string& region,
                             const std::string& ticket);

AuthResult refresh_token(const std::string& region,
                         const std::string& refresh_token);

ProfileResult get_profile(const std::string& region,
                          const std::string& access_token);

} // namespace obn::cloud
