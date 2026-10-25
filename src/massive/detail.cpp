#include "detail.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "kp/pycompat/repr.hpp"

namespace kp::massive::detail {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\v\f";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

}  // namespace

// One implementation for the whole tree: kp::json::py_repr (src/json/dumps.cpp).
std::string py_repr(const kp::json::Value& v) { return kp::json::py_repr(v); }

std::string head(const std::string& s, std::size_t n) {
    // Count code points: a byte that is not a UTF-8 continuation byte (10xxxxxx) starts one.
    std::size_t cps = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (cps == n) break;
            ++cps;
        }
    }
    return s.substr(0, i);
}

bool truthy(const kp::json::Value& v) {
    using T = kp::json::Value::Type;
    switch (v.type()) {
        case T::Null: return false;
        case T::Bool: return v.boolean();
        case T::Number: return v.is_int() ? v.integer() != 0 : v.num() != 0.0;
        case T::String: return !v.str().empty();
        case T::Array: return !v.arr().empty();
        case T::Object: return !v.obj().empty();
    }
    return false;
}

double py_float(const kp::json::Value& v) {
    using T = kp::json::Value::Type;
    if (v.type() == T::Number) return v.is_int() ? (double)v.integer() : v.num();
    if (v.type() == T::Bool) return v.boolean() ? 1.0 : 0.0;
    if (v.type() == T::String) {
        const std::string t = trim(v.str());
        if (!t.empty()) {
            // float() takes decimal, inf/infinity/nan; strtod additionally takes hex, which
            // float() does not -- refuse it so the two agree on what raises.
            const bool hex = t.find('x') != std::string::npos || t.find('X') != std::string::npos;
            char* end = nullptr;
            errno = 0;
            const double d = std::strtod(t.c_str(), &end);
            if (!hex && end && *end == '\0') return d;
        }
        throw std::invalid_argument("could not convert string to float: " + kp::json::py_repr(kp::json::Value::make_string(v.str())));
    }
    throw PyTypeError("float() argument must be a string or a real number");
}

long long py_int(const kp::json::Value& v) {
    using T = kp::json::Value::Type;
    if (v.type() == T::Number) {
        if (v.is_int()) return v.integer();
        const double d = v.num();
        if (!std::isfinite(d)) throw std::invalid_argument("cannot convert float to integer");
        return (long long)std::trunc(d);
    }
    if (v.type() == T::Bool) return v.boolean() ? 1 : 0;
    if (v.type() == T::String) {
        const std::string t = trim(v.str());
        std::size_t i = (!t.empty() && (t[0] == '+' || t[0] == '-')) ? 1 : 0;
        bool ok = i < t.size();
        for (std::size_t j = i; j < t.size(); ++j) ok = ok && t[j] >= '0' && t[j] <= '9';
        if (ok) return std::stoll(t);
        throw std::invalid_argument("invalid literal for int() with base 10: " + kp::json::py_repr(kp::json::Value::make_string(v.str())));
    }
    throw PyTypeError("int() argument must be a string or a real number");
}

}  // namespace kp::massive::detail
