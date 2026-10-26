#include "quark/net/HttpCodec.h"
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
