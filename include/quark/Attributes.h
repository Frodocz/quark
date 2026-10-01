#pragma once

#ifndef QUARK_BEGIN_NAMESPACE
  #define QUARK_BEGIN_NAMESPACE                                                                    \
    namespace quark                                                                                \
    {                                                                                              \
    inline namespace v0                                                                            \
    {
  #define QUARK_END_NAMESPACE                                                                      \
    }                                                                                              \
    }
#endif

#define QUARK_CPLUSPLUS __cplusplus

/**
 * Visibility
 */
#define QUARK_API __attribute__((visibility("default")))

constexpr inline int QUARK_VERSION_MAJOR{0};
constexpr inline int QUARK_VERSION_MINOR{1};
constexpr inline int QUARK_VERSION_PATCH{0};
inline constexpr int Version{QUARK_VERSION_MAJOR * 10000 + QUARK_VERSION_MINOR * 100 + QUARK_VERSION_PATCH};

/**
 * __has_include
 */
#ifndef QUARK_HAS_INCLUDE
    #ifdef __has_include
        #define QUARK_HAS_INCLUDE(x) __has_include(x)
    #else
        #define QUARK_HAS_INCLUDE(x) 0
    #endif
#endif

/**
 * __has_feature
 */
#ifndef QUARK_HAS_FEATURE
    #ifdef __has_feature
        #define QUARK_HAS_FEATURE(x) __has_feature(x)
    #else
        #define QUARK_HAS_FEATURE(x) 0
    #endif
#endif

/**
 * __has_attribute
 */
#ifndef QUARK_HAS_ATTRIBUTE
    #ifdef __has_attribute
        #define QUARK_HAS_ATTRIBUTE(x) __has_attribute(x)
    #else
        #define QUARK_HAS_ATTRIBUTE(x) 0
    #endif
#endif

/**
 * __has_cpp_attribute
 */
#ifndef QUARK_HAS_CPP_ATTRIBUTE
    #if defined(__cplusplus) && defined(__has_cpp_attribute)
        #define QUARK_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
    #else
        #define QUARK_HAS_CPP_ATTRIBUTE(x) 0
    #endif
#endif

/**
 * Prevents the compiler from inlining a function
 */
#ifndef QUARK_NOINLINE
    #if QUARK_HAS_ATTRIBUTE(noinline)
        #define QUARK_NOINLINE __attribute__((noinline))
    #else
        #define QUARK_NOINLINE
    #endif
#endif

/**
 * Always inline a function
 */
#ifndef QUARK_ALWAYS_INLINE
    #if QUARK_HAS_ATTRIBUTE(always_inline)
        #define QUARK_ALWAYS_INLINE inline __attribute__((always_inline))
    #else
        #define QUARK_ALWAYS_INLINE inline
    #endif
#endif

/**
 * Gcc hot/cold attributes
 * Tells GCC that a function is hot or cold. GCC can use this information to
 * improve static analysis, i.e. a conditional branch to a cold function
 * is likely to be not-taken.
 */
#ifndef QUARK_ATTRIBUTE_HOT
    #if QUARK_HAS_ATTRIBUTE(hot)
        #define QUARK_ATTRIBUTE_HOT __attribute__((hot))
    #else
        #define QUARK_ATTRIBUTE_HOT
    #endif
#endif

#ifndef QUARK_ATTRIBUTE_COLD
    #if QUARK_HAS_ATTRIBUTE(cold)
        #define QUARK_ATTRIBUTE_COLD __attribute__((cold))
    #else
        #define QUARK_ATTRIBUTE_COLD
    #endif
#endif

#ifndef QUARK_ATTRIBUTE_PACKED
    #if QUARK_HAS_ATTRIBUTE(packed)
        #define QUARK_ATTRIBUTE_PACKED __attribute__((packed))
    #else
        #define QUARK_ATTRIBUTE_PACKED
    #endif
#endif

/**
 * Used
 */
#ifndef QUARK_ATTRIBUTE_USED
    #if QUARK_HAS_ATTRIBUTE(used)
        #define QUARK_ATTRIBUTE_USED __attribute__((used))
    #else
        #define QUARK_ATTRIBUTE_USED
    #endif
#endif

/**
 * Module export helpers
 */
#ifndef QUARK_BEGIN_EXPORT
    #if defined(QUARK_MODULE)
        #define QUARK_BEGIN_EXPORT export {
        #define QUARK_END_EXPORT }
    #else
        #define QUARK_BEGIN_EXPORT
        #define QUARK_END_EXPORT
    #endif
#endif
