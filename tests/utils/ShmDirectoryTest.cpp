#include "quark/utils/FullDuplexShm.h"
#include "quark/utils/ShmDirectory.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace quark;

namespace {

using Shm = FullDuplexShm<4096>;

class TestShmDirectory : public ::testing::Test {
protected:
    std::string dir = (std::filesystem::temp_directory_path() / ("quark_shmdir_" + std::to_string(::getpid()))).string();
    ShmDirectory sessions{dir, ".ts", ".algo"};

    void SetUp() override { std::filesystem::remove_all(dir); }
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
    EXPECT_EQ(sessions.alive(), (std::vector<std::string>{"algo_a", "algo_b"}));

    Shm server(sessions.client_path("algo_a"), sessions.server_path("algo_a"), false);
    ASSERT_TRUE(a->try_write(5, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "login", 5); }));
    EXPECT_EQ(read_one(server), "login");
    ASSERT_TRUE(server.try_write(2, [](Shm::MsgHeader* h) { std::memcpy(h + 1, "ok", 2); }));
    EXPECT_EQ(read_one(*a), "ok");
    EXPECT_TRUE(sessions.is_alive("algo_a"));  // the server's own mapping doesn't count
}

TEST_F(TestShmDirectory, ExitedClientIsNotAlive) {
    auto a = client("algo_a");
    EXPECT_TRUE(sessions.is_alive("algo_a"));
    a.reset();  // its lock goes with its fd, also on a crash
    EXPECT_FALSE(sessions.is_alive("algo_a"));
    EXPECT_TRUE(sessions.alive().empty());

    a = client("algo_a");  // a restarted client reuses its files
    EXPECT_TRUE(sessions.is_alive("algo_a"));
}

TEST_F(TestShmDirectory, IgnoresIncompletePairsAndOtherFiles) {
    std::filesystem::create_directories(dir);
    std::ofstream(sessions.client_path("half"));  // no server file, no lock
    std::ofstream(dir + "/notes.txt");
    EXPECT_TRUE(sessions.alive().empty());
    EXPECT_FALSE(sessions.is_alive("missing"));
    EXPECT_TRUE(ShmDirectory("/no/such/dir", ".ts", ".algo").alive().empty());
}

// The bytes FullDuplexShm leaves in its files, as alphaframe's and tradeserver's SPSCVarQueue copies
// read and write them: 64-byte blocks, write_idx right after them, read_idx 128 bytes later.
TEST(TestShmLayout, MatchesTheLegacyQueueLayout) {
    static_assert(sizeof(SPSCVarQueue<64 * 1024 * 1024>) == 64 * 1024 * 1024 + 256,
                  "the legacy tradeserver accepts only 64 MiB + 256 byte SHM files");
    constexpr uint32_t kBytes = 4096;
    const std::string path = (std::filesystem::temp_directory_path() / "quark_shm_layout").string();
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

    // the same bytes plus a message written the legacy way (block 1, write_idx 2) pop in order
    using Queue = SPSCVarQueue<kBytes>;
    alignas(Queue) static unsigned char mem[sizeof(Queue)];
    std::memcpy(mem, raw.data(), sizeof(mem));
    const uint16_t legacy_size = 8 + 3;
    std::memcpy(mem + 64, &legacy_size, 2);
    std::memcpy(mem + 64 + 8, "old", 3);
    write_idx = 2;
    std::memcpy(mem + kBytes, &write_idx, 4);
    std::vector<std::string> got;
    auto* q = reinterpret_cast<Queue*>(mem);
    while (q->tryPop([&](Queue::MsgHeader* h) { got.emplace_back(reinterpret_cast<const char*>(h + 1), h->size - 8); })) {
    }
    EXPECT_EQ(got, (std::vector<std::string>{"hello", "old"}));
    std::filesystem::remove(path);
    std::filesystem::remove(back);
}
