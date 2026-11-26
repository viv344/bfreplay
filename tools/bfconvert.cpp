// bfconvert: one-off conversion of the indexed GB/IE WIN market files from bzip2 to zstd.
// bzip2 decompression is ~80% of single-threaded replay time; zstd decodes far faster.
//
//   bfconvert SRC_ROOT DST_ROOT [level]
// SRC_ROOT must contain .bfr_win_index (written by a first bfreplay run). DST_ROOT gets the
// same tree with .zst files plus its own index.
#include <zstd.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bfr/market_file.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bfconvert SRC_ROOT DST_ROOT [level]\n"); return 2; }
    fs::path src = argv[1], dst = argv[2];
    int level = argc > 3 ? std::stoi(argv[3]) : 3;
    std::vector<std::string> rel;
    std::ifstream idx(src / ".bfr_win_index");
    for (std::string l; std::getline(idx, l);) if (!l.empty()) rel.push_back(l);
    if (rel.empty()) { std::fprintf(stderr, "no index in %s; run bfreplay on it first\n", src.c_str()); return 1; }

    std::atomic<size_t> next{0}, in_bytes{0}, out_bytes{0}, failed{0};
    std::vector<std::string> out_rel(rel.size());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t) {
        pool.emplace_back([&] {
            bfr::MarketFile f;
            std::string comp;
            ZSTD_CCtx* cctx = ZSTD_createCCtx();
            for (size_t i; (i = next.fetch_add(1)) < rel.size();) {
                if (!f.load((src / rel[i]).string())) { ++failed; continue; }
                const auto& text = f.text();
                comp.resize(ZSTD_compressBound(text.size()));
                size_t n = ZSTD_compressCCtx(cctx, comp.data(), comp.size(), text.data(), text.size(), level);
                if (ZSTD_isError(n)) { ++failed; continue; }
                fs::path o = dst / fs::path(rel[i]).replace_extension(".zst");
                fs::create_directories(o.parent_path());
                std::ofstream(o, std::ios::binary).write(comp.data(), static_cast<std::streamsize>(n));
                out_rel[i] = fs::relative(o, dst).string();
                in_bytes += text.size();
                out_bytes += n;
            }
            ZSTD_freeCCtx(cctx);
        });
    }
    for (auto& th : pool) th.join();
    std::ofstream out_idx(dst / ".bfr_win_index");
    for (auto& r : out_rel) if (!r.empty()) out_idx << r << '\n';
    std::printf("converted %zu files (%zu failed): %.1f MB raw -> %.1f MB zstd (level %d)\n",
                rel.size() - failed, failed.load(), in_bytes / 1e6, out_bytes / 1e6, level);
}
