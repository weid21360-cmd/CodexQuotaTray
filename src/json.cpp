#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cqt::json {
namespace {

const std::string kEmptyString;
const Value::Array kEmptyArray;
const Value::Object kEmptyObject;

void append_utf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult run() {
        skip_ws();
        Value value = parse_value();
        if (error_.empty()) {
            skip_ws();
            if (pos_ != text_.size()) fail("unexpected trailing data");
        }
        return {std::move(value), error_, error_pos_};
    }

private:
    Value parse_value() {
        if (pos_ >= text_.size()) {
            fail("expected a value");
            return {};
        }
        switch (text_[pos_]) {
        case 'n': return parse_literal("null", Value(nullptr));
        case 't': return parse_literal("true", Value(true));
        case 'f': return parse_literal("false", Value(false));
        case '"': return Value(parse_string());
        case '[': return parse_array();
        case '{': return parse_object();
        default:
            if (text_[pos_] == '-' || (text_[pos_] >= '0' && text_[pos_] <= '9')) return parse_number();
            fail("unexpected character");
            return {};
        }
    }

    Value parse_literal(std::string_view literal, Value value) {
        if (text_.substr(pos_, literal.size()) != literal) {
            fail("invalid literal");
            return {};
        }
        pos_ += literal.size();
        return value;
    }

    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size()) {
                fail("incomplete unicode escape");
                return 0xfffd;
            }
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else {
                fail("invalid unicode escape");
                return 0xfffd;
            }
        }
        return value;
    }

    std::string parse_string() {
        std::string result;
        ++pos_;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return result;
            if (c < 0x20) {
                fail("control character in string");
                return {};
            }
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("incomplete escape");
                return {};
            }
            switch (text_[pos_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = parse_hex4();
                if (cp >= 0xd800 && cp <= 0xdbff && error_.empty()) {
                    if (pos_ + 2 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        const std::uint32_t low = parse_hex4();
                        if (low >= 0xdc00 && low <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        } else {
                            fail("invalid unicode surrogate pair");
                        }
                    } else {
                        fail("missing unicode surrogate pair");
                    }
                }
                append_utf8(result, cp);
                break;
            }
            default: fail("invalid escape"); return {};
            }
        }
        fail("unterminated string");
        return {};
    }

    Value parse_number() {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) {
            fail("invalid number");
            return {};
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            fail("invalid number");
            return {};
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const std::size_t fraction = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (fraction == pos_) fail("invalid fraction");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const std::size_t exponent = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (exponent == pos_) fail("invalid exponent");
        }
        if (!error_.empty()) return {};
        std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) {
            fail("number outside supported range");
            return {};
        }
        return Value(value);
    }

    Value parse_array() {
        Value::Array result;
        ++pos_;
        skip_ws();
        if (consume(']')) return Value(std::move(result));
        while (error_.empty()) {
            skip_ws();
            result.push_back(parse_value());
            skip_ws();
            if (consume(']')) break;
            if (!consume(',')) {
                fail("expected ',' or ']'");
                break;
            }
        }
        return Value(std::move(result));
    }

    Value parse_object() {
        Value::Object result;
        ++pos_;
        skip_ws();
        if (consume('}')) return Value(std::move(result));
        while (error_.empty()) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected object key");
                break;
            }
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) {
                fail("expected ':'");
                break;
            }
            skip_ws();
            result.insert_or_assign(std::move(key), parse_value());
            skip_ws();
            if (consume('}')) break;
            if (!consume(',')) {
                fail("expected ',' or '}'");
                break;
            }
        }
        return Value(std::move(result));
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void fail(std::string message) {
        if (!error_.empty()) return;
        error_ = std::move(message);
        error_pos_ = pos_;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
    std::size_t error_pos_ = 0;
};

void append_indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth * 2), ' ');
}

void write_string(std::string& out, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0xf]);
                out.push_back(kHex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void write_value(std::string& out, const Value& value, bool pretty, int depth) {
    if (value.is_null()) {
        out += "null";
    } else if (value.is_bool()) {
        out += value.as_bool() ? "true" : "false";
    } else if (value.is_number()) {
        const double number = value.as_number();
        constexpr double kInt64Minimum = -9223372036854775808.0;
        constexpr double kInt64MaximumExclusive = 9223372036854775808.0;
        if (number >= kInt64Minimum && number < kInt64MaximumExclusive) {
            const auto integer = static_cast<std::int64_t>(number);
            if (static_cast<double>(integer) == number) {
                out += std::to_string(integer);
                return;
            }
        }
        std::ostringstream stream;
        stream << std::setprecision(15) << number;
        out += stream.str();
    } else if (value.is_string()) {
        write_string(out, value.as_string());
    } else if (value.is_array()) {
        const auto& array = value.as_array();
        out.push_back('[');
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (i) out.push_back(',');
            if (pretty) { out.push_back('\n'); append_indent(out, depth + 1); }
            write_value(out, array[i], pretty, depth + 1);
        }
        if (pretty && !array.empty()) { out.push_back('\n'); append_indent(out, depth); }
        out.push_back(']');
    } else {
        const auto& object = value.as_object();
        out.push_back('{');
        std::size_t index = 0;
        for (const auto& [key, member] : object) {
            if (index++) out.push_back(',');
            if (pretty) { out.push_back('\n'); append_indent(out, depth + 1); }
            write_string(out, key);
            out += pretty ? ": " : ":";
            write_value(out, member, pretty, depth + 1);
        }
        if (pretty && !object.empty()) { out.push_back('\n'); append_indent(out, depth); }
        out.push_back('}');
    }
}

} // namespace

bool Value::as_bool(bool fallback) const {
    if (const auto* value = std::get_if<bool>(&data_)) return *value;
    return fallback;
}

double Value::as_number(double fallback) const {
    if (const auto* value = std::get_if<double>(&data_)) return *value;
    return fallback;
}

std::int64_t Value::as_int64(std::int64_t fallback) const {
    if (const auto* value = std::get_if<double>(&data_)) {
        constexpr double kInt64Minimum = -9223372036854775808.0;
        constexpr double kInt64MaximumExclusive = 9223372036854775808.0;
        if (*value >= kInt64Minimum && *value < kInt64MaximumExclusive) {
            return static_cast<std::int64_t>(*value);
        }
    }
    return fallback;
}

const std::string& Value::as_string() const {
    if (const auto* value = std::get_if<std::string>(&data_)) return *value;
    return kEmptyString;
}

std::string Value::string_or(std::string fallback) const {
    if (const auto* value = std::get_if<std::string>(&data_)) return *value;
    return fallback;
}

const Value::Array& Value::as_array() const {
    if (const auto* value = std::get_if<Array>(&data_)) return *value;
    return kEmptyArray;
}

Value::Array& Value::as_array() {
    if (!is_array()) data_ = Array{};
    return std::get<Array>(data_);
}

const Value::Object& Value::as_object() const {
    if (const auto* value = std::get_if<Object>(&data_)) return *value;
    return kEmptyObject;
}

Value::Object& Value::as_object() {
    if (!is_object()) data_ = Object{};
    return std::get<Object>(data_);
}

const Value* Value::find(std::string_view key) const {
    if (const auto* object = std::get_if<Object>(&data_)) {
        const auto iterator = object->find(key);
        if (iterator != object->end()) return &iterator->second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (auto* object = std::get_if<Object>(&data_)) {
        const auto iterator = object->find(key);
        if (iterator != object->end()) return &iterator->second;
    }
    return nullptr;
}

const Value* Value::at_path(std::initializer_list<std::string_view> keys) const {
    const Value* current = this;
    for (const auto key : keys) {
        current = current ? current->find(key) : nullptr;
        if (!current) break;
    }
    return current;
}

Value& Value::operator[](std::string key) {
    return as_object()[std::move(key)];
}

ParseResult parse(std::string_view text) {
    return Parser(text).run();
}

std::string stringify(const Value& value, bool pretty) {
    std::string output;
    output.reserve(256);
    write_value(output, value, pretty, 0);
    return output;
}

} // namespace cqt::json
