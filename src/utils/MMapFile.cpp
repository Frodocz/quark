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
    std::string why;
    if (!try_initialize(options, truncated, &why)) {
        QUARK_THROW(QuarkError{fmtlib::format("mmap failed: {}, {}", why, options.to_string())});
    }
}

bool MMapFile::try_initialize(const Options& options, bool& truncated, std::string* error) {
    std::string why;
    if (!initialize_impl(options, truncated, why)) {
        reset();  // closes the file and unmaps whatever the failed attempt left behind
        if (error != nullptr) *error = std::move(why);
        return false;
    }
    return true;
}

bool MMapFile::initialize_impl(const Options& options, bool& truncated, std::string& why) {
    QUARK_ASSERT(addr_ == nullptr, "MMapFile already initialized");
    int prot = PROT_READ;
    if (options.writable) {
        prot |= PROT_WRITE;
    }

    if (options.filename.empty()) {
        int map_flags = MAP_ANONYMOUS | MAP_SHARED;
        size_ = options.size;
        void* addr = mmap(nullptr, size_, prot, map_flags, -1, 0);
        if (addr == MAP_FAILED) {
            why = fmtlib::format("mmap: {}", std::strerror(errno));
            QUARK_LOG_ERROR("Failed to {}", why);
            return false;
        }
        addr_ = static_cast<uint8_t*>(addr);
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
            why = fmtlib::format("open {}: {}", options.filename, std::strerror(errno));
            QUARK_LOG_ERROR("Failed to {}", why);
            return false;
        }
        fd_ = fd;  // owned from here: reset() closes it if a later step fails
        struct stat st;
        if (fstat(fd, &st)) {
            why = fmtlib::format("fstat {}: {}", options.filename, std::strerror(errno));
            QUARK_LOG_ERROR("Failed to {}", why);
            return false;
        }
        if (options.writable && (st.st_mode & 0777) != options.mode) {
            if (fchmod(fd, options.mode) != 0) {
                why = fmtlib::format("chmod {}: {}", options.filename, std::strerror(errno));
                QUARK_LOG_ERROR("Failed to {}", why);
                return false;
            }
        }
        if (options.writable && options.size != 0 && static_cast<int64_t>(options.size) != st.st_size) {
            if (!options.create) {
                // not ours to resize: the process that created it may have it mapped (a shrunk file
                // raises SIGBUS there), and its layout differs from what we expect anyway
                why = fmtlib::format("{} is {} bytes, expected {}", options.filename, st.st_size, options.size);
                QUARK_LOG_ERROR("Refusing to map {}", why);
                return false;
            }
            size_ = options.size;
            if (ftruncate(fd, size_)) {
                why = fmtlib::format("ftruncate {}: {}", options.filename, std::strerror(errno));
                QUARK_LOG_ERROR("Failed to {}", why);
                return false;
            }
            truncated = true;
        } else {
            size_ = st.st_size;
            truncated = false;
        }
        void* addr = mmap(nullptr, size_, prot, map_flags, fd, 0);
        if (addr == MAP_FAILED) {
            why = fmtlib::format("mmap {}: {}", options.filename, std::strerror(errno));
            QUARK_LOG_ERROR("Failed to {}", why);
            return false;
        }
        addr_ = static_cast<uint8_t*>(addr);
        if (options.lock && mlock(addr_, size_)) {
            why = fmtlib::format("mlock {} bytes of {}: {}", size_, options.filename, std::strerror(errno));
            QUARK_LOG_ERROR("Failed to {}", why);
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
    size_ = 0;
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

QUARK_END_NAMESPACE
