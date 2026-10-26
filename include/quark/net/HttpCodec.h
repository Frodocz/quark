#pragma once

#include "quark/Attributes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace net {

enum class HttpMethod : uint8_t { Get, Post, Put, Delete };

[[nodiscard]] constexpr std::string_view http_method_name(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Put: return "PUT";
        case HttpMethod::Delete: return "DELETE";
    }
    return "GET";
}

/// Writes an HTTP/1.1 request into `dst` without allocating. `header_lines` are complete header
/// lines without CRLF (e.g. "X-MBX-APIKEY: ..."); Host and, with a body or for POST/PUT,
/// Content-Length are added (some front ends answer a bodyless POST without it with 411).
/// Returns the bytes written, 0 if `dst` is too small.
[[nodiscard]] inline QUARK_ATTRIBUTE_HOT std::size_t build_http_request(std::span<char> dst, HttpMethod method,
                                                                        std::string_view host, std::string_view target,
                                                                        std::span<const std::string_view> header_lines,
                                                                        std::span<const uint8_t> body) noexcept {
    std::size_t o = 0;
    auto put = [&](std::string_view s) noexcept {
        if (s.size() > dst.size() - o) return false;
        std::memcpy(dst.data() + o, s.data(), s.size());
        o += s.size();
        return true;
    };
    if (!put(http_method_name(method)) || !put(" ") || !put(target.empty() ? std::string_view("/") : target) ||
        !put(" HTTP/1.1\r\nHost: ") || !put(host) || !put("\r\n")) {
        return 0;
    }
    for (const std::string_view h : header_lines) {
        if (!put(h) || !put("\r\n")) return 0;
    }
    if (!body.empty() || method == HttpMethod::Post || method == HttpMethod::Put) {
        char num[24];
        std::size_t p = sizeof(num);
        for (std::size_t v = body.size(); v != 0 || p == sizeof(num); v /= 10) num[--p] = static_cast<char>('0' + v % 10);
        if (!put("Content-Length: ") || !put({num + p, sizeof(num) - p}) || !put("\r\n")) return 0;
    }
    if (!put("\r\n")) return 0;
    if (!body.empty() && !put({reinterpret_cast<const char*>(body.data()), body.size()})) return 0;
    return o;
}

struct HttpResponseView {
    bool complete{false};
    bool malformed{false};          ///< unparsable: the connection can't be reused
    bool keep_alive{true};          ///< false if the server sent "Connection: close"
    /// No Content-Length and not chunked: the body runs until the server closes the connection.
    /// complete stays false; once the peer has closed, body holds everything received after the
    /// headers (HttpClient then delivers it).
    bool close_delimited{false};
    int status{0};
    std::span<const uint8_t> body;  ///< points into the parsed buffer
    std::size_t consumed{0};        ///< bytes of this response (headers + body)
};

namespace detail {

inline bool istarts_with(std::string_view s, std::string_view lower_prefix) noexcept {
    if (s.size() < lower_prefix.size()) return false;
    for (std::size_t i = 0; i < lower_prefix.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_prefix[i]) return false;
    }
    return true;
}

inline bool iends_with(std::string_view s, std::string_view lower_suffix) noexcept {
    return s.size() >= lower_suffix.size() && istarts_with(s.substr(s.size() - lower_suffix.size()), lower_suffix);
}

inline std::string_view trim(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}

inline constexpr std::size_t kBadNumber = std::numeric_limits<std::size_t>::max();

inline std::size_t parse_decimal(std::string_view s) noexcept {
    if (s.empty() || s.size() > 18) return kBadNumber;
    std::size_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return kBadNumber;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    return v;
}

// chunk-size line: hex digits, optionally followed by ";extension"
inline std::size_t parse_chunk_size(std::string_view s) noexcept {
    s = trim(s.substr(0, s.find(';')));
    if (s.empty() || s.size() > 15) return kBadNumber;
    std::size_t v = 0;
    for (const char c : s) {
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return kBadNumber;
        }
        v = v * 16 + static_cast<std::size_t>(d);
    }
    return v;
}

} // namespace detail

/// Parses an HTTP/1.1 response at the start of `buf`: Content-Length, chunked or (see
/// close_delimited) close-delimited bodies; 1xx interim responses before it are skipped.
/// Returns complete == false while more bytes are needed. A chunked body is decoded in place
/// (the decoded body is never longer than its encoding), only once all chunks are present.
[[nodiscard]] inline QUARK_ATTRIBUTE_HOT HttpResponseView parse_http_response(std::span<uint8_t> buf) noexcept {
    HttpResponseView r;
    const std::string_view s{reinterpret_cast<const char*>(buf.data()), buf.size()};
    const auto hdr_end = s.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) return r;
    const std::size_t body_start = hdr_end + 4;

    auto malformed = [&] {
        r.complete = true;
        r.malformed = true;
        r.keep_alive = false;
        r.consumed = buf.size();
        return r;
    };

    const std::string_view status_line = s.substr(0, s.find("\r\n"));
    if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12 || status_line[8] != ' ' ||
        (status_line.size() > 12 && status_line[12] != ' ')) {
        return malformed();
    }
    for (std::size_t i = 9; i < 12; ++i) {
        if (status_line[i] < '0' || status_line[i] > '9') return malformed();
        r.status = r.status * 10 + (status_line[i] - '0');
    }
    if (r.status < 200) {
        // interim response (100 Continue, 103 Early Hints): headers only, the real one follows
        HttpResponseView next = parse_http_response(buf.subspan(body_start));
        next.consumed += next.complete ? body_start : 0;
        return next;
    }
    r.keep_alive = status_line.starts_with("HTTP/1.1");  // HTTP/1.0 closes by default

    bool chunked = false;
    bool has_length = false;
    std::size_t content_len = 0;
    for (std::size_t pos = status_line.size() + 2; pos < hdr_end;) {
        const std::size_t eol = s.find("\r\n", pos);
        const std::string_view line = s.substr(pos, eol - pos);
        if (detail::istarts_with(line, "content-length:")) {
            content_len = detail::parse_decimal(detail::trim(line.substr(15)));
            if (content_len == detail::kBadNumber) return malformed();
            has_length = true;
        } else if (detail::istarts_with(line, "transfer-encoding:")) {
            // chunked must be the last coding, or the length is unknown (RFC 9112 6.3)
            if (!detail::iends_with(detail::trim(line.substr(18)), "chunked")) return malformed();
            chunked = true;
        } else if (detail::istarts_with(line, "connection:")) {
            const std::string_view v = detail::trim(line.substr(11));
            if (detail::istarts_with(v, "close")) r.keep_alive = false;
            if (detail::istarts_with(v, "keep-alive")) r.keep_alive = true;
        }
        pos = eol + 2;
    }

    if (!chunked) {
        if (!has_length && r.status != 204 && r.status != 304) {
            r.close_delimited = true;
            r.keep_alive = false;
            r.body = {buf.data() + body_start, buf.size() - body_start};
            return r;
        }
        if (buf.size() - body_start < content_len) return r;
        r.complete = true;
        r.body = {buf.data() + body_start, content_len};
        r.consumed = body_start + content_len;
        return r;
    }

    // dry run: are all chunks present? (decoding in place is destructive)
    std::size_t rp = body_start;
    std::size_t end_of_message = 0;
    for (;;) {
        const std::size_t line_end = s.find("\r\n", rp);
        if (line_end == std::string_view::npos) return r;
        const std::size_t size = detail::parse_chunk_size(s.substr(rp, line_end - rp));
        if (size == detail::kBadNumber) return malformed();
        if (size == 0) {
            const std::size_t term = s.find("\r\n\r\n", line_end);  // optional trailers, then CRLF
            if (term == std::string_view::npos) return r;
            end_of_message = term + 4;
            break;
        }
        if (buf.size() - (line_end + 2) < size + 2) return r;
        const std::size_t data_end = line_end + 2 + size;
        if (buf[data_end] != '\r' || buf[data_end + 1] != '\n') return malformed();
        rp = data_end + 2;
    }

    std::size_t wp = body_start;
    rp = body_start;
    for (;;) {
        const std::size_t line_end = s.find("\r\n", rp);
        const std::size_t size = detail::parse_chunk_size(s.substr(rp, line_end - rp));
        if (size == 0) break;
        std::memmove(buf.data() + wp, buf.data() + line_end + 2, size);
        wp += size;
        rp = line_end + 2 + size + 2;
    }
    r.complete = true;
    r.body = {buf.data() + body_start, wp - body_start};
    r.consumed = end_of_message;
    return r;
}

} // namespace net

QUARK_END_NAMESPACE
