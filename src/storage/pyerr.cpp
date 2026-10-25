#include "kp/storage/pyerr.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "kp/pycompat/repr.hpp"

namespace kp::storage {

namespace {

// Decode one UTF-8 code point at s[i]; advances i. Invalid bytes decode as themselves.
unsigned decode(const std::string& s, std::size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    int extra = 0;
    unsigned cp = c;
    if (c >= 0xF0 && c < 0xF8) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    if (i + (std::size_t)extra >= s.size() + (extra ? 0 : 1)) { ++i; return c; }
    for (int k = 1; k <= extra; ++k) cp = (cp << 6) | ((unsigned char)s[i + (std::size_t)k] & 0x3F);
    i += (std::size_t)extra + 1;
    return cp;
}


bool is_space(unsigned cp) {
    return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x1F) || cp == 0x85 ||
           cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}


std::string strip_ws(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e) { std::size_t j = b; if (!is_space(decode(s, j))) break; b = j; }
    // trailing: walk code points from the front, remembering the last non-space end
    std::size_t last = b, i = b;
    while (i < e) { const unsigned cp = decode(s, i); if (!is_space(cp)) last = i; }
    return s.substr(b, last - b);
}

}  // namespace

std::string py_slice(const std::string& s, std::size_t n) {
    std::size_t i = 0, k = 0;
    while (i < s.size() && k < n) { decode(s, i); ++k; }
    return s.substr(0, i);
}

std::string py_repr_str(const std::string& s) { return kp::json::py_repr_str(s); }

std::string py_type_name(const kp::json::Value& v) {
    using T = kp::json::Value::Type;
    switch (v.type()) {
        case T::Null: return "NoneType";
        case T::Bool: return "bool";
        case T::Number: return v.is_int() ? "int" : "float";
        case T::String: return "str";
        case T::Array: return "list";
        case T::Object: return "dict";
    }
    return "object";
}

std::string py_repr(const kp::json::Value& v) { return kp::json::py_repr(v); }

std::string py_str(const kp::json::Value& v) { return v.is_string() ? v.str() : kp::json::py_repr(v); }

double py_float(const kp::json::Value& v) {
    using T = kp::json::Value::Type;
    if (v.type() == T::Number) return v.is_int() ? (double)v.integer() : v.num();
    if (v.type() == T::Bool) return v.boolean() ? 1.0 : 0.0;
    if (v.type() != T::String)
        throw PyError("TypeError", "float() argument must be a string or a real number, not '" +
                                       py_type_name(v) + "'");
    const std::string& raw = v.str();
    const auto bad = [&]() {
        return PyError("ValueError", "could not convert string to float: " + py_repr_str(raw), true);
    };
    const std::string s = strip_ws(raw);
    // Grammar check (CPython's _Py_string_to_number_with_underscores + PyOS_string_to_double):
    // [sign] (digits[.digits] | .digits) [(e|E)[sign]digits] | [sign] inf|infinity|nan. An
    // underscore is legal only BETWEEN two digits. No hex.
    std::size_t i = 0;
    std::string clean;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) clean.push_back(s[i++]);
    std::string rest = s.substr(i);
    std::string low;
    for (char c : rest) low.push_back((char)std::tolower((unsigned char)c));
    if (low == "inf" || low == "infinity" || low == "nan") {
        clean += low;
    } else {
        auto digits = [&](std::size_t& j, bool& any) {
            any = false;
            while (j < s.size()) {
                if (s[j] >= '0' && s[j] <= '9') { clean.push_back(s[j++]); any = true; }
                else if (s[j] == '_' && any && j + 1 < s.size() && s[j + 1] >= '0' && s[j + 1] <= '9') ++j;
                else break;
            }
        };
        bool int_part = false, frac_part = false, exp_part = false;
        digits(i, int_part);
        if (i < s.size() && s[i] == '.') { clean.push_back('.'); ++i; digits(i, frac_part); }
        if (!int_part && !frac_part) throw bad();
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            clean.push_back('e');
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) clean.push_back(s[i++]);
            digits(i, exp_part);
            if (!exp_part) throw bad();
        }
        if (i != s.size()) throw bad();
    }
    errno = 0;
    return std::strtod(clean.c_str(), nullptr);
}

}  // namespace kp::storage
