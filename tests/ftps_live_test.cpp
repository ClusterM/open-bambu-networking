// Live sanity check for the implicit-FTPS client against a real printer.
// Usage:
//   OBN_PRINTER_IP=10.13.1.30 OBN_ACCESS_CODE=03f06755
//   OBN_UPLOAD_FILE=/tmp/hello.txt OBN_UPLOAD_NAME=/obn_hello.txt ftps_live_test
// Without OBN_UPLOAD_FILE the test only connects + LISTs the root dir.

#include "obn/ftps.hpp"
#include "obn/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static std::string env(const char* name, const char* def = "")
{
    const char* v = std::getenv(name);
    return v && *v ? std::string{v} : std::string{def};
}

int main()
{
    obn::ftps::ConnectConfig cfg;
    cfg.host     = env("OBN_PRINTER_IP", "10.13.1.30");
    cfg.username = env("OBN_USER", "bblp");
    cfg.password = env("OBN_ACCESS_CODE");
    cfg.ca_file  = env("OBN_CA_FILE"); // optional
    if (cfg.password.empty()) {
        std::fprintf(stderr, "set OBN_ACCESS_CODE\n");
        return 2;
    }

    obn::ftps::Client cli;
    std::string err = cli.connect(cfg);
    if (!err.empty()) {
        std::fprintf(stderr, "connect failed: %s\n", err.c_str());
        return 1;
    }

    std::string list_err;
    std::string listing = cli.list("", list_err);
    if (!list_err.empty()) {
        std::fprintf(stderr, "LIST failed: %s\n", list_err.c_str());
    } else {
        std::printf("--- LIST / ---\n%s\n--- end ---\n", listing.c_str());
    }

    std::string local = env("OBN_UPLOAD_FILE");
    if (!local.empty()) {
        std::string remote = env("OBN_UPLOAD_NAME", "/obn_live_test.bin");
        err = cli.stor(local, remote,
                       [](std::uint64_t sent, std::uint64_t total) {
                           if (total > 0) {
                               std::printf("  upload %llu / %llu (%llu%%)\n",
                                           static_cast<unsigned long long>(sent),
                                           static_cast<unsigned long long>(total),
                                           static_cast<unsigned long long>(sent * 100 / total));
                           }
                           return true;
                       });
        if (!err.empty()) {
            std::fprintf(stderr, "STOR failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("STOR ok: %s\n", remote.c_str());

        // List again to verify.
        listing = cli.list("", list_err);
        if (list_err.empty()) std::printf("--- LIST after upload ---\n%s\n", listing.c_str());

        // Clean up.
        if ((err = cli.dele(remote)).empty()) {
            std::printf("DELE ok: %s\n", remote.c_str());
        } else {
            std::fprintf(stderr, "DELE: %s\n", err.c_str());
        }
    }

    cli.quit();
    return 0;
}
