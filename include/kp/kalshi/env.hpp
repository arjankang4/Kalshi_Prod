// config._load_env and the four Kalshi settings read from it, at RUNTIME -- secrets are never in
// kp::config and never in this tree. `<KALSHI_ROOT>/.env.kalshi` and `.env.massive` are read exactly
// as config.py reads them (a minimal .env parser: strip, skip blanks/comments/no-'=', partition on
// the first '=', strip the key, strip the value then its surrounding quotes).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace kp::kalshi {

// Ordered (file order; a repeated key keeps its first position and takes the last value, as a
// dict assignment does). Throws std::runtime_error if the file cannot be opened.
std::vector<std::pair<std::string, std::string>> load_env(const std::string& path);

struct KalshiEnv {
    std::string api_base;           // KALSHI_API_BASE, trailing '/' stripped
    std::string ws_base;            // KALSHI_WS_BASE ("" if absent)
    std::string key_id;             // KALSHI_API_KEY_ID
    std::string private_key_path;   // KALSHI_PRIVATE_KEY_PATH
};

// Throws std::runtime_error naming the missing key (config.py raises KeyError at import).
KalshiEnv kalshi_env(const std::string& kalshi_root);
// MASSIVE_API_KEY from .env.massive.
std::string massive_api_key(const std::string& kalshi_root);

}  // namespace kp::kalshi
