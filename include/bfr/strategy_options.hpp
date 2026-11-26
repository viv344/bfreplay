// Strategy selection and command-line options, shared by bfreplay and bflive so that a
// strategy runs with identical parameters in backtest and live.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include "strategy.hpp"
#ifdef BFR_HAS_PRIVATE
#include "private_hooks.hpp"   // optional, untracked
#endif

namespace bfr {

struct StrategyOptions {
    std::string name = "lay-favourite";
    LayFavouriteParams fav;
    bool calibrate = false;

    // Consume a strategy option. Returns false if `k` is not a strategy option.
    bool parse(const std::string& k, const std::function<std::string()>& val) {
        if (k == "--strategy") name = val();
        else if (k == "--lo") fav.band_lo = std::stod(val());
        else if (k == "--hi") fav.band_hi = std::stod(val());
        else if (k == "--liability") fav.liability = std::stod(val());
#ifdef BFR_HAS_PRIVATE
        else if (priv::parse_option(k, val)) calibrate = priv::calibrate();
#endif
        else return false;
        return true;
    }

    std::unique_ptr<Strategy> make() const {
        if (name == "lay-favourite") return std::make_unique<LayFavourite>(fav);
#ifdef BFR_HAS_PRIVATE
        if (auto s = priv::make(name)) return s;
#endif
        std::fprintf(stderr, "unknown strategy %s\n", name.c_str());
        std::exit(2);
    }
};

}  // namespace bfr
