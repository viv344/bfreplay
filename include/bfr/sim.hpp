// Replay one market file through a strategy and settle the resulting lay-at-BSP orders.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "strategy.hpp"

namespace bfr {

constexpr int64_t kLatencyMs = 5'000;   // orders younger than this at the off never reach the exchange

struct Bet {
    int64_t selection_id;
    double bsp;
    bool win;
    double liability;
    double stake;          // backer's stake we accept = liability / (bsp - 1)
    double gross;          // P&L before commission
    double signal;
    double price_at_order;
    double secs_before_off;
};

struct MarketResult {
    std::string market_id, path;
    std::string venue, country;
    int64_t off = 0;
    double base_rate = 0.0;
    int actions = 0;              // order placements + cancellations
    std::vector<Bet> bets;
    size_t messages = 0;
    size_t bytes = 0;
};

struct ReplayStats {
    size_t files_read = 0, messages = 0, bytes = 0, bad_lines = 0;
};

// Returns false if the file is not a GB/IE WIN market (or unreadable / never went in-play).
bool replay_file(const std::string& path, Strategy& strategy, MarketResult& out, ReplayStats& stats);

// Net-market commission settlement (Betfair charges commission on each market's net winnings).
double settle_market(const std::vector<Bet>& bets, double commission);

}  // namespace bfr
