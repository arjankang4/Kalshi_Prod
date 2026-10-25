#include "kp/storage/book.hpp"

#include "kp/storage/units.hpp"

namespace kp::storage {

namespace {

using kp::json::Value;

bool truthy(const Value& v) {
    using T = Value::Type;
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

std::map<std::int64_t, std::int64_t>& side_of(Book& b, const Value& name) {
    if (!name.is_string())
        throw PyError("TypeError", "attribute name must be string, not '" + py_type_name(name) + "'");
    if (name.str() == "yes") return b.yes;
    if (name.str() == "no") return b.no;
    throw PyError("AttributeError", "'Book' object has no attribute " + py_repr_str(name.str()));
}

const Value& key(const Value& msg, const char* k) {
    const Value* v = msg.find(k);
    if (!v) throw PyError("KeyError", py_repr_str(k));
    return *v;
}

// `for px, sz in levels:` -- the iteration and unpacking errors CPython raises.
void unpack2(const Value& lv, const Value*& a, const Value*& b, Value& tmp_a, Value& tmp_b) {
    using T = Value::Type;
    if (lv.type() == T::Array) {
        const auto n = lv.arr().size();
        if (n < 2) throw PyError("ValueError", "not enough values to unpack (expected 2, got " + std::to_string(n) + ")", true);
        if (n > 2) throw PyError("ValueError", "too many values to unpack (expected 2)", true);
        a = &lv.arr()[0];
        b = &lv.arr()[1];
        return;
    }
    if (lv.type() == T::String) {                      // a str unpacks by character
        const std::string& s = lv.str();
        std::size_t n = 0, i = 0, cut = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (++n == 1) cut = i;
        }
        if (n < 2) throw PyError("ValueError", "not enough values to unpack (expected 2, got " + std::to_string(n) + ")", true);
        if (n > 2) throw PyError("ValueError", "too many values to unpack (expected 2)", true);
        tmp_a = Value::make_string(s.substr(0, cut));
        tmp_b = Value::make_string(s.substr(cut));
        a = &tmp_a;
        b = &tmp_b;
        return;
    }
    if (lv.type() == T::Object) {                      // a dict unpacks by key
        const auto n = lv.obj().size();
        if (n < 2) throw PyError("ValueError", "not enough values to unpack (expected 2, got " + std::to_string(n) + ")", true);
        if (n > 2) throw PyError("ValueError", "too many values to unpack (expected 2)", true);
        tmp_a = Value::make_string(lv.obj()[0].first);
        tmp_b = Value::make_string(lv.obj()[1].first);
        a = &tmp_a;
        b = &tmp_b;
        return;
    }
    throw PyError("TypeError", "cannot unpack non-iterable " + py_type_name(lv) + " object");
}

}  // namespace

void Book::load(const Value& msg, std::optional<std::int64_t> recv_ns, std::optional<std::int64_t> seq) {
    yes.clear();
    no.clear();
    for (int s = 0; s < 2; ++s) {
        auto& side = s == 0 ? yes : no;
        const Value* lv = msg.find(s == 0 ? "yes_dollars_fp" : "no_dollars_fp");
        if (!lv || !truthy(*lv)) continue;
        using T = Value::Type;
        std::vector<Value> chars;                       // iterating a str / dict yields str items
        const std::vector<Value>* items = nullptr;
        if (lv->type() == T::Array) items = &lv->arr();
        else if (lv->type() == T::String || lv->type() == T::Object) {
            if (lv->type() == T::String) {
                const std::string& str = lv->str();
                for (std::size_t i = 0; i < str.size();) {
                    const unsigned char c = (unsigned char)str[i];
                    const std::size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    chars.push_back(Value::make_string(str.substr(i, k)));
                    i += k;
                }
            } else {
                for (const auto& m : lv->obj()) chars.push_back(Value::make_string(m.first));
            }
            items = &chars;
        } else {
            throw PyError("TypeError", "'" + py_type_name(*lv) + "' object is not iterable");
        }
        for (const Value& level : *items) {
            const Value *px, *sz;
            Value ta, tb;
            unpack2(level, px, sz, ta, tb);
            // `side[to_micros(px)] = to_cc(sz)`: CPython evaluates the VALUE before the subscript.
            const auto c = units::to_cc(*sz);
            const auto p = units::to_micros(*px);
            if (!c || !p)                                // None key/size: see the header note
                throw PyError("TypeError", "None price or size in a snapshot level (unsupported)");
            side[*p] = *c;
        }
    }
    snapshot_ns = recv_ns;
    snapshot_seq = seq;
    n_deltas = 0;
}

void Book::apply(const Value& msg) {
    const Value& side_name = key(msg, "side");
    auto& side = side_of(*this, side_name);
    const auto p = units::to_micros(key(msg, "price_dollars"));
    const auto d = units::to_cc(key(msg, "delta_fp"));
    if (!p) throw PyError("TypeError", "None price in a delta (unsupported)");
    if (!d) throw PyError("TypeError", "unsupported operand type(s) for +: 'int' and 'NoneType'");
    const auto it = side.find(*p);
    const std::int64_t now = it == side.end() ? 0 : it->second;
    const std::int64_t nw = now + *d;
    if (nw < 0) {
        const Value* t = msg.find("market_ticker");
        throw PyError("ValueError", "negative size " + std::to_string(nw) + " at " + std::to_string(*p) +
                                        " on " + side_name.str() + " (" + (t ? py_str(*t) : "None") +
                                        ") -- a delta was missed", true);
    }
    if (nw == 0) {
        if (it != side.end()) side.erase(it);
    } else {
        side[*p] = nw;
    }
    ++n_deltas;
}

std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>> Book::best() const {
    std::optional<std::int64_t> bb, ba;
    if (!yes.empty()) bb = yes.rbegin()->first;
    if (!no.empty()) ba = 1000000 - no.rbegin()->first;
    return {bb, ba};
}

}  // namespace kp::storage
