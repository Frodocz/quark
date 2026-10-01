include(CheckCXXSourceCompiles)

# Get Quark version from include/quark/Attributes.h and store it as QUARK_VERSION
function(quark_extract_version)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/include/quark/Attributes.h" file_contents)

    string(REGEX MATCH "constexpr inline int QUARK_VERSION_MAJOR{([0-9]+)}" _ "${file_contents}")
    if (NOT CMAKE_MATCH_COUNT EQUAL 1)
        message(FATAL_ERROR "Failed to extract major version number from quark/Attributes.h")
    endif ()
    set(version_major ${CMAKE_MATCH_1})

    string(REGEX MATCH "constexpr inline int QUARK_VERSION_MINOR{([0-9]+)}" _ "${file_contents}")
    if (NOT CMAKE_MATCH_COUNT EQUAL 1)
        message(FATAL_ERROR "Failed to extract minor version number from quark/Attributes.h")
    endif ()
    set(version_minor ${CMAKE_MATCH_1})

    string(REGEX MATCH "constexpr inline int QUARK_VERSION_PATCH{([0-9]+)}" _ "${file_contents}")
    if (NOT CMAKE_MATCH_COUNT EQUAL 1)
        message(FATAL_ERROR "Failed to extract patch version number from quark/Attributes.h")
    endif ()
    set(version_patch ${CMAKE_MATCH_1})

    set(QUARK_VERSION "${version_major}.${version_minor}.${version_patch}" PARENT_SCOPE)
endfunction()

# Define the function to set common compile options
function(set_common_compile_options target_name)
    cmake_parse_arguments(COMPILE_OPTIONS "" "VISIBILITY" "" ${ARGN})

    # Set default visibility to PRIVATE if not provided
    if (NOT DEFINED COMPILE_OPTIONS_VISIBILITY)
        set(COMPILE_OPTIONS_VISIBILITY PRIVATE)
    endif ()

    target_compile_options(${target_name} ${COMPILE_OPTIONS_VISIBILITY}
            # General warnings for Clang and GNU
            $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wextra -pedantic -Werror -Wredundant-decls -Wfloat-equal
            >

            # GCC-specific hardening and security flags
            $<$<AND:$<CXX_COMPILER_ID:GNU>,$<BOOL:${QUARK_ENABLE_GCC_HARDENING}>>:
            -fstack-protector-strong
            -fstack-clash-protection
            -Wformat
            -Werror=format-security
            -fcf-protection
            -Wdate-time
            -D_FORTIFY_SOURCE=2
            >

            # GCC >= 7.1 specific: suppress PSABI warning
            $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,7.1>>:
            -Wno-psabi
            >

            # Clang specific options
            $<$<CXX_COMPILER_ID:Clang>:
            -Wimplicit-int-float-conversion;-Wdocumentation;-Wno-gnu-zero-variadic-macro-arguments
            >

            # Disable C++20 extension warnings for Clang > 17
            $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER:$<CXX_COMPILER_VERSION>,17>>:
            -Wno-c++20-extensions
            >
    )

    if (QUARK_NO_EXCEPTIONS)
        # Add flags -fno-exceptions -fno-rtti to make sure we support them.
        target_compile_options(${target_name} ${COMPILE_OPTIONS_VISIBILITY}
                $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:
                -fno-exceptions -fno-rtti>
        )
    endif ()
endfunction()

function(check_cxx_atomics_available variable)
  set(SAVED_CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
  set(CMAKE_REQUIRED_LIBRARIES "")

  check_cxx_source_compiles("
    #include <atomic>
    #include <cstdint>
    std::atomic<uint64_t> counter;
    int main() {
      uint64_t res = std::atomic_fetch_add(&counter, 1);
      return (int)res;
    }" ${variable})

  set(CMAKE_REQUIRED_LIBRARIES "${SAVED_CMAKE_REQUIRED_LIBRARIES}")
endfunction()