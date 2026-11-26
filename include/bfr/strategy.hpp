// Strategy interface and an example strategy.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>
#include "market.hpp"

namespace bfr {

// A lay-at-BSP order. Betfair sizes lay-SP orders by liability.
struct Order {
    int64_t selection_id = 0;
    double liability = 0.0;
    int64_t placed_pt = 0;
    double signal = 0.0;
    double price_at_order = 0.0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    // Called after every stream message while the market is OPEN and pre-off.
    // Returns the orders the strategy wants resting now (keyed by selection id).
    virtual const std::map<int64_t, Order>& on_update(const MarketView& mv, int64_t pt) = 0;
};


// Example strategy shipped with the public engine: lay the current favourite at BSP when its
// last traded price is inside a band. It exists to demonstrate the Strategy interface and
// the simulator; it is not expected to be profitable.
struct LayFavouriteParams {
    double band_lo = 1.5, band_hi = 4.0;
    double liability = 20.0;
};

class LayFavourite final : public Strategy {
public:
    explicit LayFavourite(LayFavouriteParams p) : p_(p) {}

    const std::map<int64_t, Order>& on_update(const MarketView& mv, int64_t pt) override {
        const Runner* fav = nullptr;
        for (const auto& r : mv.runners) {
            if (r.status != "ACTIVE" || !r.ltp()) continue;
            if (!fav || *r.ltp() < *fav->ltp()) fav = &r;
        }
        std::map<int64_t, Order> next;
        if (fav && p_.band_lo <= *fav->ltp() && *fav->ltp() < p_.band_hi) {
            auto it = wanted_.find(fav->id);
            Order o = (it != wanted_.end()) ? it->second : Order{fav->id, p_.liability, pt, 0.0, 0.0};
            o.signal = *fav->ltp();
            o.price_at_order = *fav->ltp();
            next.emplace(fav->id, o);
        }
        wanted_.swap(next);
        return wanted_;
    }

private:
    LayFavouriteParams p_;
    std::map<int64_t, Order> wanted_;
};

}  // namespace bfr
