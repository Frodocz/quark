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

class ConfigValue;

// Restricted domain types for users of quark: bool, int64_t, double, string, array
using ConfigArray = std::vector<ConfigValue>; 
using BasicConfigValue = std::variant<bool, int64_t, double, std::string, ConfigArray>;

// Proxy node enabling chain operator[] without exposing toml::node_view
class ConfigValue {
public:
    ConfigValue() = default;
    explicit ConfigValue(BasicConfigValue val) : val_(std::move(val)) {}

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
    std::optional<BasicConfigValue> val_;
};

// PImpl to separate the toml::table dependency
class QUARK_API Config {
public:
    Config();
    ~Config() = default;

    Config(const Config& other) = delete;
    Config& operator=(const Config& other) = delete;

    Config(Config&& other) noexcept = default;
    Config& operator=(Config&& other) noexcept = default;

    // Factory methods for loading configs
    [[nodiscard]] static std::optional<Config> load_file(std::string_view file_path) noexcept;
    [[nodiscard]] static std::optional<Config> load_string(std::string_view str) noexcept;

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

    // Chaining subscript operator: cfg["A"]["B"]["C"]
    [[nodiscard]] ConfigValue operator[](std::string_view path) const noexcept {
        auto val = get_impl(path);
        return val.value_or(ConfigValue{});
    }

private:
    [[nodiscard]] std::optional<ConfigValue> get_impl(std::string_view path) const noexcept;

    struct ConfigImpl;
    std::unique_ptr<ConfigImpl> impl_;
};

QUARK_END_NAMESPACE
