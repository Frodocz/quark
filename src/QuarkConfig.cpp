#include "quark/QuarkConfig.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <variant>

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
        if (auto tbl = node.as_table()) {
            QuarkConfigTable members;
            members.reserve(tbl->size());
            for (auto&& [key, elem] : *tbl) {
                members.emplace_back(std::string{key.str()}, convert_node(elem));
            }
            return QuarkConfigValue{ BasicQuarkConfigValue{ std::move(members) } };
        }
        return QuarkConfigValue{};
    }

    // toml++ throws parse_error when exceptions are enabled and returns a parse_result otherwise,
    // on_error gets the parse_error, or nullptr for any other exception
    template <typename Parse, typename OnError>
    static std::optional<QuarkConfig> load(Parse&& parse, OnError&& on_error) noexcept {
#if TOML_EXCEPTIONS
        try {
            QuarkConfig cfg;
            cfg.impl_->tbl_ = parse();
            return cfg;
        } catch (const toml::parse_error& e) {
            on_error(&e);
        } catch (...) {
            on_error(nullptr);
        }
        return std::nullopt;
#else
        toml::parse_result result = parse();
        if (!result) {
            on_error(&result.error());
            return std::nullopt;
        }
        QuarkConfig cfg;
        cfg.impl_->tbl_ = std::move(result).table();
        return cfg;
#endif
    }
};

QuarkConfig::QuarkConfig() : impl_(std::make_unique<QuarkConfigImpl>()) {}
QuarkConfig::~QuarkConfig() = default;
QuarkConfig::QuarkConfig(QuarkConfig&& other) noexcept = default;
QuarkConfig& QuarkConfig::operator=(QuarkConfig&& other) noexcept = default;

// Errors go to stderr, not the logger: configuration is loaded before init_log() (which it
// usually configures), when nothing would print a log record yet, and logging would create the
// quark logger with default sinks before the configuration could set them.
std::optional<QuarkConfig> QuarkConfig::load_file(std::string_view file_path) noexcept {
    return QuarkConfigImpl::load([&] { return toml::parse_file(file_path); }, [&](const toml::parse_error* e) {
        const std::string path{file_path};
        if (e) {
            const std::string what{e->description()};
            std::fprintf(stderr, "quark: failed to load config file %s:%u:%u: %s\n", path.c_str(),
                         static_cast<unsigned>(e->source().begin.line), static_cast<unsigned>(e->source().begin.column),
                         what.c_str());
        } else {
            std::fprintf(stderr, "quark: failed to load config file %s\n", path.c_str());
        }
    });
}

std::optional<QuarkConfig> QuarkConfig::load_string(std::string_view str) noexcept {
    return QuarkConfigImpl::load([&] { return toml::parse(str); }, [&](const toml::parse_error* e) {
        if (e) {
            const std::string what{e->description()};
            std::fprintf(stderr, "quark: failed to parse config string at %u:%u: %s\n",
                         static_cast<unsigned>(e->source().begin.line), static_cast<unsigned>(e->source().begin.column),
                         what.c_str());
        } else {
            std::fprintf(stderr, "quark: failed to parse config string\n");
        }
    });
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

// stderr, like the load errors above: configuration is read before (and for) init_log()
void QuarkConfig::warn_type_mismatch(std::string_view path, const QuarkConfigValue& actual, const char* expected) noexcept {
    static constexpr const char* kNames[] = {"a boolean", "an integer", "a float", "a string", "an array", "a table"};
    static_assert(std::size(kNames) == std::variant_size_v<BasicQuarkConfigValue>);
    const char* name = actual.val_ ? kNames[actual.val_->index()] : "empty";
    std::fprintf(stderr, "quark: config key %.*s is %s, expected %s: using the default\n", static_cast<int>(path.size()),
                 path.data(), name, expected);
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
