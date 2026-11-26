// End-to-end: write a synthetic market stream to a .bz2 file and replay it.
#include <gtest/gtest.h>
#include <bzlib.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include "bfr/sim.hpp"

using namespace bfr;

namespace {
constexpr int64_t T0 = 1763000000000, kMin = 60'000;

std::string md(bool in_play, const char* status, const char* r1, const char* r2, const char* bsp1 = nullptr,
               const char* bsp2 = nullptr) {
    auto runner = [](int id, const char* st, const char* bsp) {
        std::string s = "{\"status\":\"" + std::string(st) + "\",\"id\":" + std::to_string(id);
        if (bsp) s += std::string(",\"bsp\":") + bsp;
        return s + "}";
    };
    return std::string("{\"marketType\":\"WIN\",\"countryCode\":\"GB\",\"venue\":\"Testford\",\"marketBaseRate\":5.0,") +
           "\"inPlay\":" + (in_play ? "true" : "false") + ",\"status\":\"" + status + "\",\"runners\":[" +
           runner(11, r1, bsp1) + "," + runner(22, r2, bsp2) + "]}";
}
std::string msg(int64_t pt, const std::string& body) {
    return "{\"op\":\"mcm\",\"pt\":" + std::to_string(pt) + ",\"mc\":[{\"id\":\"1.999\"," + body + "}]}\n";
}
std::string rc(double a, double b) {
    return "\"rc\":[{\"ltp\":" + std::to_string(a) + ",\"id\":11},{\"ltp\":" + std::to_string(b) + ",\"id\":22}]";
}
std::string write_bz2(const std::string& text) {
    auto path = (std::filesystem::temp_directory_path() / "1.999.bz2").string();
    FILE* f = std::fopen(path.c_str(), "wb");
    int err;
    BZFILE* bz = BZ2_bzWriteOpen(&err, f, 9, 0, 0);
    BZ2_bzWrite(&err, bz, const_cast<char*>(text.data()), static_cast<int>(text.size()));
    BZ2_bzWriteClose(&err, bz, 0, nullptr, nullptr);
    std::fclose(f);
    return path;
}
}  // namespace

TEST(Replay, LaysFavouriteAndSettlesAtBsp) {
    std::string s;
    s += msg(T0, "\"marketDefinition\":" + md(false, "OPEN", "ACTIVE", "ACTIVE"));
    s += msg(T0 + 1 * kMin, rc(2.0, 6.0));                        // runner 11 favourite at 1 min
    s += msg(T0 + 70 * kMin, rc(2.2, 6.0));
    s += msg(T0 + 73 * kMin, "\"marketDefinition\":" + md(true, "OPEN", "ACTIVE", "ACTIVE"));   // off
    s += msg(T0 + 80 * kMin, "\"marketDefinition\":" + md(true, "CLOSED", "LOSER", "WINNER", "5.5", "2.9"));
    auto path = write_bz2(s);
    LayFavourite strat({.liability = 20.0});
    MarketResult r;
    ReplayStats st;
    ASSERT_TRUE(replay_file(path, strat, r, st));
    ASSERT_EQ(r.bets.size(), 1u);
    const auto& b = r.bets[0];
    EXPECT_EQ(b.selection_id, 11);
    EXPECT_DOUBLE_EQ(b.bsp, 5.5);
    EXPECT_FALSE(b.win);
    EXPECT_DOUBLE_EQ(b.stake, 20.0 / 4.5);
    EXPECT_DOUBLE_EQ(b.gross, 20.0 / 4.5);
    EXPECT_DOUBLE_EQ(b.secs_before_off, 72 * 60.0);  // placed at 1m, off at 73m
    EXPECT_NEAR(settle_market(r.bets, 0.02), 20.0 / 4.5 * 0.98, 1e-12);
    EXPECT_EQ(r.venue, "Testford");
}

TEST(Replay, OrderTooCloseToOffIsNotMatched) {
    std::string s;
    s += msg(T0, "\"marketDefinition\":" + md(false, "OPEN", "ACTIVE", "ACTIVE"));
    s += msg(T0 + 70 * kMin, rc(2.0, 6.0));                       // first price only 2s before the off
    s += msg(T0 + 70 * kMin + 2000, "\"marketDefinition\":" + md(true, "OPEN", "ACTIVE", "ACTIVE"));
    s += msg(T0 + 80 * kMin, "\"marketDefinition\":" + md(true, "CLOSED", "WINNER", "LOSER", "5.5", "2.9"));
    LayFavourite strat({});
    MarketResult r;
    ReplayStats st;
    ASSERT_TRUE(replay_file(write_bz2(s), strat, r, st));
    EXPECT_TRUE(r.bets.empty());
}

TEST(Replay, NetMarketCommissionOnlyOnProfit) {
    std::vector<Bet> bets{{1, 5, true, 20, 5, -20, 0, 0, 0}, {2, 3, false, 20, 10, 10, 0, 0, 0}};
    EXPECT_DOUBLE_EQ(settle_market(bets, 0.05), -10.0);
    bets[0].gross = 0;
    EXPECT_DOUBLE_EQ(settle_market(bets, 0.05), 9.5);
}
