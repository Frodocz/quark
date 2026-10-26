#include "TestServers.h"
#include "quark/net/TlsStream.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <string>

using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {

StreamPhase open_within(TlsStream& s, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    StreamPhase ph = s.poll_open();
    while (ph == StreamPhase::Connecting && std::chrono::steady_clock::now() < deadline) ph = s.poll_open();
    return ph;
}

void echo_handler(ServerConn& c, TestServer& s) {
    char buf[4096];
    while (!s.stopping()) {
        const int n = c.read_some(buf, sizeof(buf));
        if (n <= 0) return;
        c.write_all(buf, static_cast<std::size_t>(n));
    }
}

std::string read_all_available(TlsStream& s, std::size_t want, std::chrono::milliseconds timeout = 2000ms) {
    std::string got;
    uint8_t buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
        const auto n = s.read(buf);
        if (n > 0) got.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n));
        else if (n != -EAGAIN) break;
    }
    return got;
}

}  // namespace

class TestTlsStream : public ::testing::Test {};

TEST_F(TestTlsStream, VerifiedHandshakeAndEcho) {
    TestServer server(echo_handler, {.tls = true});
    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("localhost");
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_NE(s.description().find("TLSv1.3"), std::string::npos);
    EXPECT_NE(s.description().find("TLS_AES_128_GCM_SHA256"), std::string::npos) << s.description();

    const std::string msg = "hello over tls";
    ASSERT_EQ(s.write({reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, false), static_cast<int64_t>(msg.size()));
    EXPECT_EQ(read_all_available(s, msg.size()), msg);
}

TEST_F(TestTlsStream, IpLiteralVerifiedAgainstTheCertificateIpSan) {
    TestServer server(echo_handler, {.tls = true});
    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("127.0.0.1");
    ASSERT_TRUE(s.connect(server.endpoint()));
    EXPECT_EQ(open_within(s), StreamPhase::Open);
}

TEST_F(TestTlsStream, VerificationFailures) {
    TestServer server(echo_handler, {.tls = true});
    {
        TlsContext ctx(trusting_test_ca());
        TlsStream s;
        s.set_context(ctx);
        s.set_hostname("api.example.com");  // trusted chain, wrong name
        ASSERT_TRUE(s.connect(server.endpoint()));
        EXPECT_EQ(open_within(s), StreamPhase::Failed);
    }
    {
        TlsContext ctx;  // system CAs don't trust the self-signed test certificate
        TlsStream s;
        s.set_context(ctx);
        s.set_hostname("localhost");
        ASSERT_TRUE(s.connect(server.endpoint()));
        EXPECT_EQ(open_within(s), StreamPhase::Failed);
    }
    {
        TlsContext ctx(trusting_test_ca());
        TlsStream s;
        s.set_context(ctx);  // no hostname: a verified chain alone would accept any certificate
        EXPECT_FALSE(s.connect(server.endpoint()));
    }
}

// Regression: unsent ciphertext used to be appended back behind newer ciphertext, corrupting the
// stream once more than one TLS record was pending; and write() never pushed back.
TEST_F(TestTlsStream, BackpressureKeepsCiphertextInOrder) {
    constexpr std::size_t kTotal = 2 << 20;
    std::atomic<bool> intact{false};
    std::atomic<std::size_t> received{0};
    TestServer server(
        [&](ServerConn& c, TestServer&) {
            std::this_thread::sleep_for(200ms);  // let the client fill every buffer first
            char buf[16384];
            std::size_t got = 0;
            bool ok = true;
            while (got < kTotal) {
                const int n = c.read_some(buf, sizeof(buf));
                if (n <= 0) {
                    ok = false;
                    break;
                }
                for (int i = 0; i < n && ok; ++i, ++got) ok = static_cast<uint8_t>(buf[i]) == static_cast<uint8_t>(got % 251);
                if (!ok) break;
            }
            received = got;
            intact = ok && got == kTotal;
        },
        {.tls = true, .rcvbuf = 4096});

    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("localhost");
    s.tcp().set_send_buf(4096);
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    s.tcp().set_send_buf(4096);

    std::vector<uint8_t> chunk(8192);
    std::size_t sent = 0;
    std::size_t eagain = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (sent < kTotal && std::chrono::steady_clock::now() < deadline) {
        const std::size_t len = std::min(chunk.size(), kTotal - sent);
        for (std::size_t i = 0; i < len; ++i) chunk[i] = static_cast<uint8_t>((sent + i) % 251);
        const auto r = s.write({chunk.data(), len}, false);
        if (r == -EAGAIN) {
            ++eagain;
            EXPECT_LE(s.pending_ciphertext(), TlsStream::kMaxPendingCiphertext + 16 * 1024 + 512);
            continue;
        }
        ASSERT_GT(r, 0);
        sent += static_cast<std::size_t>(r);
    }
    ASSERT_EQ(sent, kTotal);
    EXPECT_GT(eagain, 0u) << "the peer never pushed back: the test didn't exercise backpressure";
    uint8_t rb[64];
    while (s.pending_ciphertext() > 0 && std::chrono::steady_clock::now() < deadline) (void)s.read(rb);  // flushes
    server.stop();
    EXPECT_TRUE(intact) << "server got " << received << " intact bytes";
}

// Several records arriving in one segment are all returned before read() reports -EAGAIN.
TEST_F(TestTlsStream, DrainsEveryBufferedRecordBeforeEagain) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            c.cork(true);
            for (const char* rec : {"one|", "two|", "three|"}) c.write_all(std::string_view(rec));
            c.cork(false);
            s.hold();
        },
        {.tls = true});
    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("localhost");
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    std::this_thread::sleep_for(100ms);  // all three records are in the socket now

    std::string got;
    uint8_t buf[4096];
    for (;;) {
        const auto n = s.read(buf);
        if (n <= 0) {
            EXPECT_EQ(n, -EAGAIN);
            break;
        }
        got.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n));
    }
    EXPECT_EQ(got, "one|two|three|");
}

TEST_F(TestTlsStream, ReconnectResumesTheSession) {
    TestServer server(
        [](ServerConn& c, TestServer&) {
            c.write_all(std::string_view("hi"));  // tickets are processed while the client reads
            char buf[16];
            (void)c.read_some(buf, sizeof(buf));  // until the client closes
        },
        {.tls = true});
    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("localhost");
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_FALSE(s.session_resumed());
    EXPECT_EQ(read_all_available(s, 2), "hi");
    s.close();

    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_TRUE(s.session_resumed());
}

// a session belongs to the host it was negotiated with: not offered to another one
TEST_F(TestTlsStream, SessionIsNotResumedForAnotherHost) {
    TestServer server(
        [](ServerConn& c, TestServer&) {
            c.write_all(std::string_view("hi"));
            char buf[16];
            (void)c.read_some(buf, sizeof(buf));
        },
        {.tls = true});
    TlsContext ctx(trusting_test_ca());
    TlsStream s;
    s.set_context(ctx);
    s.set_hostname("localhost");
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_EQ(read_all_available(s, 2), "hi");
    s.close();

    s.set_hostname("127.0.0.1");  // also in the test certificate
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_FALSE(s.session_resumed());
    EXPECT_EQ(read_all_available(s, 2), "hi");
    s.close();

    s.set_hostname("127.0.0.1");  // unchanged: resumes the session of the last connection
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    EXPECT_TRUE(s.session_resumed());
}
