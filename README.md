<div align="center">
  <!--
  <meta name="description" content="Ultra-low-latency C++20 utility library for performance-critical applications.">
  <meta name="keywords" content="C++, utility library, high performance, low latency">
  -->

  <h1>
    Quark
  </h1>
  <p><b>Ultra-low-latency C++20 utility library for performance-critical applications</b></p>

  <div>
    <a href="https://opensource.org/licenses/MIT">
      <img src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square" alt="license" />
    </a>
    <a href="https://en.wikipedia.org/wiki/C%2B%2B20">
      <img src="https://img.shields.io/badge/language-C%2B%2B20-red.svg?style=flat-square" alt="language" />
    </a>
  </div>
</div>

---

## Table of Contents

- [Introduction](#-introduction)
- [Features](#-features)
- [Usages](#-usages)
- [License](#-license)

---

## Introduction

**Quark** is an ultra-low-latency utility library for **C++20 and later**. Just like quarks, the most fundamental
particles that build matter, `Quark` aims to provide basic building blocks like logging, configuration, containers
and lock-free queues for quickly start some latency-sensitive applications such as high frequency trading systems.

> Using Quark? Click **Star** at the top of the [GitHub repository](https://github.com/Frodocz/quark) to help other C++
> developers discover it.

---

## Features
| Modules             | Descriptions                                                                                                   |
|---------------------|---------------------------------------------------------------------------------------------------------------------|
| **Memory Arena**    | Backbone of internal modules like `net`, providing `IOBuffer`, `CircularBuffer` and `BufferPool`                    |
| **Configuration**   | `TOML` used for personal preference, wrapping [toml++](https://github.com/marzer/tomlplusplus)                      |
| **Enum Reflection** | `QuarkEnum.h`: enum <-> string and validated value casts, wrapping [magic_enum](https://github.com/Neargye/magic_enum) |
| **Crypto**          | `HMAC-SHA256` and `Ed25519` request signing, base64/hex, on [AWS-LC](https://github.com/aws/aws-lc) (default) or OpenSSL 3 |
| **Fixed-Point**     | Fixed-point numbers are needed in financial systems to prevent tiny rounding errors from binary math                |
| **Lock-Free Queues**| Lock-free [SPSC queues](https://github.com/MengRao/SPSC_Queue) for high-performance, low-latency and predictable concurrent systems |
| **Logging**         | Wrapping [`Quill`](https://github.com/odygrd/quill), one of the fastest C++ logging module                          |
| **Network**         | Support `HTTP` and `Websocket` clients (with `TLS` over AWS-LC or OpenSSL), switching between kernel sockets and `DPDK` (`F-Stack` backend)|
| **Object Pool**     | A Single-threaded pool of fixed-type objects                                                                        |
| **TimerQueue**      | A Single-threaded timer queue meant to be polled from a busy-poll event loop                                        |
| **TSC clock**       | Wrapping [`tscns`](https://github.com/MengRao/tscns), a low overhead nanosecond clock based on x86 TSC              |

---

## Usages

### External CMake

#### Building and Installing Quark

Requirements: Linux (x86-64), a C++20 compiler (tested with GCC 11 and 12) and CMake >= 3.16. Releases and
their changes are listed in [CHANGELOG.md](CHANGELOG.md); the current release is **1.0.0**.

To get started with Quark, clone the repository and install it using CMake:

```bash
git clone https://github.com/Frodocz/quark.git
cd quark
mkdir cmake_build
cd cmake_build
cmake ..
make install
```

- **Custom Installation**: Specify a custom directory with `-DCMAKE_INSTALL_PREFIX=/path/to/install/dir`.
- **Build Examples**: Include examples with `-DQUARK_BUILD_EXAMPLES=ON`.
- **TLS Backend**: `-DQUARK_TLS_BACKEND=AWS-LC` (default) or `-DQUARK_TLS_BACKEND=OpenSSL` (system OpenSSL >= 3.0)
  for `quark::crypto` and the TLS of `quark::net`. AWS-LC is the default for latency: against OpenSSL 3.5 it
  saves ~0.2 µs and two heap allocations per TLS record sent and ~2.5x per handshake (measurements in
  [PLAN.md](PLAN.md), "TLS backend" under Phase 7); with OpenSSL, prefer 3.5 over a distribution's 3.0.
  AWS-LC is taken from an installed copy (`-DAWSLC_ROOT=/path` or `CMAKE_PREFIX_PATH`) or else fetched (v5.12.0)
  and linked statically; neither Go nor Perl is needed. A fetched AWS-LC is installed along with quark, including
  `include/openssl` and `libssl`/`libcrypto`, so install into a dedicated prefix rather than `/usr/local`, where
  they would shadow the system OpenSSL. One process holds one of the two: link the application's own TLS code
  against quark's backend.

Next, add Quark to your project using `find_package()`:

```cmake
find_package(quark 1.0 REQUIRED)
target_link_libraries(your_target PUBLIC quark::quark)
```

#### Sample Directory Structure

Organize your project directory like this:

```
my_project/
├── CMakeLists.txt
├── main.cpp
```

#### Sample CMakeLists.txt

Here is a minimal `CMakeLists.txt`:

```cmake
# If Quark is in a non-standard directory, specify its path.
set(CMAKE_PREFIX_PATH /path/to/quark)

# Find and link the Quark library.
find_package(quark 1.0 REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PUBLIC quark::quark)
```

### Embedded CMake

If you prefer to vendor Quark directly, add it as a subdirectory:

#### Sample Directory Structure

```
my_project/
├── quark/            # Quark repo folder
├── CMakeLists.txt
├── main.cpp
```

#### Sample CMakeLists.txt

Use this `CMakeLists.txt` to include Quark directly:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(quark)
add_executable(my_project main.cpp)
target_link_libraries(my_project PUBLIC quark::quark)
```

### F-Stack backend (optional)

By default `quark::net` uses kernel sockets. Building with `-DQUARK_USE_FSTACK=ON` runs the same API
over [F-Stack](https://github.com/F-Stack/f-stack) (a userspace TCP/IP stack on DPDK) instead,
chosen at compile time with no runtime cost. It needs F-Stack (2.0) and DPDK installed
(`libfstack.a` under `-DFSTACK_PREFIX=...`, default `/usr/local`; `libdpdk` found by pkg-config);
[doc/FSTACK.md](doc/FSTACK.md) covers building both, host setup and configuration. Either TLS backend
works under F-Stack: `libfstack.a` needs only `RAND_bytes` from libcrypto, which AWS-LC provides.

```cpp
#include "quark/net/EventLoop.h"
#include "quark/net/FStack.h"

int main(int argc, char** argv) {
    // argv must contain "--conf <f-stack.conf>"; run as root (DPDK owns the NIC)
    if (!quark::net::init_fstack(argc, argv)) return 1;
    quark::net::EventLoop loop;
    // create sessions on `loop`, put per-iteration work in loop.set_step_hook(...)
    loop.run();  // F-Stack's loop: once per process, on this thread
}
```

F-Stack only processes packets inside its own loop, so under F-Stack `EventLoop::run()` is the
entry point: calling `step()` from your own loop isn't enough. On AWS, give DPDK a dedicated ENI
(the instance keeps its primary ENI for SSH, DNS and the kernel) and set that ENI's address,
netmask and gateway in the `[port0]` section of `f-stack.conf`
([doc/f-stack.conf.example](doc/f-stack.conf.example)). The F-Stack tests need root:
`sudo ctest -L fstack` (a plain `ctest` reports them skipped), or
`sudo ./tests/quark_test_fstack --conf <f-stack.conf>`. For a live check,
[examples/binance_ws.cpp](examples/binance_ws.cpp) streams Binance market data on either backend:
`sudo ./examples/quark_example_binance_ws --conf <f-stack.conf>`.

For host settings (CPU isolation, IRQs, huge pages, busy polling, real-time priority), see
[doc/TUNING.md](doc/TUNING.md).

### Errors and crashes

quark separates the errors that should stop a process from those that must not:

- **Startup and configuration errors** (a bad config value, an unusable key, a format string quark
  can't parse) throw `quark::QuarkError` from constructors and setup calls, so they surface before
  trading starts.
- **Errors caused by input while running** have non-throwing APIs that never take the process down:
  `TimestampFormat::parse()` and `try_parse_timestamp()` return `std::nullopt`,
  `Ed25519Signer::try_sign()` returns `std::nullopt`, network failures are reported through the
  sessions' callbacks and reconnects.
- With `-DQUARK_NO_EXCEPTIONS=ON`, a `QuarkError` becomes `quark::fatal_error()`: the message goes
  to stderr, the handler set with `quark::set_fatal_handler()` runs (alerting, for instance), the
  message is logged as CRITICAL and the logs are flushed (bounded by `fatal_flush_timeout_ms`), then
  the process aborts.
- Once `init_log()` has run, an uncaught exception or a crash signal (`SIGSEGV`, `SIGABRT`, ...)
  is logged and flushed the same way before the process dies (`crash_handler`, on by default).
  `SIGINT` and `SIGTERM` stay with the application.

```cpp
const quark::TimestampFormat fmt{"%Y-%m-%dT%H:%M:%E*S%z"};  // startup: throws on a bad format
// in the message handler:
if (auto ns = fmt.parse(msg.time)) { /* use *ns */ } else { ++malformed; /* drop, don't crash */ }
```

---

## License

Quark is licensed under the [MIT License](https://opensource.org/licenses/MIT).

Quark depends on third party libraries with separate copyright notices and license terms.
Your use of the source code for these subcomponents is subject to the terms and conditions of the following licenses.

- ([MIT License](https://opensource.org/licenses/MIT)) [magic_enum](https://github.com/Neargye/magic_enum/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [quill](https://github.com/odygrd/quill/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [SPSC_Queue](https://github.com/MengRao/SPSC_Queue/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [toml++](https://github.com/marzer/tomlplusplus/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [tscns](https://github.com/MengRao/tscns/blob/master/LICENSE)
