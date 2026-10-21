#include "asymsched/json.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace asymsched {
namespace json {
namespace {

std::string format_number(double value) {
    if (!std::isfinite(value)) {
        throw JsonError("cannot serialize a non-finite number");
    }
    if (value == 0.0) {
        return "0";
    }
    if (value == std::trunc(value) &&
        std::abs(value) <= 9007199254740992.0) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(0) << value;
        return stream.str();
    }
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

void dump_string(std::string& output, const std::string& value) {
    output.push_back('"');
    for (unsigned char character : value) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(hex[(character >> 4) & 0x0f]);
                    output.push_back(hex[character & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output.push_back('"');
}

void dump_value(std::string& output, const Value& value) {
    switch (value.type()) {
        case Type::Null:
            output += "null";
            break;
        case Type::Boolean:
            output += value.boolean() ? "true" : "false";
            break;
        case Type::Number:
            if (value.is_integer_literal()) {
                output += std::to_string(value.as_u64());
            } else {
                output += format_number(value.number());
            }
            break;
        case Type::String:
            dump_string(output, value.string());
            break;
        case Type::Array: {
            output.push_back('[');
            bool first = true;
            for (const auto& item : value.array()) {
                if (!first) {
                    output.push_back(',');
                }
                first = false;
                dump_value(output, item);
            }
            output.push_back(']');
            break;
        }
        case Type::Object: {
            output.push_back('{');
            bool first = true;
            for (const auto& entry : value.object()) {
                if (!first) {
                    output.push_back(',');
                }
                first = false;
                dump_string(output, entry.first);
                output.push_back(':');
                dump_value(output, entry.second);
            }
            output.push_back('}');
            break;
        }
    }
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parse_document() {
        skip_whitespace();
        Value result = parse_value(0);
        skip_whitespace();
        if (position_ != text_.size()) {
            fail("unexpected trailing data");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw JsonError(message + " at byte " + std::to_string(position_));
    }

    char peek() const {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    char take() {
        if (position_ >= text_.size()) {
            fail("unexpected end of input");
        }
        return text_[position_++];
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char ch = text_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                break;
            }
            ++position_;
        }
    }

    void expect(char expected) {
        if (take() != expected) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    Value parse_value(int depth) {
        if (depth > 128) {
            fail("maximum nesting depth exceeded");
        }
        skip_whitespace();
        switch (peek()) {
            case 'n':
                expect_literal("null");
                return Value::make_null();
            case 't':
                expect_literal("true");
                return Value::make_boolean(true);
            case 'f':
                expect_literal("false");
                return Value::make_boolean(false);
            case '"':
                ++position_;
                return Value::make_string(parse_string());
            case '[':
                return parse_array(depth + 1);
            case '{':
                return parse_object(depth + 1);
            default:
                return parse_number();
        }
    }

    void expect_literal(const char* literal) {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (take() != *cursor) {
                fail(std::string("invalid literal, expected ") + literal);
            }
        }
    }

    static int hex_value(char ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    }

    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hex_value(take());
            if (digit < 0) {
                fail("invalid \\u escape");
            }
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::string parse_string() {
        std::string output;
        while (true) {
            const char ch = take();
            if (ch == '"') {
                return output;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                fail("unescaped control character in string");
            }
            if (ch != '\\') {
                output.push_back(ch);
                continue;
            }
            const char escape = take();
            switch (escape) {
                case '"':
                    output.push_back('"');
                    break;
                case '\\':
                    output.push_back('\\');
                    break;
                case '/':
                    output.push_back('/');
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex4();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 1 >= text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') {
                            fail("high surrogate without low surrogate");
                        }
                        position_ += 2;
                        const std::uint32_t low = parse_hex4();
                        if (low < 0xdc00 || low > 0xdfff) {
                            fail("invalid low surrogate");
                        }
                        codepoint = 0x10000 +
                                    ((codepoint - 0xd800) << 10) +
                                    (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 &&
                               codepoint <= 0xdfff) {
                        codepoint = 0xfffd;
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default:
                    fail("invalid escape sequence");
            }
        }
    }

    Value parse_number() {
        const std::size_t start = position_;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++position_;
        }
        if (peek() == '0') {
            ++position_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        } else {
            fail("invalid number");
        }

        bool floating = false;
        if (peek() == '.') {
            floating = true;
            ++position_;
            if (peek() < '0' || peek() > '9') {
                fail("invalid number fraction");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            if (peek() < '0' || peek() > '9') {
                fail("invalid number exponent");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }

        const std::string token(text_.substr(start, position_ - start));
        if (!floating) {
            const char* first = token.data() + (negative ? 1 : 0);
            const char* last = token.data() + token.size();
            std::uint64_t magnitude = 0;
            const auto [end, error] =
                std::from_chars(first, last, magnitude);
            if (error != std::errc() || end != last) {
                fail("integer out of range");
            }
            if (negative) {
                return Value::make_number(-static_cast<double>(magnitude));
            }
            return Value::make_integer(magnitude);
        }

        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(parsed)) {
            fail("invalid number");
        }
        return Value::make_number(parsed);
    }

    Value parse_array(int depth) {
        expect('[');
        Value::Array items;
        skip_whitespace();
        if (peek() == ']') {
            ++position_;
            return Value::make_array(std::move(items));
        }
        while (true) {
            items.push_back(parse_value(depth));
            skip_whitespace();
            const char ch = take();
            if (ch == ']') {
                return Value::make_array(std::move(items));
            }
            if (ch != ',') {
                fail("expected ',' or ']' in array");
            }
            skip_whitespace();
        }
    }

    Value parse_object(int depth) {
        expect('{');
        Value::Object object;
        skip_whitespace();
        if (peek() == '}') {
            ++position_;
            return Value::make_object(std::move(object));
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("expected an object key");
            }
            ++position_;
            std::string key = parse_string();
            skip_whitespace();
            expect(':');
            Value value = parse_value(depth);
            object[std::move(key)] = std::move(value);
            skip_whitespace();
            const char ch = take();
            if (ch == '}') {
                return Value::make_object(std::move(object));
            }
            if (ch != ',') {
                fail("expected ',' or '}' in object");
            }
            skip_whitespace();
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

}  // namespace

bool Value::boolean() const {
    if (type_ != Type::Boolean) {
        throw JsonError("expected a boolean");
    }
    return boolean_;
}

double Value::number() const {
    if (type_ != Type::Number) {
        throw JsonError("expected a number");
    }
    return integer_literal_ ? static_cast<double>(unsigned_integer_) : number_;
}

std::uint64_t Value::as_u64() const {
    if (type_ != Type::Number || !integer_literal_) {
        throw JsonError("expected a nonnegative integer");
    }
    return unsigned_integer_;
}

const std::string& Value::string() const {
    if (type_ != Type::String) {
        throw JsonError("expected a string");
    }
    return string_;
}

const Value::Array& Value::array() const {
    if (type_ != Type::Array) {
        throw JsonError("expected an array");
    }
    return array_;
}

const Value::Object& Value::object() const {
    if (type_ != Type::Object) {
        throw JsonError("expected an object");
    }
    return object_;
}

bool Value::contains(const std::string& key) const {
    return type_ == Type::Object && object_.find(key) != object_.end();
}

const Value& Value::at(const std::string& key) const {
    if (type_ != Type::Object) {
        throw JsonError("expected an object");
    }
    const auto it = object_.find(key);
    if (it == object_.end()) {
        throw JsonError("missing key: " + key);
    }
    return it->second;
}

Value Value::make_null() {
    return Value{};
}

Value Value::make_boolean(bool value) {
    Value result;
    result.type_ = Type::Boolean;
    result.boolean_ = value;
    return result;
}

Value Value::make_number(double value) {
    if (!std::isfinite(value)) {
        throw JsonError("cannot create a non-finite JSON number");
    }
    Value result;
    result.type_ = Type::Number;
    result.number_ = value;
    return result;
}

Value Value::make_integer(std::uint64_t value) {
    Value result;
    result.type_ = Type::Number;
    result.integer_literal_ = true;
    result.unsigned_integer_ = value;
    return result;
}

Value Value::make_string(std::string value) {
    Value result;
    result.type_ = Type::String;
    result.string_ = std::move(value);
    return result;
}

Value Value::make_array(Array value) {
    Value result;
    result.type_ = Type::Array;
    result.array_ = std::move(value);
    return result;
}

Value Value::make_object(Object value) {
    Value result;
    result.type_ = Type::Object;
    result.object_ = std::move(value);
    return result;
}

Value parse(std::string_view text) {
    return Parser(text).parse_document();
}

std::string dump(const Value& value) {
    std::string output;
    dump_value(output, value);
    return output;
}

}  // namespace json
}  // namespace asymsched
