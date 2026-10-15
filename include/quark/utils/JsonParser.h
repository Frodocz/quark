#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"

#include "simdjson.h"
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

class QUARK_API JsonParser {
public:
    static constexpr size_t kInitialCapacity = 64 * 1024; // 64KB
    JsonParser() = default;

    // Preallocates the copy buffer and simdjson's internal buffers for documents up to
    // initial_capacity bytes and touches them, so the hot path neither allocates nor page-faults.
    // Larger documents still parse, and both buffers grow to fit them.
    explicit JsonParser(size_t initial_capacity = kInitialCapacity) {
        // parser first: it rejects sizes above its max capacity before json_buf_ reserves anything
        if (auto err = json_parser_.allocate(initial_capacity); err != simdjson::SUCCESS) {
            QUARK_THROW(QuarkError{std::string{"JsonParser failed to preallocate "} + std::to_string(initial_capacity)
                                   + " bytes: " + simdjson::error_message(err)});
        }
        json_buf_.reserve(initial_capacity + simdjson::SIMDJSON_PADDING);
        warm_up(initial_capacity);
    }

    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;

    // Returns simdjson's result type so this compiles with and without exceptions: check error(), or
    // with exceptions convert to ondemand::document, which throws on error. The document stays valid
    // until the next parse_json call.
    simdjson::simdjson_result<simdjson::ondemand::document> parse_json(std::string_view str) {
        // reserve after assign: reserve guarantees capacity >= n afterwards, assign makes no capacity promise
        json_buf_.assign(str);
        json_buf_.reserve(str.size() + simdjson::SIMDJSON_PADDING);
        return json_parser_.iterate(simdjson::padded_string_view(json_buf_.data(), json_buf_.size(), json_buf_.capacity()));
    }

    simdjson::simdjson_result<simdjson::ondemand::document> parse_json(const std::string& str) {
        return parse_json(str.data(), str.size(), str.capacity());
    }

    // the returned document points into str, so a temporary would leave it dangling
    simdjson::simdjson_result<simdjson::ondemand::document> parse_json(std::string&& str) = delete;

    simdjson::simdjson_result<simdjson::ondemand::document> parse_json(const char* data, size_t size, size_t capacity) {
        if (capacity >= size + simdjson::SIMDJSON_PADDING) {
            return json_parser_.iterate(simdjson::padded_string_view(data, size, capacity));
        } else {
            return parse_json(std::string_view{data, size});
        }
    }
private:
    // new[] only reserves address space, the first large document would page-fault on fresh pages.
    // Two synthetic documents of the full capacity fault them in up front: a token-dense array fills
    // the structural index (4 bytes per token) and a long string fills the string buffer. Both are
    // built in json_buf_, so it gets touched too.
    void warm_up(size_t capacity) {
        if (capacity < 3) {
            return;
        }

        // [0,0,...,0], with a trailing space when the capacity is even
        json_buf_.assign(capacity, ' ');
        size_t i = 0;
        json_buf_[i++] = '[';
        json_buf_[i++] = '0';
        while (i + 3 <= capacity) {
            json_buf_[i++] = ',';
            json_buf_[i++] = '0';
        }
        json_buf_[i] = ']';
        // best-effort: if a warm-up parse fails, the first large document faults the pages in instead
        [[maybe_unused]] auto array_err = json_parser_.iterate(padded_buf()).error();

        // "xx...x": on-demand only writes the string buffer when a string is decoded
        json_buf_.assign(capacity, 'x');
        json_buf_.front() = '"';
        json_buf_.back() = '"';
        std::string_view str;
        auto doc = json_parser_.iterate(padded_buf());
        [[maybe_unused]] auto string_err = doc.get_string().get(str);

        json_buf_.clear(); // keeps the capacity
    }

    simdjson::padded_string_view padded_buf() {
        return simdjson::padded_string_view(json_buf_.data(), json_buf_.size(), json_buf_.capacity());
    }

    simdjson::ondemand::parser json_parser_;
    std::string json_buf_;
};

QUARK_END_NAMESPACE
