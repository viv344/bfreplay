// BSP reconciliation at the off: LAPSE orders matched only when worse than BSP for us, TAKE_SP
// orders converted to SP bets (lay keeps liability, minimums apply), persistence changes keep the
// queue place, and orders still in flight at the off are not included.
#include <gtest/gtest.h>
#include <string>
#include "bfr/mm/sim.hpp"

using namespace bfr;
using namespace bfr::mm;

namespace {
constexpr int64_t T0 = 1'800'000'000'000;
constexpr const char* kTime = "2027-01-15T08:00:00.000Z";

struct Fixed final : MakerStrategy {
    std::map<int64_t, Quote> q;
    std::map<int64_t, Quote> quotes(const MarketBook&, int64_t, const Positions&) override { return q; }
};

std::string md(const char* status, bool in_play, const char* r1 = "ACTIVE", const char* bsp1 = nullptr) {
    std::string r = std::string("{\"id\":1,\"status\":\"") + r1 + "\"" + (bsp1 ? std::string(",\"bsp\":") + bsp1 : "") + "}";
    std::string r2 = std::string("{\"id\":2,\"status\":\"") + (std::string(r1) == "LOSER" ? "WINNER" : "ACTIVE") + "\"" +
                     (bsp1 ? ",\"bsp\":1.3" : "") + "}";
    return std::string("\"marketDefinition\":{\"status\":\"") + status + "\",\"inPlay\":" + (in_play ? "true" : "false") +
           ",\"marketTime\":\"" + kTime + "\",\"venue\":\"Testford\",\"runners\":[" + r + "," + r2 + "]}";
}

struct Market {
    Fixed st;
    MakerSim sim;
    json::Document doc;
    explicit Market(Quote q, SimParams p = {}) : sim("1.1", st, p) { st.q[1] = q; }
    void feed(const std::string& body, int64_t pt) {
        std::string m = "{\"id\":\"1.1\"," + body + "}";
        ASSERT_TRUE(doc.parse(m));
        sim.on_change(doc.root(), pt);
    }
    // Book: back 4.9 / lay 5.1, so quotes at 5.0 on either side rest passively. Nothing trades pre-off.
    void to_close(const char* bsp, const char* result = "LOSER", int64_t off = T0 + 30'000) {
        feed(md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.1,30]],\"trd\":[[5.0,1]]}]",
             T0 - 600'000);
        feed("\"rc\":[{\"id\":1,\"atb\":[[4.9,51]]}]", T0 - 590'000);
        feed(md("OPEN", true), off);
        feed(md("CLOSED", true, result, bsp), T0 + 400'000);
        ASSERT_TRUE(sim.done());
    }
};
}  // namespace

TEST(Reconcile, LapseLayMatchedOnlyWhenWorseThanSP) {
    Market worse(Quote{-1, 0, tick_of(5.0), 4});
    worse.to_close("4.5");                       // laid at 5.0 > BSP 4.5: included at our (worse) price
    const auto& r = worse.sim.result();
    EXPECT_EQ(r.recon_fills, 1);
    EXPECT_NEAR(r.recon_matched, 4, 1e-9);
    EXPECT_NEAR(r.edge_vs_bsp, 4 * (1 - 5.0 / 4.5), 1e-9);
    EXPECT_NEAR(r.pnl_result, 4 * 0.98, 1e-9);   // runner lost: lay wins the stake
    Market better(Quote{-1, 0, tick_of(5.0), 4});
    better.to_close("5.5");                      // 5.0 < BSP 5.5 would be better than SP: lapses
    EXPECT_EQ(better.sim.result().recon_fills, 0);
}

TEST(Reconcile, LapseBackMatchedOnlyWhenWorseThanSP) {
    Market worse(Quote{tick_of(5.0), 4, -1, 0});
    worse.to_close("5.5");                       // backed at 5.0 < BSP 5.5: included
    EXPECT_EQ(worse.sim.result().recon_fills, 1);
    EXPECT_NEAR(worse.sim.result().edge_vs_bsp, 4 * (5.0 / 5.5 - 1), 1e-9);
    Market better(Quote{tick_of(5.0), 4, -1, 0});
    better.to_close("4.5");
    EXPECT_EQ(better.sim.result().recon_fills, 0);
}

TEST(Reconcile, TakeSpLayKeepsLiabilityAndMinimum) {
    Quote q{-1, 0, tick_of(5.0), 4};
    q.lay_take_sp = true;
    Market big(q);
    big.to_close("4.5");                         // liability 4 x 4 = 16 -> SP lay of 16 / 3.5 at 4.5
    ASSERT_EQ(big.sim.fills().size(), 1u);
    EXPECT_NEAR(big.sim.fills()[0].size, 16 / 3.5, 1e-9);
    EXPECT_DOUBLE_EQ(big.sim.fills()[0].price, 4.5);
    EXPECT_NEAR(big.sim.result().edge_vs_bsp, 0, 1e-12);
    Quote small_q{-1, 0, tick_of(5.0), 2};       // liability 8 < £10: lapses
    small_q.lay_take_sp = true;
    Market small(small_q);
    small.to_close("4.5");
    EXPECT_EQ(small.sim.result().recon_fills, 0);
}

TEST(Reconcile, TakeSpBackKeepsStakeAndMinimum) {
    Quote q{tick_of(5.0), 3, -1, 0};
    q.back_take_sp = true;
    Market big(q);
    big.to_close("6.0");
    ASSERT_EQ(big.sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(big.sim.fills()[0].size, 3);
    EXPECT_DOUBLE_EQ(big.sim.fills()[0].price, 6.0);
    Quote small_q{tick_of(5.0), 1.5, -1, 0};     // below the £2 SP back minimum
    small_q.back_take_sp = true;
    Market small(small_q);
    small.to_close("6.0");
    EXPECT_EQ(small.sim.result().recon_fills, 0);
}

TEST(Reconcile, PersistenceChangeKeepsQueuePlace) {
    Market run(Quote{-1, 0, tick_of(4.9), 4});
    run.feed(md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.1,30]],\"trd\":[[4.9,1]]}]",
             T0 - 600'000);
    run.feed("\"rc\":[{\"id\":1,\"atb\":[[4.9,50]]}]", T0 - 590'000);   // arrives behind £50
    ASSERT_EQ(run.sim.resting().size(), 1u);
    EXPECT_DOUBLE_EQ(run.sim.resting()[0].queue_ahead, 50);
    EXPECT_FALSE(run.sim.resting()[0].take_sp);
    run.st.q[1].lay_take_sp = true;                                     // updateOrders: same order, same place
    run.feed("\"rc\":[{\"id\":1,\"atb\":[[4.9,50]]}]", T0 - 580'000);
    ASSERT_EQ(run.sim.resting().size(), 1u);
    EXPECT_TRUE(run.sim.resting()[0].take_sp);
    EXPECT_DOUBLE_EQ(run.sim.resting()[0].queue_ahead, 50);
    EXPECT_EQ(run.sim.result().orders, 1);
}

TEST(Reconcile, InFlightAtOffAndDisabled) {
    SimParams slow;
    slow.latency_ms = 3'600'000;                 // never arrives before the off
    Market inflight(Quote{-1, 0, tick_of(5.0), 4}, slow);
    inflight.to_close("4.5");
    EXPECT_EQ(inflight.sim.result().recon_fills, 0);
    SimParams off;
    off.reconcile_at_off = false;
    Market disabled(Quote{-1, 0, tick_of(5.0), 4}, off);
    disabled.to_close("4.5");
    EXPECT_EQ(disabled.sim.result().recon_fills, 0);
}
