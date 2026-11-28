// Simulator realism: traded-volume scale, queue on arrival, cancel latency, suspensions,
// non-runner reductions, minimum stake and transaction counting.
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

std::string md(const char* status, bool in_play, const char* r1 = "ACTIVE", const char* r2 = "ACTIVE", double af2 = 0) {
    std::string s = std::string("\"marketDefinition\":{\"status\":\"") + status + "\",\"inPlay\":" + (in_play ? "true" : "false") +
                    ",\"marketTime\":\"" + kTime + "\",\"venue\":\"Testford\",\"runners\":[{\"id\":1,\"status\":\"" + r1 +
                    "\"},{\"id\":2,\"status\":\"" + r2 + "\",\"adjustmentFactor\":" + std::to_string(af2) + "}]}";
    return s;
}

struct Harness {
    json::Document doc;
    void feed(MakerSim& s, const std::string& body, int64_t pt) {
        std::string m = "{\"id\":\"1.1\"," + body + "}";
        ASSERT_TRUE(doc.parse(m));
        s.on_change(doc.root(), pt);
    }
};
const std::string kImg = ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,30]],\"trd\":[[5.0,100]]}]";
}  // namespace

// [FIX1] trd counts backer's stake x 2: 40 traded at 5.0 = 20 matched, which only clears 20 of the 30 ahead.
TEST(Proposed, TradedScaleHalf) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + kImg, T0 - 600'000);
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,10]],\"trd\":[[5.0,140]]}]", T0 - 590'000);
    EXPECT_TRUE(sim.fills().empty());
    EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 10);
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,0]],\"trd\":[[5.0,172]]}]", T0 - 580'000);   // 16 matched: 10 queue + 6 us
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 6);
}

// [FIX2] Size that joins at our price while our order is in flight is ahead of us.
TEST(Proposed, QueueSnapshotAtArrival) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    SimParams p;
    p.latency_ms = 5'000;
    MakerSim sim("1.1", st, p);
    Harness h;
    h.feed(sim, md("OPEN", false) + kImg, T0 - 600'000);
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,80]]}]", T0 - 599'000);   // 50 more joins during latency
    h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,80]]}]", T0 - 590'000);   // arrival
    EXPECT_DOUBLE_EQ(sim.resting()[0].queue_ahead, 80);
}

// [FIX3] An order we decided to cancel is still picked off if the market trades through it within cancel latency.
TEST(Proposed, CancelLatencyAdverseFill) {
    Fixed st;
    st.q[1] = Quote{-1, 0, tick_of(4.9), 8};
    SimParams p;
    p.latency_ms = 0;
    p.cancel_latency_ms = 500;
    MakerSim sim("1.1", st, p);
    Harness h;
    h.feed(sim, md("OPEN", false) + kImg, T0 - 600'000);
    h.feed(sim, "\"rc\":[{\"id\":1,\"atb\":[[4.9,50]]}]", T0 - 599'000);   // order arrives
    st.q.clear();                                                           // strategy pulls the quote
    h.feed(sim, "\"rc\":[{\"id\":1,\"atb\":[[4.9,40]]}]", T0 - 598'000);   // cancel requested now
    ASSERT_EQ(sim.resting().size(), 1u);
    EXPECT_GE(sim.resting()[0].cancel_pt, 0);
    h.feed(sim, "\"rc\":[{\"id\":1,\"atb\":[[4.9,0],[4.8,5]],\"trd\":[[4.8,21]]}]", T0 - 597'800);   // through, inside latency
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 8);
}

// [FIX6] Non-runner with RF >= 2.5%: unmatched cancelled, matched prices reduced, removed runner's bets void.
TEST(Proposed, NonRunnerReduction) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
    SimParams p;
    p.latency_ms = 0;
    MakerSim sim("1.1", st, p);
    Harness h;
    h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,0.5]],\"trd\":[[5.0,1]]}]",
           T0 - 600'000);
    h.feed(sim, "\"rc\":[{\"id\":1,\"trd\":[[5.0,9]]}]", T0 - 590'000);   // 4 matched: 0.5 queue then 3.5 us
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_DOUBLE_EQ(sim.fills()[0].size, 3.5);
    st.q.clear();
    h.feed(sim, md("SUSPENDED", false, "ACTIVE", "REMOVED", 20.0), T0 - 300'000);
    EXPECT_TRUE(sim.resting().empty());
    EXPECT_NEAR(sim.fills()[0].price, 4.0, 1e-9);                     // 5.0 x (1 - 0.20)
    EXPECT_NEAR(sim.positions().at(1).win_pl, 3.5 * 3.0, 1e-9);
}

TEST(Proposed, MinStakeRejected) {
    Fixed st;
    st.q[1] = Quote{tick_of(5.0), 0.5, -1, 0};
    MakerSim sim("1.1", st, SimParams{});
    Harness h;
    h.feed(sim, md("OPEN", false) + kImg, T0 - 600'000);
    EXPECT_TRUE(sim.resting().empty());
    EXPECT_EQ(sim.result().rejected_min_stake, 1);
}

TEST(Proposed, CancelModels) {
    for (auto model : {CancelModel::ProRata, CancelModel::Behind, CancelModel::Power}) {
        Fixed st;
        st.q[1] = Quote{tick_of(5.0), 10, -1, 0};
        SimParams p;
        p.latency_ms = 0;
        p.cancel = model;
        MakerSim sim("1.1", st, p);
        Harness h;
        h.feed(sim, md("OPEN", false) + ",\"img\":true,\"rc\":[{\"id\":1,\"atb\":[[4.9,50]],\"atl\":[[5.0,30]],\"trd\":[[5.0,1]]}]",
               T0 - 600'000);
        h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,40]]}]", T0 - 599'000);   // arrival: 30 ahead (prev), 10 behind joins
        h.feed(sim, "\"rc\":[{\"id\":1,\"atl\":[[5.0,30]]}]", T0 - 598'000);   // 10 cancelled
        double q = sim.resting()[0].queue_ahead;
        if (model == CancelModel::ProRata) { EXPECT_NEAR(q, 30 * 30.0 / 40, 1e-9); }
        if (model == CancelModel::Behind) { EXPECT_NEAR(q, 30, 1e-9); }
        if (model == CancelModel::Power) { EXPECT_NEAR(q, 30 - 10 * 900.0 / 1000, 1e-9); }
    }
}
