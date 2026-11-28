// bfmm: replay recorded Stream API data through the maker simulator.
//
//   bfmm [options] FILE...
//     FILE: .jsonl recordings from `bflive --record-only` (many markets per file, read in order),
//           or Betfair historic .bz2 / .zst market files (one market each).
//   --latency-ms 150  --traded-scale 1.0  --cancel prorata|behind  --commission 0.02
//   --size 2  --min-spread 2  --improve-from 3  --min-edge 0  --lo 2 --hi 20  --start-min 30 --stop-sec 15
//   --markets CSV_OUT   per-market results
//
// Needs order-book fields (EX_ALL_OFFERS, EX_TRADED; SP_PROJECTED optional). BASIC historic files
// only carry LTP, so they produce no quotes.
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "bfr/json.hpp"
#include "bfr/market_file.hpp"
#include "bfr/mm/join_touch.hpp"

using namespace bfr;
using namespace bfr::mm;

int main(int argc, char** argv) {
    SimParams sp;
    JoinTouchParams jp;
    std::string markets_csv;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (k == "--latency-ms") sp.latency_ms = std::stoll(val());
        else if (k == "--traded-scale") sp.traded_scale = std::stod(val());
        else if (k == "--cancel") sp.cancel = val() == "behind" ? CancelModel::Behind : CancelModel::ProRata;
        else if (k == "--commission") sp.commission = std::stod(val());
        else if (k == "--size") jp.size = std::stod(val());
        else if (k == "--min-spread") jp.min_spread = std::stoi(val());
        else if (k == "--improve-from") jp.improve_from = std::stoi(val());
        else if (k == "--min-edge") jp.min_edge = std::stod(val());
        else if (k == "--lo") jp.lo = std::stod(val());
        else if (k == "--hi") jp.hi = std::stod(val());
        else if (k == "--start-min") jp.start_min = std::stoi(val());
        else if (k == "--stop-sec") jp.stop_sec = std::stoi(val());
        else if (k == "--markets") markets_csv = val();
        else if (!k.empty() && k[0] == '-') { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
        else files.push_back(k);
    }
    if (files.empty()) { std::fprintf(stderr, "usage: bfmm [options] FILE...\n"); return 2; }

    struct Slot { std::unique_ptr<JoinTouch> strat; std::unique_ptr<MakerSim> sim; };
    std::map<std::string, Slot> sims;
    std::vector<MarketResult> done;
    json::Document doc;
    size_t lines = 0;

    auto feed = [&](std::string_view line) {
        ++lines;
        if (!doc.parse(line)) return;
        auto msg = doc.root();
        if (msg["op"].str() != "mcm") return;
        auto pt = static_cast<int64_t>(msg["pt"].number(0));
        for (auto mc : msg["mc"]) {
            std::string id(mc["id"].str());
            auto& s = sims[id];
            if (!s.sim) {
                s.strat = std::make_unique<JoinTouch>(jp);
                s.sim = std::make_unique<MakerSim>(id, *s.strat, sp);
            }
            if (s.sim->done()) continue;
            s.sim->on_change(mc, pt);
            if (s.sim->done()) done.push_back(s.sim->result());
        }
    };

    for (const auto& f : files) {
        if (f.ends_with(".jsonl")) {
            std::ifstream in(f);
            for (std::string line; std::getline(in, line);) feed(line);
        } else {
            MarketFile mf;
            if (!mf.load(f)) { std::fprintf(stderr, "cannot read %s\n", f.c_str()); continue; }
            for (auto line : mf.lines()) feed(line);
        }
    }

    size_t open_markets = 0;
    for (auto& [id, s] : sims) if (!s.sim->done()) ++open_markets;
    double matched = 0, edge = 0, res = 0, hedged = 0;
    int orders = 0, fills = 0, unsettled = 0;
    std::ofstream csv;
    if (!markets_csv.empty()) {
        csv.open(markets_csv);
        csv << "market_id,venue,orders,fills,matched,edge_vs_bsp,pnl_result,pnl_hedged,settled\n";
    }
    for (const auto& r : done) {
        orders += r.orders; fills += r.fills; matched += r.matched; edge += r.edge_vs_bsp;
        res += r.pnl_result; hedged += r.pnl_hedged; unsettled += !r.settled;
        if (csv) csv << r.market_id << ",\"" << r.venue << "\"," << r.orders << ',' << r.fills << ',' << r.matched << ','
                     << r.edge_vs_bsp << ',' << r.pnl_result << ',' << r.pnl_hedged << ',' << r.settled << '\n';
    }
    std::printf("%zu lines | %zu markets settled, %zu still open | orders %d | fills %d | matched £%.0f\n", lines,
                done.size(), open_markets, orders, fills, matched);
    std::printf("edge vs BSP £%.2f (%+.2f%% of matched) | P&L on result £%.2f | P&L hedged at BSP £%.2f "
                "(commission %.0f%%)%s\n",
                edge, matched > 0 ? 100 * edge / matched : 0.0, res, hedged, sp.commission * 100,
                unsettled ? " | some runners lacked BSP/result" : "");
    return 0;
}
