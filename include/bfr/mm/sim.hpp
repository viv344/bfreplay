// Queue-aware maker simulator for pre-off Betfair markets.
//
// A MakerStrategy says which passive quotes it wants per runner after each update. The simulator
// keeps those as resting limit orders and fills them from the recorded stream:
//   * Queue: an order joins the back of the displayed queue AS IT IS WHEN THE ORDER ARRIVES at the
//     exchange (placement + place latency), not as it was when we decided.
//   * Trades at our price (EX_TRADED deltas x traded_scale) consume the queue ahead, then us.
//     Betfair counts traded volume as backer's stake x 2, so traded_scale defaults to 0.5.
//   * Cancellations: displayed size that disappears without trading shrinks queue_ahead pro-rata,
//     only from behind (Behind), or with a power-law probability of being ahead (Power).
//   * Trade-through: a trade at a worse price than ours, or the opposite best crossing our price,
//     fills us completely.
//   * Latency: an order can be filled only from active_pt = placement + place_latency_ms, and a
//     cancelled order stays fillable until cancel_pt = request + cancel_latency_ms.
//   * No fills while the market is not OPEN; on a runner removal with reduction factor >= 2.5%
//     unmatched orders are cancelled and matched prices on other runners are reduced.
//   * Post-only is a local check only: Betfair has no post-only flag, so a quote that crosses on
//     arrival is matched as a taker (captured by the trade-through rule at our price).
//   * Minimum stake and the hourly transaction count are tracked.
// At the off (BSP reconciliation, Betfair SP rules): an order still resting with persistence LAPSE is
// matched at its own price only if that price is worse than BSP for us (lay P >= BSP, back P <= BSP),
// otherwise it lapses. TAKE_SP orders become SP bets at BSP (lays keep their liability and lapse
// below the SP minimum; backs keep their stake). Orders still in flight at the off are not included.
// At settlement the result P&L, a hedged-at-BSP P&L and the edge
// of every fill relative to BSP are reported. Commission is charged on net market winnings.
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
    bool back_take_sp = false;   // persistence: false = LAPSE, true = TAKE_SP (MARKET_ON_CLOSE)
    bool lay_take_sp = false;
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
    // NOTE (live parity): in the live stream our own orders ARE inside atb/atl; in a recording they are not.
    // A live strategy must subtract its own resting size from the book before calling the same logic.
    virtual std::map<int64_t, Quote> quotes(const MarketBook& book, int64_t pt, const Positions& pos) = 0;
};

enum class CancelModel { ProRata, Behind, Power };

struct SimParams {
    int64_t latency_ms = 150;          // place latency: decision -> order resting at the exchange
    int64_t cancel_latency_ms = 150;   // decision -> cancel effective
    double traded_scale = 0.5;         // trd/tv = backer's stake x 2 (Betfair PRO data docs)
    CancelModel cancel = CancelModel::ProRata;
    double power_n = 2.0;              // Power model exponent (hftbacktest-style)
    double commission = 0.02;
    double min_stake = 1.0;            // GBP minimum backer's stake
    double min_reduction_factor = 2.5; // win market threshold for NR reductions/cancellations
    bool reconcile_at_off = true;      // false: every resting order simply lapses at the off (old behaviour)
    double min_sp_lay_liability = 10;  // a TAKE_SP lay remainder below this lapses
    double min_sp_back_stake = 2;      // a TAKE_SP back remainder below this lapses
};

struct Fill {
    int64_t sel;
    bool back;
    int tick;
    double size;
    int64_t pt;
    double price = 0;   // matched price after any reduction factors
    bool at_off = false;  // matched in the BSP reconciliation
};

struct MarketResult {
    std::string market_id, venue;
    int orders = 0, fills = 0;
    int cancels = 0;
    int max_tx_hour = 0;         // max place+cancel instructions in any clock hour (this market)
    int rejected_min_stake = 0;
    int recon_fills = 0;         // fills in the BSP reconciliation
    double recon_matched = 0;    // backer stakes matched in the reconciliation
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
        std::map<int64_t, std::string> prev_status;
        for (const auto& r : book_.runners) prev_status[r.id] = r.status;
        apply_change(book_, mc, deltas_);
        if (book_.status == "CLOSED") { settle(); return; }
        if (book_.in_play) {                                // the off: keep what stood in the book for reconciliation
            if (!off_seen_) {
                off_seen_ = true;
                for (const auto& o : orders_)
                    if (o.remaining > 1e-9 && pt >= o.active_pt &&                      // in flight: never arrived
                        (o.cancel_pt < 0 || o.cancel_pt > pt))                             // cancel already landed
                        at_off_.push_back(o);
            }
            orders_.clear();
            return;
        }
        handle_removals(prev_status, pt);
        expire_cancels(pt);
        if (book_.status == "OPEN") match(pt);              // nothing trades while suspended
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
        double prev_shown = 0;   // displayed size at our price (our side) before the current update
        bool arrived = false;    // queue re-snapshotted at arrival
        int64_t cancel_pt = -1;  // >= 0: cancel requested, effective at this time
        bool take_sp = false;    // persistence TAKE_SP (else LAPSE)
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

    void count_tx(int64_t pt) {
        int64_t hour = pt / 3'600'000;
        int n = ++tx_by_hour_[hour];
        res_.max_tx_hour = std::max(res_.max_tx_hour, n);
    }

    // Cancels take effect only after cancel latency; until then the order can still fill.
    void expire_cancels(int64_t pt) {
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                     [&](const Resting& o) { return o.cancel_pt >= 0 && pt >= o.cancel_pt; }),
                      orders_.end());
    }

    // Runner removed (ACTIVE -> REMOVED). Betfair horse racing rules: with a reduction factor
    // >= 2.5% (win market) matched bets on remaining runners are price-reduced and unmatched bets are
    // cancelled (the rules name lay offers; we cancel all unmatched, the conservative reading).
    // Matched bets on the removed runner are void.
    void handle_removals(const std::map<int64_t, std::string>& prev_status, int64_t pt) {
        for (const auto& r : book_.runners) {
            auto it = prev_status.find(r.id);
            if (it == prev_status.end() || it->second != "ACTIVE" || r.status != "REMOVED") continue;
            const double rf = r.adjustment_factor;
            std::erase_if(fills_, [&](const Fill& f) { return f.sel == r.id; });   // void
            if (rf >= p_.min_reduction_factor) {
                for (auto& f : fills_) if (f.pt <= pt) f.price = std::max(1.01, f.price * (1 - rf / 100));
                orders_.clear();
            } else {
                std::erase_if(orders_, [&](const Resting& o) { return o.sel == r.id; });
            }
            rebuild_positions();
        }
    }

    void rebuild_positions() {
        pos_.clear();
        for (const auto& f : fills_) apply_fill(f);
    }

    void apply_fill(const Fill& f) {
        auto& ps = pos_[f.sel];
        if (f.back) { ps.win_pl += f.size * (f.price - 1); ps.lose_pl -= f.size; ps.backed += f.size; }
        else        { ps.win_pl -= f.size * (f.price - 1); ps.lose_pl += f.size; ps.laid += f.size; }
    }

    void match(int64_t pt) {
        for (auto& o : orders_) {
            if (o.remaining <= 0) continue;
            const auto* r = book_.find(o.sel);
            if (!r) continue;
            const auto* d = delta_for(o.sel);
            if (pt < o.active_pt) continue;   // not live yet; the queue is snapshotted on arrival below
            if (!o.arrived) {                 // join the back of the queue as it stood on arrival
                o.queue_ahead = o.prev_shown;
                o.arrived = true;
            }
            double filled = 0;
            bool through = false;
            double traded_here = 0;
            if (d) {
                for (const auto& [t, v] : d->traded_delta) {
                    if (t == o.tick) {
                        double vol = v * p_.traded_scale;
                        traded_here = vol;
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
            double expected = std::max(0.0, o.prev_shown - traded_here);
            if (shown < expected - 1e-9 && expected > 0) {
                const double gone = expected - shown;
                const double ahead = o.queue_ahead, behind = std::max(0.0, expected - ahead);
                switch (p_.cancel) {
                    case CancelModel::ProRata: o.queue_ahead *= shown / expected; break;
                    case CancelModel::Behind:  o.queue_ahead -= std::max(0.0, gone - behind); break;   // only once behind is empty
                    case CancelModel::Power: {   // P(cancel ahead) = f(ahead)/(f(ahead)+f(behind)), f(x)=x^n
                        double fa = std::pow(ahead, p_.power_n), fb = std::pow(behind, p_.power_n);
                        double share = fa + fb > 0 ? fa / (fa + fb) : 0.0;
                        o.queue_ahead -= std::min(ahead, gone * share);
                        break;
                    }
                }
                o.queue_ahead = std::max(0.0, o.queue_ahead);
            }
            o.queue_ahead = std::min(o.queue_ahead, shown);   // can never have more ahead than is shown
        }
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(), [](const Resting& o) { return o.remaining <= 1e-9; }),
                      orders_.end());
    }

    void record_fill(Resting& o, double size, int64_t pt) {
        o.remaining -= size;
        Fill f{o.sel, o.back, o.tick, size, pt, price_of(o.tick)};
        apply_fill(f);
        fills_.push_back(f);
        res_.fills++;
        res_.matched += size;
    }

    void requote(int64_t pt) {
        auto want = strat_.quotes(book_, pt, pos_);
        // Request cancels for orders whose price is no longer wanted; they stay fillable until cancel_pt.
        for (auto& o : orders_) {
            if (o.cancel_pt >= 0) continue;
            auto it = want.find(o.sel);
            int t = it == want.end() ? -1 : (o.back ? it->second.back_tick : it->second.lay_tick);
            if (t != o.tick) { o.cancel_pt = pt + p_.cancel_latency_ms; res_.cancels++; count_tx(pt); continue; }
            bool want_sp = o.back ? it->second.back_take_sp : it->second.lay_take_sp;
            if (want_sp != o.take_sp) { o.take_sp = want_sp; count_tx(pt); }   // updateOrders: keeps queue place
        }
        for (const auto& [sel, q] : want) {
            const auto* r = book_.find(sel);
            if (!r || r->status != "ACTIVE") continue;
            auto place = [&](bool back, int tick, double size, bool take_sp) {
                if (tick < 0 || size <= 0 || !valid_tick(tick)) return;
                if (size < p_.min_stake) { res_.rejected_min_stake++; return; }   // INVALID_BET_SIZE
                for (const auto& o : orders_)                                      // already resting (not cancelling)
                    if (o.sel == sel && o.back == back && o.cancel_pt < 0) return;
                if (back) { int bb = r->best_back(); if (bb >= 0 && tick <= bb) return; }    // would cross: local post-only
                else      { int bl = r->best_lay();  if (bl >= 0 && tick >= bl) return; }
                for (const auto& o : orders_)                                      // nor cross our own (incl. cancelling)
                    if (o.sel == sel && o.back != back && (back ? tick <= o.tick : tick >= o.tick)) return;
                orders_.push_back({sel, back, tick, size, r->size_at(!back, tick), pt + p_.latency_ms, 0, false, -1, take_sp});
                res_.orders++;
                count_tx(pt);
            };
            place(true, q.back_tick, q.back_size, q.back_take_sp);
            place(false, q.lay_tick, q.lay_size, q.lay_take_sp);
        }
    }

    static double net_after_commission(double net, double c) { return net > 0 ? net * (1 - c) : net; }

    // BSP reconciliation of the orders that stood in the book at the off (BSP known from the closing definition).
    void reconcile() {
        if (!p_.reconcile_at_off) return;
        for (const auto& o : at_off_) {
            const auto* r = book_.find(o.sel);
            if (!r || r->bsp <= 1.0 || r->status == "REMOVED") continue;
            const double bsp = r->bsp, price = price_of(o.tick);
            double stake = 0, matched_price = 0;
            if (o.take_sp) {
                matched_price = bsp;
                if (o.back) stake = o.remaining >= p_.min_sp_back_stake ? o.remaining : 0;
                else {
                    double liability = o.remaining * (price - 1);
                    stake = liability >= p_.min_sp_lay_liability ? liability / (bsp - 1) : 0;
                }
            } else if (o.back ? price <= bsp : price >= bsp) {   // LAPSE: included only when worse than SP for us
                matched_price = price;
                stake = o.remaining;
            }
            if (stake <= 1e-9) continue;
            Fill f{o.sel, o.back, o.tick, stake, book_.market_time_ms, matched_price, true};
            apply_fill(f);
            fills_.push_back(f);
            res_.fills++;
            res_.matched += stake;
            res_.recon_fills++;
            res_.recon_matched += stake;
        }
        at_off_.clear();
    }

    void settle() {
        done_ = true;
        orders_.clear();
        reconcile();
        res_.market_id = book_.market_id;
        res_.venue = book_.venue;
        double result = 0, hedged = 0;
        bool ok = true;
        for (const auto& [sel, ps] : pos_) {
            const auto* r = book_.find(sel);
            if (r && r->status == "REMOVED") continue;   // void (fills already dropped)
            if (!r || (r->status != "WINNER" && r->status != "LOSER") || r->bsp <= 1.0) { ok = false; continue; }
            result += r->winner ? ps.win_pl : ps.lose_pl;
            double s = (ps.lose_pl - ps.win_pl) / r->bsp;   // back s at BSP (lay if negative) equalises the outcomes
            hedged += ps.lose_pl - s;
        }
        for (const auto& f : fills_) {
            const auto* r = book_.find(f.sel);
            if (!r || r->bsp <= 1.0) continue;
            double ratio = f.price / r->bsp;
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
    std::map<int64_t, int> tx_by_hour_;
    std::vector<Resting> at_off_;
    bool off_seen_ = false;
    bool done_ = false;
};

}  // namespace bfr::mm
