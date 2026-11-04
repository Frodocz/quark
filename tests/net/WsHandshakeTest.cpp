#include "quark/net/WsHandshake.h"
#include "gtest/gtest.h"

#include "QuarkTest.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/syscall.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace quark::net;

namespace {
std::span<const uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }
const std::string kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";  // RFC 6455 section 1.3 example
}  // namespace

class TestWsHandshake : public ::testing::Test {};

TEST_F(TestWsHandshake, AcceptOfTheRfcExampleKey) {
    char out[28];
    compute_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", out);
    EXPECT_EQ(std::string_view(out, 28), kAccept);
}

TEST_F(TestWsHandshake, FreshKeysAreRandomAndConsistent) {
    const WsHandshakeKey a = make_ws_handshake_key();
    const WsHandshakeKey b = make_ws_handshake_key();
    EXPECT_NE(a.key_view(), b.key_view());
    char expected[28];
    compute_ws_accept(a.key_view(), expected);
    EXPECT_EQ(a.accept_view(), std::string_view(expected, 28));
    EXPECT_EQ(a.key_view().back(), '=');  // base64 of 16 bytes
}

// Child of the test below: a seccomp filter makes getrandom() fail with ENOSYS. Keys must still be
// produced (the fallback), differ, and carry the right accept value.
[[noreturn]] void make_keys_without_getrandom() {
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getrandom, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        std::_Exit(3);
    }
    char probe[4];
    if (::getrandom(probe, sizeof(probe), 0) != -1 || errno != ENOSYS) std::_Exit(4);  // the filter is active
    const WsHandshakeKey a = make_ws_handshake_key();
    const WsHandshakeKey b = make_ws_handshake_key();
    if (a.key_view() == b.key_view()) std::_Exit(5);
    char accept[28];
    compute_ws_accept(a.key_view(), accept);
    if (std::string_view(accept, 28) != a.accept_view()) std::_Exit(6);
    if (os_random_u64() == os_random_u64()) std::_Exit(7);
    std::_Exit(0);
}

// a warning goes to stderr (no logger exists in this binary)
TEST_F(TestWsHandshake, KeysSurviveAFailingGetrandom) {
    static_assert(noexcept(make_ws_handshake_key()));
    static_assert(noexcept(os_random_u64()));
    EXPECT_EXIT(make_keys_without_getrandom(), ::testing::ExitedWithCode(0), "getrandom failed \\(errno 38\\)");
}

TEST_F(TestWsHandshake, UpgradeRequest) {
    std::string req;
    build_ws_upgrade_request(req, "stream.binance.com:9443", "/stream?streams=btcusdt@trade", "KEY",
                             "X-MBX-APIKEY: abc\r\n");
    EXPECT_TRUE(req.starts_with("GET /stream?streams=btcusdt@trade HTTP/1.1\r\nHost: stream.binance.com:9443\r\n"));
    EXPECT_NE(req.find("\r\nSec-WebSocket-Key: KEY\r\n"), std::string::npos);
    EXPECT_NE(req.find("\r\nSec-WebSocket-Version: 13\r\n"), std::string::npos);
    EXPECT_NE(req.find("\r\nX-MBX-APIKEY: abc\r\n"), std::string::npos);
    EXPECT_TRUE(req.ends_with("\r\n\r\n"));
}

TEST_F(TestWsHandshake, AcceptsOnlyAMatching101) {
    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + kAccept + "\r\n\r\n";
    auto r = parse_ws_upgrade_response(bytes(ok + "\x81\x02hi"), kAccept);  // a frame may follow
    EXPECT_TRUE(r.complete && r.accepted);
    EXPECT_EQ(r.status, 101);
    EXPECT_EQ(r.header_len, ok.size());

    // header names and the Upgrade value are case-insensitive
    const std::string lower = "HTTP/1.1 101 OK\r\nupgrade: WebSocket\r\nsec-websocket-accept:  " + kAccept + " \r\n\r\n";
    EXPECT_TRUE(parse_ws_upgrade_response(bytes(lower), kAccept).accepted);

    EXPECT_FALSE(parse_ws_upgrade_response(bytes(ok.substr(0, ok.size() - 2)), kAccept).complete);
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(ok), "AAAAAAAAAAAAAAAAAAAAAAAAAAA=").accepted);  // wrong accept

    const std::string no_upgrade = "HTTP/1.1 101 OK\r\nSec-WebSocket-Accept: " + kAccept + "\r\n\r\n";
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(no_upgrade), kAccept).accepted);

    // rate limited: a complete, rejected response that reports the status
    const auto limited = parse_ws_upgrade_response(bytes("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 5\r\n\r\n"), kAccept);
    EXPECT_TRUE(limited.complete);
    EXPECT_FALSE(limited.accepted);
    EXPECT_EQ(limited.status, 429);

    EXPECT_FALSE(parse_ws_upgrade_response(bytes("garbage\r\n\r\n"), kAccept).accepted);

    // a 4-digit "status" isn't 101
    const std::string four_digits = "HTTP/1.1 1010 Switching\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: " +
                                    kAccept + "\r\n\r\n";
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(four_digits), kAccept).accepted);
    // an extension that was never offered (frames would be compressed)
    const std::string deflate = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                "Sec-WebSocket-Extensions: permessage-deflate\r\nSec-WebSocket-Accept: " + kAccept + "\r\n\r\n";
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(deflate), kAccept).accepted);
    // no space after the colon is still a valid header
    const std::string tight = "HTTP/1.1 101 Switching Protocols\r\nUpgrade:websocket\r\nSec-WebSocket-Accept:" + kAccept +
                              "\r\n\r\n";
    EXPECT_TRUE(parse_ws_upgrade_response(bytes(tight), kAccept).accepted);
}
