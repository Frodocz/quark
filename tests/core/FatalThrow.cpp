// Throws like an application's own code does. Compiled with exceptions even when quark (and the
// rest of the tests) are built with QUARK_NO_EXCEPTIONS (-fno-exceptions): applications such as
// trading still throw, and quark's terminate handler must still report what escaped.
#include <stdexcept>

namespace quark_test {

[[noreturn]] void throw_runtime_error(const char* what) { throw std::runtime_error(what); }

}  // namespace quark_test
