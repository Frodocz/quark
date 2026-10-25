#include "quark/net/FStack.h"

#include "quark/QuarkLog.h"

#include <ff_api.h>

#include <atomic>
#include <cstring>
#include <mutex>

QUARK_BEGIN_NAMESPACE

namespace net {

namespace {
std::atomic<bool> g_active{false};
std::mutex g_init_mutex;
bool g_initialized{false};
}  // namespace

bool init_fstack(int argc, char** argv) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) return true;
    bool has_conf = false;
    for (int i = 1; i < argc; ++i) has_conf |= argv[i] != nullptr && std::strcmp(argv[i], "--conf") == 0;
    if (!has_conf) {
        QUARK_LOG_ERROR("init_fstack: missing \"--conf <f-stack.conf>\"");
        return false;
    }
    if (::ff_init(argc, argv) < 0) {
        QUARK_LOG_ERROR("init_fstack: ff_init failed");
        return false;
    }
    g_initialized = true;
    g_active.store(true, std::memory_order_release);
    QUARK_LOG_INFO("F-Stack initialized");
    return true;
}

bool fstack_active() noexcept { return g_active.load(std::memory_order_acquire); }

namespace detail {
void set_fstack_active(bool active) noexcept { g_active.store(active, std::memory_order_release); }
}  // namespace detail

} // namespace net

QUARK_END_NAMESPACE
