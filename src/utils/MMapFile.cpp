#include "quark/utils/MMapFile.h"

#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"
#include "quark/QuarkLog.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

QUARK_BEGIN_NAMESPACE

std::string MMapFile::Options::to_string() const {
    return fmtlib::format("filename:{}, writable:{}, lock:{}, create:{}, mode:{}, size:{}",
        filename, writable, lock, create, mode, size);
}

MMapFile& MMapFile::operator=(MMapFile&& other) {
    reset();
    addr_ = other.addr_;
    size_ = other.size_;
    fd_ = other.fd_;

    other.addr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    return *this;
}

void MMapFile::initialize(const Options& options) {
    bool truncated = false;
    initialize(options, truncated);
}

void MMapFile::initialize(const Options& options, bool& truncated) {
    if (!initialize_impl(options, truncated)) {
        QUARK_THROW(QuarkError{fmtlib::format("mmap failed, {}", options.to_string())});
    }
}

bool MMapFile::initialize_impl(const Options& options, bool& truncated) {
    QUARK_ASSERT(addr_ == nullptr, "MMapFile already initialized");
    int prot = PROT_READ;
    if (options.writable) {
        prot |= PROT_WRITE;
    }

    if (options.filename.empty()) {
        int map_flags = MAP_ANONYMOUS | MAP_SHARED;
        size_ = options.size;
        addr_ = reinterpret_cast<uint8_t*>(mmap(nullptr, size_, prot, map_flags, 0, 0));
        if (addr_ == MAP_FAILED) {
            return false;
        }
        truncated = true;
    } else {
        int map_flags = MAP_SHARED;
        int file_flags = 0;
        if (options.writable) {
            file_flags = O_RDWR;
            if (options.create) {
                file_flags |= O_CREAT;
            }
        } else {
            file_flags = O_RDONLY;
        }
        int fd = open(options.filename.c_str(), file_flags, options.mode);
        if (fd == -1) {
            return false;
        }
        struct stat st;
        if (fstat(fd, &st)) {
            return false;
        }
        if (options.writable && (st.st_mode & 0777) != options.mode) {
            if (fchmod(fd, options.mode) != 0) {
                return false;
            }
        }
        if (options.writable && options.size != 0 && static_cast<int64_t>(options.size) != st.st_size) {
            size_ = options.size;
            if (ftruncate(fd, size_)) {
                return false;
            }
            truncated = true;
        } else {
            size_ = st.st_size;
            truncated = false;
        }
        addr_ = reinterpret_cast<uint8_t *>(mmap(nullptr, size_, prot, map_flags, fd, 0));
        if (addr_ == MAP_FAILED) {
            return false;
        }
        fd_ = fd;
        if (options.lock && mlock(addr_, size_)) {
            return false;
        }
    }
    return true;
}

void MMapFile::reset() {
    if (addr_) {
        munmap(addr_, size_);
        addr_ = nullptr;
    }
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

QUARK_END_NAMESPACE
