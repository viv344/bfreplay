// bflive: run a strategy against the live Betfair Exchange Stream API.
//
// The same MarketEngine and Strategy code used by bfreplay processes every stream message.
// By default it is a DRY RUN: intended orders are logged and settled on paper at BSP when
// each market closes. Real orders require --live, and are subject to hard safety limits
// and a kill switch (create the file <config dir>/STOP to cancel everything and halt).
//
//   bflive --config ~/.config/bflive/config.ini [strategy options] [--live]
//          [--max-liability 50] [--max-orders-per-day 60] [--max-daily-liability 1500]
//          [--record-dir DIR] [--log FILE] [--window-min 30] [--warmup-min 60]
//          [--fields '["EX_MARKET_DEF","EX_LTP"]'] [--record-only]
//
// --record-only: only record the stream (no strategy, no orders). With --fields it can capture
// the order book, e.g. '["EX_MARKET_DEF","EX_ALL_OFFERS","EX_TRADED","EX_TRADED_VOL","EX_LTP","SP_PROJECTED","SP_TRADED"]'.
//
// config.ini keys: username, app_key, cert, key, [password] (or env BF_PASSWORD).
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bfr/engine.hpp"
#include "bfr/json.hpp"
#include "bfr/live/rest.hpp"
#include "bfr/live/stream.hpp"
#include "bfr/sim.hpp"
#include "bfr/strategy_options.hpp"

namespace fs = std::filesystem;
using namespace bfr;
using namespace bfr::live;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string now_utc(const char* fmt = "%Y-%m-%dT%H:%M:%SZ") {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, fmt, std::gmtime(&t));
    return buf;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Args {
    std::string config = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/bflive/config.ini";
    StrategyOptions strat;
    bool live = false;
    double max_liability = 50.0;
    int max_orders_per_day = 60;
    double max_daily_liability = 1500.0;
    std::string record_dir = "live_data";
    int warmup_min = 60;   // after start or a data gap > 5 min: rebuild this much history before acting
    std::string log = "bflive_log.csv";
    std::string fields;        // empty: StreamConfig default
    bool record_only = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    a.strat.window_min = 30;   // live default: only act in the last 30 min before the scheduled start
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a.strat.parse(k, val)) continue;
        if (k == "--config") a.config = val();
        else if (k == "--live") a.live = true;
        else if (k == "--max-liability") a.max_liability = std::stod(val());
        else if (k == "--max-orders-per-day") a.max_orders_per_day = std::stoi(val());
        else if (k == "--max-daily-liability") a.max_daily_liability = std::stod(val());
        else if (k == "--record-dir") a.record_dir = val();
        else if (k == "--warmup-min") a.warmup_min = std::stoi(val());
        else if (k == "--log") a.log = val();
        else if (k == "--fields") a.fields = val();
        else if (k == "--record-only") a.record_only = true;
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); std::exit(2); }
    }
    return a;
}

Credentials load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot read config %s\n", path.c_str()); std::exit(2); }
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && (st.st_mode & 077))
        std::fprintf(stderr, "warning: %s is readable by other users; run: chmod 600 %s\n", path.c_str(), path.c_str());
    Credentials c;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "username") c.username = v;
        else if (k == "password") c.password = v;
        else if (k == "app_key") c.app_key = v;
        else if (k == "cert") c.cert_file = v;
        else if (k == "key") c.key_file = v;
    }
    if (const char* p = std::getenv("BF_PASSWORD")) c.password = p;
    for (auto [name, v] : {std::pair{"username", &c.username}, {"app_key", &c.app_key}, {"cert", &c.cert_file},
                           {"key", &c.key_file}, {"password (or BF_PASSWORD)", &c.password}})
        if (v->empty()) { std::fprintf(stderr, "config is missing %s\n", name); std::exit(2); }
    return c;
}

// Append-only CSV event log (orders, cancels, settlements, errors).
class EventLog {
public:
    explicit EventLog(const std::string& path) : out_(path, std::ios::app) {
        if (fs::file_size(path) == 0)
            out_ << "time_utc,mode,event,market_id,venue,selection_id,signal,price,liability,bet_id,bsp,win,gross,note\n";
    }
    void write(const std::string& mode, const std::string& event, const std::string& market, const std::string& venue,
               int64_t sel, double signal, double price, double liability, const std::string& bet_id = "",
               double bsp = 0, int win = -1, double gross = 0, const std::string& note = "") {
        std::lock_guard lk(mu_);
        out_ << now_utc() << ',' << mode << ',' << event << ',' << market << ",\"" << venue << "\"," << sel << ','
             << signal << ',' << price << ',' << liability << ',' << bet_id << ',' << bsp << ',' << win << ',' << gross
             << ",\"" << note << "\"\n";
        out_.flush();
    }

private:
    std::ofstream out_;
    std::mutex mu_;
};

// Executes order actions. Dry run: record only. Live: REST calls on a worker thread so the
// stream reader never blocks on HTTP.
class Executor {
public:
    Executor(const Args& a, Rest& rest, EventLog& log) : a_(a), rest_(rest), log_(log) {
        if (a_.live) worker_ = std::thread([this] { run(); });
    }
    ~Executor() {
        { std::lock_guard lk(mu_); done_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void submit(const OrderAction& act, const std::string& venue) {
        const char* mode = a_.live ? "LIVE" : "DRY";
        if (act.kind == OrderAction::Kind::Place) {
            double liab = act.order.liability;
            std::string why;
            if (halted_) why = "halted (kill switch)";
            else if (liab > a_.max_liability) why = "liability above --max-liability";
            else if (orders_today_ >= a_.max_orders_per_day) why = "--max-orders-per-day reached";
            else if (liability_today_ + liab > a_.max_daily_liability) why = "--max-daily-liability reached";
            if (!why.empty()) {
                log_.write(mode, "REJECT", act.market_id, venue, act.order.selection_id, act.order.signal,
                           act.order.price_at_order, liab, "", 0, -1, 0, why);
                return;
            }
            ++orders_today_;
            liability_today_ += liab;
        }
        if (!a_.live) {
            log_.write(mode, act.kind == OrderAction::Kind::Place ? "PLACE" : "CANCEL", act.market_id, venue,
                       act.order.selection_id, act.order.signal, act.order.price_at_order, act.order.liability);
            return;
        }
        { std::lock_guard lk(mu_); queue_.push_back({act, venue}); }
        cv_.notify_one();
    }

    // Kill switch: stop placing and cancel everything we know about.
    void halt_and_cancel_all(const char* reason = "KILL SWITCH") {
        if (halted_.exchange(true)) return;
        std::fprintf(stderr, "%s: halting%s\n", reason, a_.live ? " and cancelling all open orders" : "");
        if (!a_.live) return;
        std::lock_guard lk(mu_);
        for (auto& [key, bet] : bets_) queue_.push_back({{OrderAction::Kind::Cancel, key.first, Order{key.second}}, "kill"});
        cv_.notify_one();
    }

    void new_day() { orders_today_ = 0; liability_today_ = 0; }
    bool halted() const { return halted_; }

private:
    struct Item { OrderAction act; std::string venue; };

    void run() {
        while (true) {
            Item it;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [&] { return done_ || !queue_.empty(); });
                if (queue_.empty()) return;
                it = std::move(queue_.front());
                queue_.pop_front();
            }
            auto key = std::pair{it.act.market_id, it.act.order.selection_id};
            std::string err;
            if (it.act.kind == OrderAction::Kind::Place) {
                auto ref = it.act.market_id + "-" + std::to_string(it.act.order.selection_id) + "-" + std::to_string(now_ms());
                if (auto bet = rest_.place_moc_lay(it.act.market_id, it.act.order.selection_id, it.act.order.liability,
                                                   ref.substr(0, 32), err)) {
                    { std::lock_guard lk(mu_); bets_[key] = *bet; }
                    log_.write("LIVE", "PLACE", it.act.market_id, it.venue, it.act.order.selection_id, it.act.order.signal,
                               it.act.order.price_at_order, it.act.order.liability, *bet);
                } else {
                    log_.write("LIVE", "PLACE_FAILED", it.act.market_id, it.venue, it.act.order.selection_id,
                               it.act.order.signal, it.act.order.price_at_order, it.act.order.liability, "", 0, -1, 0, err);
                }
            } else {
                std::string bet;
                { std::lock_guard lk(mu_); if (auto f = bets_.find(key); f != bets_.end()) { bet = f->second; bets_.erase(f); } }
                if (bet.empty()) continue;
                bool ok = rest_.cancel(it.act.market_id, bet, err);
                log_.write("LIVE", ok ? "CANCEL" : "CANCEL_FAILED", it.act.market_id, it.venue, it.act.order.selection_id,
                           it.act.order.signal, it.act.order.price_at_order, it.act.order.liability, bet, 0, -1, 0, err);
            }
        }
    }

    const Args& a_;
    Rest& rest_;
    EventLog& log_;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::map<std::pair<std::string, int64_t>, std::string> bets_;   // (market, selection) -> betId
    bool done_ = false;
    std::atomic<bool> halted_{false};
    int orders_today_ = 0;
    double liability_today_ = 0;
};

struct LiveMarket {
    std::unique_ptr<Strategy> strategy;
    std::unique_ptr<MarketEngine> engine;
    std::string venue;
    std::map<int64_t, Order> at_off;   // orders resting when the market went in-play
    bool off_recorded = false;
};

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered even when redirected to a file
    Args a = parse_args(argc, argv);
    Credentials creds = load_config(a.config);
    const fs::path stop_file = fs::path(a.config).parent_path() / "STOP";
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    fs::create_directories(a.record_dir);
    if (!fs::exists(a.log)) std::ofstream(a.log).close();
    EventLog log(a.log);

    std::printf("bflive %s | strategy %s | log %s | recording to %s/\n", a.live ? "LIVE (real orders)" : "DRY RUN",
                a.strat.name.c_str(), a.log.c_str(), a.record_dir.c_str());
    if (a.record_only && a.live) { std::fprintf(stderr, "--record-only and --live are exclusive\n"); return 2; }
    if (a.record_only) std::printf("RECORD ONLY: no strategy, no orders\n");
    if (a.live) std::printf("limits: £%.0f per order, %d orders/day, £%.0f liability/day; kill switch: touch %s\n",
                            a.max_liability, a.max_orders_per_day, a.max_daily_liability, stop_file.c_str());

    Rest rest(creds);
    std::string err;
    if (!rest.login(err)) { std::fprintf(stderr, "login failed: %s\n", err.c_str()); return 1; }
    std::printf("logged in\n");

    Executor exec(a, rest, log);
    StreamConfig scfg;
    if (!a.fields.empty()) scfg.data_fields = a.fields;
    StreamClient stream(scfg);
    std::map<std::string, LiveMarket> markets;
    json::Document doc;
    std::vector<OrderAction> actions;
    std::ofstream record;
    std::string record_day, line;
    double paper_pnl_2 = 0, paper_pnl_6 = 0;
    size_t msgs = 0, settled = 0;
    auto last_keepalive = std::chrono::steady_clock::now(), last_status = last_keepalive;
    int backoff_s = 1;
    // Warm-up: strategies need history. Do not act until we have seen `warmup_min` of
    // continuous data, after start-up and after any gap longer than 5 minutes.
    // Wall clock on purpose: steady_clock stops while the machine is suspended, which would hide gaps.
    using wall = std::chrono::system_clock;
    auto warm_until = wall::now() + std::chrono::minutes(a.warmup_min);
    auto last_msg = wall::now();
    std::printf("warming up: no orders before %d min of continuous data\n", a.warmup_min);

    while (!g_stop) {
        if (!stream.connect(rest.app_key(), rest.session(), err)) {
            std::fprintf(stderr, "%s stream connect failed: %s (retry in %ds)\n", now_utc().c_str(), err.c_str(), backoff_s);
            if (err.find("SESSION") != std::string::npos) rest.login(err);
            std::this_thread::sleep_for(std::chrono::seconds(backoff_s));
            backoff_s = std::min(backoff_s * 2, 60);
            continue;
        }
        std::printf("%s stream connected and subscribed\n", now_utc().c_str());
        backoff_s = 1;

        while (!g_stop && stream.read_line(line, err)) {
            ++msgs;
            {
                auto now = wall::now();
                if (now - last_msg > std::chrono::minutes(5)) {          // data gap: cancel and re-warm
                    std::printf("%s data gap of %lds: cancelling resting orders, warming up again\n", now_utc().c_str(),
                                static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - last_msg).count()));
                    warm_until = now + std::chrono::minutes(a.warmup_min);
                    for (auto& [id, m] : markets) {
                        if (!m.engine || m.engine->is_off()) continue;
                        actions.clear();
                        m.engine->cancel_all(actions);
                        for (auto& act : actions) exec.submit(act, m.venue);
                    }
                }
                last_msg = now;
            }
            if (auto day = now_utc("%Y%m%d"); day != record_day) {        // daily rotation
                record_day = day;
                record.close();
                record.open(fs::path(a.record_dir) / ("stream-" + day + ".jsonl"), std::ios::app);
                exec.new_day();
            }
            record << line << '\n';
            record.flush();                                             // low message rate: flush every line

            if (!doc.parse(line)) continue;
            auto msg = doc.root();
            auto op = msg["op"].str();
            if (op == "status" && msg["connectionClosed"].boolean()) {
                err = "server closed connection: " + std::string(msg["errorCode"].str());
                break;
            }
            if (op != "mcm") continue;
            stream.note_clocks(std::string(msg["initialClk"].str()), std::string(msg["clk"].str()));
            if (msg["ct"].str() == "HEARTBEAT") continue;
            const auto pt = static_cast<int64_t>(msg["pt"].number(0));

            std::vector<std::string> touched;
            if (!a.record_only) for (auto mc : msg["mc"]) {
                std::string id(mc["id"].str());
                auto& m = markets[id];
                if (!m.engine) {
                    m.strategy = a.strat.make();
                    m.engine = std::make_unique<MarketEngine>(id, *m.strategy, a.strat.window_min);
                }
                auto md = mc["marketDefinition"];
                if (md && !md.is_null() && m.venue.empty()) m.venue = std::string(md["venue"].str());
                m.engine->on_change(mc, pt);
                if (m.engine->is_off() && !m.off_recorded) {              // snapshot orders live at the off
                    m.off_recorded = true;
                    for (auto& [sel, o] : m.engine->resting())
                        if (m.engine->off_pt() - o.placed_pt >= kLatencyMs) m.at_off.emplace(sel, o);
                }
                // Settlement once the market closes with BSP and results.
                if (md && !md.is_null() && md["status"].str() == "CLOSED") {
                    std::vector<Bet> bets;
                    for (auto r : md["runners"]) {
                        auto sel = static_cast<int64_t>(r["id"].number(0));
                        auto it = m.at_off.find(sel);
                        if (it == m.at_off.end()) continue;
                        auto status = r["status"].str();
                        double bsp = r["bsp"].number(0);
                        if ((status != "WINNER" && status != "LOSER") || bsp <= 1.0) continue;
                        bool win = status == "WINNER";
                        double stake = it->second.liability / (bsp - 1);
                        bets.push_back({sel, bsp, win, it->second.liability, stake, win ? -it->second.liability : stake,
                                        it->second.signal, it->second.price_at_order, 0});
                        log.write(a.live ? "LIVE" : "DRY", "SETTLE", id, m.venue, sel, it->second.signal,
                                  it->second.price_at_order, it->second.liability, "", bsp, win, win ? -it->second.liability : stake);
                    }
                    if (!bets.empty()) { paper_pnl_2 += settle_market(bets, 0.02); paper_pnl_6 += settle_market(bets, 0.06); ++settled; }
                    markets.erase(id);
                    continue;
                }
                touched.push_back(id);
            }
            if (fs::exists(stop_file)) exec.halt_and_cancel_all();
            const bool warm = wall::now() >= warm_until;
            for (auto& id : touched) {
                if (!warm) break;
                auto it = markets.find(id);
                if (it == markets.end() || !it->second.engine) continue;
                actions.clear();
                it->second.engine->decide(pt, actions);
                for (auto& act : actions) exec.submit(act, it->second.venue);
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_status > std::chrono::seconds(60)) {
                last_status = now;
                size_t resting = 0;
                for (auto& [id, m] : markets) if (m.engine && !m.engine->is_off()) resting += m.engine->resting().size();
                std::printf("%s msgs %zu | markets %zu | resting %zu | settled %zu | paper P&L £%.2f @2%%, £%.2f @6%%%s%s\n",
                            now_utc().c_str(), msgs, markets.size(), resting, settled, paper_pnl_2, paper_pnl_6,
                            wall::now() < warm_until ? " | warming up" : "", exec.halted() ? " | HALTED" : "");
                std::fflush(stdout);
            }
            if (now - last_keepalive > std::chrono::minutes(30)) {
                last_keepalive = now;
                if (!rest.keep_alive(err)) { std::fprintf(stderr, "%s\n", err.c_str()); rest.login(err); }
            }
        }
        if (!g_stop) std::fprintf(stderr, "%s stream dropped: %s; reconnecting\n", now_utc().c_str(), err.c_str());
        stream.close();
    }
    exec.halt_and_cancel_all("shutdown");
    return 0;
}
