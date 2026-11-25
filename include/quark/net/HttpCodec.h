#pragma once

#include "quark/Attributes.h"

#include <algorithm>
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
    std::string_view headers;       ///< the header lines (CRLF separated, no status line), in the buffer

    /// Value of the first header called `name` (any case), trimmed; empty if there is none.
    /// E.g. header("X-MBX-USED-WEIGHT-1M"), header("Retry-After").
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
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

/// ASCII case-insensitive comparison of two header names.
inline bool header_name_equal(std::string_view a, std::string_view b) noexcept {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

} // namespace detail

inline std::string_view HttpResponseView::header(std::string_view name) const noexcept {
    for (std::string_view rest = headers; !rest.empty();) {
        const std::size_t eol = rest.find("\r\n");
        const std::string_view line = rest.substr(0, eol);
        rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 2);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && detail::header_name_equal(detail::trim(line.substr(0, colon)), name)) {
            return detail::trim(line.substr(colon + 1));
        }
    }
    return {};
}

/// Most 1xx interim responses accepted before the real one (each is skipped).
inline constexpr int kMaxInterimResponses = 16;

/// Where parse_http_response() stopped in a buffer that keeps growing (more bytes of the same
/// response appended): passed back, the next call resumes there instead of rescanning from the
/// start. Reset it (a new value) for each new response.
struct HttpParseState {
    std::size_t start{0};       ///< where the current (interim or final) response begins
    std::size_t searched{0};    ///< bytes from `start` already searched for the end of its headers
    std::size_t chunk_pos{0};   ///< chunked: the next chunk-size line to check, 0 until the headers are parsed
    int interim{0};             ///< 1xx responses skipped so far
};

/// Parses an HTTP/1.1 response at the start of `buf`: Content-Length, chunked or (see
/// close_delimited) close-delimited bodies; up to kMaxInterimResponses 1xx interim responses
/// before it are skipped. Returns complete == false while more bytes are needed. A chunked body is
/// decoded in place (the decoded body is never longer than its encoding), only once all chunks are
/// present. `state` (optional): see HttpParseState.
///
/// Malformed (RFC 9112 6.3): conflicting Content-Lengths, Content-Length together with
/// Transfer-Encoding, a Transfer-Encoding whose last coding isn't chunked, a status below 100,
/// whitespace before a header's colon, obsolete line folding. 204 and 304 never have a body.
[[nodiscard]] inline QUARK_ATTRIBUTE_HOT HttpResponseView parse_http_response(std::span<uint8_t> buf,
                                                                              HttpParseState* state = nullptr) noexcept {
    HttpResponseView r;
    HttpParseState local;
    HttpParseState& st = state != nullptr ? *state : local;
    const std::string_view s{reinterpret_cast<const char*>(buf.data()), buf.size()};

    auto malformed = [&] {
        r.complete = true;
        r.malformed = true;
        r.keep_alive = false;
        r.consumed = buf.size();
        return r;
    };

    // the status line and headers of the final response, skipping interim ones
    std::size_t hdr_end = 0;
    std::string_view status_line;
    for (;;) {
        // resume a little before where the last search ended: "\r\n\r\n" may straddle it
        const std::size_t from = st.start + (st.searched > 3 ? st.searched - 3 : 0);
        hdr_end = from < s.size() ? s.find("\r\n\r\n", from) : std::string_view::npos;
        if (hdr_end == std::string_view::npos) {
            st.searched = s.size() - st.start;
            return r;
        }
        status_line = s.substr(st.start, s.find("\r\n", st.start) - st.start);
        if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12 || status_line[8] != ' ' ||
            (status_line.size() > 12 && status_line[12] != ' ')) {
            return malformed();
        }
        r.status = 0;
        for (std::size_t i = 9; i < 12; ++i) {
            if (status_line[i] < '0' || status_line[i] > '9') return malformed();
            r.status = r.status * 10 + (status_line[i] - '0');
        }
        if (r.status < 100) return malformed();
        if (r.status >= 200) break;
        // interim response (100 Continue, 103 Early Hints): headers only, the real one follows
        if (++st.interim > kMaxInterimResponses) return malformed();
        st.start = hdr_end + 4;
        st.searched = 0;
        r.status = 0;  // an incomplete result reports no status, not the skipped interim one's
    }
    const std::size_t body_start = hdr_end + 4;
    const std::size_t line1_end = st.start + status_line.size() + 2;
    r.keep_alive = status_line.starts_with("HTTP/1.1");  // HTTP/1.0 closes by default
    r.headers = s.substr(std::min(line1_end, hdr_end), hdr_end - std::min(line1_end, hdr_end));

    bool chunked = false;
    bool has_encoding = false;
    bool has_length = false;
    std::size_t content_len = 0;
    for (std::size_t pos = line1_end; pos < hdr_end;) {
        const std::size_t eol = s.find("\r\n", pos);
        const std::string_view line = s.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.front() == ' ' || line.front() == '\t') return malformed();  // obsolete line folding
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line[colon - 1] == ' ' || line[colon - 1] == '\t') {
            return malformed();  // no field name, or whitespace before the colon
        }
        const std::string_view name = line.substr(0, colon);
        const std::string_view value = detail::trim(line.substr(colon + 1));
        if (detail::header_name_equal(name, "content-length")) {
            const std::size_t len = detail::parse_decimal(value);
            if (len == detail::kBadNumber || (has_length && len != content_len)) return malformed();
            content_len = len;
            has_length = true;
        } else if (detail::header_name_equal(name, "transfer-encoding")) {
            // the last coding must be chunked, or the length is unknown
            const std::size_t comma = value.rfind(',');
            const std::string_view last = detail::trim(comma == std::string_view::npos ? value : value.substr(comma + 1));
            if (!detail::header_name_equal(last, "chunked")) return malformed();
            chunked = true;
            has_encoding = true;
        } else if (detail::header_name_equal(name, "connection")) {
            bool close = false, keep = false;
            for (std::string_view rest = value; !rest.empty();) {
                const std::size_t c = rest.find(',');
                const std::string_view token = detail::trim(rest.substr(0, c));
                rest = c == std::string_view::npos ? std::string_view{} : rest.substr(c + 1);
                close |= detail::header_name_equal(token, "close");
                keep |= detail::header_name_equal(token, "keep-alive");
            }
            if (close) {
                r.keep_alive = false;
            } else if (keep) {
                r.keep_alive = true;
            }
        }
    }
    if (has_length && has_encoding) return malformed();  // which one frames it? (response splitting)

    if (r.status == 204 || r.status == 304) {  // never a body (RFC 9110 15.3.5, 15.4.5)
        r.complete = true;
        r.consumed = body_start;
        return r;
    }

    if (!chunked) {
        if (!has_length) {
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

    // dry run: are all chunks present? (decoding in place is destructive). Chunks checked by an
    // earlier call (st.chunk_pos) are not checked again.
    std::size_t rp = st.chunk_pos != 0 ? st.chunk_pos : body_start;
    std::size_t end_of_message = 0;
    for (;;) {
        st.chunk_pos = rp;
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
