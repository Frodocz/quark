#include "quark/net/WsHandshake.h"

#include "quark/QuarkLog.h"
#include "quark/crypto/Base64Encoding.h"
#include "quark/utils/RdtscClock.h"

#include <openssl/evp.h>
#include <sys/random.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

QUARK_BEGIN_NAMESPACE

namespace net {

namespace {

constexpr std::string_view kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// splitmix64 over the TSC, an address and a counter: not cryptographic, only the fallback below
void fill_fallback(uint8_t* p, std::size_t len) noexcept {
    static std::atomic<uint64_t> counter{0};
    uint64_t x = static_cast<uint64_t>(RdtscClock::rdtsc()) ^ reinterpret_cast<uintptr_t>(p) ^
                 (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull);
    while (len > 0) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        const std::size_t n = len < sizeof(z) ? len : sizeof(z);
        std::memcpy(p, &z, n);
        p += n;
        len -= n;
    }
}

// getrandom() (blocking mode, EINTR retried) doesn't fail on Linux >= 3.17 once booted. Should it
// anyway (a seccomp filter, ENOSYS), fall back instead of failing every reconnect: RFC 6455 only
// needs the handshake nonce and masking keys to be unpredictable to intermediaries.
void fill_random(void* buf, std::size_t len) noexcept {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        const ssize_t n = ::getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                const int err = errno;
                // only an existing logger: creating one could fail, and this must not throw
                if (Logger* logger = Frontend::get_logger(std::string{kQuarkLoggerName})) {
                    QUILL_LOG_WARNING(logger, "getrandom failed (errno {}), WebSocket keys fall back to a "
                                              "TSC-seeded generator", err);
                } else {
                    std::fprintf(stderr, "quark: getrandom failed (errno %d), WebSocket keys fall back to a "
                                         "TSC-seeded generator\n", err);
                }
            }
            fill_fallback(p, len);
            return;
        }
        p += n;
        len -= static_cast<std::size_t>(n);
    }
}

bool iequals_prefix(std::string_view line, std::string_view lower_prefix) noexcept {
    if (line.size() < lower_prefix.size()) return false;
    for (std::size_t i = 0; i < lower_prefix.size(); ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_prefix[i]) return false;
    }
    return true;
}

std::string_view trim(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}

} // namespace

uint64_t os_random_u64() noexcept {
    uint64_t v = 0;
    fill_random(&v, sizeof(v));
    return v;
}

void compute_ws_accept(std::string_view key, std::span<char, 28> out) {
    unsigned char input[64];  // 24-char key + 36-char GUID
    std::size_t len = 0;
    if (key.size() + kWsGuid.size() <= sizeof(input)) {
        std::memcpy(input, key.data(), key.size());
        std::memcpy(input + key.size(), kWsGuid.data(), kWsGuid.size());
        len = key.size() + kWsGuid.size();
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (len == 0 || ::EVP_Digest(input, len, digest, &digest_len, EVP_sha1(), nullptr) != 1 || digest_len != 20) {
        std::memset(out.data(), 0, out.size());
        return;
    }
    (void)base64_encode({digest, 20}, out);  // 20 bytes -> exactly 28 chars
}

WsHandshakeKey make_ws_handshake_key() noexcept {
    uint8_t nonce[16];
    fill_random(nonce, sizeof(nonce));
    WsHandshakeKey k{};
    (void)base64_encode(nonce, k.key);  // 16 bytes -> exactly 24 chars
    compute_ws_accept(k.key_view(), k.accept);
    return k;
}

void build_ws_upgrade_request(std::string& out, std::string_view host, std::string_view target, std::string_view key,
                              std::string_view extra_headers) {
    out.append("GET ").append(target.empty() ? std::string_view("/") : target).append(" HTTP/1.1\r\nHost: ");
    out.append(host);
    out.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    out.append(key);
    out.append("\r\nSec-WebSocket-Version: 13\r\n");
    out.append(extra_headers);
    out.append("\r\n");
}

WsUpgradeResponse parse_ws_upgrade_response(std::span<const uint8_t> resp, std::string_view expected_accept) noexcept {
    WsUpgradeResponse r;
    const std::string_view s{reinterpret_cast<const char*>(resp.data()), resp.size()};
    const auto end = s.find("\r\n\r\n");
    if (end == std::string_view::npos) return r;
    r.complete = true;
    r.header_len = end + 4;

    // status line: HTTP/1.x <code> <reason>
    const std::string_view status_line = s.substr(0, s.find("\r\n"));
    if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12 || status_line[8] != ' ' ||
        (status_line.size() > 12 && status_line[12] != ' ')) {
        return r;
    }
    for (std::size_t i = 9; i < 12; ++i) {
        if (status_line[i] < '0' || status_line[i] > '9') {
            r.status = 0;
            return r;
        }
        r.status = r.status * 10 + (status_line[i] - '0');
    }
    if (r.status != 101) return r;

    bool upgrade_ok = false;
    bool accept_ok = false;
    bool extension = false;
    std::size_t pos = status_line.size() + 2;
    while (pos < end) {
        const std::size_t eol = s.find("\r\n", pos);
        const std::string_view line = s.substr(pos, eol - pos);
        if (iequals_prefix(line, "upgrade:")) {
            const std::string_view v = trim(line.substr(8));
            upgrade_ok = v.size() == 9 && iequals_prefix(v, "websocket");
        } else if (iequals_prefix(line, "sec-websocket-accept:")) {
            accept_ok = trim(line.substr(21)) == expected_accept;
        } else if (iequals_prefix(line, "sec-websocket-extensions:")) {
            // none was offered: e.g. permessage-deflate would make every frame unreadable (6455 4.1)
            extension = !trim(line.substr(25)).empty();
        }
        pos = eol + 2;
    }
    r.accepted = upgrade_ok && accept_ok && !extension;
    return r;
}

} // namespace net

QUARK_END_NAMESPACE
