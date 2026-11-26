// Betfair Exchange Stream API client: TLS connection, authentication, market subscription,
// line framing, heartbeat monitoring and clk-based resubscription after reconnects.
#pragma once
#include <cstdint>
#include <functional>
#include <deque>
#include <string>

namespace bfr::live {

struct StreamConfig {
    std::string host = "stream-api.betfair.com";
    int port = 443;
    int heartbeat_ms = 5000;
    // Market filter / fields as raw JSON fragments.
    std::string market_filter = R"({"eventTypeIds":["7"],"countryCodes":["GB","IE"],"marketTypes":["WIN"]})";
    std::string data_fields = R"(["EX_MARKET_DEF","EX_LTP"])";
};

class StreamClient {
public:
    explicit StreamClient(StreamConfig cfg) : cfg_(std::move(cfg)) {}
    ~StreamClient() { close(); }
    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // Connect, authenticate and subscribe (resuming from the last clk if we have one).
    bool connect(const std::string& app_key, const std::string& session, std::string& error);
    // Read the next complete line. Returns false on disconnect/timeout (caller reconnects).
    bool read_line(std::string& line, std::string& error);
    // Track subscription clocks from an mcm message so a reconnect resumes without a full image.
    void note_clocks(const std::string& initial_clk, const std::string& clk);
    void close();

private:
    bool send_line(const std::string& s, std::string& error);
    bool read_raw(std::string& line, std::string& error);
    bool expect_status(int id, std::string& error);

    StreamConfig cfg_;
    void* ctx_ = nullptr;   // SSL_CTX*
    void* ssl_ = nullptr;   // SSL*
    int fd_ = -1;
    std::string buf_;
    std::deque<std::string> pending_;   // non-status lines seen while waiting for a status reply
    std::string initial_clk_, clk_;
    int next_id_ = 1;
};

}  // namespace bfr::live
