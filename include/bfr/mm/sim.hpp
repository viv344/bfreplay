// Queue-aware maker simulator for pre-off Betfair markets.
//
// A MakerStrategy says which passive quotes it wants per runner after each update. The simulator
// keeps those as resting limit orders and fills them from the recorded stream:
//   * Queue: a new order joins the back of the displayed queue at its price (queue_ahead = shown size).
//   * Trades at our price (EX_TRADED deltas, times traded_scale) consume the queue ahead, then us.
//   * Cancellations: displayed size that disappears without trading shrinks queue_ahead either
//     pro-rata (default) or only once the queue behind us is gone (CancelModel::Behind, pessimistic).
//   * Trade-through: a trade at a worse price than ours, or the opposite best crossing our price,
//     fills us completely (the market could only get there through our order).
//   * Latency: an order can be filled only from active_pt = placement + latency_ms.
//   * Post-only: a quote that would cross the book is not placed (we never take).
// Resting orders lapse at the off. At settlement the result P&L, a hedged-at-BSP P&L (exposure
// closed with SP orders at BSP) and the edge of every fill relative to BSP are reported.
// Commission is charged on net market winnings.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
#include "bfr/mm/book.hpp"

namespace bfr::mm {

struct Quote {
    int back_tick = -1;   // our BACK order price (rests in the atl queue); -1 = none
    double back_size = 0;
    int lay_tick = -1;    // our LAY order price (rests in the atb queue); -1 = none
    double lay_size = 0;  // backer's stake
};

struct Position {
    double win_pl = 0, lose_pl = 0;   // P&L if this runner wins / loses
    double backed = 0, laid = 0;      // matched backer stakes
};

using Positions = std::map<int64_t, Position>;

class MakerStrategy {
public:
    virtual ~MakerStrategy() = default;
    // After each update while the market is OPEN and pre-off. Runners absent from the map get no quotes.
    virtual std::map<int64_t, Quote> quotes(const MarketBook& book, int64_t pt, const Positions& pos) = 0;
};

enum class CancelModel { ProRata, Behind };

struct SimParams {
    int64_t latency_ms = 150;
    double traded_scale = 1.0;      // set 0.5 to test if EX_TRADED counts both sides of each match
    CancelModel cancel = CancelModel::ProRata;
    double commission = 0.02;
};

struct Fill {
    int64_t sel;
    bool back;
    int tick;
    double size;
    int64_t pt;
};

struct MarketResult {
    std::string market_id, venue;
    int orders = 0, fills = 0;
    double matched = 0;          // backer stakes matched
    double edge_vs_bsp = 0;      // sum of fill EV at BSP-implied probabilities (before commission)
    double pnl_result = 0;       // settled on the result, after commission
    double pnl_hedged = 0;       // exposure closed at BSP, after commission
    bool settled = false;
};

class MakerSim {
public:
    MakerSim(std::string market_id, MakerStrategy& s, SimParams p) : strat_(s), p_(p) { book_.market_id = std::move(market_id); }

    // Feed one "mc" entry for this market with its message publish time.
    void on_change(json::Value mc, int64_t pt) {
        if (done_) return;
        snapshot_sizes();
        apply_change(book_, mc, deltas_);
        if (book_.status == "CLOSED") { settle(); return; }
        if (book_.in_play) { orders_.clear(); return; }   // persistence LAPSE: unmatched orders cancelled at the off
        match(pt);
        if (book_.status == "OPEN" && book_.has_definition) requote(pt);
    }

    const MarketBook& book() const { return book_; }
    const Positions& positions() const { return pos_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const MarketResult& result() const { return res_; }
    bool done() const { return done_; }

    struct Resting {
        int64_t sel;
        bool back;
        int tick;
        double remaining;
        double queue_ahead;
        int64_t active_pt;
        double prev_shown = 0;   // displayed size at our price before the current update
    };
    const std::vector<Resting>& resting() const { return orders_; }

private:
    void snapshot_sizes() {
        for (auto& o : orders_) {
            const auto* r = book_.find(o.sel);
            o.prev_shown = r ? r->size_at(!o.back, o.tick) : 0.0;   // our back rests in atl, lay in atb
        }
    }

    const RunnerDelta* delta_for(int64_t sel) const {
        for (const auto& d : deltas_) if (d.id == sel) return &d;
        return nullptr;
    }

    void match(int64_t pt) {
        for (auto& o : orders_) {
            if (o.remaining <= 0) continue;
            const auto* r = book_.find(o.sel);
            if (!r) continue;
            const auto* d = delta_for(o.sel);
            if (pt < o.active_pt) {               // not live yet: the queue still moves ahead of us
                if (d) if (auto it = d->traded_delta.find(o.tick); it != d->traded_delta.end())
                    o.queue_ahead = std::max(0.0, o.queue_ahead - it->second * p_.traded_scale);
                o.queue_ahead = std::min(o.queue_ahead, r->size_at(!o.back, o.tick));
                continue;
            }
            double filled = 0;
            bool through = false;
            if (d) {
                for (const auto& [t, v] : d->traded_delta) {
                    if (t == o.tick) {
                        double vol = v * p_.traded_scale;
                        double used = std::min(o.queue_ahead, vol);
                        o.queue_ahead -= used;
                        filled += vol - used;
                    } else if (o.back ? t > o.tick : t < o.tick) {
                        through = true;
                    }
                }
            }
            // Opposite best crossing our price: the market moved through us.
            if (o.back) { int bb = r->best_back(); if (bb >= 0 && bb >= o.tick) through = true; }
            else        { int bl = r->best_lay();  if (bl >= 0 && bl <= o.tick) through = true; }
            if (through) filled = o.remaining;
            filled = std::min(filled, o.remaining);
            if (filled > 1e-9) record_fill(o, filled, pt);

            // Cancellations at our price that were not trades.
            double shown = r->size_at(!o.back, o.tick);
            double traded_here = 0;
            if (d) if (auto it = d->traded_delta.find(o.tick); it != d->traded_delta.end()) traded_here = it->second * p_.traded_scale;
            double expected = std::max(0.0, o.prev_shown - traded_here);
            if (shown < expected - 1e-9 && expected > 0) {
                if (p_.cancel == CancelModel::ProRata) o.queue_ahead *= shown / expected;
            }
            o.queue_ahead = std::min(o.queue_ahead, shown);   // can never have more ahead than is shown
        }
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(), [](const Resting& o) { return o.remaining <= 1e-9; }),
                      orders_.end());
    }

    void record_fill(Resting& o, double size, int64_t pt) {
        o.remaining -= size;
        double price = price_of(o.tick);
        auto& ps = pos_[o.sel];
        if (o.back) { ps.win_pl += size * (price - 1); ps.lose_pl -= size; ps.backed += size; }
        else        { ps.win_pl -= size * (price - 1); ps.lose_pl += size; ps.laid += size; }
        fills_.push_back({o.sel, o.back, o.tick, size, pt});
        res_.fills++;
        res_.matched += size;
    }

    void requote(int64_t pt) {
        auto want = strat_.quotes(book_, pt, pos_);
        // Cancel orders whose price is no longer wanted (or whose runner is gone).
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(), [&](const Resting& o) {
                          auto it = want.find(o.sel);
                          if (it == want.end()) return true;
                          int t = o.back ? it->second.back_tick : it->second.lay_tick;
                          return t != o.tick;
                      }),
                      orders_.end());
        for (const auto& [sel, q] : want) {
            const auto* r = book_.find(sel);
            if (!r || r->status != "ACTIVE") continue;
            auto place = [&](bool back, int tick, double size) {
                if (tick < 0 || size <= 0 || !valid_tick(tick)) return;
                for (const auto& o : orders_) if (o.sel == sel && o.back == back) return;   // already resting at this price
                if (back) { int bb = r->best_back(); if (bb >= 0 && tick <= bb) return; }    // would cross: post-only
                else      { int bl = r->best_lay();  if (bl >= 0 && tick >= bl) return; }
                for (const auto& o : orders_)                                                  // nor cross our own order
                    if (o.sel == sel && o.back != back && (back ? tick <= o.tick : tick >= o.tick)) return;
                orders_.push_back({sel, back, tick, size, r->size_at(!back, tick), pt + p_.latency_ms, 0});
                res_.orders++;
            };
            place(true, q.back_tick, q.back_size);
            place(false, q.lay_tick, q.lay_size);
        }
    }

    static double net_after_commission(double net, double c) { return net > 0 ? net * (1 - c) : net; }

    void settle() {
        done_ = true;
        orders_.clear();
        res_.market_id = book_.market_id;
        res_.venue = book_.venue;
        double result = 0, hedged = 0;
        bool ok = true;
        for (const auto& [sel, ps] : pos_) {
            const auto* r = book_.find(sel);
            if (!r || (r->status != "WINNER" && r->status != "LOSER") || r->bsp <= 1.0) { ok = false; continue; }
            result += r->winner ? ps.win_pl : ps.lose_pl;
            double s = (ps.lose_pl - ps.win_pl) / r->bsp;   // back s at BSP (lay if negative) equalises the outcomes
            hedged += ps.lose_pl - s;
        }
        for (const auto& f : fills_) {
            const auto* r = book_.find(f.sel);
            if (!r || r->bsp <= 1.0) continue;
            double ratio = price_of(f.tick) / r->bsp;
            res_.edge_vs_bsp += f.size * (f.back ? ratio - 1 : 1 - ratio);
        }
        res_.pnl_result = net_after_commission(result, p_.commission);
        res_.pnl_hedged = net_after_commission(hedged, p_.commission);
        res_.settled = ok;
    }

    MakerStrategy& strat_;
    SimParams p_;
    MarketBook book_;
    std::vector<RunnerDelta> deltas_;
    std::vector<Resting> orders_;
    std::vector<Fill> fills_;
    Positions pos_;
    MarketResult res_;
    bool done_ = false;
};

}  // namespace bfr::mm
