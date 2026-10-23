#-------------------------------------------------------------------------------------------------------
# GoogleTest: reuse the one of the parent project or of the system, fetch it otherwise
#-------------------------------------------------------------------------------------------------------
if (NOT TARGET GTest::gtest_main)
    find_package(GTest CONFIG QUIET)
endif ()
if (NOT TARGET GTest::gtest_main)
    message(STATUS "GoogleTest not found locally, fetching it from github")
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.18.0
    )
    # gtest must not be installed along with quark
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
else ()
    message(STATUS "Using GoogleTest found at ${GTest_DIR}")
endif ()

include(GoogleTest)

# Builds a gtest executable and registers each of its test cases to ctest
function(quark_add_test test_target source_file)
    add_executable(${test_target} ${source_file})
    quark_set_common_compile_options(${test_target})

    target_link_libraries(${test_target} PRIVATE quark::quark GTest::gtest_main)
    target_compile_definitions(${test_target} PRIVATE
            QUARK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    # PRE_TEST: discover the cases when ctest runs, so a crashing test binary doesn't break the build
    gtest_discover_tests(${test_target} DISCOVERY_MODE PRE_TEST)
endfunction()

# maths
quark_add_test(quark_test_double_ops math/DoubleOpsTest.cpp)
quark_add_test(quark_test_math_ops math/MathOpsTest.cpp)
quark_add_test(quark_test_scale_int math/ScaleIntTest.cpp)

# net
quark_add_test(quark_test_rx_buf net/RxBufTest.cpp)
quark_add_test(quark_test_tx_buf net/TxBufTest.cpp)

# utils
quark_add_test(quark_test_json_parser utils/JsonParserTest.cpp)
quark_add_test(quark_test_timer_queue utils/TimerQueueTest.cpp)
quark_add_test(quark_test_trading_day utils/TradingDayTest.cpp)
