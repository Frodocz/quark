#pragma once

#include "quark/Attributes.h"

/**
 * ASan manual poisoning, used by the memory pools so a use-after-release of a pooled slot or
 * buffer is reported. No-ops in builds without AddressSanitizer.
 */
#if defined(__SANITIZE_ADDRESS__) || QUARK_HAS_FEATURE(address_sanitizer)
    #include <sanitizer/asan_interface.h>
    #define QUARK_ASAN_POISON(addr, size) ASAN_POISON_MEMORY_REGION(addr, size)
    #define QUARK_ASAN_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION(addr, size)
#else
    #define QUARK_ASAN_POISON(addr, size) ((void)(addr), (void)(size))
    #define QUARK_ASAN_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif
