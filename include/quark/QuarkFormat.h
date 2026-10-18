#pragma once

#if defined (QUARK_USE_STD_FORMAT)
    #include <format>
    #include <iterator>
    #include <utility>

    namespace quark::fmtlib {
        using std::format;
        using std::format_string;
        using std::format_to;

        template <typename... Args>
        void print(std::ostream& os, std::format_string<Args...> fmt, Args&&... args) {
            std::format_to(std::ostreambuf_iterator<char>(os), fmt, std::forward<Args>(args)...);
        }
    } // namespace quark::fmtlib
#else
    // Use Quill's internal bundled fmt headers
    #include <quill/bundled/fmt/format.h>
    #include <quill/bundled/fmt/ostream.h>

    namespace quark::fmtlib {
        using fmtquill::format;
        using fmtquill::format_int;
        using fmtquill::format_string;
        using fmtquill::format_to;
        using fmtquill::print; // fmt::print(std::ostream&, ...)
    } // namespace quark::fmtlib
#endif
