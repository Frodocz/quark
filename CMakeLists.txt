cmake_minimum_required(VERSION 3.12...4.2)

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
set(QUARK_LOG_INITIAL_QUEUE_CAPACITY 67108864 CACHE STRING "Capacity in bytes of each logging thread's queue (quill only accepts it at compile time).")
option(QUARK_ENABLE_TIME_TRACE "Enable -ftime-trace to generate compile-time profiling data. Requires Clang." OFF)
option(QUARK_NO_EXCEPTIONS "Build without exception handling support." OFF)
option(QUARK_SANITIZE_ADDRESS "Enable AddressSanitizer (ASan) for memory error detection in tests." OFF)
option(QUARK_SANITIZE_THREAD "Enable ThreadSanitizer (TSan) for detecting thread-related issues in tests. Note: Using this option with non-Clang compilers may produce false positives." OFF)
option(QUARK_USE_VALGRIND "Use Valgrind as the default memory checking tool in CTest. Valgrind must be installed." OFF)

# Internal option
set(QUARK_ENABLE_GCC_HARDENING OFF CACHE INTERNAL "")

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
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG        v13.0.0
)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)

# whether to use std::format or use external fmtlib
set(QUARK_USE_STD_FORMAT OFF)

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

FetchContent_MakeAvailable(quill tomlplusplus)

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
message(STATUS "QUARK_LOG_INITIAL_QUEUE_CAPACITY: " ${QUARK_LOG_INITIAL_QUEUE_CAPACITY})
message(STATUS "QUARK_NO_EXCEPTIONS: " ${QUARK_NO_EXCEPTIONS})
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
if (QUARKCODE_COVERAGE)
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
        include/quark/QuarkError.h
        include/quark/QuarkFormat.h
        include/quark/QuarkLog.h
)

set(SOURCE_FILES
        src/QuarkConfig.cpp
        src/QuarkLog.cpp
)

# Add as a library
add_library(${TARGET_NAME} ${SOURCE_FILES} ${HEADER_FILES})
add_library(${TARGET_NAME}::${TARGET_NAME} ALIAS ${TARGET_NAME})

# Link dependencies
# quill is PUBLIC: QuarkLog.h macros expand to quill code in the user's translation units
target_link_libraries(${TARGET_NAME}
        PRIVATE
            $<BUILD_INTERFACE:tomlplusplus::tomlplusplus>
        PUBLIC
            quill::quill
            Threads::Threads
)

target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)

target_compile_options(${TARGET_NAME} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra -Wpedantic>)

# Add include directories for this library
target_include_directories(${TARGET_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

if (QUARK_NO_EXCEPTIONS)
    target_compile_definitions(${TARGET_NAME} PRIVATE QUARK_NO_EXCEPTIONS)
    target_compile_options(${TARGET_NAME} PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fno-exceptions>)
endif ()

if (QUARK_ENABLE_ASSERTIONS)
    target_compile_definitions(${TARGET_NAME} PRIVATE -DQUARK_ENABLE_ASSERTIONS)
endif ()

if (QUARK_USE_STD_FORMAT)
    target_compile_definitions(${TARGET_NAME} PRIVATE -DQUARK_USE_STD_FORMAT)
endif ()

# PUBLIC: it is part of the logger types in QuarkLog.h, quark and its users must agree on it
target_compile_definitions(${TARGET_NAME} PUBLIC QUARK_LOG_INITIAL_QUEUE_CAPACITY=${QUARK_LOG_INITIAL_QUEUE_CAPACITY})

# Compiles quark's internal QUARK_LOG_* statements out entirely
if (NOT QUARK_ENABLE_LOGGER)
    target_compile_definitions(${TARGET_NAME} PRIVATE -DQUARK_DISABLE_INTERNAL_LOG)
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

    set(QUARK_PKGCONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig CACHE PATH
            "Installation directory for pkgconfig (.pc) files, relative to ${CMAKE_INSTALL_PREFIX}.")

    # Use the thread lib flag that find_package(Threads) resolved for this platform
    # (e.g. -pthread on FreeBSD, -lpthread on older Linux, empty where pthread is in libc).
    set(QUARK_PTHREAD_LIBS "${CMAKE_THREAD_LIBS_INIT}")

    # ---- pkg-config ---- #
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
            COMPATIBILITY AnyNewerVersion)

    configure_package_config_file(
            ${CMAKE_CURRENT_LIST_DIR}/cmake/quark-config.cmake.in
            ${project_config}
            INSTALL_DESTINATION ${QUARK_CMAKE_DIR})

    # Install version, config files
    install(FILES ${project_config} ${version_config}
            DESTINATION ${QUARK_CMAKE_DIR})

    # Install the headers
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/quark DESTINATION ${QUARK_INC_DIR})

    # Get the real target name and insert into export set
    set(DEPENDENCY_TARGETS quill)
    # Install the library
    install(TARGETS ${TARGET_NAME} ${DEPENDENCY_TARGETS} EXPORT ${targets_export_name}
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
