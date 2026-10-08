#define QUARK_LOG_COMPONENT "quark.config"

#include "quark/QuarkConfig.h"

#include "detail/InternalLog.h"
#include "toml++/toml.hpp"

QUARK_BEGIN_NAMESPACE

struct QuarkConfig::QuarkConfigImpl {
    toml::table tbl_;

    static QuarkConfigValue convert_node(const toml::node& node) {
        if (auto b = node.as_boolean()) {
            return QuarkConfigValue{ BasicQuarkConfigValue{ b->get() } };
        }
        if (auto i = node.as_integer()) {
            return QuarkConfigValue{ BasicQuarkConfigValue{ static_cast<int64_t>(i->get()) } };
        }
        if (auto d = node.as_floating_point()) {
            return QuarkConfigValue{ BasicQuarkConfigValue{ d->get() } };
        }
        if (auto s = node.as_string()) {
            return QuarkConfigValue{ BasicQuarkConfigValue{ s->get() } };
        }
        if (auto arr = node.as_array()) {
            QuarkConfigArray vec;
            vec.reserve(arr->size());
            for (auto&& elem : *arr) {
                vec.push_back(convert_node(elem));
            }
            return QuarkConfigValue{ BasicQuarkConfigValue{ std::move(vec) } };
        }
        return QuarkConfigValue{};
    }
};

QuarkConfig::QuarkConfig() : impl_(std::make_unique<QuarkConfigImpl>()) {}
QuarkConfig::~QuarkConfig() = default;
QuarkConfig::QuarkConfig(QuarkConfig&& other) noexcept = default;
QuarkConfig& QuarkConfig::operator=(QuarkConfig&& other) noexcept = default;

std::optional<QuarkConfig> QuarkConfig::load_file(std::string_view file_path) noexcept {
    try {
        toml::table tbl = toml::parse_file(file_path);
        QuarkConfig cfg;
        cfg.impl_->tbl_ = std::move(tbl);
        return cfg;
    } catch (const toml::parse_error& e) {
        QUARK_LOG_WARN("failed to load config file {}:{}:{}: {}", file_path, e.source().begin.line,
                       e.source().begin.column, e.description());
        return std::nullopt;
    } catch (...) {
        QUARK_LOG_WARN("failed to load config file {}", file_path);
        return std::nullopt;
    }
}

std::optional<QuarkConfig> QuarkConfig::load_string(std::string_view str) noexcept {
    try {
        toml::table tbl = toml::parse(str);
        QuarkConfig cfg;
        cfg.impl_->tbl_ = std::move(tbl);
        return cfg;
    } catch (const toml::parse_error& e) {
        QUARK_LOG_WARN("failed to parse config string at {}:{}: {}", e.source().begin.line,
                       e.source().begin.column, e.description());
        return std::nullopt;
    } catch (...) {
        QUARK_LOG_WARN("failed to parse config string");
        return std::nullopt;
    }
}

std::vector<std::string> QuarkConfig::table_names(std::string_view path) const {
    std::vector<std::string> names;
    if (!impl_) return names;

    const toml::table* tbl = path.empty() ? &impl_->tbl_ : toml::at_path(impl_->tbl_, path).as_table();
    if (!tbl) return names;

    for (auto&& [key, node] : *tbl) {
        if (node.is_table()) {
            names.emplace_back(key.str());
        }
    }
    return names;
}

std::optional<QuarkConfigValue> QuarkConfig::get_impl(std::string_view path) const noexcept {
    if (!impl_) return std::nullopt;

    auto node = toml::at_path(impl_->tbl_, path);
    if (!node) {
        return std::nullopt;
    }

    return QuarkConfigImpl::convert_node(*node.node());
}

QUARK_END_NAMESPACE
