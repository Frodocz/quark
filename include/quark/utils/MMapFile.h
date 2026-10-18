#pragma once

#include "quark/Attributes.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <sys/types.h>

QUARK_BEGIN_NAMESPACE

class QUARK_API MMapFile {
public:
    struct Options {
        std::string filename; // empty file name means anonymous mapping
        bool writable = true; // whether the mmap file is writable or read-only
        bool lock = true;     // use mlock to lock the address into the memory
        bool create = true;   // create if the file doesn't exist
        mode_t mode = 0644;   // access mode: owner rw-, group r--, others r--
        std::size_t size = 0; // use a non-zero value to truncate the file
    
        std::string to_string() const;
    };

    MMapFile() = default;
    ~MMapFile() { reset(); }

    MMapFile(const MMapFile& other) = delete;
    MMapFile& operator=(const MMapFile& other) = delete;

    MMapFile(MMapFile&& other) { this->operator=(std::move(other)); }
    MMapFile& operator=(MMapFile&& other); // close may throw

    void initialize(const Options& options);
    void initialize(const Options& options, bool& truncated);

    uint8_t* addr() const noexcept { return addr_; }
    std::size_t size() const noexcept { return size_; }
    int fd() const noexcept { return fd_; }

    void reset();
private:
    bool initialize_impl(const Options& options, bool& truncated);

    uint8_t* addr_{nullptr};
    std::size_t size_{0};
    int fd_{-1};
};

QUARK_END_NAMESPACE
