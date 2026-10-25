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

# crypto
quark_add_test(quark_test_encoding crypto/EncodingTest.cpp)
quark_add_test(quark_test_crypto crypto/SignerTest.cpp)

# maths
quark_add_test(quark_test_double_ops math/DoubleOpsTest.cpp)
quark_add_test(quark_test_math_ops math/MathOpsTest.cpp)
quark_add_test(quark_test_scale_int math/ScaleIntTest.cpp)

# net
quark_add_test(quark_test_alive_monitor net/AliveMonitorTest.cpp)
quark_add_test(quark_test_buffer_pool net/BufferPoolTest.cpp)
quark_add_test(quark_test_exp_backoff net/ExpBackoffTest.cpp)
quark_add_test(quark_test_http_codec net/HttpCodecTest.cpp)
quark_add_test(quark_test_rx_buf net/RxBufTest.cpp)
quark_add_test(quark_test_tx_buf net/TxBufTest.cpp)
quark_add_test(quark_test_url net/UrlTest.cpp)
quark_add_test(quark_test_ws_frame net/WsFrameTest.cpp)
quark_add_test(quark_test_ws_handshake net/WsHandshakeTest.cpp)

if (NOT QUARK_USE_FSTACK)
    # event loop and integration tests against loopback TCP/TLS servers (tests/net/TestServers.h,
    # OpenSSL server side); they need kernel sockets
    quark_add_test(quark_test_event_loop net/EventLoopTest.cpp)
    quark_add_test(quark_test_tcp_socket net/TcpSocketTest.cpp)
    quark_add_test(quark_test_tls_stream net/TlsStreamTest.cpp)
    quark_add_test(quark_test_ws_session net/WsSessionTest.cpp)
    quark_add_test(quark_test_http_client net/HttpClientTest.cpp)
    foreach (net_test quark_test_tcp_socket quark_test_tls_stream quark_test_ws_session quark_test_http_client)
        target_link_libraries(${net_test} PRIVATE OpenSSL::SSL)
    endforeach ()
else ()
    # F-Stack: one binary with its own main (F-Stack's loop must own the main thread), run by hand
    # as root: sudo ./quark_test_fstack --conf <f-stack.conf>. Not registered with ctest.
    add_executable(quark_test_fstack net/FStackTest.cpp)
    quark_set_common_compile_options(quark_test_fstack)
    target_link_libraries(quark_test_fstack PRIVATE quark::quark GTest::gtest OpenSSL::SSL)
    target_compile_definitions(quark_test_fstack PRIVATE QUARK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif ()

# utils
quark_add_test(quark_test_hw_endian utils/HwEndianTest.cpp)
quark_add_test(quark_test_inplace_function utils/InplaceFunctionTest.cpp)
quark_add_test(quark_test_json_parser utils/JsonParserTest.cpp)
quark_add_test(quark_test_memory_arena utils/MemoryArenaTest.cpp)
quark_add_test(quark_test_simple_object_pool utils/SimpleObjectPoolTest.cpp)
quark_add_test(quark_test_timer_queue utils/TimerQueueTest.cpp)
quark_add_test(quark_test_trading_day utils/TradingDayTest.cpp)
