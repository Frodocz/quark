# Fails if libquark defines or references a quark:: symbol outside the versioned inline namespace
# quark::v1 (QUARK_BEGIN_NAMESPACE). An unversioned symbol defeats the major-version guard (0.x and 1.x
# objects must not link together), and compilers disagree on whether `namespace quark::net::detail`
# extends quark::v1::net::detail: GCC reopens the inline namespace, clang 14 creates a new one, which
# turned into undefined references to quark::net::detail::* in clang builds.
#
# Usage: cmake -DNM=<nm> -DLIB=<libquark.a or .so> -P CheckVersionedSymbols.cmake
if (NOT NM OR NOT LIB)
    message(FATAL_ERROR "usage: cmake -DNM=<nm> -DLIB=<library> -P CheckVersionedSymbols.cmake")
endif ()

execute_process(COMMAND ${NM} -C ${LIB}
        OUTPUT_VARIABLE symbols
        ERROR_VARIABLE nm_errors
        RESULT_VARIABLE nm_result)
if (NOT nm_result EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${LIB}: ${nm_errors}")
endif ()

# Every "quark::" that doesn't start a "quark::v1::" (demangled names, defined and undefined symbols).
# quark::fmtlib is a namespace alias target outside v1 by design (QuarkFormat.h) and has no symbols of its own.
string(REGEX MATCHALL "(^|[^A-Za-z0-9_:])quark::[A-Za-z0-9_]+" candidates "${symbols}")
set(unversioned "")
foreach (c IN LISTS candidates)
    string(REGEX MATCH "quark::[A-Za-z0-9_]+" c "${c}")
    if (NOT c STREQUAL "quark::v1" AND NOT c STREQUAL "quark::fmtlib")
        list(APPEND unversioned "${c}")
    endif ()
endforeach ()
list(REMOVE_DUPLICATES unversioned)

if (unversioned)
    list(TRANSFORM unversioned REPLACE "^quark::" "" OUTPUT_VARIABLE names)
    list(JOIN names "|" names)
    string(REGEX MATCHALL "[^\n]*quark::(${names})::[^\n]*" lines "${symbols}")
    list(REMOVE_DUPLICATES lines)
    list(JOIN lines "\n  " lines)
    message(FATAL_ERROR "symbols outside quark::v1 in ${LIB} (wrap the declaration in QUARK_BEGIN_NAMESPACE):\n  ${lines}")
endif ()
message(STATUS "all quark:: symbols in ${LIB} are in quark::v1")
