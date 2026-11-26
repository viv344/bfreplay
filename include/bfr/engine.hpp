// MarketEngine: the per-market core shared by historical replay and live trading.
//
// Feed it every market change ("mc" object) for its market, in order, then call decide()
// once per stream message. It keeps the MarketView up to date, detects the off, asks the
// strategy which orders it wants, and diffs that against what is resting to produce
// place/cancel actions. Historical files are recorded Stream API output, so the same calls
// serve both backtest and live.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"
#include "market.hpp"
#include "strategy.hpp"

namespace bfr {

struct OrderAction {
    enum class Kind { Place, Cancel } kind;
    std::string market_id;
    Order order;
};

class MarketEngine {
public:
    MarketEngine(std::string market_id, Strategy& strategy) : strategy_(strategy) { view_.market_id = std::move(market_id); }

    // Apply one market change. Returns false once the market has gone in-play: the caller
    // should stop feeding pre-off logic (the remaining contents of this change are ignored).
    bool on_change(json::Value mc, int64_t pt) {
        if (off_pt_ >= 0) return false;
        if (auto md = mc["marketDefinition"]; md && !md.is_null()) {
            apply_definition(md);
            if (view_.in_play) { off_pt_ = pt; return false; }
        }
        for (auto rc : mc["rc"]) {
            auto ltp = rc["ltp"];
            if (!ltp || ltp.is_null()) continue;
            auto id = static_cast<int64_t>(rc["id"].number(0));
            view_.get_or_add(id).history.push_back({pt, ltp.number()});
        }
        return true;
    }

    // Run the strategy after a message and emit the resulting order changes.
    void decide(int64_t pt, std::vector<OrderAction>& actions) {
        if (off_pt_ >= 0 || !view_.has_definition || view_.status != "OPEN") return;
        const auto& wanted = strategy_.on_update(view_, pt);
        for (auto it = resting_.begin(); it != resting_.end();) {
            if (!wanted.count(it->first)) {
                actions.push_back({OrderAction::Kind::Cancel, view_.market_id, it->second});
                it = resting_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [sel, o] : wanted) {
            auto [it, inserted] = resting_.try_emplace(sel, o);
            if (inserted) actions.push_back({OrderAction::Kind::Place, view_.market_id, o});
            else it->second = o;   // same order (strategy keeps original placed_pt); refresh signal/price
        }
    }

    bool is_off() const { return off_pt_ >= 0; }
    int64_t off_pt() const { return off_pt_; }
    const MarketView& view() const { return view_; }
    const std::map<int64_t, Order>& resting() const { return resting_; }

private:
    void apply_definition(json::Value md) {
        view_.has_definition = true;
        view_.status = std::string(md["status"].str());
        view_.in_play = md["inPlay"].boolean();
        for (auto r : md["runners"]) {
            auto id = static_cast<int64_t>(r["id"].number(0));
            view_.get_or_add(id).status = std::string(r["status"].str());
        }
    }

    Strategy& strategy_;
    MarketView view_;
    std::map<int64_t, Order> resting_;
    int64_t off_pt_ = -1;
};

}  // namespace bfr
