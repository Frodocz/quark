#pragma once

#include "quark/Attributes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

QUARK_BEGIN_NAMESPACE

class QuarkConfigValue;

// Restricted domain types for users of Quark: bool, int64_t, double, string, array, table
using QuarkConfigArray = std::vector<QuarkConfigValue>;
using QuarkConfigTable = std::vector<std::pair<std::string, QuarkConfigValue>>;  ///< in key order
using BasicQuarkConfigValue = std::variant<bool, int64_t, double, std::string, QuarkConfigArray, QuarkConfigTable>;

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

    // Type extraction. A double also reads an integer that converts exactly (|i| <= 2^53): TOML
    // writes 5 and 5.0 as different types. Never the other way: a float doesn't read as an integer.
    template <typename T>
    [[nodiscard]] std::optional<T> get() const noexcept {
        if (is<T>()) {
            return std::get<T>(*val_);
        }
        if constexpr (std::is_same_v<T, double>) {
            if (is<int64_t>()) {
                const int64_t i = std::get<int64_t>(*val_);
                constexpr int64_t kExact = int64_t{1} << 53;
                if (i >= -kExact && i <= kExact) return static_cast<double>(i);
            }
        }
        return std::nullopt;
    }

    template <typename T>
    [[nodiscard]] T get_or(T&& default_val) const noexcept {
        if (auto v = get<T>()) {
            return std::move(*v);
        }
        return std::forward<T>(default_val);
    }

    // A table's member, an array's element, or an empty value: v["fills"][0]["qty"]
    [[nodiscard]] QuarkConfigValue operator[](std::string_view key) const {
        if (const auto* t = is<QuarkConfigTable>() ? &std::get<QuarkConfigTable>(*val_) : nullptr) {
            for (const auto& [k, v] : *t) {
                if (k == key) return v;
            }
        }
        return {};
    }
    [[nodiscard]] QuarkConfigValue operator[](std::size_t i) const {
        const auto* a = is<QuarkConfigArray>() ? &std::get<QuarkConfigArray>(*val_) : nullptr;
        return a != nullptr && i < a->size() ? (*a)[i] : QuarkConfigValue{};
    }
    // Elements of an array, members of a table, 0 otherwise
    [[nodiscard]] std::size_t size() const noexcept {
        if (is<QuarkConfigArray>()) return std::get<QuarkConfigArray>(*val_).size();
        if (is<QuarkConfigTable>()) return std::get<QuarkConfigTable>(*val_).size();
        return 0;
    }

private:
    std::optional<BasicQuarkConfigValue> val_;

    friend class QuarkConfig;  // names the actual type in its type-mismatch warning
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

    // Dot-path lookup: cfg.get_or("A.B.C") or cfg.get("D.E"); "venues[1].name" indexes arrays (of tables).
    // A double also reads an integer (see QuarkConfigValue::get).
    //
    // get_or: the value, or default_val when the key is missing. A key that is there with another type
    // (cpu = "3" for an integer) also gives default_val, and says so on stderr, naming the key and both
    // types: a typed value silently replaced by the default is a wrong parameter nobody notices.
    template <typename T>
    [[nodiscard]] T get_or(std::string_view full_path, T&& default_val) const noexcept {
        auto val = get_impl(full_path);
        if (val) {
            if (auto v = val->template get<T>()) {
                return std::move(*v);
            }
            warn_type_mismatch(full_path, *val, type_name<T>());
        }
        return std::forward<T>(default_val);
    }

    // get: std::nullopt when the key is missing or has another type, without a warning (so a caller
    // can try one type, then another)
    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view path) const noexcept {
        auto val = get_impl(path);
        if (val) {
            return val->template get<T>();
        }
        return std::nullopt;
    }

    // Names of the tables nested directly under `path` ("" for the root), in key order.
    // e.g. "log" -> {"app", "net"} for [log.app] and [log.net]
    [[nodiscard]] std::vector<std::string> table_names(std::string_view path) const;

    // Number of elements of the array at `path` (e.g. [[venues]]), 0 if it's not an array
    [[nodiscard]] std::size_t array_size(std::string_view path) const noexcept { return (*this)[path].size(); }

    // The value at a path, then chained: cfg["A.B"]["C"], cfg["venues"][0]["name"]
    [[nodiscard]] QuarkConfigValue operator[](std::string_view path) const noexcept {
        auto val = get_impl(path);
        return val.value_or(QuarkConfigValue{});
    }

private:
    [[nodiscard]] std::optional<QuarkConfigValue> get_impl(std::string_view path) const noexcept;

    template <typename T>
    static constexpr const char* type_name() noexcept {
        if constexpr (std::is_same_v<T, bool>) return "a boolean";
        else if constexpr (std::is_same_v<T, int64_t>) return "an integer";
        else if constexpr (std::is_same_v<T, double>) return "a float";
        else if constexpr (std::is_same_v<T, std::string>) return "a string";
        else if constexpr (std::is_same_v<T, QuarkConfigArray>) return "an array";
        else return "a table";
    }
    static void warn_type_mismatch(std::string_view path, const QuarkConfigValue& actual, const char* expected) noexcept;

    struct QuarkConfigImpl;
    std::unique_ptr<QuarkConfigImpl> impl_;
};

QUARK_END_NAMESPACE
