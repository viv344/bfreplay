// Example maker strategy: join (or improve by one tick) the best back/lay prices of runners with
// a wide enough spread, only when the quote is not worse than a fair value (Betfair's projected
// near BSP when available, else the mid), skewing away from accumulated exposure.
// A baseline for the simulator, not a tuned strategy.
#pragma once
#include <cmath>
#include "bfr/mm/sim.hpp"

namespace bfr::mm {

struct JoinTouchParams {
    double lo = 2.0, hi = 20.0;        // only runners whose mid price is in [lo, hi)
    int min_spread = 2;                // ticks between best back and best lay required to quote
    int improve_from = 3;              // spread at which we step one tick inside the touch
    double size = 2.0;                 // backer's stake per quote (Betfair minimum is £1 for most orders)
    double max_exposure = 20.0;        // stop adding to a side once |P&L if win| or |P&L if lose| exceeds this
    double min_edge = 0.0;             // require price/fair - 1 >= min_edge (back) or 1 - price/fair >= min_edge (lay)
    int start_min = 30, stop_sec = 15; // quoting window relative to the scheduled start
};

class JoinTouch final : public MakerStrategy {
public:
    explicit JoinTouch(JoinTouchParams p) : p_(p) {}

    std::map<int64_t, Quote> quotes(const MarketBook& b, int64_t pt, const Positions& pos) override {
        std::map<int64_t, Quote> out;
        if (b.market_time_ms <= 0 || pt < b.market_time_ms - p_.start_min * 60'000LL ||
            pt > b.market_time_ms - p_.stop_sec * 1000LL)
            return out;
        for (const auto& r : b.runners) {
            if (r.status != "ACTIVE") continue;
            int bb = r.best_back(), bl = r.best_lay();
            if (bb < 0 || bl < 0 || bl - bb < p_.min_spread) continue;
            double mid = std::sqrt(price_of(bb) * price_of(bl));
            if (mid < p_.lo || mid >= p_.hi) continue;
            double fair = (r.spn > 1.0 && std::isfinite(r.spn)) ? r.spn : mid;
            bool improve = bl - bb >= p_.improve_from;
            Quote q;
            Position ps;
            if (auto it = pos.find(r.id); it != pos.end()) ps = it->second;
            // Our back rests where layers take it: at the best lay price (or one tick lower).
            int back_t = improve ? bl - 1 : bl;
            int lay_t = improve ? bb + 1 : bb;
            if (price_of(back_t) / fair - 1 >= p_.min_edge && ps.lose_pl > -p_.max_exposure) {
                q.back_tick = back_t;
                q.back_size = p_.size;
            }
            if (1 - price_of(lay_t) / fair >= p_.min_edge && ps.win_pl > -p_.max_exposure) {
                q.lay_tick = lay_t;
                q.lay_size = p_.size;
            }
            if (q.back_tick >= 0 || q.lay_tick >= 0) out.emplace(r.id, q);
        }
        return out;
    }

private:
    JoinTouchParams p_;
};

}  // namespace bfr::mm
