#include "obn/print_params_ftp_prefs.hpp"

#include <mutex>

namespace obn {

namespace {
std::mutex g_mu;
bool       g_use_ssl_for_ftp = true;
} // namespace

void print_params_set_use_ssl_for_ftp(bool v)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_use_ssl_for_ftp = v;
}

bool print_params_get_use_ssl_for_ftp()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_use_ssl_for_ftp;
}

} // namespace obn
