#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sys/file.h>

#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"
#include "quark/utils/MMapFile.h"
#include "quark/third_party/SPSC_Queue/SPSCVarQueue.h"

QUARK_BEGIN_NAMESPACE

template <uint32_t Bytes>
class QUARK_API FullDuplexShm {
    using Queue = SPSCVarQueue<Bytes>;
public:
    using MsgHeader = typename Queue::MsgHeader;

    FullDuplexShm(std::string_view read_shm_path, std::string_view write_shm_path, bool create) {
        initialize(read_shm_path, write_shm_path, create);
        // Clear the read queue
        while (try_read([](auto&&) {}));
    }

    template <class Reader>
    QUARK_ALWAYS_INLINE bool try_read(Reader&& reader) {
        return read_queue()->tryPop(std::forward<Reader>(reader));
    }

    /// Largest payload try_write() accepts: the header and payload must fit the 16-bit size field
    /// and the ring.
    static constexpr uint32_t kMaxPayload =
        std::min<uint32_t>(UINT16_MAX, Bytes) - static_cast<uint32_t>(sizeof(MsgHeader));

    /// False if the queue is full or size > kMaxPayload (the queue's 16-bit size would wrap).
    template <class Writer>
    QUARK_ALWAYS_INLINE bool try_write(uint16_t size, Writer&& writer) {
        if (size > kMaxPayload) [[unlikely]] {
            return false;
        }
        return write_queue()->tryPush(size, std::forward<Writer>(writer));
    }
private:
    MMapFile write_file_;
    MMapFile read_file_;

    void initialize(std::string_view read_shm_path, std::string_view write_shm_path, bool create) {
        open_write(write_file_, write_shm_path, create);
        open_write(read_file_, read_shm_path, create);

        if (create) {
            set_permission(write_shm_path);
            set_permission(read_shm_path);
        }

        if (write_file_.fd() != -1) {
            if (flock(write_file_.fd(), LOCK_EX | LOCK_NB) != 0) {
                auto err = fmtlib::format("flock failed, likely occupied by other programs. path: {}, fd: {}, error: {}",
                    write_shm_path, write_file_.fd(), errno);
                QUARK_THROW(QuarkError{std::move(err)});
            }
        }
    }

    static void set_permission(std::string_view file) {
        namespace fs = std::filesystem;
        std::error_code ec;  // error_code overloads: no exception, also without QUARK_NO_EXCEPTIONS
        fs::permissions(file,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::group_write | fs::perms::others_read | fs::perms::others_write,
                        fs::perm_options::replace, ec);
        if (ec) {
            QUARK_THROW(QuarkError{fmtlib::format("failed to set 0666 to file: {}, error: {}", file, ec.message())});
        }
    }

    static void open_write(MMapFile& file, std::string_view filename, bool create) {
        if (create) {
            create_dir(filename);
        }
        MMapFile::Options options;
        options.filename = filename;
        options.writable = true;
        options.lock = true;
        options.create = create;
        options.mode = 0666;
        options.size = sizeof(Queue);
        bool truncated = false;
        file.initialize(options, truncated);
        if (truncated) {
            new (reinterpret_cast<Queue*>(file.addr())) Queue;
        }
    }

    static void create_dir(std::string_view filename) {
        namespace fs = std::filesystem;
        if (filename.empty()) {
            QUARK_THROW(QuarkError{"Empty shm path"});
        }
        std::error_code ec;
        auto path = fs::path(filename);
        if (const auto parent_path = path.parent_path(); !parent_path.empty()) {
            fs::create_directories(parent_path, ec);
            if (ec) {
                QUARK_THROW(QuarkError{fmtlib::format("cannot create {}: {}", parent_path.string(), ec.message())});
            }
        }
        // every ancestor directory must be traversable by the peer process
        while (path.has_parent_path()) {
            path = path.parent_path();
            if (path.parent_path() == path) {
                break;
            }
            const fs::perms p = fs::status(path, ec).permissions();
            if (ec) {
                QUARK_THROW(QuarkError{fmtlib::format("cannot stat {}: {}", path.string(), ec.message())});
            }
            const fs::perms new_p = p | fs::perms::others_exec | fs::perms::group_exec | fs::perms::owner_exec;
            if (p != new_p) {
                fs::permissions(path, new_p, ec);
                if (ec) {
                    QUARK_THROW(QuarkError{fmtlib::format("cannot chmod {}: {}", path.string(), ec.message())});
                }
            }
        }
    }

    QUARK_ALWAYS_INLINE Queue *write_queue() {
        return reinterpret_cast<Queue *>(write_file_.addr());
    }

    QUARK_ALWAYS_INLINE Queue *read_queue() {
        return reinterpret_cast<Queue *>(read_file_.addr());
    }
};

QUARK_END_NAMESPACE
