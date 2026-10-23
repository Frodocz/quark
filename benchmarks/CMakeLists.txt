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
quark_add_benchmark(quark_bench_simple_object_pool utils/SimpleObjectPoolBench.cpp)
