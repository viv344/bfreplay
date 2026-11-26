// bfreplay: replay Betfair historic stream files through a strategy.
//
//   bfreplay ROOT [--strategy lay-favourite] [--lo L] [--hi H] [--liability £]
//                 [--threads N] [--out bets.csv]
// Private builds (with ../private/ present) add further strategies and options.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include "bfr/sim.hpp"
#include "bfr/strategy_options.hpp"

namespace fs = std::filesystem;
using namespace bfr;

namespace {
constexpr int64_t kYear2StartMs = 1761955200000;   // 2025-11-01T00:00:00Z

struct Args {
    std::string root, out = "bets.csv";
    StrategyOptions strat;
    unsigned threads = std::thread::hardware_concurrency();
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) { std::fprintf(stderr, "usage: bfreplay ROOT [options]\n"); std::exit(2); }
    a.root = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
        if (a.strat.parse(k, val)) continue;
        if (k == "--threads") a.threads = std::stoul(val());
        else if (k == "--out") a.out = val();
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); std::exit(2); }
    }
    return a;
}

double quantile(std::vector<double> v, double q) {
    if (v.empty()) return NAN;
    size_t k = static_cast<size_t>(q * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}
}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    // Index cache: after the first full scan, remember which files are GB/IE WIN markets so
    // later runs (parameter sweeps) open ~26k files instead of ~644k.
    const fs::path index_path = fs::path(a.root) / ".bfr_win_index";
    std::vector<std::string> files;
    bool from_index = false;
    if (std::ifstream in(index_path); in) {
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) files.push_back((fs::path(a.root) / line).string());   // stored relative to ROOT
        from_index = !files.empty();
    }
    if (!from_index) {
        for (const auto& e : fs::recursive_directory_iterator(a.root)) {
            auto name = e.path().filename().string();
            if (e.is_regular_file() && name.rfind("1.", 0) == 0 && (name.ends_with(".bz2") || name.ends_with(".zst"))) files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    std::printf("%zu candidate files (%s)\n", files.size(), from_index ? "from index cache" : "full scan");

    auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    std::mutex mu;
    std::vector<MarketResult> results;
    ReplayStats total;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < a.threads; ++t) {
        pool.emplace_back([&] {
            std::vector<MarketResult> local;
            ReplayStats st;
            MarketResult r;
            for (size_t i; (i = next.fetch_add(1)) < files.size();) {
                auto strat = a.strat.make();
                if (replay_file(files[i], *strat, r, st)) { r.path = files[i]; local.push_back(std::move(r)); }
            }
            std::lock_guard lk(mu);
            for (auto& m : local) results.push_back(std::move(m));
            total.files_read += st.files_read; total.messages += st.messages;
            total.bytes += st.bytes; total.bad_lines += st.bad_lines;
        });
    }
    for (auto& th : pool) th.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::sort(results.begin(), results.end(), [](auto& x, auto& y) { return x.market_id < y.market_id; });

    if (!from_index) {
        std::ofstream idx(index_path);
        for (auto& m : results) idx << fs::relative(m.path, a.root).string() << '\n';
    }
    size_t msgs = 0, bytes = 0;
    for (auto& m : results) { msgs += m.messages; bytes += m.bytes; }
    std::printf("%zu files scanned, %zu GB/IE win markets replayed in %.2fs on %u threads\n",
                files.size(), results.size(), secs, a.threads);
    std::printf("replayed %zu messages (%.1f MB decompressed): %.2fM msg/s, %.0f MB/s, %zu bad lines\n",
                msgs, bytes / 1e6, msgs / secs / 1e6, bytes / 1e6 / secs, total.bad_lines);

    if (a.strat.calibrate) {
        std::vector<double> y1;
        for (auto& m : results) if (m.off < kYear2StartMs) for (auto& b : m.bets) y1.push_back(b.signal);
        std::printf("first-period runners in band at last decision: %zu\n", y1.size());
        for (double q : {0.025, 0.05, 0.10}) std::printf("  bottom %.1f%%: signal <= %+.4f\n", q * 100, quantile(y1, q));
        return 0;
    }

    std::ofstream out(a.out);
    out << "market_id,off,venue,country,selection_id,bsp,win,liability,stake,gross,signal,price_at_order,secs_before_off\n";
    out.precision(10);
    for (auto& m : results)
        for (auto& b : m.bets)
            out << m.market_id << ',' << m.off << ',' << '"' << m.venue << '"' << ',' << m.country << ','
                << b.selection_id << ',' << b.bsp << ',' << b.win << ',' << b.liability << ',' << b.stake << ','
                << b.gross << ',' << b.signal << ',' << b.price_at_order << ',' << b.secs_before_off << '\n';

    for (double c : {0.02, 0.06}) {
        std::printf("\ncommission %.0f%%\n", c * 100);
        for (int y = 0; y < 3; ++y) {
            double pnl = 0, liab = 0, stake = 0, wins = 0, exp = 0;
            size_t n = 0;
            for (auto& m : results) {
                bool is_y2 = m.off >= kYear2StartMs;
                if ((y == 0 && is_y2) || (y == 1 && !is_y2) || m.bets.empty()) continue;
                pnl += settle_market(m.bets, c);
                for (auto& b : m.bets) { liab += b.liability; stake += b.stake; wins += b.win; exp += 1 / b.bsp; ++n; }
            }
            std::printf("  %-4s bets %5zu  A/E %.3f  P&L £%7.0f  ROI/liability %+.2f%%  ROI/backer stake %+.2f%%\n",
                        y == 0 ? "y1" : y == 1 ? "y2" : "all", n, wins / exp, pnl, 100 * pnl / liab, 100 * pnl / stake);
        }
    }
    std::printf("\nbets written to %s\n", a.out.c_str());
}
