#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <utility>

namespace cqt::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool value) : data_(value) {}
    Value(int value) : data_(static_cast<double>(value)) {}
    Value(std::int64_t value) : data_(static_cast<double>(value)) {}
    Value(double value) : data_(value) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(Array value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(data_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(data_); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(data_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(data_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(data_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(data_); }

    [[nodiscard]] bool as_bool(bool fallback = false) const;
    [[nodiscard]] double as_number(double fallback = 0.0) const;
    [[nodiscard]] std::int64_t as_int64(std::int64_t fallback = 0) const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] std::string string_or(std::string fallback = {}) const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] Value* find(std::string_view key);
    [[nodiscard]] const Value* at_path(std::initializer_list<std::string_view> keys) const;

    Value& operator[](std::string key);

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_{nullptr};
};

struct ParseResult {
    Value value;
    std::string error;
    std::size_t error_offset = 0;

    [[nodiscard]] explicit operator bool() const { return error.empty(); }
};

[[nodiscard]] ParseResult parse(std::string_view text);
[[nodiscard]] std::string stringify(const Value& value, bool pretty = false);

} // namespace cqt::json
