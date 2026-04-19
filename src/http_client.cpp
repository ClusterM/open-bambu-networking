#include "obn/http_client.hpp"

#include "obn/log.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>

namespace obn::http {

namespace {

std::atomic<bool> g_inited{false};
std::mutex        g_init_mu;

// We pretend to be Studio as closely as we can: Cloudflare has been
// observed to 403 generic libcurl UAs against bambulab.com. Studio's
// real plugin sets a UA like "BBL-Slicer/v02.05.02.51". We keep that
// format so the servers don't treat us as a scraper.
std::string default_user_agent()
{
#ifdef OBN_VERSION_STRING
    return std::string("BBL-Slicer/v") + OBN_VERSION_STRING;
#else
    return "BBL-Slicer/v02.05.02.99";
#endif
}

size_t on_body(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

size_t on_header(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* resp = static_cast<Response*>(userdata);
    size_t n = size * nitems;
    std::string line(buffer, n);
    // Strip CRLF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    // case-insensitive prefix check
    auto ieq = [](const std::string& s, const char* p) {
        auto len = std::strlen(p);
        if (s.size() < len) return false;
        for (size_t i = 0; i < len; ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(p[i])))
                return false;
        return true;
    };
    if (ieq(line, "content-type:")) {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string v = line.substr(pos + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            resp->content_type = v;
        }
    } else if (ieq(line, "set-cookie:")) {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string v = line.substr(pos + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            resp->set_cookies.push_back(v);
        }
    }
    return n;
}

const char* method_verb(Method m)
{
    switch (m) {
        case Method::GET:  return "GET";
        case Method::POST: return "POST";
        case Method::PUT:  return "PUT";
        case Method::DEL:  return "DELETE";
    }
    return "GET";
}

} // namespace

void global_init()
{
    if (g_inited.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (g_inited.load(std::memory_order_relaxed)) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_inited.store(true, std::memory_order_release);
}

Response perform(const Request& req)
{
    global_init();

    Response resp;
    CURL*    curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        return resp;
    }

    curl_slist* hdrs = nullptr;
    bool        have_ua = false;
    bool        have_accept = false;
    bool        have_ct = false;
    for (const auto& [k, v] : req.headers) {
        std::string kv = k + ": " + v;
        hdrs = curl_slist_append(hdrs, kv.c_str());
        std::string lk = k;
        for (auto& c : lk) c = std::tolower(static_cast<unsigned char>(c));
        if (lk == "user-agent")   have_ua = true;
        if (lk == "accept")       have_accept = true;
        if (lk == "content-type") have_ct = true;
    }
    if (!have_ua) {
        std::string kv = "User-Agent: " + default_user_agent();
        hdrs = curl_slist_append(hdrs, kv.c_str());
    }
    if (!have_accept) {
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
    }
    if ((req.method == Method::POST || req.method == Method::PUT) && !have_ct) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL,              req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,    method_verb(req.method));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   static_cast<long>(req.connect_timeout_s));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          static_cast<long>(req.timeout_s));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       hdrs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,        10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,  "");       // auto gzip/br if libcurl was built with it
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,   on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,       &resp);

    if (req.method == Method::POST || req.method == Method::PUT) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req.body.data());
    }

    if (req.insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    } else if (!req.ca_file.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, req.ca_file.c_str());
    }

    OBN_DEBUG("http %s %s (body=%zu)",
              method_verb(req.method), req.url.c_str(), req.body.size());

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.error = curl_easy_strerror(rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.status_code = status;
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    OBN_DEBUG("http %s %s -> %ld (%zu bytes)%s%s",
              method_verb(req.method), req.url.c_str(),
              resp.status_code, resp.body.size(),
              resp.error.empty() ? "" : " err=",
              resp.error.c_str());
    return resp;
}

Response get_json(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& ca_file)
{
    Request r;
    r.method  = Method::GET;
    r.url     = url;
    r.headers = headers;
    r.ca_file = ca_file;
    return perform(r);
}

Response post_json(const std::string& url,
                   const std::string& body,
                   const std::map<std::string, std::string>& headers,
                   const std::string& ca_file)
{
    Request r;
    r.method  = Method::POST;
    r.url     = url;
    r.body    = body;
    r.headers = headers;
    r.ca_file = ca_file;
    return perform(r);
}

std::string url_encode(const std::string& in)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(HEX[(c >> 4) & 0xF]);
            out.push_back(HEX[c & 0xF]);
        }
    }
    return out;
}

} // namespace obn::http
