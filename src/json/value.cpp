#include "kp/json/value.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kp::json {

namespace {
const std::string EMPTY_STR;
const Array EMPTY_ARR;
const std::vector<Member> EMPTY_OBJ;
}

double Value::num() const { return num_; }
bool Value::boolean() const { return bool_; }
const std::string& Value::str() const { return type_ == Type::String ? str_ : EMPTY_STR; }
const Array& Value::arr() const { return type_ == Type::Array ? arr_ : EMPTY_ARR; }
const std::vector<Member>& Value::obj() const { return type_ == Type::Object ? obj_ : EMPTY_OBJ; }

const Value* Value::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : obj_) if (m.first == key) return &m.second;
    return nullptr;
}

std::optional<double> Value::opt_num(const std::string& key) const {
    const Value* v = find(key);
    if (v == nullptr || v->is_null() || !v->is_number()) return std::nullopt;
    return v->num();
}

std::optional<std::string> Value::opt_str(const std::string& key) const {
    const Value* v = find(key);
    if (v == nullptr || v->is_null() || !v->is_string()) return std::nullopt;
    return v->str();
}

std::vector<double> Value::num_array(const std::string& key) const {
    std::vector<double> out;
    const Value* v = find(key);
    if (v == nullptr || !v->is_array()) return out;
    out.reserve(v->arr().size());
    for (const auto& e : v->arr()) out.push_back(e.is_number() ? e.num() : 0.0);
    return out;
}

Value Value::make_number(double d) { Value v; v.type_ = Type::Number; v.num_ = d; return v; }
Value Value::make_int(long long i) {
    Value v; v.type_ = Type::Number; v.is_int_ = true; v.int_ = i; v.num_ = (double)i; return v;
}

Value& Value::set(const std::string& key, Value v) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw std::runtime_error("json: set() on a non-object");
    for (auto& m : obj_) if (m.first == key) { m.second = std::move(v); return m.second; }
    obj_.emplace_back(key, std::move(v));
    return obj_.back().second;
}

Value& Value::push(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw std::runtime_error("json: push() on a non-array");
    arr_.push_back(std::move(v));
    return arr_.back();
}

Value* Value::find_mut(const std::string& key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& m : obj_) if (m.first == key) return &m.second;
    return nullptr;
}
Value Value::make_string(std::string s) { Value v; v.type_ = Type::String; v.str_ = std::move(s); return v; }
Value Value::make_bool(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
Value Value::make_array(Array a) { Value v; v.type_ = Type::Array; v.arr_ = std::move(a); return v; }
Value Value::make_object(std::vector<Member> m) { Value v; v.type_ = Type::Object; v.obj_ = std::move(m); return v; }

namespace {

struct Parser {
    const std::string& s;
    std::size_t i = 0;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error("json: " + std::string(what) + " at byte " + std::to_string(i));
    }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    void expect(char c) { if (peek() != c) fail("expected a delimiter"); ++i; }

    Value value() {
        ws();
        switch (peek()) {
            case '{': return object();
            case '[': return array();
            case '"': return Value::make_string(string());
            case 't': lit("true");  return Value::make_bool(true);
            case 'f': lit("false"); return Value::make_bool(false);
            case 'n': lit("null");  return Value();
            default:  return number();
        }
    }

    void lit(const char* w) {
        const std::size_t n = std::char_traits<char>::length(w);
        if (s.compare(i, n, w) != 0) fail("bad literal");
        i += n;
    }

    Value number() {
        const std::size_t start = i;
        if (peek() == '-' || peek() == '+') ++i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        if (i == start) fail("expected a value");
        // strtod, and only strtod. See the header: a hand-rolled accumulator loses ulps, and
        // those ulps reach the required edge.
        const std::string tok = s.substr(start, i - start);
        const char* c = tok.c_str();
        char* end = nullptr;
        if (tok.find_first_of(".eE") == std::string::npos) {
            errno = 0;
            const long long v = std::strtoll(c, &end, 10);
            if (end != c && *end == '\0' && errno != ERANGE) return Value::make_int(v);
            // Beyond int64: Python would hold an exact bigint. Nothing on the live path carries
            // one; fall through to the nearest double rather than failing the whole document.
        }
        const double d = std::strtod(c, &end);
        if (end == c) fail("malformed number");
        return Value::make_number(d);
    }

    std::string string() {
        expect('"');
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) fail("truncated escape");
            const char e = s[i++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) fail("truncated \\u");
                    unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    // Surrogate pair, as Python's json emits for non-BMP.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                        s[i] == '\\' && s[i + 1] == 'u') {
                        const unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp < 0x80) out.push_back((char)cp);
                    else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xF0 | (cp >> 18)));
                        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: fail("unknown escape");
            }
        }
        if (i >= s.size()) fail("unterminated string");
        ++i;   // closing quote
        return out;
    }

    Value array() {
        expect('[');
        Array out;
        ws();
        if (peek() == ']') { ++i; return Value::make_array(std::move(out)); }
        for (;;) {
            out.push_back(value());
            ws();
            if (peek() == ',') { ++i; continue; }
            expect(']');
            break;
        }
        return Value::make_array(std::move(out));
    }

    Value object() {
        expect('{');
        std::vector<Member> out;      // INSERTION ORDER, as a Python dict
        ws();
        if (peek() == '}') { ++i; return Value::make_object(std::move(out)); }
        for (;;) {
            ws();
            std::string k = string();
            ws();
            expect(':');
            out.emplace_back(std::move(k), value());
            ws();
            if (peek() == ',') { ++i; continue; }
            expect('}');
            break;
        }
        return Value::make_object(std::move(out));
    }
};

}  // namespace

Value parse(const std::string& text) {
    Parser p{text};
    Value v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing content");
    return v;
}

Value parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("json: cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

}  // namespace kp::json
