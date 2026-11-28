// Betfair price ladder ("CLASSIC"): 350 valid prices from 1.01 to 1000. Prices are handled as
// tick indices so that quoting, queue matching and "one tick better" are exact integer maths.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace bfr::mm {

namespace detail {
struct Band { double lo, hi, step; };
inline constexpr std::array<Band, 10> kBands{{{1.01, 2.0, 0.01}, {2.0, 3.0, 0.02}, {3.0, 4.0, 0.05}, {4.0, 6.0, 0.1},
                                             {6.0, 10.0, 0.2}, {10.0, 20.0, 0.5}, {20.0, 30.0, 1.0}, {30.0, 50.0, 2.0},
                                             {50.0, 100.0, 5.0}, {100.0, 1000.0, 10.0}}};

inline const std::array<double, 350>& ladder() {
    static const std::array<double, 350> l = [] {
        std::array<double, 350> a{};
        int i = 0;
        a[i++] = 1.01;
        for (const auto& b : kBands) {
            int n = static_cast<int>(std::lround((b.hi - b.lo) / b.step));
            for (int k = 1; k <= n; ++k) a[i++] = std::round((b.lo + k * b.step) * 100.0) / 100.0;
        }
        return a;
    }();
    return l;
}
}  // namespace detail

inline constexpr int kNumTicks = 350;

inline double price_of(int tick) { return detail::ladder()[static_cast<size_t>(tick)]; }

// Tick index of a valid ladder price; -1 if `price` is not on the ladder (within 1e-6).
inline int tick_of(double price) {
    const auto& l = detail::ladder();
    int lo = 0, hi = kNumTicks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (std::fabs(l[mid] - price) < 1e-6) return mid;
        if (l[mid] < price) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

inline bool valid_tick(int t) { return t >= 0 && t < kNumTicks; }

}  // namespace bfr::mm
