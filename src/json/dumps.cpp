// json.dumps, byte for byte -- see DumpStyle in value.hpp.
#include <cstdio>
#include <string>

#include "kp/json/value.hpp"
#include "kp/pycompat/repr.hpp"

namespace kp::json {
namespace {

// json.encoder.py_encode_basestring_ascii: '"' and '\\' escaped, the five short control escapes,
// every other byte outside printable ASCII (0x20-0x7e) as \uXXXX of its CODE POINT -- UTF-8 is
// decoded first, and a code point past the BMP becomes a surrogate pair. Lowercase hex.
void put_string(std::string& out, const std::string& s) {
    out.push_back('"');
    const auto u = [&](unsigned cp) {
        char b[8];
        std::snprintf(b, sizeof b, "\\u%04x", cp);
        out += b;
    };
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            ++i;
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || c == 0x7f) u(c);
                    else out.push_back((char)c);
            }
            continue;
        }
        unsigned cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { u(c); ++i; continue; }                  // stray byte: not valid UTF-8
        if (i + (std::size_t)extra >= s.size()) { u(c); ++i; continue; }   // truncated sequence
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | ((unsigned char)s[i + (std::size_t)k] & 0x3F);
        i += (std::size_t)extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u(0xD800 + (cp >> 10));
            u(0xDC00 + (cp & 0x3FF));
        } else {
            u(cp);
        }
    }
    out.push_back('"');
}

void put(std::string& out, const Value& v, const DumpStyle& st, int level) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; return;
        case Value::Type::Bool: out += v.boolean() ? "true" : "false"; return;
        case Value::Type::Number:
            out += v.is_int() ? std::to_string(v.integer()) : kp::pycompat::py_json_float(v.num());
            return;
        case Value::Type::String: put_string(out, v.str()); return;
        case Value::Type::Array:
        case Value::Type::Object: break;
    }
    const bool obj = v.is_object();
    const std::size_t n = obj ? v.obj().size() : v.arr().size();
    out.push_back(obj ? '{' : '[');
    if (n == 0) { out.push_back(obj ? '}' : ']'); return; }
    // With indent, Python forces the item separator to "," (the trailing space would dangle).
    const std::string item = st.indent >= 0 ? std::string(",") : std::string(st.item_sep);
    const auto nl = [&](int lvl) {
        if (st.indent < 0) return;
        out.push_back('\n');
        out.append((std::size_t)(st.indent * lvl), ' ');
    };
    for (std::size_t k = 0; k < n; ++k) {
        if (k) out += item;
        nl(level + 1);
        if (obj) {
            put_string(out, v.obj()[k].first);
            out += st.key_sep;
            put(out, v.obj()[k].second, st, level + 1);
        } else {
            put(out, v.arr()[k], st, level + 1);
        }
    }
    nl(level);
    out.push_back(obj ? '}' : ']');
}

}  // namespace

// repr() of a JSON-shaped value, for the lines where the reference prints a dict with an f-string
// ({'cold': 26}) rather than json.dumps ({"cold": 26}). Strings take repr's quoting.
namespace {
// Python str.__repr__ over UTF-8: quote choice, backslash escapes, and Py_UNICODE_ISPRINTABLE.
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

bool printable(unsigned cp) {
    if (cp < 0x20 || cp == 0x7F) return false;
    if (cp < 0x7F) return true;
    if (cp <= 0xA0 || cp == 0xAD) return false;
    if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
        (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF) return false;
    if (cp >= 0xD800 && cp <= 0xF8FF) return false;
    if ((cp >= 0xFFF9 && cp <= 0xFFFB) || cp == 0xFFFE || cp == 0xFFFF) return false;
    if (cp >= 0xE0000) return false;
    return true;
}

void put_cp(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                             out.push_back((char)(0x80 | (cp & 0x3F))); }
    else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
           out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
}
}  // namespace

std::string py_repr_str(const std::string& s) {
    const bool has_sq = s.find('\'') != std::string::npos, has_dq = s.find('"') != std::string::npos;
    const char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    char buf[16];
    for (std::size_t i = 0; i < s.size();) {
        const unsigned cp = decode(s, i);
        if (cp == (unsigned)q || cp == '\\') { out.push_back('\\'); out.push_back((char)cp); }
        else if (cp == '\t') out += "\\t";
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (printable(cp)) put_cp(out, cp);
        else if (cp < 0x100) { std::snprintf(buf, sizeof buf, "\\x%02x", cp); out += buf; }
        else if (cp < 0x10000) { std::snprintf(buf, sizeof buf, "\\u%04x", cp); out += buf; }
        else { std::snprintf(buf, sizeof buf, "\\U%08x", cp); out += buf; }
    }
    out.push_back(q);
    return out;
}

std::string py_repr(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: return "None";
        case Value::Type::Bool: return v.boolean() ? "True" : "False";
        case Value::Type::Number:
            return v.is_int() ? std::to_string(v.integer()) : kp::pycompat::py_float_repr(v.num());
        case Value::Type::String: return py_repr_str(v.str());
        case Value::Type::Array: {
            std::string o = "[";
            for (std::size_t i = 0; i < v.arr().size(); ++i) o += (i ? ", " : "") + py_repr(v.arr()[i]);
            return o + "]";
        }
        case Value::Type::Object: {
            std::string o = "{";
            for (std::size_t i = 0; i < v.obj().size(); ++i)
                o += (i ? ", " : "") + py_repr_str(v.obj()[i].first) + ": " + py_repr(v.obj()[i].second);
            return o + "}";
        }
    }
    return "";
}


std::string dumps(const Value& v, const DumpStyle& style) {
    std::string out;
    put(out, v, style, 0);
    return out;
}

}  // namespace kp::json
