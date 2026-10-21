#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace asymsched {
namespace json {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& message)
        : std::runtime_error(message) {}
};

enum class Type { Null, Boolean, Number, String, Array, Object };

/// A small, dependency-free JSON value used by the compatibility API.
///
/// Integers retain their literal form so the planner can distinguish an
/// integer access count from a floating-point JSON number.
class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_boolean() const { return type_ == Type::Boolean; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_integer_literal() const {
        return type_ == Type::Number && integer_literal_;
    }

    bool boolean() const;
    double number() const;
    std::uint64_t as_u64() const;
    const std::string& string() const;
    const Array& array() const;
    const Object& object() const;

    bool contains(const std::string& key) const;
    const Value& at(const std::string& key) const;

    static Value make_null();
    static Value make_boolean(bool value);
    static Value make_number(double value);
    static Value make_integer(std::uint64_t value);
    static Value make_string(std::string value);
    static Value make_array(Array value = {});
    static Value make_object(Object value = {});

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    bool integer_literal_ = false;
    std::uint64_t unsigned_integer_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

Value parse(std::string_view text);
std::string dump(const Value& value);

}  // namespace json
}  // namespace asymsched
