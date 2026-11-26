// Parser micro-benchmark: decompress N market files once, then parse every line repeatedly.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "bfr/market_file.hpp"
#include "bfr/json.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: bench_json ROOT [nfiles]\n"); return 2; }
    size_t nfiles = argc > 2 ? std::stoul(argv[2]) : 2000;
    std::vector<std::string> lines;
    size_t bytes = 0;
    bfr::MarketFile f;
    for (const auto& e : std::filesystem::recursive_directory_iterator(argv[1])) {
        auto name = e.path().filename().string();
        if (!e.is_regular_file() || name.rfind("1.", 0) != 0) continue;
        if (!f.load(e.path().string())) continue;
        for (auto l : f.lines()) { lines.emplace_back(l); bytes += l.size(); }
        if (--nfiles == 0) break;
    }
    bfr::json::Document d;
    size_t ok = 0, nodes = 0;
    const int reps = 5;
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
        for (auto& l : lines) { ok += d.parse(l); nodes += d.size(); }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu lines, %.1f MB x %d reps: %.3fs -> %.0f MB/s, %.2fM lines/s, %.0f ns/line (single thread), ok=%zu\n",
                lines.size(), bytes / 1e6, reps, s, bytes * reps / 1e6 / s, lines.size() * reps / s / 1e6,
                s * 1e9 / (lines.size() * reps), ok);
}
