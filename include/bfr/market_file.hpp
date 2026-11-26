// Load a market file (.bz2 as Betfair ships it, or .zst from our converted cache) fully into
// memory and split it into lines (views into one buffer). Market files are small (KBs to a
// few hundred KB), so whole-file decompression is simpler and faster than streaming.
#pragma once
#include <bzlib.h>
#include <zstd.h>
#include <fstream>
#include <functional>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bfr {

class MarketFile {
public:
    bool load(const std::string& path, const std::function<bool(std::string_view)>& keep = {}) {
        lines_.clear();
        bool ok = path.ends_with(".zst") ? load_zst(path) : load_bz2(path, keep);
        if (!ok) return false;
        if (keep && !buf_.empty() && !keep(std::string_view(buf_).substr(0, buf_.find('\n')))) return false;
        split();
        return true;
    }
    const std::vector<std::string_view>& lines() const { return lines_; }
    size_t bytes() const { return buf_.size(); }
    const std::string& text() const { return buf_; }

private:
    // `keep` is checked as soon as the first line is available, so rejected .bz2 files are only
    // partially decompressed (the first 64 KB block).
    bool load_bz2(const std::string& path, const std::function<bool(std::string_view)>& keep) {
        buf_.clear();
        lines_.clear();
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        int err = BZ_OK;
        BZFILE* bz = BZ2_bzReadOpen(&err, f, 0, 0, nullptr, 0);
        if (err != BZ_OK) { std::fclose(f); return false; }
        char chunk[1 << 16];
        bool checked = !keep;
        while (err == BZ_OK) {
            int n = BZ2_bzRead(&err, bz, chunk, sizeof chunk);
            if (err == BZ_OK || err == BZ_STREAM_END) buf_.append(chunk, n);
            if (!checked) {
                auto nl = buf_.find('\n');
                if (nl != std::string::npos || err == BZ_STREAM_END) {
                    checked = true;
                    if (!keep(std::string_view(buf_).substr(0, nl))) {
                        BZ2_bzReadClose(&err, bz);
                        std::fclose(f);
                        buf_.clear();
                        return false;
                    }
                }
            }
        }
        bool ok = (err == BZ_STREAM_END);
        BZ2_bzReadClose(&err, bz);
        std::fclose(f);
        return ok;
    }
    bool load_zst(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        comp_.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        in.read(comp_.data(), static_cast<std::streamsize>(comp_.size()));
        auto n = ZSTD_getFrameContentSize(comp_.data(), comp_.size());
        if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN) return false;
        buf_.resize(n);
        thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
        auto r = ZSTD_decompressDCtx(dctx, buf_.data(), n, comp_.data(), comp_.size());
        return !ZSTD_isError(r) && r == n;
    }
    void split() {
        size_t start = 0;
        for (size_t i = 0; i < buf_.size(); ++i) {
            if (buf_[i] == '\n') {
                if (i > start) lines_.emplace_back(buf_.data() + start, i - start);
                start = i + 1;
            }
        }
        if (start < buf_.size()) lines_.emplace_back(buf_.data() + start, buf_.size() - start);
    }

    std::string buf_, comp_;
    std::vector<std::string_view> lines_;
};

}  // namespace bfr
