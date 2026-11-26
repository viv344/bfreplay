#include <gtest/gtest.h>
#include "bfr/strategy.hpp"

using namespace bfr;

namespace {
constexpr int64_t kMin = 60'000;

MarketView market_with(std::vector<std::pair<int64_t, std::vector<PricePoint>>> runners) {
    MarketView mv;
    mv.has_definition = true;
    mv.status = "OPEN";
    for (auto& [id, h] : runners) mv.runners.push_back(Runner{id, "ACTIVE", h});
    return mv;
}
}  // namespace

TEST(LayFavourite, LaysLowestPricedActiveRunnerInBand) {
    auto mv = market_with({{1, {{0, 5.0}}}, {2, {{0, 2.4}}}, {3, {{0, 8.0}}}});
    LayFavourite s({.band_lo = 1.5, .band_hi = 4.0});
    auto& w = s.on_update(mv, 10 * kMin);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_TRUE(w.count(2));
    EXPECT_EQ(w.at(2).placed_pt, 10 * kMin);
    EXPECT_DOUBLE_EQ(w.at(2).liability, 20.0);
}

TEST(LayFavourite, NoOrderWhenFavouriteOutsideBand) {
    auto mv = market_with({{1, {{0, 5.0}}}, {2, {{0, 1.2}}}});
    LayFavourite s({.band_lo = 1.5, .band_hi = 4.0});
    EXPECT_TRUE(s.on_update(mv, 10 * kMin).empty());
}

TEST(LayFavourite, IgnoresNonActiveAndUnpricedRunners) {
    auto mv = market_with({{1, {{0, 2.0}}}, {2, {}}, {3, {{0, 3.0}}}});
    mv.runners[0].status = "REMOVED";
    LayFavourite s({});
    auto& w = s.on_update(mv, kMin);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_TRUE(w.count(3));
}

TEST(LayFavourite, KeepsPlacementTimeThenSwitchesWhenFavouriteChanges) {
    auto mv = market_with({{1, {{0, 2.0}}}, {2, {{0, 3.0}}}});
    LayFavourite s({});
    s.on_update(mv, 1 * kMin);
    mv.runners[0].history.push_back({2 * kMin, 2.2});
    auto& w1 = s.on_update(mv, 2 * kMin);
    ASSERT_TRUE(w1.count(1));
    EXPECT_EQ(w1.at(1).placed_pt, 1 * kMin);           // not re-placed
    EXPECT_DOUBLE_EQ(w1.at(1).price_at_order, 2.2);    // but refreshed
    mv.runners[1].history.push_back({3 * kMin, 1.9});  // runner 2 becomes favourite
    auto& w2 = s.on_update(mv, 3 * kMin);
    ASSERT_EQ(w2.size(), 1u);
    EXPECT_TRUE(w2.count(2));
    EXPECT_EQ(w2.at(2).placed_pt, 3 * kMin);
}
