// Market state as seen by a strategy: runner statuses from the latest market definition
// and every last-traded-price update received so far.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bfr {

struct PricePoint {
    int64_t pt;     // publish time, ms since epoch
    double ltp;
};

struct Runner {
    int64_t id = 0;
    std::string status;                // ACTIVE / REMOVED / WINNER / LOSER ...
    std::vector<PricePoint> history;   // in arrival order

    std::optional<double> ltp() const {
        if (history.empty()) return std::nullopt;
        return history.back().ltp;
    }
    // Latest price at or before `pt` (history is time-ordered).
    std::optional<double> ltp_at(int64_t pt) const {
        std::optional<double> last;
        for (const auto& p : history) {
            if (p.pt > pt) break;
            last = p.ltp;
        }
        return last;
    }
};

struct MarketView {
    std::string market_id;
    bool has_definition = false;
    std::string status;                // OPEN / SUSPENDED / CLOSED
    bool in_play = false;
    std::vector<Runner> runners;       // order = market definition order

    Runner* find(int64_t id) {
        for (auto& r : runners) if (r.id == id) return &r;
        return nullptr;
    }
    Runner& get_or_add(int64_t id) {
        if (Runner* r = find(id)) return *r;
        runners.push_back(Runner{id, "", {}});
        return runners.back();
    }
};

}  // namespace bfr
