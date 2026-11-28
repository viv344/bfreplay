// Betfair REST calls used by the live client: certificate login, keep-alive, and
// market-on-close (BSP) lay orders. Thin wrapper over libcurl.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace bfr::live {

struct Credentials {
    std::string username, password, app_key, cert_file, key_file;
};

struct HttpResult {
    long status = 0;
    std::string body, error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

class Rest {
public:
    explicit Rest(Credentials c);
    ~Rest();
    Rest(const Rest&) = delete;
    Rest& operator=(const Rest&) = delete;

    // Non-interactive (certificate) login. Returns the session token or an error message.
    bool login(std::string& error);
    bool keep_alive(std::string& error);
    const std::string& session() const { return session_; }
    const std::string& app_key() const { return creds_.app_key; }

    // Lay at BSP sized by liability (Betfair MARKET_ON_CLOSE order). Returns betId on success.
    std::optional<std::string> place_moc_lay(const std::string& market_id, int64_t selection_id, double liability,
                                             const std::string& customer_ref, std::string& error);
    bool cancel(const std::string& market_id, const std::string& bet_id, std::string& error);
    // Accounts API call (e.g. "getDeveloperAppKeys"); returns the raw response.
    HttpResult account(const std::string& op, const std::string& json_body = "{}");

private:
    HttpResult post(const std::string& url, const std::string& body, const std::string& content_type,
                    bool client_cert, bool authenticated);
    HttpResult betting(const std::string& op, const std::string& json_body);

    Credentials creds_;
    std::string session_;
    void* curl_ = nullptr;   // CURL*
};

// Minimal JSON string escaping for request bodies.
std::string json_escape(const std::string& s);

}  // namespace bfr::live
