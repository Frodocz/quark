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
