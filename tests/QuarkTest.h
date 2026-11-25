#pragma once

// Test helpers for errors quark reports through QUARK_THROW: a QuarkError in a normal build, a
// fatal error (message on stderr, then abort) with QUARK_NO_EXCEPTIONS, where the tests are built
// with -fno-exceptions too. The same expectation covers both builds.

#include "quark/QuarkError.h"

#include <gtest/gtest.h>

#include <regex>

namespace quark::test {

// Death tests fork; quill's backend thread (and the tests' own threads) make gtest's default
// "fast" style unsafe, "threadsafe" re-executes the binary for the child instead. Set from a test
// environment, once gtest is initialized: a static initializer could run before gtest's flag
// string is constructed (static initialization order).
class DeathTestStyle : public ::testing::Environment {
public:
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};
inline ::testing::Environment* const kDeathTestStyle = ::testing::AddGlobalTestEnvironment(new DeathTestStyle);

}  // namespace quark::test

#if defined(QUARK_NO_EXCEPTIONS)
    // `pattern` (a regex) must match the fatal message on stderr ("Quark fatal error: <what> (file:line)")
    #define QUARK_EXPECT_ERROR(statement, pattern) EXPECT_DEATH({ statement; }, pattern)
    // errors that are not quark's (quill's, with QUILL_NO_EXCEPTIONS): any death
    #define QUARK_EXPECT_ANY_ERROR(statement) EXPECT_DEATH({ statement; }, "")
#else
    // `pattern` (a regex) must match QuarkError::what()
    #define QUARK_EXPECT_ERROR(statement, pattern)                                                  \
        do {                                                                                        \
            try {                                                                                   \
                statement;                                                                          \
                ADD_FAILURE() << "expected a QuarkError from: " #statement;                         \
            } catch (const ::quark::QuarkError& quark_test_error_) {                               \
                EXPECT_TRUE(std::regex_search(quark_test_error_.what(), std::regex{pattern}))         \
                    << "QuarkError message: " << quark_test_error_.what() << "\nexpected: " << pattern; \
            }                                                                                       \
        } while (0)
    #define QUARK_EXPECT_ANY_ERROR(statement) EXPECT_ANY_THROW(statement)
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/vfs.h>
#include <unistd.h>

namespace quark::test {

/// f_type of the filesystem holding `path`, 0 if it can't be determined.
inline long fs_type(const std::string& path) {
    struct statfs s {};
    return ::statfs(path.c_str(), &s) == 0 ? static_cast<long>(s.f_type) : 0;
}

/// The test process's pid, also inside a death test's child (threadsafe style re-runs the test in a
/// child, which dies before its cleanup): per-process paths are then the same as the parent's, which
/// removes them.
inline int test_pid() {
    return GTEST_FLAG_GET(internal_run_death_test).empty() ? ::getpid() : ::getppid();
}

/// A per-process directory on tmpfs (FullDuplexShm refuses anything else); empty if /dev/shm isn't tmpfs.
inline std::string tmpfs_dir(std::string_view name) {
    constexpr long kTmpfsMagic = 0x01021994;
    if (fs_type("/dev/shm") != kTmpfsMagic) return {};
    return "/dev/shm/quark_test_" + std::to_string(test_pid()) + "_" + std::string(name);
}

/// A per-process directory that is NOT on tmpfs/hugetlbfs (to check refusals); empty if none is found.
inline std::string non_tmpfs_dir(std::string_view name) {
    constexpr long kTmpfsMagic = 0x01021994, kHugetlbfsMagic = 0x958458f6;
    for (const std::filesystem::path& base : {std::filesystem::temp_directory_path(), std::filesystem::current_path()}) {
        const long t = fs_type(base.string());
        if (t != 0 && t != kTmpfsMagic && t != kHugetlbfsMagic) {
            return base.string() + "/quark_test_" + std::to_string(test_pid()) + "_" + std::string(name);
        }
    }
    return {};
}

}  // namespace quark::test
