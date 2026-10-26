#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"
#include "quark/utils/FullDuplexShm.h"
#include "quark/utils/MMapFile.h"

#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

using namespace quark;

namespace {

int open_fd_count() {
    int n = 0;
    if (DIR* d = ::opendir("/proc/self/fd")) {
        while (::readdir(d) != nullptr) ++n;
        ::closedir(d);
    }
    return n;
}

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("quark_mmap_test_") + std::to_string(::getpid()) + "_" + name))
        .string();
}

}  // namespace

TEST(MMapFileTest, CreatesMapsAndTruncates) {
    const std::string path = temp_path("basic");
    std::filesystem::remove(path);
    MMapFile f;
    bool truncated = false;
    f.initialize({.filename = path, .lock = false, .size = 4096}, truncated);
    EXPECT_TRUE(truncated);
    ASSERT_NE(f.addr(), nullptr);
    EXPECT_EQ(f.size(), 4096u);
    std::memcpy(f.addr(), "quark", 5);
    f.reset();
    EXPECT_EQ(f.addr(), nullptr);
    EXPECT_EQ(f.fd(), -1);

    MMapFile g;  // reopen with the same size: not truncated, contents kept
    g.initialize({.filename = path, .lock = false, .size = 4096}, truncated);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(std::memcmp(g.addr(), "quark", 5), 0);
    g.reset();
    std::filesystem::remove(path);
}

TEST(MMapFileTest, FailuresLeakNothingAndCanBeRetried) {
    const std::string path = temp_path("empty");
    std::filesystem::remove(path);
    QUARK_LOG_INFO("MMapFileTest: opens the log file before the fds are counted");
    const int fds_before = open_fd_count();
    MMapFile f;
    for (int i = 0; i < 5; ++i) {
        // an empty file mapped with size 0: open succeeds, mmap fails
        EXPECT_THROW(f.initialize({.filename = path, .lock = false, .size = 0}), QuarkError);
        EXPECT_EQ(f.addr(), nullptr);
        EXPECT_EQ(f.fd(), -1);
    }
    EXPECT_THROW(f.initialize({.filename = "/nonexistent-dir/x", .lock = false, .size = 64}), QuarkError);
    EXPECT_EQ(open_fd_count(), fds_before);
    f.initialize({.filename = path, .lock = false, .size = 64});  // the object is still usable
    EXPECT_NE(f.addr(), nullptr);
    f.reset();
    std::filesystem::remove(path);
}

TEST(MMapFileTest, AnonymousMapping) {
    MMapFile f;
    f.initialize({.filename = "", .lock = false, .size = 8192});
    ASSERT_NE(f.addr(), nullptr);
    EXPECT_EQ(f.fd(), -1);
    f.addr()[8191] = 1;
}

TEST(FullDuplexShmTest, RoundTripAndOversizeWrites) {
    using Shm = FullDuplexShm<4096>;
    const std::string a_to_b = temp_path("a2b"), b_to_a = temp_path("b2a");
    std::filesystem::remove(a_to_b);
    std::filesystem::remove(b_to_a);
    {
        Shm a(b_to_a, a_to_b, true);
        Shm b(a_to_b, b_to_a, true);

        EXPECT_TRUE(a.try_write(5, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "hello", 5); }));
        std::string got;
        EXPECT_TRUE(b.try_read([&](Shm::MsgHeader* h) {
            got.assign(reinterpret_cast<const char*>(h + 1), h->size - sizeof(Shm::MsgHeader));
        }));
        EXPECT_EQ(got, "hello");
        EXPECT_FALSE(b.try_read([](Shm::MsgHeader*) {}));

        // sizes whose header + payload would wrap the queue's 16-bit size, or exceed the ring
        bool wrote = false;
        auto writer = [&](Shm::MsgHeader*) { wrote = true; };
        EXPECT_FALSE(a.try_write(65528, writer));
        EXPECT_FALSE(a.try_write(65530, writer));
        EXPECT_FALSE(a.try_write(Shm::kMaxPayload + 1, writer));
        EXPECT_FALSE(wrote);
        EXPECT_FALSE(b.try_read([](Shm::MsgHeader*) {}));

        // a second writer on the same queue is refused (flock)
        EXPECT_THROW(Shm(b_to_a, a_to_b, true), QuarkError);
    }
    std::filesystem::remove(a_to_b);
    std::filesystem::remove(b_to_a);
}
