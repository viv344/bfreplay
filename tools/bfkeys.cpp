// bfkeys: list this account's developer app keys (delayed and live) and whether each is active.
//   bfkeys [--config ~/.config/bflive/config.ini]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "bfr/json.hpp"
#include "bfr/live/rest.hpp"

using namespace bfr;
using namespace bfr::live;

int main(int argc, char** argv) {
    std::string path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/bflive/config.ini";
    if (argc == 3 && std::string(argv[1]) == "--config") path = argv[2];
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
    Credentials c;
    for (std::string line; std::getline(in, line);) {
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "username") c.username = v;
        else if (k == "password") c.password = v;
        else if (k == "app_key") c.app_key = v;
        else if (k == "cert") c.cert_file = v;
        else if (k == "key") c.key_file = v;
    }
    if (const char* p = std::getenv("BF_PASSWORD")) c.password = p;
    Rest rest(c);
    std::string err;
    if (!rest.login(err)) { std::fprintf(stderr, "login failed: %s\n", err.c_str()); return 1; }
    auto r = rest.account("getDeveloperAppKeys");
    if (!r.ok()) { std::fprintf(stderr, "getDeveloperAppKeys failed (HTTP %ld): %s %s\n", r.status, r.error.c_str(), r.body.c_str()); return 1; }
    json::Document doc;
    if (!doc.parse(r.body)) { std::fprintf(stderr, "bad response\n"); return 1; }
    for (auto app : doc.root()) {
        std::printf("app: %s\n", std::string(app["appName"].str()).c_str());
        for (auto v : app["appVersions"])
            std::printf("  %-8s key %s  active=%s\n", v["delayData"].boolean() ? "DELAYED" : "LIVE",
                        std::string(v["applicationKey"].str()).c_str(), v["active"].boolean() ? "yes" : "no");
    }
    return 0;
}
