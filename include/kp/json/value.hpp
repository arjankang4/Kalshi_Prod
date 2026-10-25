// A minimal JSON reader for the model artifacts and API payloads.
//
// Hand-rolled rather than vendored, for one reason that outweighs the rest: NUMBER PARSING MUST
// GO THROUGH strtod. Python writes doubles as shortest round-trip decimal, and only a correctly
// rounded parser recovers the identical bits. A hand-rolled `digit * 10 + ...` accumulator loses
// ulps, and those ulps land directly in varcal.phi -> var_left -> theta -> R -> the required
// edge. RapidJSON's default parser is NOT correctly rounded either (it needs
// kParseFullPrecisionFlag), so vendoring would not have removed this decision, only hidden it.
//
// Object members keep INSERTION ORDER. Python dicts are insertion-ordered and the live path
// iterates them in places where order reaches the wire (strategy/orders.py:176 -> batch
// chunking), so a std::map or std::unordered_map here would silently reorder.
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kp::json {

class Value;
using Array  = std::vector<Value>;
using Member = std::pair<std::string, Value>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() : type_(Type::Null) {}

    Type type() const { return type_; }
    bool is_null()   const { return type_ == Type::Null; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array()  const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_bool()   const { return type_ == Type::Bool; }

    // Python keeps int and float apart and so must we: json.dumps writes 0 and 0.0 differently,
    // and a nanosecond timestamp (~1.8e18) is not representable in a double. A number parsed
    // without '.', 'e' or 'E' that fits int64 is an INT: num() still returns it as a double, and
    // integer() returns it exactly.
    bool is_int() const { return type_ == Type::Number && is_int_; }
    long long integer() const { return is_int_ ? int_ : (long long)num_; }

    double             num() const;
    bool               boolean() const;
    const std::string& str() const;
    const Array&       arr() const;
    const std::vector<Member>& obj() const;

    // Absent key -> nullptr. Null and absent are DIFFERENT: Python distinguishes a missing key
    // from a key whose value is None, and several artifact readers branch on that.
    const Value* find(const std::string& key) const;

    // Convenience for artifact reads. Returns nullopt for absent OR json null, which matches
    // `art.get("x")` returning None either way.
    std::optional<double>      opt_num(const std::string& key) const;
    std::optional<std::string> opt_str(const std::string& key) const;
    std::vector<double>        num_array(const std::string& key) const;

    static Value make_number(double d);          // a Python float
    static Value make_int(long long i);          // a Python int
    static Value make_string(std::string s);
    static Value make_bool(bool b);
    static Value make_array(Array a);
    static Value make_object(std::vector<Member> m);

    // Building values, dict-style: set() replaces an existing key IN PLACE (insertion order is
    // kept, as a dict's is) and appends a new one; push() appends to an array. Both convert a
    // Null value to the container type first, so `Value v; v.set("a", ...)` works.
    Value& set(const std::string& key, Value v);
    Value& push(Value v);
    Value* find_mut(const std::string& key);

private:
    Type type_;
    bool is_int_ = false;
    long long int_ = 0;
    double num_ = 0.0;
    bool bool_ = false;
    std::string str_;
    Array arr_;
    std::vector<Member> obj_;
};

// json.dumps, byte for byte. `item_sep`/`key_sep` are Python's `separators`; the defaults are
// json.dumps' own (", ", ": "). `indent` < 0 is None; >= 0 reproduces json.dumps(indent=n), which
// switches the item separator to "," and puts each element on its own line. ensure_ascii is
// always on, as it is at every call site on the live path: non-ASCII leaves as \uXXXX escapes.
struct DumpStyle {
    const char* item_sep = ", ";
    const char* key_sep = ": ";
    int indent = -1;
};
inline constexpr DumpStyle COMPACT{",", ":", -1};
std::string dumps(const Value& v, const DumpStyle& style = DumpStyle{});

// repr() of the same value as Python would print it -- for log lines where the reference formats a
// dict or list with an f-string ({'cold': 26}, ['advanced']) rather than json.dumps.
std::string py_repr(const Value& v);
std::string py_repr_str(const std::string& s);   // repr() of a str

// Throws std::runtime_error with a byte offset on malformed input.
Value parse(const std::string& text);
Value parse_file(const std::string& path);

}  // namespace kp::json
