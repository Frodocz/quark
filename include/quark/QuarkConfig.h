#pragma once

#include "quark/Attributes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

QUARK_BEGIN_NAMESPACE

class QuarkConfigValue;

// Restricted domain types for users of Quark: bool, int64_t, double, string, array
using QuarkConfigArray = std::vector<QuarkConfigValue>; 
using BasicQuarkConfigValue = std::variant<bool, int64_t, double, std::string, QuarkConfigArray>;

// Proxy node enabling chain operator[] without exposing toml::node_view
class QuarkConfigValue {
public:
    QuarkConfigValue() = default;
    explicit QuarkConfigValue(BasicQuarkConfigValue val) : val_(std::move(val)) {}

    [[nodiscard]] bool has_value() const noexcept { return val_.has_value(); }

    // Check internal type
    template <typename T>
    [[nodiscard]] bool is() const noexcept {
        return val_.has_value() && std::holds_alternative<T>(*val_);
    }

    // Type extraction
    template <typename T>
    [[nodiscard]] std::optional<T> get() const noexcept {
        if (is<T>()) {
            return std::get<T>(*val_);
        }
        return std::nullopt;
    }

    template <typename T>
    [[nodiscard]] T get_or(T&& default_val) const noexcept {
        if (is<T>()) {
            return std::get<T>(*val_);
        }
        return std::forward<T>(default_val);
    }
private:
    std::optional<BasicQuarkConfigValue> val_;
};

// PImpl to separate the toml::table dependency
class QUARK_API QuarkConfig {
public:
    QuarkConfig();
    ~QuarkConfig();

    QuarkConfig(const QuarkConfig& other) = delete;
    QuarkConfig& operator=(const QuarkConfig& other) = delete;

    // Defined out-of-line: QuarkConfigImpl is incomplete here
    QuarkConfig(QuarkConfig&& other) noexcept;
    QuarkConfig& operator=(QuarkConfig&& other) noexcept;

    // Factory methods for loading configs
    [[nodiscard]] static std::optional<QuarkConfig> load_file(std::string_view file_path) noexcept;
    [[nodiscard]] static std::optional<QuarkConfig> load_string(std::string_view str) noexcept;

    // Dot-path lookup: cfg.get_or("A.B.C") or cfg.get("D.E")
    template <typename T>
    [[nodiscard]] T get_or(std::string_view full_path, T&& default_val) const noexcept {
        auto val = get_impl(full_path);
        if (val && val->is<T>()) {
            return *val->template get<T>();
        }
        return std::forward<T>(default_val);
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view path) const noexcept {
        auto val = get_impl(path);
        if (val && val->is<T>()) {
            return val->template get<T>();
        }
        return std::nullopt;
    }

    // Names of the tables nested directly under `path` ("" for the root), in key order.
    // e.g. "log" -> {"app", "net"} for [log.app] and [log.net]
    [[nodiscard]] std::vector<std::string> table_names(std::string_view path) const;

    // Chaining subscript operator: cfg["A"]["B"]["C"]
    [[nodiscard]] QuarkConfigValue operator[](std::string_view path) const noexcept {
        auto val = get_impl(path);
        return val.value_or(QuarkConfigValue{});
    }

private:
    [[nodiscard]] std::optional<QuarkConfigValue> get_impl(std::string_view path) const noexcept;

    struct QuarkConfigImpl;
    std::unique_ptr<QuarkConfigImpl> impl_;
};

QUARK_END_NAMESPACE
