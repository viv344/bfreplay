// Maker simulator: tick ladder, book maintenance, queue position, cancellations, trade-through,
// latency, post-only, lapse at the off and settlement.
#include <gtest/gtest.h>
#include <string>
#include "bfr/mm/join_touch.hpp"

using namespace bfr;
using namespace bfr::mm;

TEST(Ticks, Ladder) {
    EXPECT_DOUBLE_EQ(price_of(0), 1.01);
    EXPECT_DOUBLE_EQ(price_of(kNumTicks - 1), 1000.0);
    EXPECT_EQ(tick_of(2.0) + 1, tick_of(2.02));
    EXPECT_EQ(tick_of(5.9) + 1, tick_of(6.0));
    EXPECT_EQ(tick_of(6.0) + 1, tick_of(6.2));
    EXPECT_EQ(tick_of(1000), kNumTicks - 1);
    EXPECT_EQ(tick_of(5.05), -1);
    for (int t = 0; t < kNumTicks; ++t) EXPECT_EQ(tick_of(price_of(t)), t);
}

namespace {
constexpr int64_t T0 = 1'800'000'000'000;           // scheduled start
constexpr const char* kTime = "2027-01-15T08:00:00.000Z";

// A strategy that wants exactly the quote it is given (or nothing).
struct Fixed final : MakerStrategy {
    std::map<int64_t, Quote> q;
    std::map<int64_t, Quote> quotes(const MarketBook&, int64_t, const Positions&) override { return q; }
};

std::string md(const char* status, bool in_play, const char* r1 = "ACTIVE", const char* r2 = "ACTIVE",
               const char* bsp1 = nullptr, const char* bsp2 = nullptr) {
    auto runner = [](int id, const char* st, const char* bsp) {
        std::string s = "{\"id\":" + std::to_string(id) + ",\"status\":\"" + st + "\"";
        if (bsp) s += std::string(",\"bsp\":") + bsp;
        return s + "}";
    };
    return std::string("\"marketDefinition\":{\"status\":\"") + status + "\",\"inPlay\":" + (in_play ? "true" : "false") +
           ",\"marketTime\":\"" + kTime + "\",\"venue\":\"Testford\",\"runners\":[" + runner(1, r1, bsp1) + "," +
           runner(2, r2, bsp2) + "]}";
}

struct Harness {
    json::Document doc;
    void feed(MakerSim& s, const std::string& body, int64_t pt) {
        std::string m = "{\"id\":\"1.1\"," + body + "}";
        ASSERT_TRUE(doc.parse(m));
        s.on_change(doc.root(), pt);
    }
};
}  // namespace

// Runner 1 book: back 4.9 (£50), lay 5.0 (£30). Our back at 5.0 joins behind £30.
TEST(MakerSim, QueueThenFill) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,30]],\"trd\":[[5.0,100]]}]", T0 - 600'000);
    ASSERT_EQ(sim.resting().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 30);
    // 20 traded at 5.0: still 10 ahead of us, no fill.
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,10]],\"trd\":[[5.0,120]]}]", T0 - 590'000);
    EXPECT_TRUE(sim.fills().empty());
    EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 10);
    // 16 more traded: 10 clears the queue, 6 fills us.
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,0]],\"trd\":[[5.0,136]]}]", T0 - 580'000);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 6);
    EXPECT_DOUBLE_EQ(sim.positions().at(1).win_pl, 6 * 4.0);
    EXPECT_DOUBLE_EQ(sim.positions().at(1).lose_pl, -6);
}

TEST(MakerSim, FirstTradedSnapshotIsBaseline) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,0.5]]}]", T0 - 600'000);
    // First trd for this runner arrives later with large cumulative volume: not new trades.
    h.feed(sim, "\"rc\":[{\"id\":1,\"trd\":[[5.0,5000]]}]", T0 - 590'000);
    EXPECT_TRUE(sim.fills().empty());
}

TEST(MakerSim, CancellationsProRataVsBehind) {
    for (auto model : {CancelModel::ProRata, CancelModel::Behind}) {
        Fixed st;
        st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
        SimParams p;
        p.cancel = model;
        MakerSim sim("1.1", st, p);
        Harness h;
        h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,40]],\"trd\":[[5.0,1]]}]", T0 - 600'000);
        // Shown size halves with no trading.
        h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,20]]}]", T0 - 590'000);
        EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, model == CancelModel::ProRata ? 20 : 20);   // clamp also gives 20
        // Shown size 30 (others joined behind us), then 15 cancel.
        h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,30]]}]", T0 - 580'000);
        h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,15]]}]", T0 - 570'000);
        if (model == CancelModel::ProRata) EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 10);
        else EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 15);
    }
}

TEST(MakerSim, TradeThroughFillsCompletely) {
    Fixed st;
    st.q[1] = Quote{-1, 0, tick_of(4.9), 8};   // our lay at 4.9 behind £50
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,30]],\"trd\":[[4.8,1]]}]", T0 - 600'000);
    // A trade at 4.8 (worse for backers than our 4.9) means our lay was taken.
    h.feed(sim, "\"rc\":[{\"id\":1,\"atb\":[[4.9,0],[4.8,5]],\"trd\":[[4.8,21]]}]", T0 - 590'000);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 8);
    EXPECT_FALSE(sim.fills()[0].back);
}

TEST(MakerSim, LatencyAndPostOnly) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, tick_of(5.0), 10};   // lay at 5.0 would cross best lay 5.0: rejected
    SimParams p;
    p.latency_ms = 5'000;
    MakerSim sim("1.1", st, p);
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,5]],\"trd\":[[5.0,1]]}]", T0 - 600'000);
    ASSERT_EQ(sim.resting().size(), 1u);
    EXPECT_TRUE(sim.resting()[0].back);
    // Within the latency window 20 trades: clears the 5 ahead but cannot fill us.
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,0]],\"trd\":[[5.0,21]]}]", T0 - 598'000);
    EXPECT_TRUE(sim.fills().empty());
    EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 0);
    h.feed(sim, "\"rc\":[{\"id\":1,\"trd\":[[5.0,25]]}]", T0 - 590'000);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 4);
}

TEST(MakerSim, LapsesAtOffAndSettles) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,0.1]],\"trd\":[[5.0,1]]}]", T0 - 600'000);
    h.feed(sim, "\"rc\":[{\"id\":1,\"trd\":[[5.0,5.1]]}]", T0 - 590'000);   // 4 matched for us
    ASSERT_EQ(sim.fills().size(), 1u);
    h.feed(sim, md("OPEN", true), T0 + 30'000);
    EXPECT_TRUE(sim.resting().empty());
    // Runner 1 loses, BSP 4.0: backing at 5.0 had +25% edge vs BSP.
    h.feed(sim, md("CLOSED", true, "LOSER", "WINNER", "4.0", "1.33"), T0 + 400'000);
    ASSERT_TRUE(sim.done());
    const auto& r = sim.result();
    EXPECT_TRUE(r.settled);
    EXPECT_NEAR(r.edge_vs_bsp, 4 * (5.0 / 4.0 - 1), 1e-9);
    EXPECT_NEAR(r.pnl_result, -4, 1e-9);
    // Hedge: win_pl=16, lose_pl=-4; back s=(-4-16)/4=-5 (lay 5 at BSP) -> -4+5 = +1, after 2% commission.
    EXPECT_NEAR(r.pnl_hedged, 1 * 0.98, 1e-9);
}

TEST(JoinTouch, QuotesInsideWideSpreadOnlyInWindow) {
    JoinTouch jt(JoinTouchParams{});
    MarketBook b;
    b.market_time_ms = T0;
    auto& r = b.get(1);
    r.atb[tick_of(4.8)] = 20;
    r.atl[tick_of(5.1)] = 20;   // 3-tick spread: improve
    r.spn = 4.95;
    Positions pos;
    auto q = jt.quotes(b, T0 - 600'000, pos);
    ASSERT_EQ(q.count(1), 1u);
    EXPECT_EQ(q[1].back_tick, tick_of(5.0));
    EXPECT_EQ(q[1].lay_tick, tick_of(4.9));
    EXPECT_TRUE(jt.quotes(b, T0 - 40 * 60'000, pos).empty());   // before the window
    EXPECT_TRUE(jt.quotes(b, T0 - 5'000, pos).empty());          // after stop
    pos[1].lose_pl = -25;                                        // too long already: no more backs
    q = jt.quotes(b, T0 - 600'000, pos);
    EXPECT_EQ(q[1].back_tick, -1);
    EXPECT_EQ(q[1].lay_tick, tick_of(4.9));
}
