#include "bfr/live/stream.hpp"
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "bfr/json.hpp"
#include "bfr/live/rest.hpp"

namespace bfr::live {

void StreamClient::close() {
    if (ssl_) { SSL_shutdown(static_cast<SSL*>(ssl_)); SSL_free(static_cast<SSL*>(ssl_)); ssl_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (ctx_) { SSL_CTX_free(static_cast<SSL_CTX*>(ctx_)); ctx_ = nullptr; }
    buf_.clear();
    pending_.clear();
}

bool StreamClient::send_line(const std::string& s, std::string& error) {
    std::string out = s + "\r\n";
    size_t sent = 0;
    while (sent < out.size()) {
        int n = SSL_write(static_cast<SSL*>(ssl_), out.data() + sent, static_cast<int>(out.size() - sent));
        if (n <= 0) { error = "stream write failed"; return false; }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool StreamClient::read_line(std::string& line, std::string& error) {
    if (!pending_.empty()) { line = std::move(pending_.front()); pending_.pop_front(); return true; }
    return read_raw(line, error);
}

bool StreamClient::read_raw(std::string& line, std::string& error) {
    while (true) {
        if (auto nl = buf_.find('\n'); nl != std::string::npos) {
            line.assign(buf_, 0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf_.erase(0, nl + 1);
            return true;
        }
        char chunk[1 << 16];
        int n = SSL_read(static_cast<SSL*>(ssl_), chunk, sizeof chunk);
        if (n <= 0) {
            error = "stream read failed or timed out (no data within 3 heartbeats)";
            return false;
        }
        buf_.append(chunk, static_cast<size_t>(n));
    }
}

bool StreamClient::expect_status(int id, std::string& error) {
    std::string line;
    json::Document d;
    for (int i = 0; i < 20; ++i) {
        if (!read_raw(line, error)) return false;
        if (!d.parse(line)) continue;
        auto r = d.root();
        if (r["op"].str() != "status" || static_cast<int>(r["id"].number(-1)) != id) { pending_.push_back(line); continue; }
        if (r["statusCode"].str() == "SUCCESS") return true;
        error = "stream status " + std::string(r["errorCode"].str()) + ": " + std::string(r["errorMessage"].str());
        return false;
    }
    error = "no status reply from stream";
    return false;
}

void StreamClient::note_clocks(const std::string& initial_clk, const std::string& clk) {
    if (!initial_clk.empty()) initial_clk_ = initial_clk;
    if (!clk.empty()) clk_ = clk;
}

bool StreamClient::connect(const std::string& app_key, const std::string& session, std::string& error) {
    close();
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &res) != 0 || !res) {
        error = "DNS lookup failed for " + cfg_.host;
        return false;
    }
    for (auto* a = res; a; a = a->ai_next) {
        fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd_ < 0) continue;
        if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd_);
        fd_ = -1;
    }
    freeaddrinfo(res);
    if (fd_ < 0) { error = "TCP connect failed"; return false; }
    timeval tv{};
    tv.tv_sec = std::max(30, (3 * cfg_.heartbeat_ms) / 1000);   // 3 missed heartbeats (min 30 s) = dead link
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

    auto* ctx = SSL_CTX_new(TLS_client_method());
    ctx_ = ctx;
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    auto* ssl = SSL_new(ctx);
    ssl_ = ssl;
    SSL_set_tlsext_host_name(ssl, cfg_.host.c_str());
    SSL_set1_host(ssl, cfg_.host.c_str());
    SSL_set_fd(ssl, fd_);
    if (SSL_connect(ssl) != 1) {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
        error = std::string("TLS handshake failed: ") + buf;
        return false;
    }

    std::string line;
    if (!read_raw(line, error)) return false;               // {"op":"connection",...}
    if (line.find("\"connection\"") == std::string::npos) { error = "unexpected first message: " + line; return false; }

    int auth_id = next_id_++;
    if (!send_line("{\"op\":\"authentication\",\"id\":" + std::to_string(auth_id) + ",\"appKey\":\"" + json_escape(app_key) +
                       "\",\"session\":\"" + json_escape(session) + "\"}", error) ||
        !expect_status(auth_id, error))
        return false;

    int sub_id = next_id_++;
    std::string sub = "{\"op\":\"marketSubscription\",\"id\":" + std::to_string(sub_id) +
                      ",\"heartbeatMs\":" + std::to_string(cfg_.heartbeat_ms) + ",\"marketFilter\":" + cfg_.market_filter +
                      ",\"marketDataFilter\":{\"fields\":" + cfg_.data_fields + "}";
    if (!initial_clk_.empty() && !clk_.empty())
        sub += ",\"initialClk\":\"" + json_escape(initial_clk_) + "\",\"clk\":\"" + json_escape(clk_) + "\"";
    sub += "}";
    return send_line(sub, error) && expect_status(sub_id, error);
}

}  // namespace bfr::live
