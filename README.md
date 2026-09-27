<div align="center">
  <!--
  <meta name="description" content="Ultra-low-latency C++20 utility library for performance-critical applications.">
  <meta name="keywords" content="C++, utility library, high performance, low latency">
  -->

  <h1>
    Quark
  </h1>
  <p><b>Ultra-low-latency asynchronous C++20 utility library for performance-critical applications</b></p>

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
|---------------------|----------------------------------------------------------------------------------------------------------------|
| **Logging**         | Wrapping [`Quill`](https://github.com/odygrd/quill/blob/master/LICENSE), one of the fastest C++ logging module |
| **Configuration**   | `TOML` used for personal preference, wrapping [toml++](https://github.com/marzer/tomlplusplus)                 |
| **Fixed-Point**     | Fixed-point numbers are needed in financial systems to prevent tiny rounding errors from binary math           |
| **Lock-Free Queues**| Lock-Free queues to for high-performance, low-latency and predictable concurrent systems                       |

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
---

## License

Quark is licensed under the [MIT License](https://opensource.org/licenses/MIT).

Quark depends on third party libraries with separate copyright notices and license terms.
Your use of the source code for these subcomponents is subject to the terms and conditions of the following licenses.

- ([MIT License](https://opensource.org/licenses/MIT)) [quill](https://github.com/odygrd/quill/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [toml++](https://github.com/marzer/tomlplusplus)
