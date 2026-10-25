// kalshi.client.KalshiError: a non-2xx answer from the exchange, status and body kept.
#pragma once
#include <stdexcept>
#include <string>

namespace kp::kalshi {

class KalshiError : public std::runtime_error {
public:
    KalshiError(int http_status, std::string response_body)
        : std::runtime_error("HTTP " + std::to_string(http_status) + ": " + head400(response_body)),
          status(http_status), body(std::move(response_body)) {}
    int status;
    std::string body;

private:
    // body[:400] on a Python str: 400 CODE POINTS of the UTF-8, not 400 bytes.
    static std::string head400(const std::string& s) {
        std::size_t i = 0, n = 0;
        while (i < s.size() && n < 400) {
            const unsigned char c = (unsigned char)s[i];
            i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            ++n;
        }
        return s.substr(0, i < s.size() ? i : s.size());
    }
};

}  // namespace kp::kalshi
