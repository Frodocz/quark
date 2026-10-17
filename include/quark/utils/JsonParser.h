#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"

#include "simdjson.h"
#include <cstddef>
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

// Wraps a simdjson on-demand parser. There are two ways to parse, pick by where the JSON lives:
//   - parse_in_place: zero-copy, the buffer itself is parsed. It must have kPadding readable bytes after
//     the JSON (e.g. a receive buffer allocated with kPadding spare bytes), otherwise the parse fails with
//     simdjson::INSUFFICIENT_PADDING instead of silently copying.
//   - parse_copy: works on any buffer, copies the JSON into an internal padded buffer first.
// Each returns simdjson's result type so this compiles with and without exceptions: check error(), or
// with exceptions convert to ondemand::document, which throws on error. The document is valid until the
// next parse, and a parse_in_place document also points into the caller's buffer.
class QUARK_API JsonParser {
public:
    static constexpr size_t kInitialCapacity = 64 * 1024; // 64KB
    static constexpr size_t kPadding = simdjson::SIMDJSON_PADDING;

    // Preallocates the copy buffer and simdjson's internal buffers for documents up to
    // initial_capacity bytes and touches them, so the hot path neither allocates nor page-faults.
    // Larger documents still parse, and both buffers grow to fit them.
    explicit JsonParser(size_t initial_capacity = kInitialCapacity) {
        // parser first: it rejects sizes above its max capacity before json_buf_ reserves anything
        if (auto err = json_parser_.allocate(initial_capacity); err != simdjson::SUCCESS) {
            QUARK_THROW(QuarkError{std::string{"JsonParser failed to preallocate "} + std::to_string(initial_capacity)
                                   + " bytes: " + simdjson::error_message(err)});
        }
        json_buf_.reserve(initial_capacity + kPadding);
        warm_up(initial_capacity);
    }

    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;

    // a moved-from parser stays usable, simdjson reallocates its buffers on the next parse
    JsonParser(JsonParser&&) noexcept = default;
    JsonParser& operator=(JsonParser&&) noexcept = default;

    // Zero-copy: parses data[0, size), capacity is the number of readable bytes from data and must be
    // at least size + kPadding.
    simdjson::simdjson_result<simdjson::ondemand::document> parse_in_place(const char* data, size_t size,
                                                                           size_t capacity) {
        return json_parser_.iterate(simdjson::padded_string_view(data, size, capacity));
    }

    // Zero-copy: uses the spare capacity of str as padding, so str.reserve(str.size() + kPadding) first
    simdjson::simdjson_result<simdjson::ondemand::document> parse_in_place(const std::string& str) {
        return parse_in_place(str.data(), str.size(), str.capacity());
    }

    // Zero-copy: e.g. a simdjson::padded_string or padded_string::load()
    simdjson::simdjson_result<simdjson::ondemand::document> parse_in_place(simdjson::padded_string_view json) {
        return json_parser_.iterate(json);
    }

    // the returned document points into the buffer, so a temporary would leave it dangling
    simdjson::simdjson_result<simdjson::ondemand::document> parse_in_place(std::string&&) = delete;
    simdjson::simdjson_result<simdjson::ondemand::document> parse_in_place(simdjson::padded_string&&) = delete;

    // Copies str into the internal padded buffer, so str needs no padding and may be released right after
    simdjson::simdjson_result<simdjson::ondemand::document> parse_copy(std::string_view str) {
        // reserve before assign so a growing buffer reallocates once, and again after it as reserve
        // guarantees capacity >= n while assign makes no capacity promise (a no-op in practice)
        json_buf_.reserve(str.size() + kPadding);
        json_buf_.assign(str);
        json_buf_.reserve(str.size() + kPadding);
        return json_parser_.iterate(padded_buf());
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
