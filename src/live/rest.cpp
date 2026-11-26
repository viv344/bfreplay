#include "bfr/live/rest.hpp"
#include <curl/curl.h>
#include <cstdio>
#include "bfr/json.hpp"

namespace bfr::live {
namespace {
constexpr const char* kCertLoginUrl = "https://identitysso-cert.betfair.com/api/certlogin";
constexpr const char* kKeepAliveUrl = "https://identitysso.betfair.com/api/keepAlive";
constexpr const char* kBettingUrl = "https://api.betfair.com/exchange/betting/rest/v1.0/";

size_t write_cb(char* ptr, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * n);
    return size * n;
}
}  // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

Rest::Rest(Credentials c) : creds_(std::move(c)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
}

Rest::~Rest() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

HttpResult Rest::post(const std::string& url, const std::string& body, const std::string& content_type,
                      bool client_cert, bool authenticated) {
    HttpResult r;
    CURL* c = static_cast<CURL*>(curl_);
    curl_easy_reset(c);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-Application: " + creds_.app_key).c_str());
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (authenticated) headers = curl_slist_append(headers, ("X-Authentication: " + session_).c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    if (client_cert) {
        curl_easy_setopt(c, CURLOPT_SSLCERT, creds_.cert_file.c_str());
        curl_easy_setopt(c, CURLOPT_SSLKEY, creds_.key_file.c_str());
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(headers);
    return r;
}

bool Rest::login(std::string& error) {
    CURL* c = static_cast<CURL*>(curl_);
    char* u = curl_easy_escape(c, creds_.username.c_str(), 0);
    char* p = curl_easy_escape(c, creds_.password.c_str(), 0);
    std::string body = std::string("username=") + u + "&password=" + p;
    curl_free(u);
    curl_free(p);
    auto r = post(kCertLoginUrl, body, "application/x-www-form-urlencoded", true, false);
    if (!r.ok()) { error = "login HTTP " + std::to_string(r.status) + " " + r.error + " " + r.body; return false; }
    json::Document d;
    if (!d.parse(r.body)) { error = "login: unparseable response"; return false; }
    if (d.root()["loginStatus"].str() != "SUCCESS") { error = "login: " + std::string(d.root()["loginStatus"].str()); return false; }
    session_ = std::string(d.root()["sessionToken"].str());
    return !session_.empty();
}

bool Rest::keep_alive(std::string& error) {
    auto r = post(kKeepAliveUrl, "", "application/x-www-form-urlencoded", false, true);
    json::Document d;
    if (!r.ok() || !d.parse(r.body) || d.root()["status"].str() != "SUCCESS") {
        error = "keepAlive failed: HTTP " + std::to_string(r.status) + " " + r.error + " " + r.body;
        return false;
    }
    return true;
}

HttpResult Rest::betting(const std::string& op, const std::string& json_body) {
    return post(std::string(kBettingUrl) + op + "/", json_body, "application/json", false, true);
}

std::optional<std::string> Rest::place_moc_lay(const std::string& market_id, int64_t selection_id, double liability,
                                               const std::string& customer_ref, std::string& error) {
    char liab[32];
    std::snprintf(liab, sizeof liab, "%.2f", liability);
    std::string body = "{\"marketId\":\"" + json_escape(market_id) + "\",\"customerRef\":\"" + json_escape(customer_ref) +
                       "\",\"instructions\":[{\"selectionId\":" + std::to_string(selection_id) +
                       ",\"handicap\":0,\"side\":\"LAY\",\"orderType\":\"MARKET_ON_CLOSE\",\"marketOnCloseOrder\":{\"liability\":" +
                       liab + "}}]}";
    auto r = betting("placeOrders", body);
    json::Document d;
    if (!r.ok() || !d.parse(r.body)) { error = "placeOrders HTTP " + std::to_string(r.status) + " " + r.error + " " + r.body; return {}; }
    if (d.root()["status"].str() != "SUCCESS") { error = "placeOrders: " + r.body; return {}; }
    for (auto ir : d.root()["instructionReports"]) return std::string(ir["betId"].str());
    error = "placeOrders: no instruction report";
    return {};
}

bool Rest::cancel(const std::string& market_id, const std::string& bet_id, std::string& error) {
    std::string body = "{\"marketId\":\"" + json_escape(market_id) + "\",\"instructions\":[{\"betId\":\"" +
                       json_escape(bet_id) + "\"}]}";
    auto r = betting("cancelOrders", body);
    json::Document d;
    if (!r.ok() || !d.parse(r.body) || d.root()["status"].str() != "SUCCESS") {
        error = "cancelOrders HTTP " + std::to_string(r.status) + " " + r.error + " " + r.body;
        return false;
    }
    return true;
}

}  // namespace bfr::live
