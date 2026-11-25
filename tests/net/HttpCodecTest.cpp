#include "quark/net/HttpCodec.h"
#include "quark/net/Url.h"  // both define helpers in net::detail
#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace quark::net;

namespace {
std::vector<uint8_t> buf(std::string_view s) { return {s.begin(), s.end()}; }
std::string_view body_of(const HttpResponseView& r) { return {reinterpret_cast<const char*>(r.body.data()), r.body.size()}; }
}  // namespace

class TestHttpCodec : public ::testing::Test {};

TEST_F(TestHttpCodec, BuildRequest) {
    char out[512];
    const std::string_view headers[] = {"X-MBX-APIKEY: key", "Content-Type: application/json"};
    const std::string_view body = R"({"a":1})";
    const std::size_t n = build_http_request(out, HttpMethod::Post, "fapi.binance.com", "/fapi/v1/order", headers,
                                             {reinterpret_cast<const uint8_t*>(body.data()), body.size()});
    EXPECT_EQ(std::string_view(out, n),
              "POST /fapi/v1/order HTTP/1.1\r\nHost: fapi.binance.com\r\nX-MBX-APIKEY: key\r\n"
              "Content-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}");

    const std::size_t g = build_http_request(out, HttpMethod::Get, "h", "", {}, {});
    EXPECT_EQ(std::string_view(out, g), "GET / HTTP/1.1\r\nHost: h\r\n\r\n");

    char small[20];
    EXPECT_EQ(build_http_request(small, HttpMethod::Get, "a.long.host.name", "/x", {}, {}), 0u);
}

TEST_F(TestHttpCodec, ContentLength) {
    auto b = buf("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA");
    auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_FALSE(r.malformed);
    EXPECT_TRUE(r.keep_alive);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(body_of(r), "hello");
    EXPECT_EQ(r.consumed, b.size() - 5);

    auto partial = buf("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel");
    EXPECT_FALSE(parse_http_response(partial).complete);
    auto no_headers_end = buf("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    EXPECT_FALSE(parse_http_response(no_headers_end).complete);

    auto empty = buf("HTTP/1.1 204 No Content\r\n\r\n");
    auto e = parse_http_response(empty);
    EXPECT_TRUE(e.complete);
    EXPECT_EQ(e.status, 204);
    EXPECT_TRUE(e.body.empty());
}

TEST_F(TestHttpCodec, HeaderLookup) {
    auto b = buf("HTTP/1.1 429 Too Many Requests\r\nx-mbx-used-weight-1m:  2401 \r\nRetry-After: 7\r\n"
                 "Content-Length: 0\r\n\r\n");
    const auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.header("X-MBX-USED-WEIGHT-1M"), "2401");
    EXPECT_EQ(r.header("retry-after"), "7");
    EXPECT_EQ(r.header("Content-Length"), "0");
    EXPECT_EQ(r.header("Retry"), "");
    EXPECT_EQ(r.header("X-Missing"), "");

    auto none = buf("HTTP/1.1 204 No Content\r\n\r\n");
    EXPECT_EQ(parse_http_response(none).header("Content-Length"), "");
}

TEST_F(TestHttpCodec, ChunkedDecodedInPlace) {
    auto b = buf("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n"
                 "X-Trailer: t\r\n\r\n");
    auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(body_of(r), "hello world");
    EXPECT_EQ(r.consumed, b.size());

    auto partial = buf("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n wor");
    const auto before = partial;
    EXPECT_FALSE(parse_http_response(partial).complete);
    EXPECT_EQ(partial, before);  // not decoded until every chunk is there
}

TEST_F(TestHttpCodec, ConnectionHandling) {
    auto close = buf("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_FALSE(parse_http_response(close).keep_alive);
    auto http10 = buf("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_FALSE(parse_http_response(http10).keep_alive);
    auto http10_keep = buf("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(parse_http_response(http10_keep).keep_alive);
}

TEST_F(TestHttpCodec, Malformed) {
    for (const char* bad : {"HTTP/1.1 2x0 OK\r\n\r\n", "NOTHTTP 200\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: 12a\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"}) {
        auto b = buf(bad);
        auto r = parse_http_response(b);
        EXPECT_TRUE(r.complete && r.malformed) << bad;
        EXPECT_FALSE(r.keep_alive);
    }
}

TEST_F(TestHttpCodec, MalformedFraming) {
    for (const char* bad : {"HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n",                    // 4-digit status
                            "HTTP/1.1 200OK\r\nContent-Length: 0\r\n\r\n",                       // no space after it
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nokXX0\r\n\r\n",  // no CRLF after data
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n2\r\nok\r\n0\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nxx"}) {
        auto b = buf(bad);
        auto r = parse_http_response(b);
        EXPECT_TRUE(r.complete && r.malformed) << bad;
    }
}

TEST_F(TestHttpCodec, TransferEncodingIsCaseInsensitive) {
    auto b = buf("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
    auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_FALSE(r.malformed);
    EXPECT_EQ(body_of(r), "ok");
}

TEST_F(TestHttpCodec, InterimResponsesAreSkipped) {
    const std::string_view text = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                  "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokNEXT";
    auto b = buf(text);
    auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_FALSE(r.malformed);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(body_of(r), "ok");
    EXPECT_EQ(r.consumed, text.size() - 4);

    auto partial = buf("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Le");
    EXPECT_FALSE(parse_http_response(partial).complete);
}

TEST_F(TestHttpCodec, CloseDelimitedBody) {
    auto b = buf("HTTP/1.1 200 OK\r\nServer: x\r\n\r\n{\"bids\":[]}");
    auto r = parse_http_response(b);
    EXPECT_FALSE(r.complete);  // only the close ends it
    EXPECT_TRUE(r.close_delimited);
    EXPECT_FALSE(r.keep_alive);
    EXPECT_EQ(body_of(r), "{\"bids\":[]}");

    for (const char* empty : {"HTTP/1.1 204 No Content\r\n\r\n", "HTTP/1.1 304 Not Modified\r\n\r\n"}) {
        auto e = buf(empty);
        auto er = parse_http_response(e);
        EXPECT_TRUE(er.complete) << empty;  // no body by definition
        EXPECT_FALSE(er.close_delimited) << empty;
        EXPECT_TRUE(er.body.empty()) << empty;
    }
}

TEST_F(TestHttpCodec, ChunkedSplitAtEveryByte) {
    const std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "5;ext=1\r\nhello\r\n1\r\n \r\n5\r\nworld\r\n0\r\nTrailer: x\r\n\r\n";
    for (std::size_t cut = 0; cut < text.size(); ++cut) {
        auto b = buf(std::string_view(text).substr(0, cut));
        const auto before = b;
        auto r = parse_http_response(b);
        EXPECT_FALSE(r.complete) << cut;
        EXPECT_EQ(b, before) << cut;  // untouched until every chunk is there
    }
    auto b = buf(text);
    auto r = parse_http_response(b);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(body_of(r), "hello world");
    EXPECT_EQ(r.consumed, text.size());
}

TEST_F(TestHttpCodec, BodylessPostCarriesContentLength) {
    char out[256];
    const std::size_t n = build_http_request(out, HttpMethod::Post, "h", "/listenKey", {}, {});
    EXPECT_EQ(std::string_view(out, n), "POST /listenKey HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    const std::size_t d = build_http_request(out, HttpMethod::Delete, "h", "/x", {}, {});
    EXPECT_EQ(std::string_view(out, d), "DELETE /x HTTP/1.1\r\nHost: h\r\n\r\n");
}

// NET-02: interim responses were skipped by recursion, one stack frame each: a server sending
// ~58k "100 Continue" responses into a 1 MiB buffer overflowed the stack (SIGSEGV)
TEST_F(TestHttpCodec, InterimResponseFloodIsMalformedNotACrash) {
    std::string s;
    while (s.size() < (1u << 20)) s += "HTTP/1.1 100 Continue\r\n\r\n";
    auto b = buf(s);
    const auto r = parse_http_response(b);
    EXPECT_TRUE(r.complete);
    EXPECT_TRUE(r.malformed);

    // a few are fine
    auto ok = buf("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                  "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    const auto r2 = parse_http_response(ok);
    ASSERT_TRUE(r2.complete);
    EXPECT_FALSE(r2.malformed);
    EXPECT_EQ(body_of(r2), "hi");
    EXPECT_EQ(r2.consumed, ok.size());
}

// NET-09: framing the server can't mean, or that hides another response, is an error (RFC 9112 6.3)
TEST_F(TestHttpCodec, AmbiguousFramingIsMalformed) {
    auto malformed = [](std::string_view s) {
        auto b = buf(s);
        const auto r = parse_http_response(b);
        return r.complete && r.malformed;
    };
    EXPECT_TRUE(malformed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 5\r\n\r\nabcde"));
    EXPECT_TRUE(malformed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));
    EXPECT_TRUE(malformed("HTTP/1.1 200 OK\r\nTransfer-Encoding: xchunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));
    EXPECT_TRUE(malformed("HTTP/1.1 000 Zero\r\nContent-Length: 0\r\n\r\n"));
    EXPECT_TRUE(malformed("HTTP/1.1 099 Low\r\nContent-Length: 0\r\n\r\n"));
    EXPECT_TRUE(malformed("HTTP/1.1 200 OK\r\nContent-Length : 3\r\n\r\nabc"));      // space before the colon
    EXPECT_TRUE(malformed("HTTP/1.1 200 OK\r\nX-A: 1\r\n folded\r\nContent-Length: 0\r\n\r\n"));  // obs-fold

    // still accepted: an identical repeated Content-Length, a coding list ending in chunked
    auto same = buf("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    const auto r1 = parse_http_response(same);
    ASSERT_TRUE(r1.complete);
    EXPECT_FALSE(r1.malformed);
    EXPECT_EQ(body_of(r1), "abc");
    auto list = buf("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
    const auto r2 = parse_http_response(list);
    ASSERT_TRUE(r2.complete);
    EXPECT_FALSE(r2.malformed);
}

TEST_F(TestHttpCodec, ConnectionTokensAndBodylessStatuses) {
    auto b = buf("HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_FALSE(parse_http_response(b).keep_alive);  // close wins wherever it is in the list

    // 204 and 304 never have a body, whatever their headers say: waiting for one stalls until the timeout
    for (const char* status : {"204 No Content", "304 Not Modified"}) {
        auto r = buf(std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 5\r\n\r\n");
        const auto v = parse_http_response(r);
        ASSERT_TRUE(v.complete) << status;
        EXPECT_FALSE(v.malformed);
        EXPECT_TRUE(v.body.empty());
        EXPECT_EQ(v.consumed, r.size());
    }
}

// NET-09: with a parse state the next call resumes where the last one stopped, instead of rescanning
// every chunk from the start on each read (1-byte chunks made a 1 MiB body cost ~60 ms to parse)
TEST_F(TestHttpCodec, ParseStateResumesAndGivesTheSameResult) {
    std::string body_chunks;
    std::string expected;
    for (int i = 0; i < 2000; ++i) {
        body_chunks += "1\r\n" + std::string(1, char('a' + i % 26)) + "\r\n";
        expected += char('a' + i % 26);
    }
    const std::string msg = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
                            body_chunks + "0\r\nX-Trailer: t\r\n\r\n";
    for (std::size_t step : {1u, 7u, 64u}) {
        std::vector<uint8_t> b;
        HttpParseState state;
        HttpResponseView r;
        for (std::size_t off = 0; off < msg.size() && !r.complete; off += step) {
            b.insert(b.end(), msg.begin() + static_cast<std::ptrdiff_t>(off),
                     msg.begin() + static_cast<std::ptrdiff_t>(std::min(msg.size(), off + step)));
            r = parse_http_response(b, &state);
        }
        ASSERT_TRUE(r.complete) << step;
        EXPECT_FALSE(r.malformed);
        EXPECT_EQ(body_of(r), expected) << step;
        EXPECT_EQ(r.consumed, msg.size());
    }
}
