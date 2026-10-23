#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"

// Calling an empty inplace_function raises std::bad_function_call: route it through QUARK_THROW
// so builds with QUARK_NO_EXCEPTIONS abort instead of failing to compile.
#ifndef SG14_INPLACE_FUNCTION_THROW
    #define SG14_INPLACE_FUNCTION_THROW(x) QUARK_THROW(x)
#endif

#include "quark/third_party/inplace_function/inplace_function.h"

#include <cstddef>

QUARK_BEGIN_NAMESPACE

/// Type-erased callable stored inline in `Capacity` bytes: never allocates. Constructing it
/// from a callable larger than `Capacity` is a compile-time error.
template <class Signature, std::size_t Capacity = 64,
          std::size_t Alignment = alignof(std::max_align_t)>
using InplaceFunction = stdext::inplace_function<Signature, Capacity, Alignment>;

QUARK_END_NAMESPACE
