// Command-line helper that performs the password login against Bambu
// Cloud and writes the resulting session file to
// <config_dir>/obn.auth.json. Intended for offline sign-in when the
// Studio UI's WebView is unable to drive the ticket flow (e.g. in CI,
// on headless boxes, or - as of today - before we've reverse-engineered
// the ticket endpoint).
//
// Usage:
//   obn-login [--region GLOBAL|CN] [--config-dir DIR] [--email E] [--password P]
//
// If --email or --password are omitted, the tool falls back to env
// vars OBN_EMAIL / OBN_PASSWORD, then to an interactive prompt on
// stdin (password is read via termios with echo off).

#include "obn/auth.hpp"
#include "obn/cloud_auth.hpp"
#include "obn/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

std::string default_config_dir()
{
    if (const char* p = std::getenv("OBN_CONFIG_DIR")) return p;
    if (const char* p = std::getenv("HOME"))
        return std::string(p) + "/.config/BambuStudio";
    return "./obn-config";
}

std::string prompt(const std::string& label, bool hidden)
{
    std::cerr << label;
    if (!hidden) {
        std::string v;
        std::getline(std::cin, v);
        return v;
    }
    termios old{}, ne{};
    if (tcgetattr(STDIN_FILENO, &old) != 0) {
        std::string v;
        std::getline(std::cin, v);
        return v;
    }
    ne = old;
    ne.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &ne);
    std::string v;
    std::getline(std::cin, v);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << "\n";
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    std::string region = "GLOBAL";
    std::string config_dir = default_config_dir();
    std::string email;
    std::string password;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if      (a == "--region")     region = need("--region");
        else if (a == "--config-dir") config_dir = need("--config-dir");
        else if (a == "--email")      email = need("--email");
        else if (a == "--password")   password = need("--password");
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "usage: obn-login [--region GLOBAL|CN] [--config-dir DIR]\n"
                "                 [--email E] [--password P]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    if (email.empty()) {
        if (const char* e = std::getenv("OBN_EMAIL")) email = e;
        else email = prompt("Email: ", false);
    }
    if (password.empty()) {
        if (const char* p = std::getenv("OBN_PASSWORD")) password = p;
        else password = prompt("Password: ", true);
    }
    if (email.empty() || password.empty()) {
        std::fprintf(stderr, "email and password required\n");
        return 2;
    }

    std::cerr << "Logging in as " << email << " against "
              << obn::cloud::api_host(region) << " ...\n";
    auto r = obn::cloud::login_with_password(region, email, password);

    if (r.login_type == "tfa") {
        std::fprintf(stderr,
            "ERROR: account requires TFA; the CLI doesn't drive that flow yet.\n"
            "Disable TFA temporarily or wait for the next patch.\n");
        return 3;
    }
    if (r.login_type == "verifyCode") {
        std::cerr << "Account requires email verification. Requesting code...\n";
        auto send = obn::cloud::send_email_code(region, email);
        if (!send.ok) {
            std::fprintf(stderr, "failed to send code (http %ld): %s\n",
                         send.http_status, send.error_message.c_str());
            return 4;
        }
        std::cerr << "Code sent to " << email << ". Check your mailbox.\n";
        std::string code = prompt("Verification code: ", false);
        if (code.empty()) { std::fprintf(stderr, "no code provided\n"); return 4; }
        r = obn::cloud::login_with_code(region, email, code);
    }

    if (!r.ok) {
        std::fprintf(stderr, "login failed (http %ld): %s\nbody: %.500s\n",
                     r.http_status, r.error_message.c_str(), r.raw_body.c_str());
        return 1;
    }

    auto prof = obn::cloud::get_profile(region, r.access_token);
    if (!prof.ok) {
        std::fprintf(stderr, "WARN: profile fetch failed: %s\n",
                     prof.error_message.c_str());
    }

    obn::auth::Session s;
    s.region        = region;
    s.account       = email;
    s.access_token  = r.access_token;
    s.refresh_token = r.refresh_token;
    s.expires_at    = std::chrono::system_clock::now() +
                      std::chrono::seconds(r.expires_in > 0 ? r.expires_in : 3 * 30 * 24 * 3600);
    if (prof.ok) {
        s.user_id   = prof.user_id;
        s.user_name = prof.user_name;
        s.nick_name = prof.nick_name;
        s.avatar    = prof.avatar;
    }
    obn::auth::Store store(config_dir + "/obn.auth.json");
    store.set(std::move(s));
    std::cerr << "ok, session stored in " << config_dir << "/obn.auth.json\n";
    if (prof.ok) {
        std::cerr << "user_id="   << prof.user_id   << "\n"
                  << "user_name=" << prof.user_name << "\n"
                  << "nickname="  << prof.nick_name << "\n";
    }
    return 0;
}
