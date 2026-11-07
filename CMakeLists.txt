cmake_minimum_required(VERSION 3.16...4.2)  # 3.16: GoogleTest's minimum (tests build by default)

project(quark)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#-------------------------------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------------------------------
option(BUILD_SHARED_LIBS "Build quark as a shared library instead of a static library." OFF)

option(QUARK_BUILD_BENCHMARKS "Build the benchmarks." OFF)
option(QUARK_BUILD_EXAMPLES "Build example programs" ON)
option(QUARK_BUILD_TESTS "Build unit tests" ON)
option(QUARK_CODE_COVERAGE "Enable code coverage analysis during the build." OFF)
option(QUARK_DOCS_GEN "Generate documentation during the build process." OFF)
option(QUARK_ENABLE_ASSERTIONS "Enable assertions in release builds. When disabled, assertions are only active in debug builds." OFF)
option(QUARK_ENABLE_INSTALL "Enable the CMake install target when Quark is not the master project." OFF)
option(QUARK_ENABLE_LOGGER "Enable logs for the framework" ON)
option(QUARK_ENABLE_TIME_TRACE "Enable -ftime-trace to generate compile-time profiling data. Requires Clang." OFF)
option(QUARK_LOG_HUGE_PAGES "Back logging queues with 2 MiB huge pages, falling back to normal pages when none are available (Linux)." ON)
option(QUARK_NO_EXCEPTIONS "Build without exception handling support." OFF)
option(QUARK_SANITIZE_ADDRESS "Enable AddressSanitizer (ASan) for memory error detection in tests." OFF)
option(QUARK_SANITIZE_THREAD "Enable ThreadSanitizer (TSan) for detecting thread-related issues in tests. Note: Using this option with non-Clang compilers may produce false positives." OFF)
option(QUARK_USE_FSTACK "Run quark::net over F-Stack (DPDK userspace TCP/IP) instead of kernel sockets." OFF)
option(QUARK_USE_VALGRIND "Use Valgrind as the default memory checking tool in CTest. Valgrind must be installed." OFF)

# TLS and crypto backend of quark::crypto and quark::net. AWS-LC (default) avoids the lock and allocation
# overhead OpenSSL 3 added to its hot paths; it is found installed (CMAKE_PREFIX_PATH / AWSLC_ROOT) or fetched.
set(QUARK_TLS_BACKEND AWS-LC CACHE STRING "TLS/crypto library used by quark: AWS-LC or OpenSSL (>= 3.0)")
set_property(CACHE QUARK_TLS_BACKEND PROPERTY STRINGS AWS-LC OpenSSL)
if (NOT QUARK_TLS_BACKEND MATCHES "^(AWS-LC|OpenSSL)$")
    message(FATAL_ERROR "Invalid QUARK_TLS_BACKEND: ${QUARK_TLS_BACKEND} (expected AWS-LC or OpenSSL)")
endif ()
set(QUARK_AWSLC_VERSION 5.12.0)

# Internal option
set(QUARK_ENABLE_GCC_HARDENING OFF CACHE INTERNAL "")
set(QUARK_LOG_INITIAL_QUEUE_CAPACITY 2097152 CACHE STRING "Capacity in bytes of each logging thread's queue, default 2MB (quill only accepts it at compile time).")
set(QUARK_LOG_QUEUE_TYPE BoundedDropping CACHE STRING "Queue type of every logging thread's queue (quill only accepts it at compile time).")
set_property(CACHE QUARK_LOG_QUEUE_TYPE PROPERTY STRINGS UnboundedBlocking UnboundedDropping BoundedBlocking BoundedDropping)
if (NOT QUARK_LOG_QUEUE_TYPE MATCHES "^(UnboundedBlocking|UnboundedDropping|BoundedBlocking|BoundedDropping)$")
    message(FATAL_ERROR "Invalid QUARK_LOG_QUEUE_TYPE: ${QUARK_LOG_QUEUE_TYPE}")
endif ()
# Every module of a process must use the same quill, also checked by a static_assert in QuarkLog.cpp
set(QUARK_QUILL_VERSION 13.1.0)
# magic_enum is header-only and exposed through QuarkEnum.h: every module of a process must use the same one
set(QUARK_MAGIC_ENUM_VERSION 0.9.8)

#-------------------------------------------------------------------------------------------------------
# Determine if quark is built as a subproject (using add_subdirectory) or if it is the master project.
#-------------------------------------------------------------------------------------------------------
set(QUARK_MASTER_PROJECT FALSE CACHE BOOL "Master Project" FORCE)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(QUARK_MASTER_PROJECT TRUE CACHE BOOL "Master Project" FORCE)
endif ()

#-------------------------------------------------------------------------------------------------------
# Custom cmake functions
#-------------------------------------------------------------------------------------------------------
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
include(QuarkUtils)

#-------------------------------------------------------------------------------------------------------
# Resolve version
#-------------------------------------------------------------------------------------------------------
quark_extract_version()
project(quark VERSION ${QUARK_VERSION} LANGUAGES CXX)

#-------------------------------------------------------------------------------------------------------
# Set default build to release
#-------------------------------------------------------------------------------------------------------
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif ()

#-------------------------------------------------------------------------------------------------------
# Compiler config
#-------------------------------------------------------------------------------------------------------
if (NOT CMAKE_CXX_STANDARD)
    # If CMAKE_CXX_STANDARD is not set, configure it to 20 by default
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
elseif (CMAKE_CXX_STANDARD LESS 20)
    # If CMAKE_CXX_STANDARD is set but less than 20, issue a fatal error
    message(FATAL_ERROR "Quark requires at least C++20. Please set CMAKE_CXX_STANDARD to 20 or higher.")
endif ()

#-------------------------------------------------------------------------------------------------------
# Required Packages
#-------------------------------------------------------------------------------------------------------
find_package(Threads REQUIRED)

if (QUARK_BUILD_TESTS)
    enable_testing()

    if (QUARK_USE_VALGRIND)
        # find valgrind
        find_program(MEMORYCHECK_COMMAND NAMES valgrind)
        if (NOT MEMORYCHECK_COMMAND)
            message(WARNING "Valgrind not found")
        endif ()

        # set valgrind params
        set(MEMORYCHECK_COMMAND_OPTIONS "--tool=memcheck --leak-check=full --leak-resolution=med --show-leak-kinds=all --track-origins=yes --vgdb=no --fair-sched=yes")

        # add memcheck test action to ctest
        include(CTest)
    endif ()
endif ()

#-------------------------------------------------------------------------------------------------------
# Dependency Management
#-------------------------------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v5.0.3
)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)

# whether to use std::format or use external fmtlib
set(QUARK_USE_STD_FORMAT OFF)

# must be set before FetchContent_MakeAvailable / add_subdirectory
# quark exposes simdjson in its public headers (JsonParser.h), so simdjson must export its own targets
set(SIMDJSON_INSTALL ON CACHE BOOL "" FORCE)
# quill is exposed through QuarkLog.h, so it installs its own headers, config and targets alongside quark
set(QUILL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
# magic_enum is exposed through QuarkEnum.h, same as quill; its tests and examples are not built
set(MAGIC_ENUM_OPT_INSTALL ON CACHE BOOL "" FORCE)
set(MAGIC_ENUM_OPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MAGIC_ENUM_OPT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 13.1 and above fully support std::format
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13.0")
        set(QUARK_USE_STD_FORMAT ON)
    endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang 16.0 and above support most of functionalities of libc++ <format>
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0")
        set(QUARK_USE_STD_FORMAT ON)
    endif ()
endif ()

if (QUARK_USE_STD_FORMAT)
    message(STATUS "Configuring with C++20 std::format (Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
else()
    message(STATUS "Configuring with quill bundled fmtlib (Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif ()

FetchContent_MakeAvailable(simdjson tomlplusplus)

#-------------------------------------------------------------------------------------------------------
# TLS backend: QUARK_SSL_LIB and QUARK_CRYPTO_LIB name the libssl / libcrypto targets used everywhere below
#-------------------------------------------------------------------------------------------------------
if (QUARK_TLS_BACKEND STREQUAL "OpenSSL")
    find_package(OpenSSL 3.0 REQUIRED)
    set(QUARK_SSL_LIB OpenSSL::SSL)
    set(QUARK_CRYPTO_LIB OpenSSL::Crypto)
else ()
    # AWS-LC installs CMake packages named "ssl" and "crypto" (targets AWS::ssl, AWS::crypto); OpenSSL doesn't,
    # so these can't pick up the system OpenSSL by mistake. Reuse a parent project's AWS-LC or an installed one.
    if (NOT TARGET AWS::ssl AND NOT TARGET ssl)
        if (AWSLC_ROOT)
            list(PREPEND CMAKE_PREFIX_PATH ${AWSLC_ROOT})
        endif ()
        find_package(ssl CONFIG QUIET)
    endif ()
    if (TARGET AWS::ssl)
        message(STATUS "Using AWS-LC found at ${ssl_DIR}")
    elseif (TARGET ssl AND TARGET crypto)
        message(STATUS "Using the AWS-LC targets of the parent project")
    else ()
        message(STATUS "AWS-LC not found locally, fetching v${QUARK_AWSLC_VERSION} from github")
        FetchContent_Declare(
            aws-lc
            GIT_REPOSITORY https://github.com/aws/aws-lc.git
            GIT_TAG        v${QUARK_AWSLC_VERSION}
        )
        # libssl + libcrypto only, static and PIC (it may end up inside a shared libquark), and buildable
        # with neither Go nor Perl (pre-generated assembly). Normal variables, restored below; CMP0077 lets
        # AWS-LC's option() calls honor them.
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(_quark_saved_shared ${BUILD_SHARED_LIBS})
        set(_quark_saved_testing ${BUILD_TESTING})
        set(_quark_saved_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TESTING OFF)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        set(BUILD_TOOL OFF)
        set(BUILD_LIBSSL ON)
        set(DISABLE_GO ON)
        set(DISABLE_PERL ON)
        set(ENABLE_SOURCE_MODIFICATION OFF)
        # AWS-LC warns `if (DEFINED ENABLE_PRE_SONAME_BUILD)` before its own option() caches that variable, so every
        # re-configure warns although nobody set it. It only matters for a shared AWS-LC, never built here.
        unset(ENABLE_PRE_SONAME_BUILD CACHE)
        FetchContent_MakeAvailable(aws-lc)
        set(BUILD_SHARED_LIBS ${_quark_saved_shared})
        set(BUILD_TESTING ${_quark_saved_testing})
        set(CMAKE_POSITION_INDEPENDENT_CODE ${_quark_saved_pic})
        unset(CMAKE_POLICY_DEFAULT_CMP0077)
        # AWS-LC installs include/openssl and libssl/libcrypto, which shadow the system OpenSSL in /usr or /usr/local.
        # Checked when installing, against the prefix actually used (`cmake --install --prefix` overrides the
        # configured one), so configuring alone, or installing into a dedicated prefix, doesn't warn.
        install(CODE [[
            if (CMAKE_INSTALL_PREFIX MATCHES "^/usr(/local)?/?$")
                message(WARNING "The fetched AWS-LC is installed along with quark into ${CMAKE_INSTALL_PREFIX}, where "
                        "its openssl/ headers and libssl/libcrypto shadow the system OpenSSL for other software. "
                        "Prefer a dedicated prefix (cmake --install <build> --prefix /opt/quark), or install AWS-LC "
                        "into its own prefix and pass AWSLC_ROOT when configuring quark.")
            endif ()
        ]])
    endif ()
    if (NOT TARGET AWS::ssl)
        add_library(AWS::ssl ALIAS ssl)
        add_library(AWS::crypto ALIAS crypto)
    endif ()
    set(QUARK_SSL_LIB AWS::ssl)
    set(QUARK_CRYPTO_LIB AWS::crypto)
endif ()

# JsonParser.h pulls simdjson.h into user code, mark it as a system header so its warnings
# (e.g. -Wredundant-decls) don't break targets built with -Werror
get_target_property(SIMDJSON_INCLUDE_DIRS simdjson INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(simdjson PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${SIMDJSON_INCLUDE_DIRS}")

# Reuse the quill of the parent project or of the system, fetch it otherwise
if (NOT TARGET quill::quill)
    find_package(quill ${QUARK_QUILL_VERSION} EXACT CONFIG QUIET)
endif ()
if (NOT TARGET quill::quill)
    FetchContent_Declare(
        quill
        GIT_REPOSITORY https://github.com/odygrd/quill.git
        GIT_TAG        v${QUARK_QUILL_VERSION}
    )
    FetchContent_MakeAvailable(quill)
endif ()

# Reuse the magic_enum of the parent project or of the system, fetch it otherwise
if (NOT TARGET magic_enum::magic_enum)
    find_package(magic_enum ${QUARK_MAGIC_ENUM_VERSION} EXACT CONFIG QUIET)
endif ()
if (NOT TARGET magic_enum::magic_enum)
    FetchContent_Declare(
        magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        GIT_TAG        v${QUARK_MAGIC_ENUM_VERSION}
    )
    FetchContent_MakeAvailable(magic_enum)
endif ()
# QuarkEnum.h pulls magic_enum.hpp into user code, mark it as a system header (as simdjson above), so its
# warnings don't break targets built with -Werror
get_target_property(_quark_magic_enum_target magic_enum::magic_enum ALIASED_TARGET)
if (_quark_magic_enum_target)
    get_target_property(MAGIC_ENUM_INCLUDE_DIRS ${_quark_magic_enum_target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${_quark_magic_enum_target} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${MAGIC_ENUM_INCLUDE_DIRS}")
endif ()

#-------------------------------------------------------------------------------------------------------
# Log Info
#-------------------------------------------------------------------------------------------------------
if (QUARK_MASTER_PROJECT)
    option(QUARK_VERBOSE_MAKEFILE "Enable verbose output for makefiles when Quark is the master project. This provides detailed information about the build process." OFF)
    message(STATUS "CMAKE_BUILD_TYPE: " ${CMAKE_BUILD_TYPE})
    message(STATUS "QUARK_VERSION: ${QUARK_VERSION}")
endif ()

message(STATUS "BUILD_SHARED_LIBS: " ${BUILD_SHARED_LIBS})
message(STATUS "CMAKE_CXX_STANDARD: " ${CMAKE_CXX_STANDARD})

message(STATUS "QUARK_BUILD_EXAMPLES: " ${QUARK_BUILD_EXAMPLES})
message(STATUS "QUARK_BUILD_MODULE: " ${QUARK_BUILD_MODULE})
message(STATUS "QUARK_BUILD_TESTS: " ${QUARK_BUILD_TESTS})
message(STATUS "QUARK_ENABLE_ASSERTIONS: " ${QUARK_ENABLE_ASSERTIONS})
message(STATUS "QUARK_ENABLE_INSTALL: " ${QUARK_ENABLE_INSTALL})
message(STATUS "QUARK_ENABLE_LOGGER: " ${QUARK_ENABLE_LOGGER})
message(STATUS "QUARK_LOG_HUGE_PAGES: " ${QUARK_LOG_HUGE_PAGES})
message(STATUS "QUARK_LOG_INITIAL_QUEUE_CAPACITY: " ${QUARK_LOG_INITIAL_QUEUE_CAPACITY})
message(STATUS "QUARK_LOG_QUEUE_TYPE: " ${QUARK_LOG_QUEUE_TYPE})
message(STATUS "QUARK_NO_EXCEPTIONS: " ${QUARK_NO_EXCEPTIONS})
message(STATUS "QUARK_TLS_BACKEND: " ${QUARK_TLS_BACKEND})
message(STATUS "QUARK_USE_FSTACK: " ${QUARK_USE_FSTACK})
message(STATUS "QUARK_USE_STD_FORMAT: " ${QUARK_USE_STD_FORMAT})

#-------------------------------------------------------------------------------------------------------
# Verbose make file option
#-------------------------------------------------------------------------------------------------------
if (QUARK_VERBOSE_MAKEFILE)
    set(CMAKE_VERBOSE_MAKEFILE TRUE CACHE BOOL "Verbose output" FORCE)
endif ()

# address sanitizer flags
if (QUARK_SANITIZE_ADDRESS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer -g")
endif ()

# thread sanitizer flags
if (QUARK_SANITIZE_THREAD)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer -g")
endif ()

# Append extra options for coverage
if (QUARK_CODE_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -fno-omit-frame-pointer -g -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
endif ()

# Clang -ftime-trace for compile-time profiling
if (QUARK_ENABLE_TIME_TRACE)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ftime-trace")
    else ()
        message(WARNING "QUARK_ENABLE_TIME_TRACE is only supported with Clang. Ignoring.")
    endif ()
endif ()

# library name
set(TARGET_NAME quark)

# header files
set(HEADER_FILES
        include/quark/Attributes.h
        include/quark/QuarkConfig.h
        include/quark/QuarkEnum.h
        include/quark/QuarkError.h
        include/quark/QuarkFormat.h
        include/quark/QuarkLog.h
        include/quark/Sanitizers.h

        include/quark/crypto/Base64Encoding.h
        include/quark/crypto/Ed25519Signer.h
        include/quark/crypto/HmacSha256.h

        include/quark/math/DoubleOps.h
        include/quark/math/MathOps.h
        include/quark/math/ScaleInt.h

        include/quark/net/AliveMonitor.h
        include/quark/net/BufferPool.h
        include/quark/net/Endpoint.h
        include/quark/net/EventLoop.h
        include/quark/net/ExpBackoff.h
        include/quark/net/FStack.h
        include/quark/net/HttpClient.h
        include/quark/net/HttpCodec.h
        include/quark/net/IOBuffer.h
        include/quark/net/Poller.h
        include/quark/net/Stream.h
        include/quark/net/SysApi.h
        include/quark/net/TcpSocket.h
        include/quark/net/TlsContext.h
        include/quark/net/TlsStream.h
        include/quark/net/Url.h
        include/quark/net/WsFrame.h
        include/quark/net/WsHandshake.h
        include/quark/net/WsSession.h

        include/quark/utils/CircularBuffer.h
        include/quark/utils/CommandQueue.h
        include/quark/utils/DateTime.h
        include/quark/utils/FullDuplexShm.h
        include/quark/utils/HwEndian.h
        include/quark/utils/InplaceFunction.h
        include/quark/utils/JsonParser.h
        include/quark/utils/LatencyHistogram.h
        include/quark/utils/MemoryArena.h
        include/quark/utils/MMapFile.h
        include/quark/utils/RdtscClock.h
        include/quark/utils/SimpleObjectPool.h
        include/quark/utils/System.h
        include/quark/utils/TimerQueue.h

        # third party header-only libs
        include/quark/third_party/inplace_function/inplace_function.h
        include/quark/third_party/SPSC_Queue/SPSCQueue.h
        include/quark/third_party/SPSC_Queue/SPSCQueueOPT.h
        include/quark/third_party/SPSC_Queue/SPSCVarQueue.h
        include/quark/third_party/SPSC_Queue/SPSCVarQueueOPT.h
        include/quark/third_party/tscns/tscns.h
)

set(SOURCE_FILES
        src/QuarkConfig.cpp
        src/QuarkLog.cpp

        src/crypto/Ed25519Signer.cpp
        src/crypto/HmacSha256.cpp
        src/crypto/OpenSslError.h
        src/TlsBackend.h

        src/net/Endpoint.cpp
        src/net/TlsContext.cpp
        src/net/TlsInternal.h
        src/net/TlsStream.cpp
        src/net/WsHandshake.cpp

        src/utils/DateTime.cpp
        src/utils/MMapFile.cpp
        src/utils/System.cpp
)

if (QUARK_USE_FSTACK)
    list(APPEND SOURCE_FILES src/net/FStack.cpp)
endif ()

# Add as a library
add_library(${TARGET_NAME} ${SOURCE_FILES} ${HEADER_FILES})
add_library(${TARGET_NAME}::${TARGET_NAME} ALIAS ${TARGET_NAME})

# A shared quark finds its installed dependencies (simdjson) next to itself
set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN")
# libquark.so.1 -> libquark.so.1.0.0: the soname changes with the major version (ABI break)
set_target_properties(${TARGET_NAME} PROPERTIES VERSION ${QUARK_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

# Link dependencies
# quill is PUBLIC: QuarkLog.h macros expand to quill code in the user's translation units
# simdjson is PUBLIC: JsonParser.h includes simdjson.h and returns simdjson types
# magic_enum is PUBLIC: QuarkEnum.h is a header-only wrapper of it, instantiated in the user's translation units
target_link_libraries(${TARGET_NAME}
        PRIVATE
            $<BUILD_INTERFACE:tomlplusplus::tomlplusplus>
            ${QUARK_SSL_LIB}
            ${QUARK_CRYPTO_LIB}
        PUBLIC
            quill::quill
            simdjson::simdjson
            magic_enum::magic_enum
            Threads::Threads
)

target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)

# Checked against the openssl/ headers actually included (src/TlsBackend.h), so a build that picks up the
# system OpenSSL headers while linking AWS-LC (or the reverse) fails to compile instead of crashing
if (QUARK_TLS_BACKEND STREQUAL "AWS-LC")
    target_compile_definitions(${TARGET_NAME} PRIVATE QUARK_TLS_AWSLC)
endif ()

# Only QUARK_API symbols are exported from a shared quark; header-only code needs no export
set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if (BUILD_SHARED_LIBS)
    # keep the symbols of statically linked dependencies (simdjson, AWS-LC) out of libquark.so
    target_link_options(${TARGET_NAME} PRIVATE "LINKER:--exclude-libs,ALL")
endif ()

target_compile_options(${TARGET_NAME} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra -Wpedantic>)

# Add include directories for this library
target_include_directories(${TARGET_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PUBLIC: QUARK_THROW in quark's inline headers (e.g. JsonParser.h) must abort rather than throw in user code.
# quill is header-only, so QUILL_NO_EXCEPTIONS works whether quill was fetched, found or given by a parent.
# F-Stack backend: PUBLIC, since quark::net's headers call the ff_* API in the user's translation units
if (QUARK_USE_FSTACK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED libdpdk)
    set(FSTACK_PREFIX /usr/local CACHE PATH "F-Stack install prefix (include/ff_api.h, lib/libfstack.a)")
    target_compile_definitions(${TARGET_NAME} PUBLIC QUARK_USE_FSTACK)
    target_include_directories(${TARGET_NAME} SYSTEM PUBLIC ${FSTACK_PREFIX}/include)
    target_compile_options(${TARGET_NAME} PRIVATE ${DPDK_CFLAGS_OTHER})
    # --whole-archive: F-Stack registers its DPDK drivers from static constructors that would
    # otherwise be dropped as unreferenced. The FreeBSD-derived subsystems inside libfstack.a
    # need zlib, pcap, numa and libcrypto after it (only RAND_bytes, which AWS-LC provides too,
    # so libfstack.a built against the system OpenSSL links against AWS-LC unchanged).
    target_link_options(${TARGET_NAME} PUBLIC
            -L${FSTACK_PREFIX}/lib
            ${DPDK_STATIC_LDFLAGS}
            "SHELL:-Wl,--whole-archive,-lfstack,--no-whole-archive"
            -pthread -lm -lrt -ldl -lnuma -lz -lpcap)
    # PUBLIC: libfstack.a lands in the user's executable, which must resolve its RAND_bytes too (a
    # shared quark hides its own copy of libcrypto); link libraries come after the options above
    target_link_libraries(${TARGET_NAME} PUBLIC ${QUARK_CRYPTO_LIB})
endif ()

if (QUARK_NO_EXCEPTIONS)
    target_compile_definitions(${TARGET_NAME} PUBLIC QUARK_NO_EXCEPTIONS QUILL_NO_EXCEPTIONS)
    target_compile_options(${TARGET_NAME} PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fno-exceptions>)
endif ()

if (QUARK_ENABLE_ASSERTIONS)
    target_compile_definitions(${TARGET_NAME} PRIVATE -DQUARK_ENABLE_ASSERTIONS)
endif ()

# PUBLIC: QuarkFormat.h selects std::format or quill's bundled fmt with it, and quark's inline headers that format
# (e.g. FullDuplexShm.h) must be compiled the same way in quark and in the user's translation units (ODR)
if (QUARK_USE_STD_FORMAT)
    target_compile_definitions(${TARGET_NAME} PUBLIC QUARK_USE_STD_FORMAT)
endif ()

# QuarkLog.cpp static_asserts that the quill it compiles against is QUARK_QUILL_VERSION
string(REPLACE "." ";" _quark_quill_version_parts ${QUARK_QUILL_VERSION})
list(GET _quark_quill_version_parts 0 _quark_quill_major)
list(GET _quark_quill_version_parts 1 _quark_quill_minor)
list(GET _quark_quill_version_parts 2 _quark_quill_patch)
target_compile_definitions(${TARGET_NAME} PRIVATE
        QUARK_QUILL_VERSION_MAJOR=${_quark_quill_major}
        QUARK_QUILL_VERSION_MINOR=${_quark_quill_minor}
        QUARK_QUILL_VERSION_PATCH=${_quark_quill_patch})

# PUBLIC: quark::FrontendOptions defined in QuarkLog.h, quark users must agree on it
target_compile_definitions(${TARGET_NAME} PUBLIC
        QUARK_LOG_QUEUE_TYPE=${QUARK_LOG_QUEUE_TYPE}
        QUARK_LOG_INITIAL_QUEUE_CAPACITY=${QUARK_LOG_INITIAL_QUEUE_CAPACITY}
        QUARK_LOG_HUGE_PAGES=$<BOOL:${QUARK_LOG_HUGE_PAGES}>)

# Compiles quark's own QUARK_LOG_* statements out, PUBLIC as quark's headers may use them
if (NOT QUARK_ENABLE_LOGGER)
    target_compile_definitions(${TARGET_NAME} PUBLIC QUARK_DISABLE_INTERNAL_LOG)
endif ()

#-------------------------------------------------------------------------------------------------------
# Sub directories (only added if they exist)
#-------------------------------------------------------------------------------------------------------
if (QUARK_BUILD_BENCHMARKS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    add_subdirectory(benchmarks)
endif ()

if (QUARK_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    add_subdirectory(examples)
endif ()

if (QUARK_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_subdirectory(tests)
endif ()

#-------------------------------------------------------------------------------------------------------
# Install
#-------------------------------------------------------------------------------------------------------
if (QUARK_MASTER_PROJECT OR QUARK_ENABLE_INSTALL)
    # ---- Install ---- #
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(version_config ${PROJECT_BINARY_DIR}/quark-config-version.cmake)
    set(project_config ${PROJECT_BINARY_DIR}/quark-config.cmake)
    set(pkgconfig ${PROJECT_BINARY_DIR}/quark.pc)
    set(targets_export_name quark-targets)

    set(QUARK_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/quark CACHE STRING
            "Installation directory for cmake files, relative to ${CMAKE_INSTALL_PREFIX}.")

    set(QUARK_LIB_DIR ${CMAKE_INSTALL_LIBDIR} CACHE STRING
            "Installation directory for libraries, relative to ${CMAKE_INSTALL_PREFIX}.")

    set(QUARK_INC_DIR ${CMAKE_INSTALL_INCLUDEDIR} CACHE STRING
            "Installation directory for include files, relative to ${CMAKE_INSTALL_PREFIX}.")

    # STRING, not PATH: a relative -DQUARK_PKGCONFIG_DIR=... must stay relative to the prefix (a PATH cache entry
    # given on the command line is made absolute against the current directory)
    set(QUARK_PKGCONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig CACHE STRING
            "Installation directory for pkgconfig (.pc) files, relative to ${CMAKE_INSTALL_PREFIX}.")

    # Use the thread lib flag that find_package(Threads) resolved for this platform
    # (e.g. -pthread on FreeBSD, -lpthread on older Linux, empty where pthread is in libc).
    set(QUARK_PTHREAD_LIBS "${CMAKE_THREAD_LIBS_INIT}")

    # PUBLIC compile definitions must reach pkg-config users too, see target_compile_definitions above
    if (QUARK_LOG_HUGE_PAGES)
        set(QUARK_LOG_HUGE_PAGES_VALUE 1)
    else ()
        set(QUARK_LOG_HUGE_PAGES_VALUE 0)
    endif ()
    set(QUARK_PKGCONFIG_CFLAGS "")
    if (NOT QUARK_ENABLE_LOGGER)
        set(QUARK_PKGCONFIG_CFLAGS " -DQUARK_DISABLE_INTERNAL_LOG")
    endif ()
    if (QUARK_NO_EXCEPTIONS)
        string(APPEND QUARK_PKGCONFIG_CFLAGS " -DQUARK_NO_EXCEPTIONS -DQUILL_NO_EXCEPTIONS")
    endif ()
    if (QUARK_USE_STD_FORMAT)
        string(APPEND QUARK_PKGCONFIG_CFLAGS " -DQUARK_USE_STD_FORMAT")
    endif ()
    set(QUARK_PKGCONFIG_LIBS_EXTRA "")
    if (QUARK_USE_FSTACK)
        string(APPEND QUARK_PKGCONFIG_CFLAGS " -DQUARK_USE_FSTACK -I${FSTACK_PREFIX}/include")
        string(JOIN " " DPDK_STATIC_LDFLAGS_STR ${DPDK_STATIC_LDFLAGS})
        set(QUARK_PKGCONFIG_LIBS_EXTRA " -L${FSTACK_PREFIX}/lib ${DPDK_STATIC_LDFLAGS_STR} -Wl,--whole-archive,-lfstack,--no-whole-archive -lm -lrt -ldl -lnuma -lz -lpcap")
    endif ()

    # The TLS backend (libssl, libcrypto) is PRIVATE, but a static quark still needs it on the
    # user's link line, and pkg-config only adds Requires.private libs with --static, so promote them to
    # Requires for static builds. AWS-LC installs libssl.pc / libcrypto.pc under the same names as OpenSSL:
    # PKG_CONFIG_PATH decides which one is found, so it must list the AWS-LC prefix first.
    set(QUARK_PKGCONFIG_REQUIRES "quill simdjson magic_enum")
    set(QUARK_PKGCONFIG_REQUIRES_PRIVATE "")
    if (BUILD_SHARED_LIBS)
        set(QUARK_PKGCONFIG_REQUIRES_PRIVATE "libssl libcrypto")
        if (QUARK_USE_FSTACK)
            string(APPEND QUARK_PKGCONFIG_REQUIRES " libcrypto")  # libfstack.a in the user's executable
        endif ()
    else ()
        string(APPEND QUARK_PKGCONFIG_REQUIRES " libssl libcrypto")
    endif ()

    # ---- pkg-config ---- #
    # The prefix is found relative to the installed quark.pc (${pcfiledir}, supported by pkg-config and pkgconf),
    # not baked in at configure time: the install tree stays valid with `cmake --install --prefix`, DESTDIR, or
    # when moved. Absolute install directories (e.g. -DQUARK_INC_DIR=/opt/inc) are used as given.
    if (IS_ABSOLUTE "${QUARK_PKGCONFIG_DIR}")
        set(QUARK_PKGCONFIG_PREFIX "${CMAKE_INSTALL_PREFIX}")
    else ()
        file(RELATIVE_PATH _quark_pc_to_prefix "${CMAKE_INSTALL_PREFIX}/${QUARK_PKGCONFIG_DIR}" "${CMAKE_INSTALL_PREFIX}")
        string(REGEX REPLACE "/$" "" _quark_pc_to_prefix "${_quark_pc_to_prefix}")
        set(QUARK_PKGCONFIG_PREFIX "\${pcfiledir}/${_quark_pc_to_prefix}")
    endif ()
    foreach (_quark_pc_dir INC LIB)
        if (IS_ABSOLUTE "${QUARK_${_quark_pc_dir}_DIR}")
            set(QUARK_PKGCONFIG_${_quark_pc_dir}_DIR "${QUARK_${_quark_pc_dir}_DIR}")
        else ()
            set(QUARK_PKGCONFIG_${_quark_pc_dir}_DIR "\${prefix}/${QUARK_${_quark_pc_dir}_DIR}")
        endif ()
    endforeach ()

    # Generate pkgconfig
    configure_file(
            "${CMAKE_CURRENT_LIST_DIR}/cmake/quark.pc.in"
            "${pkgconfig}"
            @ONLY)

    # Copy pkgconfig
    install(FILES "${pkgconfig}" DESTINATION "${QUARK_PKGCONFIG_DIR}")

    # Generate the version, config and target files into the build directory.
    write_basic_package_version_file(
            ${version_config}
            VERSION ${QUARK_VERSION}
            COMPATIBILITY SameMajorVersion)  # semver: find_package(quark 1.x) must not accept 2.0

    configure_package_config_file(
            ${CMAKE_CURRENT_LIST_DIR}/cmake/quark-config.cmake.in
            ${project_config}
            INSTALL_DESTINATION ${QUARK_CMAKE_DIR})

    # Install version, config files
    install(FILES ${project_config} ${version_config}
            DESTINATION ${QUARK_CMAKE_DIR})

    # Install the headers
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/quark DESTINATION ${QUARK_INC_DIR})

    # simdjson only installs its headers when SIMDJSON_SINGLEHEADER is on, which is a top-level-only
    # option that also enables python amalgamation, so install the pre-generated single header ourselves
    install(FILES ${simdjson_SOURCE_DIR}/singleheader/simdjson.h DESTINATION ${QUARK_INC_DIR})

    # Install the library
    install(TARGETS ${TARGET_NAME} EXPORT ${targets_export_name}
            LIBRARY DESTINATION ${QUARK_LIB_DIR}
            ARCHIVE DESTINATION ${QUARK_LIB_DIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${QUARK_INC_DIR})

    # Export the library
    install(EXPORT ${targets_export_name} DESTINATION ${QUARK_CMAKE_DIR}
            NAMESPACE quark::)

    # ---- Packaging ---- #
    set(CPACK_GENERATOR ZIP)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 0)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR}" "${PROJECT_NAME}" ALL .)
    set(CPACK_PROJECT_URL "https://github.com/Frodocz/quark")
    set(CPACK_PACKAGE_VENDOR "Charles Zhang")
    set(CPACK_PACKAGE_CONTACT "Charles Zhang <frodocz1226@gmail.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Ultra-low-latency asynchronous C++20 utility library for performance-critical applications")
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_RELOCATABLE ON)
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "System Environment/Libraries")
    set(CPACK_RPM_PACKAGE_URL ${CPACK_PROJECT_URL})
    set(CPACK_RPM_PACKAGE_DESCRIPTION "Ultra-low-latency asynchronous C++20 utility library for performance-critical applications")
    include(CPack)
endif ()

if (QUARK_DOCS_GEN)
    if (NOT QUARK_BUILD_EXAMPLES)
        message(FATAL_ERROR "QUARK_DOCS_GEN requires QUARK_BUILD_EXAMPLES to be ON. Please enable QUARK_BUILD_EXAMPLES.")
    endif ()

    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/docs)
        add_subdirectory(docs)
    endif ()
endif ()
