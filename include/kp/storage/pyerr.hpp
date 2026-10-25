// The Python exceptions the storage and feed ports have to reproduce BY NAME.
//
// Why by name: several reference paths turn an exception into a string that is recorded and
// reported -- `MarketFeed.pump_error` is f"handle: {type(e).__name__}: {str(e)[:80]}", `FeedTee.error`
// is the same shape -- and a handler that catches `ValueError` treats `PrecisionError` (a subclass)
// differently from `KeyError` or `TypeError`. So an error carries its Python type name and whether
// it IS-A ValueError, and the message text is built exactly as CPython builds it.
#pragma once
#include <optional>
#include <stdexcept>
#include <string>

#include "kp/json/value.hpp"

namespace kp::storage {

class PyError : public std::runtime_error {
public:
    PyError(std::string type_name, const std::string& message, bool value_error = false)
        : std::runtime_error(message), type(std::move(type_name)), is_value_error(value_error) {}
    std::string type;          // type(e).__name__
    bool is_value_error;       // isinstance(e, ValueError)
};

// repr() of a Python str whose UTF-8 bytes are `s` (quotes chosen as CPython chooses them).
std::string py_repr_str(const std::string& s);
// str() of a JSON-decoded Python value: None, True, 12, 1.5, 'x' stays x, [1, 'a'], {'k': None}.
std::string py_str(const kp::json::Value& v);
// repr() of the same (a str is quoted).
std::string py_repr(const kp::json::Value& v);
// type(v).__name__ for a JSON-decoded value: NoneType, bool, int, float, str, list, dict.
std::string py_type_name(const kp::json::Value& v);
// s[:n] on a Python str: n CODE POINTS of the UTF-8 in `s`, not n bytes.
std::string py_slice(const std::string& s, std::size_t n);

// float(v) for a JSON-decoded value, as CPython does it. Throws PyError ValueError/TypeError with
// CPython's message ("could not convert string to float: 'banana'").
double py_float(const kp::json::Value& v);

}  // namespace kp::storage
