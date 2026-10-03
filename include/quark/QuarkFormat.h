#pragma once

#include "quark/Attributes.h"

#if defined (QUARK_USE_STD_FORMAT)
    #include <format>
#else
    // Use Quill's internal bundled fmt headers
    #include <quill/bundled/fmt/format.h>

#endif

QUARK_BEGIN_NAMESPACE

#if defined (QUARK_USE_STD_FORMAT)
    namespace fmtlib = std;
#else
    namespace fmtlib = fmtquill;
#endif

QUARK_END_NAMESPACE
