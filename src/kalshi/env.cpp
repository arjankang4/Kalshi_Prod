#include "kp/kalshi/env.hpp"

#include <fstream>
#include <stdexcept>

namespace kp::kalshi {

namespace {

// str.strip() with no argument: Python whitespace. ASCII is all a .env file carries.
std::string strip(const std::string& s, const char* chars = " \t\n\r\x0b\x0c\x1c\x1d\x1e\x1f") {
    const auto b = s.find_first_not_of(chars);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

std::string get(const std::vector<std::pair<std::string, std::string>>& env, const std::string& k,
                const std::string& file, bool required) {
    for (const auto& [kk, v] : env) if (kk == k) return v;
    if (required) throw std::runtime_error("missing " + k + " in " + file);
    return "";
}

}  // namespace

std::vector<std::pair<std::string, std::string>> load_env(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<std::pair<std::string, std::string>> out;
    std::string line;
    // Python's text-mode iteration splits on universal newlines; getline on '\n' plus the strip of
    // a trailing '\r' is the same for any file a person writes.
    while (std::getline(in, line)) {
        line = strip(line);
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
        const auto eq = line.find('=');
        const std::string k = strip(line.substr(0, eq));
        const std::string v = strip(strip(strip(line.substr(eq + 1)), "\""), "'");
        bool found = false;
        for (auto& kv : out) if (kv.first == k) { kv.second = v; found = true; }
        if (!found) out.emplace_back(k, v);
    }
    return out;
}

KalshiEnv kalshi_env(const std::string& root) {
    const std::string f = root + "/.env.kalshi";
    const auto env = load_env(f);
    KalshiEnv e;
    e.api_base = get(env, "KALSHI_API_BASE", f, true);
    while (!e.api_base.empty() && e.api_base.back() == '/') e.api_base.pop_back();
    e.ws_base = get(env, "KALSHI_WS_BASE", f, false);
    e.key_id = get(env, "KALSHI_API_KEY_ID", f, true);
    e.private_key_path = get(env, "KALSHI_PRIVATE_KEY_PATH", f, true);
    return e;
}

std::string massive_api_key(const std::string& root) {
    const std::string f = root + "/.env.massive";
    return get(load_env(f), "MASSIVE_API_KEY", f, true);
}

}  // namespace kp::kalshi
