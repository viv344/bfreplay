// Order book per runner, maintained from Stream API market changes with the fields
// EX_ALL_OFFERS (atb/atl), EX_TRADED (trd), EX_TRADED_VOL (tv), EX_LTP (ltp), SP_PROJECTED (spn/spf).
//
// Naming follows Betfair: atb = prices you can BACK at (resting lay orders from others; best = highest),
// atl = prices you can LAY at (resting back orders; best = lowest). Normally best_back < best_lay.
// trd holds cumulative matched volume per price (absolute values, updated as deltas).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "bfr/json.hpp"
#include "bfr/mm/ticks.hpp"

namespace bfr::mm {

struct RunnerBook {
    int64_t id = 0;
    std::string status = "ACTIVE";
    std::map<int, double> atb;         // tick -> size available to back
    std::map<int, double> atl;         // tick -> size available to lay
    std::map<int, double> traded;      // tick -> cumulative matched
    double ltp = 0, tv = 0, spn = 0, spf = 0;
    double bsp = 0;                    // from the closing definition
    double adjustment_factor = 0;      // reduction factor (%) from the market definition
    bool winner = false;
    bool trd_init = false;             // first trd seen: later increases are new matches

    int best_back() const { return atb.empty() ? -1 : atb.rbegin()->first; }   // highest price to back at
    int best_lay() const { return atl.empty() ? -1 : atl.begin()->first; }     // lowest price to lay at
    double size_at(bool atb_side, int tick) const {
        const auto& m = atb_side ? atb : atl;
        auto it = m.find(tick);
        return it == m.end() ? 0.0 : it->second;
    }
    double traded_at(int tick) const {
        auto it = traded.find(tick);
        return it == traded.end() ? 0.0 : it->second;
    }
};

// What changed for one runner in one update (used by the fill simulator).
struct RunnerDelta {
    int64_t id = 0;
    std::map<int, double> traded_delta;   // tick -> newly matched volume
    bool ltp_changed = false;
};

struct MarketBook {
    std::string market_id;
    std::string status = "OPEN", venue;
    bool in_play = false, has_definition = false;
    int64_t market_time_ms = 0;
    std::vector<RunnerBook> runners;

    RunnerBook* find(int64_t id) {
        for (auto& r : runners) if (r.id == id) return &r;
        return nullptr;
    }
    const RunnerBook* find(int64_t id) const {
        for (const auto& r : runners) if (r.id == id) return &r;
        return nullptr;
    }
    RunnerBook& get(int64_t id) {
        if (auto* r = find(id)) return *r;
        runners.push_back(RunnerBook{});
        runners.back().id = id;
        return runners.back();
    }
};

namespace detail {
inline void apply_ladder(std::map<int, double>& side, json::Value arr) {
    for (auto pair : arr) {
        auto it = pair.begin();
        if (!(it != pair.end())) continue;
        double price = (*it).number(0);
        ++it;
        if (!(it != pair.end())) continue;
        double size = (*it).number(0);
        int t = tick_of(price);
        if (t < 0) continue;
        if (size <= 0) side.erase(t); else side[t] = size;
    }
}
}  // namespace detail

// "2026-11-26T16:30:00.000Z" -> epoch ms. (Same as engine.hpp's parse_iso_ms; duplicated to keep mm/ standalone.)
inline int64_t iso_ms(std::string_view s);

// Apply one "mc" entry. Fills `deltas` with per-runner matched-volume changes.
inline void apply_change(MarketBook& b, json::Value mc, std::vector<RunnerDelta>& deltas) {
    deltas.clear();
    const bool img = mc["img"].boolean();   // full image: volumes are not new trades
    if (img)
        for (auto& r : b.runners) { r.atb.clear(); r.atl.clear(); r.traded.clear(); }
    if (auto md = mc["marketDefinition"]; md && !md.is_null()) {
        b.has_definition = true;
        b.status = std::string(md["status"].str());
        b.in_play = md["inPlay"].boolean();
        if (auto v = md["venue"]; v && !v.is_null()) b.venue = std::string(v.str());
        if (auto t = md["marketTime"]; t && !t.is_null()) b.market_time_ms = iso_ms(t.str());
        for (auto r : md["runners"]) {
            auto& rb = b.get(static_cast<int64_t>(r["id"].number(0)));
            rb.status = std::string(r["status"].str());
            rb.winner = rb.status == "WINNER";
            if (auto bsp = r["bsp"]; bsp && !bsp.is_null()) rb.bsp = bsp.number(0);
            if (auto af = r["adjustmentFactor"]; af && !af.is_null()) rb.adjustment_factor = af.number(0);
        }
    }
    for (auto rc : mc["rc"]) {
        auto& rb = b.get(static_cast<int64_t>(rc["id"].number(0)));
        RunnerDelta d;
        d.id = rb.id;
        if (auto v = rc["atb"]; v && !v.is_null()) detail::apply_ladder(rb.atb, v);
        if (auto v = rc["atl"]; v && !v.is_null()) detail::apply_ladder(rb.atl, v);
        if (auto v = rc["trd"]; v && !v.is_null()) {
            for (auto pair : v) {
                auto it = pair.begin();
                double price = (*it).number(0);
                ++it;
                double vol = (*it).number(0);
                int t = tick_of(price);
                if (t < 0) continue;
                double prev = rb.traded_at(t);
                if (!img && rb.trd_init && vol > prev + 1e-9) d.traded_delta[t] = vol - prev;
                if (vol <= 0) rb.traded.erase(t); else rb.traded[t] = vol;
            }
            rb.trd_init = true;
        }
        if (auto v = rc["ltp"]; v && !v.is_null()) { d.ltp_changed = std::fabs(v.number(0) - rb.ltp) > 1e-9; rb.ltp = v.number(0); }
        if (auto v = rc["tv"]; v && !v.is_null()) rb.tv = v.number(0);
        if (auto v = rc["spn"]; v && !v.is_null()) rb.spn = v.number(0);
        if (auto v = rc["spf"]; v && !v.is_null()) rb.spf = v.number(0);
        deltas.push_back(std::move(d));
    }
}

inline int64_t iso_ms(std::string_view s) {
    std::tm tm{};
    int ms = 0;
    std::string str(s);
    if (std::sscanf(str.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec, &ms) < 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<int64_t>(timegm(&tm)) * 1000 + ms;
}

}  // namespace bfr::mm
