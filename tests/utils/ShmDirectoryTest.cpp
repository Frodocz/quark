#include "quark/utils/FullDuplexShm.h"
#include "quark/utils/ShmDirectory.h"

#include <gtest/gtest.h>

#include "QuarkTest.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace quark;

namespace {

using Shm = FullDuplexShm<4096>;

class TestShmDirectory : public ::testing::Test {
protected:
    std::string dir = quark::test::tmpfs_dir("shmdir");  // FullDuplexShm accepts tmpfs only
    ShmDirectory sessions{dir, ".ts", ".algo"};

    void SetUp() override {
        if (dir.empty()) GTEST_SKIP() << "/dev/shm is not tmpfs";
        std::filesystem::remove_all(dir);
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    // a client creates both files and writes to <name>.ts
    std::unique_ptr<Shm> client(const std::string& name) {
        return std::make_unique<Shm>(sessions.server_path(name), sessions.client_path(name), true);
    }
};

std::string read_one(Shm& shm) {
    std::string got;
    shm.try_read([&](Shm::MsgHeader* h) { got.assign(reinterpret_cast<const char*>(h + 1), h->size - sizeof(*h)); });
    return got;
}

}  // namespace

TEST_F(TestShmDirectory, FindsRunningClientsAndTalksToThem) {
    auto a = client("algo_a");
    auto b = client("algo_b");
    EXPECT_EQ(sessions.alive(), (std::vector<std::string>{"algo_a", "algo_b"}));  // engaged, both names

    Shm server(sessions.client_path("algo_a"), sessions.server_path("algo_a"), false);
    ASSERT_TRUE(a->try_write(5, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "login", 5); }));
    EXPECT_EQ(read_one(server), "login");
    ASSERT_TRUE(server.try_write(2, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "ok", 2); }));
    EXPECT_EQ(read_one(*a), "ok");
    EXPECT_EQ(sessions.liveness("algo_a"), Liveness::Alive);  // the server's own mapping doesn't count
}

TEST_F(TestShmDirectory, ExitedClientIsNotAlive) {
    auto a = client("algo_a");
    EXPECT_EQ(sessions.liveness("algo_a"), Liveness::Alive);
    a.reset();  // its lock goes with its fd, also on a crash
    EXPECT_EQ(sessions.liveness("algo_a"), Liveness::Dead);
    EXPECT_EQ(sessions.alive(), std::vector<std::string>{});

    a = client("algo_a");  // a restarted client reuses its files
    EXPECT_EQ(sessions.liveness("algo_a"), Liveness::Alive);
}

TEST_F(TestShmDirectory, IgnoresIncompletePairsAndOtherFiles) {
    std::filesystem::create_directories(dir);
    std::ofstream(sessions.client_path("half"));  // no server file, no lock
    std::ofstream(dir + "/notes.txt");
    EXPECT_EQ(sessions.alive(), std::vector<std::string>{});
    EXPECT_EQ(sessions.liveness("missing"), Liveness::Dead);
    // no directory yet (no client ever started): no clients, not an error
    EXPECT_EQ(ShmDirectory("/no/such/dir", ".ts", ".algo").alive(), std::vector<std::string>{});
}

// A scan that fails must say so: reporting "no clients" instead made the tradeserver log every live
// algo out (and cancel its orders with cancel_all_on_disconnect).
TEST_F(TestShmDirectory, ScanErrorsAreReportedNotTakenForNoClients) {
    auto a = client("algo_a");
    ASSERT_EQ(sessions.alive(), std::vector<std::string>{"algo_a"});

    rlimit saved{};
    ::getrlimit(RLIMIT_NOFILE, &saved);
    int used = 0;
    for (int fd = 0; fd < 4096; ++fd) used += ::fcntl(fd, F_GETFD) != -1;
    const rlimit full{static_cast<rlim_t>(used), saved.rlim_max};  // no fd left: EMFILE
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &full), 0);
    const auto scanned = sessions.alive();
    const Liveness live = sessions.liveness("algo_a");
    ::setrlimit(RLIMIT_NOFILE, &saved);
    EXPECT_FALSE(scanned.has_value());
    EXPECT_EQ(live, Liveness::Unknown);

    if (::geteuid() != 0) {  // root lists the directory anyway
        ::chmod(dir.c_str(), 0300);
        const auto unreadable = sessions.alive();
        ::chmod(dir.c_str(), 0700);
        EXPECT_FALSE(unreadable.has_value());
    }
    EXPECT_EQ(sessions.alive(), std::vector<std::string>{"algo_a"});
}

// A server built with another queue size must refuse the client's files, not resize them: a shrunk
// file kills the client (SIGBUS) on its next write past the new end.
TEST(TestFullDuplexShm, OpeningFilesOfAnotherSizeFailsAndLeavesThemAlone) {
    const std::string dir = quark::test::tmpfs_dir("size");
    if (dir.empty()) GTEST_SKIP() << "/dev/shm is not tmpfs";
    std::filesystem::remove_all(dir);
    {
        FullDuplexShm<8192> client(dir + "/c.ts", dir + "/c.algo", true);
        const auto size = std::filesystem::file_size(dir + "/c.algo");
        QUARK_EXPECT_ERROR((FullDuplexShm<4096>(dir + "/c.algo", dir + "/c.ts", false)), "is 8448 bytes, expected 4352");
        EXPECT_EQ(std::filesystem::file_size(dir + "/c.algo"), size);
        EXPECT_EQ(std::filesystem::file_size(dir + "/c.ts"), size);
        ASSERT_TRUE(client.try_write(4, [](auto* h) { std::memcpy(h + 1, "live", 4); }));  // still usable
    }
    std::filesystem::remove_all(dir);
}

// On a disk filesystem the kernel writes the queue pages back (~every 30 s) and write-protects them:
// every next write to each page faults on the order path. Only tmpfs/hugetlbfs are accepted.
TEST(TestFullDuplexShm, RefusesFilesOutsideTmpfs) {
    const std::string dir = quark::test::non_tmpfs_dir("disk");
    if (dir.empty()) GTEST_SKIP() << "no non-tmpfs directory to test with";
    std::filesystem::remove_all(dir);
    QUARK_EXPECT_ERROR((FullDuplexShm<4096>(dir + "/c.ts", dir + "/c.algo", true)), "tmpfs");
    EXPECT_FALSE(std::filesystem::exists(dir + "/c.algo"));  // refused before creating anything

    // the files of a client (created another way) are refused too
    std::filesystem::create_directories(dir);
    for (const char* f : {"/c.ts", "/c.algo"}) {
        MMapFile m;
        m.initialize({.filename = dir + f, .lock = false, .size = sizeof(SPSCVarQueue<4096>)});
    }
    QUARK_EXPECT_ERROR((FullDuplexShm<4096>(dir + "/c.algo", dir + "/c.ts", false)), "tmpfs");
    std::filesystem::remove_all(dir);
}

// Messages a client queued before the server opened the session are stale (e.g. orders sent while the
// tradeserver restarted): they are dropped, by design, and counted.
TEST(TestFullDuplexShm, DropsUnreadMessagesOnOpenAndCountsThem) {
    const std::string dir = quark::test::tmpfs_dir("drop");
    if (dir.empty()) GTEST_SKIP() << "/dev/shm is not tmpfs";
    std::filesystem::remove_all(dir);
    {
        FullDuplexShm<4096> client(dir + "/c.ts", dir + "/c.algo", true);
        EXPECT_EQ(client.dropped_on_open(), 0u);
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(client.try_write(1, [](auto* h) { *reinterpret_cast<char*>(h + 1) = 'x'; }));
        FullDuplexShm<4096> server(dir + "/c.algo", dir + "/c.ts", false);
        EXPECT_EQ(server.dropped_on_open(), 3u);
        EXPECT_FALSE(server.try_read([](auto*) {}));
    }
    std::filesystem::remove_all(dir);
}

// A server opens its clients' files at run time: failures must be reportable without throwing (or,
// with QUARK_NO_EXCEPTIONS, aborting the server).
TEST(TestFullDuplexShm, TryOpenReportsErrorsWithoutThrowing) {
    using Shm4k = FullDuplexShm<4096>;
    EXPECT_EQ(Shm4k::kDefaultDir, "/dev/shm");
    const std::string dir = quark::test::tmpfs_dir("try");
    if (dir.empty()) GTEST_SKIP() << "/dev/shm is not tmpfs";
    std::filesystem::remove_all(dir);
    std::string error;
    EXPECT_FALSE(Shm4k::try_open(dir + "/none.algo", dir + "/none.ts", false, &error));  // no such client
    EXPECT_NE(error.find("none"), std::string::npos) << error;

    auto client = Shm4k::try_open(dir + "/c.ts", dir + "/c.algo", true, &error);
    ASSERT_TRUE(client) << error;
    auto server = Shm4k::try_open(dir + "/c.algo", dir + "/c.ts", false, &error);
    ASSERT_TRUE(server) << error;
    ASSERT_TRUE(client->try_write(2, [](auto* h) { std::memcpy(h + 1, "hi", 2); }));
    EXPECT_TRUE(server->try_read([](auto*) {}));

    error.clear();
    EXPECT_FALSE(Shm4k::try_open(dir + "/c.ts", dir + "/c.algo", true, &error));  // a second client: flock
    EXPECT_NE(error.find("flock"), std::string::npos) << error;
    EXPECT_FALSE(FullDuplexShm<8192>::try_open(dir + "/c.algo", dir + "/c.ts", false, &error));  // wrong size
    EXPECT_NE(error.find("expected"), std::string::npos) << error;

    if (const std::string disk = quark::test::non_tmpfs_dir("try"); !disk.empty()) {
        EXPECT_FALSE(Shm4k::try_open(disk + "/c.ts", disk + "/c.algo", true, &error));
        EXPECT_NE(error.find("tmpfs"), std::string::npos) << error;
        std::filesystem::remove_all(disk);
    }
    client.reset();
    server.reset();
    std::filesystem::remove_all(dir);
}

// The bytes FullDuplexShm leaves in its files, as alphaframe's and tradeserver's SPSCVarQueue copies
// read and write them: 64-byte blocks, write_idx right after them, read_idx 128 bytes later.
TEST(TestShmLayout, MatchesTheLegacyQueueLayout) {
    static_assert(sizeof(SPSCVarQueue<64 * 1024 * 1024>) == 64 * 1024 * 1024 + 256,
                  "the legacy tradeserver accepts only 64 MiB + 256 byte SHM files");
    constexpr uint32_t kBytes = 4096;
    const std::string dir = quark::test::tmpfs_dir("layout");
    if (dir.empty()) GTEST_SKIP() << "/dev/shm is not tmpfs";
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/quark_shm_layout";
    const std::string back = path + ".back";
    std::filesystem::remove(path);
    std::filesystem::remove(back);
    {
        Shm writer(back, path, true);
        ASSERT_TRUE(writer.try_write(5, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "hello", 5); }));
    }
    std::ifstream in(path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), {});
    ASSERT_EQ(raw.size(), kBytes + 256);
    uint16_t size;
    uint32_t write_idx, read_idx;
    std::memcpy(&size, raw.data(), 2);
    std::memcpy(&write_idx, raw.data() + kBytes, 4);
    std::memcpy(&read_idx, raw.data() + kBytes + 128, 4);
    EXPECT_EQ(size, 8 + 5);                 // MsgHeader{size, msg_type, userdata} + payload
    EXPECT_EQ(raw.substr(8, 5), "hello");
    EXPECT_EQ(write_idx, 1u);                // one block used
    EXPECT_EQ(read_idx, 0u);

    // the same bytes plus a message written the legacy way (block 1, write_idx 2) pop in order,
    // from a real queue object (trivially copyable: it may take the bytes)
    using Queue = SPSCVarQueue<kBytes>;
    static_assert(std::is_trivially_copyable_v<Queue>);
    auto q = std::make_unique<Queue>();
    auto* bytes = reinterpret_cast<unsigned char*>(q.get());
    std::memcpy(bytes, raw.data(), sizeof(Queue));
    const uint16_t legacy_size = 8 + 3;
    std::memcpy(bytes + 64, &legacy_size, 2);
    std::memcpy(bytes + 64 + 8, "old", 3);
    write_idx = 2;
    std::memcpy(bytes + kBytes, &write_idx, 4);
    // The queue's asm barriers declare write_idx and the blocks as overwritten ("=m"), so an
    // optimizer drops these stores as dead: the real writer is another process. A compiler
    // barrier keeps them, like that process's writes.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::vector<std::string> got;
    while (q->tryPop([&](Queue::MsgHeader* h) { got.emplace_back(reinterpret_cast<const char*>(h + 1), h->size - 8); })) {
    }
    EXPECT_EQ(got, (std::vector<std::string>{"hello", "old"}));
    std::filesystem::remove_all(dir);
}
