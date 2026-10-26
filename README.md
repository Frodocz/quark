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
| **Fixed-Point**     | Fixed-point numbers are needed in financial systems to prevent tiny rounding errors from binary math                |
| **Lock-Free Queues**| Lock-Free queues to for high-performance, low-latency and predictable concurrent systems                            |
| **Logging**         | Wrapping [`Quill`](https://github.com/odygrd/quill), one of the fastest C++ logging module                          |
| **Network**         | Support `HTTP` and `Websocket` clients (with `TLS`), switching between kernel sockets and `DPDK` (`F-Stack` backend)|
| **Object Pool**     | A Single-threaded pool of fixed-type objects                                                                        |
| **TimerQueue**      | A Single-threaded timer queue meant to be polled from a busy-poll event loop                                        |
| **Tsc closk**       | Wrapping [`tscns`](https://github.com/odygrd/tscns), a low overhead nanosecond clock based on x86 TSC               |

---

## Usages

### External CMake

#### Building and Installing Quark

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

Next, add Quark to your project using `find_package()`:

```cmake
find_package(quark REQUIRED)
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
find_package(quark REQUIRED)
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
cmake_minimum_required(VERSION 3.8)
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
chosen at compile time with no runtime cost. It needs F-Stack and DPDK installed
(`libfstack.a` under `-DFSTACK_PREFIX=...`, default `/usr/local`; `libdpdk` found by pkg-config).

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
netmask and gateway in the `[port0]` section of `f-stack.conf`; ENA doesn't support promiscuous
mode, so its warning at startup is expected. The F-Stack tests run by hand:
`sudo ./tests/quark_test_fstack --conf <f-stack.conf>`.

For host settings (CPU isolation, IRQs, huge pages, busy polling, real-time priority), see
[docs/TUNING.md](docs/TUNING.md).

---

## License

Quark is licensed under the [MIT License](https://opensource.org/licenses/MIT).

Quark depends on third party libraries with separate copyright notices and license terms.
Your use of the source code for these subcomponents is subject to the terms and conditions of the following licenses.

- ([MIT License](https://opensource.org/licenses/MIT)) [quill](https://github.com/odygrd/quill/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [SPSC_Queue](https://github.com/MengRao/SPSC_Queue/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [toml++](https://github.com/marzer/tomlplusplus/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [tscns](https://github.com/MengRao/tscns/blob/master/LICENSE)
