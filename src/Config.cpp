#include "quark/Config.h"

#include "toml++/toml.hpp"

QUARK_BEGIN_NAMESPACE

struct Config::ConfigImpl {
    toml::table tbl_;

    static ConfigValue convert_node(const toml::node& node) {
        if (auto b = node.as_boolean()) {
            return ConfigValue{ BasicConfigValue{ b->get() } };
        }
        if (auto i = node.as_integer()) {
            return ConfigValue{ BasicConfigValue{ static_cast<int64_t>(i->get()) } };
        }
        if (auto d = node.as_floating_point()) {
            return ConfigValue{ BasicConfigValue{ d->get() } };
        }
        if (auto s = node.as_string()) {
            return ConfigValue{ BasicConfigValue{ s->get() } };
        }
        if (auto arr = node.as_array()) {
            ConfigArray vec;
            vec.reserve(arr->size());
            for (auto&& elem : *arr) {
                vec.push_back(convert_node(elem));
            }
            return ConfigValue{ BasicConfigValue{ std::move(vec) } };
        }
        return ConfigValue{};
    }
};

Config::Config() : impl_(std::make_unique<ConfigImpl>()) {}

std::optional<Config> Config::load_file(std::string_view file_path) noexcept {
    try {
        toml::table tbl = toml::parse_file(file_path);
        Config cfg;
        cfg.impl_->tbl_ = std::move(tbl);
        return cfg;
    } catch(...) {
        return std::nullopt;
    }
}

std::optional<Config> Config::load_string(std::string_view str) noexcept {
    try {
        toml::table tbl = toml::parse(str);
        Config cfg;
        cfg.impl_->tbl_ = std::move(tbl);
        return cfg;
    } catch(...) {
        return std::nullopt;
    }
}

std::optional<ConfigValue> Config::get_impl(std::string_view path) const noexcept {
    if (!impl_) return std::nullopt;

    auto node = toml::at_path(impl_->tbl_, path);
    if (!node) {
        return std::nullopt;
    }

    return ConfigImpl::convert_node(*node.node());
}

QUARK_END_NAMESPACE
