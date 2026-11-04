#-------------------------------------------------------------------------------------------------------
# Google Benchmark: reuse the one of the parent project, fetch a pinned version otherwise. A system
# package is deliberately not used: distro builds can ship LTO bytecode from another GCC release.
#-------------------------------------------------------------------------------------------------------
if (NOT TARGET benchmark::benchmark_main)
    message(STATUS "Fetching Google Benchmark from github")
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.10.0
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
    # system headers, so their warnings (e.g. -Wredundant-decls) don't break targets built with -Werror
    get_target_property(BENCHMARK_INCLUDE_DIRS benchmark INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(benchmark PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${BENCHMARK_INCLUDE_DIRS}")
else ()
    message(STATUS "Using Google Benchmark of the parent project")
endif ()

# Builds a benchmark executable; run it directly (not registered with ctest)
function(quark_add_benchmark bench_target source_file)
    add_executable(${bench_target} ${source_file})
    quark_set_common_compile_options(${bench_target})
    target_link_libraries(${bench_target} PRIVATE quark::quark benchmark::benchmark_main)
endfunction()

quark_add_benchmark(quark_bench_buffer_pool net/BufferPoolBench.cpp)
quark_add_benchmark(quark_bench_simple_object_pool net/SimpleObjectPoolBench.cpp)
quark_add_benchmark(quark_bench_loop_utils utils/LoopUtilsBench.cpp)
quark_add_benchmark(quark_bench_utils utils/UtilsBench.cpp)
target_compile_definitions(quark_bench_utils PRIVATE QUARK_BENCH_DATA_DIR="${PROJECT_SOURCE_DIR}/tests")
quark_add_benchmark(quark_bench_codec net/CodecBench.cpp)
quark_add_benchmark(quark_bench_crypto crypto/CryptoBench.cpp)
quark_add_benchmark(quark_bench_math math/MathBench.cpp)

# reuses the loopback servers and test certificate of the net tests (kernel sockets)
if (NOT QUARK_USE_FSTACK)
    quark_add_benchmark(quark_bench_event_loop net/EventLoopBench.cpp)
    quark_add_benchmark(quark_bench_ws_session net/WsSessionBench.cpp)
    target_include_directories(quark_bench_ws_session PRIVATE ${PROJECT_SOURCE_DIR}/tests/net)
    target_compile_definitions(quark_bench_ws_session PRIVATE QUARK_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(quark_bench_ws_session PRIVATE ${QUARK_SSL_LIB})
endif ()

# hash container comparison: std vs absl vs ankerl::unordered_dense, both fetched for this
# benchmark only (quark itself depends on neither) and never installed
if (NOT TARGET absl::flat_hash_map)
    FetchContent_Declare(
        absl
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG        20260817.0
    )
    set(ABSL_PROPAGATE_CXX_STD ON)
    set(ABSL_ENABLE_INSTALL OFF)
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(absl)
endif ()
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v4.5.0
)
FetchContent_MakeAvailable(unordered_dense)
quark_add_benchmark(quark_bench_containers utils/ContainerBench.cpp)
target_link_libraries(quark_bench_containers PRIVATE absl::flat_hash_map absl::flat_hash_set
                      unordered_dense::unordered_dense)
# system headers, so absl's __int128 doesn't trip -Wpedantic under -Werror
target_include_directories(quark_bench_containers SYSTEM PRIVATE ${absl_SOURCE_DIR})
