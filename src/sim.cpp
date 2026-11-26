#include "bfr/sim.hpp"
#include <filesystem>
#include <map>
#include "bfr/market_file.hpp"
#include "bfr/json.hpp"

namespace bfr {
namespace {

bool is_target_market(std::string_view first_line) {
    if (first_line.find("\"marketType\":\"WIN\"") == std::string_view::npos) return false;
    return first_line.find("\"countryCode\":\"GB\"") != std::string_view::npos ||
           first_line.find("\"countryCode\":\"IE\"") != std::string_view::npos;
}

void apply_definition(MarketView& mv, json::Value md) {
    mv.has_definition = true;
    mv.status = std::string(md["status"].str());
    mv.in_play = md["inPlay"].boolean();
    for (auto r : md["runners"]) {
        auto id = static_cast<int64_t>(r["id"].number(0));
        mv.get_or_add(id).status = std::string(r["status"].str());
    }
}

}  // namespace

bool replay_file(const std::string& path, Strategy& strategy, MarketResult& out, ReplayStats& stats) {
    thread_local MarketFile file;
    thread_local json::Document doc;
    ++stats.files_read;
    if (!file.load(path, is_target_market)) return false;
    const auto& lines = file.lines();
    if (lines.empty()) return false;

    out = MarketResult{};
    out.market_id = std::filesystem::path(path).stem().string();
    out.bytes = file.bytes();
    MarketView mv;
    mv.market_id = out.market_id;
    std::map<int64_t, Order> resting;
    int64_t off = -1;

    for (auto line : lines) {
        if (!doc.parse(line)) { ++stats.bad_lines; continue; }
        ++out.messages;
        auto msg = doc.root();
        const auto pt = static_cast<int64_t>(msg["pt"].number(0));
        for (auto mc : msg["mc"]) {
            if (auto md = mc["marketDefinition"]; md && !md.is_null()) {
                apply_definition(mv, md);
                if (mv.in_play) { off = pt; break; }
            }
            for (auto rc : mc["rc"]) {
                auto ltp = rc["ltp"];
                if (!ltp || ltp.is_null()) continue;
                auto id = static_cast<int64_t>(rc["id"].number(0));
                mv.get_or_add(id).history.push_back({pt, ltp.number()});
            }
        }
        if (off >= 0) break;
        if (!mv.has_definition || mv.status != "OPEN") continue;
        const auto& wanted = strategy.on_update(mv, pt);
        for (auto it = resting.begin(); it != resting.end();) {          // cancels
            if (!wanted.count(it->first)) { it = resting.erase(it); ++out.actions; }
            else ++it;
        }
        for (const auto& [sel, o] : wanted) {                              // places / refreshes
            auto [it, inserted] = resting.try_emplace(sel, o);
            if (inserted) ++out.actions;
            else it->second = o;   // same order (strategy keeps original placed_pt); refresh signal
        }
    }
    stats.messages += out.messages;
    stats.bytes += out.bytes;
    if (off < 0) return false;
    out.off = off;

    // Settlement from the final market definition in the file.
    json::Value final_md;
    for (auto rit = lines.rbegin(); rit != lines.rend() && !final_md; ++rit) {
        if (!doc.parse(*rit)) continue;
        for (auto mc : doc.root()["mc"]) {
            if (auto md = mc["marketDefinition"]; md && !md.is_null()) { final_md = md; break; }
        }
    }
    if (!final_md) return false;
    out.venue = std::string(final_md["venue"].str());
    out.country = std::string(final_md["countryCode"].str());
    out.base_rate = final_md["marketBaseRate"].number(0);
    for (const auto& [sel, o] : resting) {
        if (off - o.placed_pt < kLatencyMs) continue;
        json::Value runner;
        for (auto r : final_md["runners"])
            if (static_cast<int64_t>(r["id"].number(0)) == sel) { runner = r; break; }
        if (!runner) continue;
        auto status = runner["status"].str();
        if (status != "WINNER" && status != "LOSER") continue;      // withdrawn: void
        auto bsp_v = runner["bsp"];
        if (!bsp_v || bsp_v.is_null()) continue;                     // no BSP published
        double bsp = bsp_v.number();                                 // handles "Infinity" strings
        if (bsp == 0.0 || bsp <= 1.0) continue;                      // (NaN passes, as in the Python sim)
        bool win = (status == "WINNER");
        double stake = o.liability / (bsp - 1.0);
        out.bets.push_back({sel, bsp, win, o.liability, stake, win ? -o.liability : stake, o.signal,
                            o.price_at_order, (off - o.placed_pt) / 1000.0});
    }
    return true;
}

double settle_market(const std::vector<Bet>& bets, double commission) {
    double net = 0.0;
    for (const auto& b : bets) net += b.gross;
    return net > 0 ? net * (1.0 - commission) : net;
}

}  // namespace bfr
