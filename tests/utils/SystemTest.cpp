#include "quark/utils/System.h"
#include "gtest/gtest.h"

#include <sched.h>
#include <sys/mman.h>

#include <csignal>
#include <cstring>
#include <thread>

using namespace quark;

class TestSystem : public ::testing::Test {};

TEST_F(TestSystem, CpuListParsing) {
    EXPECT_TRUE(cpu_list_contains("0-3,8,10-11\n", 2));
    EXPECT_TRUE(cpu_list_contains("0-3,8,10-11", 8));
    EXPECT_TRUE(cpu_list_contains("0-3,8,10-11", 11));
    EXPECT_FALSE(cpu_list_contains("0-3,8,10-11", 9));
    EXPECT_FALSE(cpu_list_contains("", 0));
    EXPECT_FALSE(cpu_list_contains("\n", 0));
    EXPECT_FALSE(cpu_list_contains("x-3,5-2", 4));  // malformed items are ignored
}

TEST_F(TestSystem, PinThreadToCpu) {
    std::thread t([] {
        cpu_set_t allowed;
        ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
        int cpu = 0;
        while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu;
        ASSERT_TRUE(pin_thread_to_cpu(cpu));
        cpu_set_t now;
        ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(now), &now), 0);
        EXPECT_EQ(CPU_COUNT(&now), 1);
        EXPECT_TRUE(CPU_ISSET(cpu, &now));
        EXPECT_EQ(current_cpu(), cpu);
    });
    t.join();
    EXPECT_FALSE(pin_thread_to_cpu(-1));
}

TEST_F(TestSystem, ThreadNameIsTruncatedTo15Chars) {
    std::thread t([] {
        ASSERT_TRUE(set_thread_name("io-loop-primary-0"));
        char name[32] = {};
        ASSERT_EQ(pthread_getname_np(pthread_self(), name, sizeof(name)), 0);
        EXPECT_STREQ(name, "io-loop-primary");
    });
    t.join();
}

TEST_F(TestSystem, RealtimeRejectsBadPriority) {
    EXPECT_FALSE(set_thread_realtime(0));
    EXPECT_FALSE(set_thread_realtime(100));
    // a valid priority needs CAP_SYS_NICE: either outcome is fine, it must just not crash
    std::thread t([] { (void)set_thread_realtime(1); });
    t.join();
}

TEST_F(TestSystem, LockAllMemory) {
    // in a child process: MCL_FUTURE would otherwise lock every later allocation of this binary
    EXPECT_EXIT(
        {
            if (!lock_all_memory()) std::_Exit(2);  // e.g. RLIMIT_MEMLOCK too small
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(TestSystem, CrashHandlerPrintsTheSignalAndABacktrace) {
    EXPECT_DEATH(
        {
            install_crash_handler();
            std::raise(SIGSEGV);
        },
        "fatal signal SEGV, backtrace:");
}

TEST_F(TestSystem, ShutdownHandlerFlagsTheFirstSignalAndKillsOnTheSecond) {
    EXPECT_EXIT(
        {
            install_shutdown_handler();
            if (shutdown_requested()) std::_Exit(3);
            std::raise(SIGTERM);
            if (!shutdown_requested()) std::_Exit(4);
            std::raise(SIGTERM);  // the second one terminates
            std::_Exit(5);
        },
        ::testing::KilledBySignal(SIGTERM), "");
}
