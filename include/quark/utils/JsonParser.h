#pragma once

#include "quark/Attributes.h"

#include "simdjson.h"
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

class QUARK_API JsonParser {
public:
    JsonParser() = default;
    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;

    simdjson::ondemand::document parse_json(std::string_view str) {
        json_buf_.reserve(str.size() + simdjson::SIMDJSON_PADDING);
        json_buf_ = str;
        return json_parser_.iterate(simdjson::padded_string_view(json_buf_));
    }

    simdjson::ondemand::document parse_json(const std::string& str) {
        return parse_json(str.data(), str.size(), str.capacity());
    }

    simdjson::ondemand::document parse_json(const char* data, size_t size, size_t capacity) {
        if (capacity >= size + simdjson::SIMDJSON_PADDING) {
            return json_parser_.iterate(simdjson::padded_string_view(data, size, capacity));
        } else {
            return parse_json(std::string_view{data, size});
        }
    }
private:
    simdjson::ondemand::parser json_parser_;
    std::string json_buf_;
};

QUARK_END_NAMESPACE
