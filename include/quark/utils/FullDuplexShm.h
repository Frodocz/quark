#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/vfs.h>

#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"
#include "quark/utils/MMapFile.h"
#include "quark/third_party/SPSC_Queue/SPSCVarQueue.h"

QUARK_BEGIN_NAMESPACE

/// A pair of SPSCVarQueue files between two processes: each side writes one queue and reads the other.
///
/// - The files must be on tmpfs (or hugetlbfs), e.g. under kDefaultDir: on a disk filesystem the kernel
///   writes the queue pages back (about every 30 s) and write-protects them, so every next write to
///   each page faults (~3 us, sometimes ms) on the path the messages take. Other locations are refused.
/// - The side that creates the files (`create`, the client) sizes and initializes them. The other side
///   (the server) opens them as they are: files of another size are refused, never resized under the
///   process that has them mapped.
/// - Opening drops the messages already waiting in the read queue (stale: e.g. sent while this side
///   was down), and counts them: dropped_on_open().
/// - Each side holds an exclusive flock on the file it writes while it runs: a second writer is
///   refused, and a crashed one releases it with its fd (see ShmDirectory).
template <uint32_t Bytes>
class QUARK_API FullDuplexShm {
    using Queue = SPSCVarQueue<Bytes>;
    struct Unopened {
        explicit Unopened() = default;
    };

public:
    using MsgHeader = typename Queue::MsgHeader;

    /// Where SHM session files belong (tmpfs on Linux).
    static constexpr std::string_view kDefaultDir = "/dev/shm";

    /// Throws QuarkError (a fatal error with QUARK_NO_EXCEPTIONS) if the files can't be opened; for
    /// files that come from another process at run time, use try_open().
    FullDuplexShm(std::string_view read_shm_path, std::string_view write_shm_path, bool create) {
        if (std::string error = open(read_shm_path, write_shm_path, create); !error.empty()) {
            QUARK_THROW(QuarkError{std::move(error)});
        }
    }

    /// Same as the constructor, but returns nullopt on failure (the reason in `*error` if given)
    /// instead of throwing, also with QUARK_NO_EXCEPTIONS.
    [[nodiscard]] static std::optional<FullDuplexShm> try_open(std::string_view read_shm_path,
                                                               std::string_view write_shm_path, bool create,
                                                               std::string* error = nullptr) {
        std::optional<FullDuplexShm> shm{std::in_place, Unopened{}};
        if (std::string why = shm->open(read_shm_path, write_shm_path, create); !why.empty()) {
            if (error != nullptr) *error = std::move(why);
            return std::nullopt;
        }
        return shm;
    }

    /// The state try_open() starts from; not usable until opened.
    explicit FullDuplexShm(Unopened) noexcept {}

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

    /// Messages that were waiting in the read queue when it was opened, and were dropped.
    [[nodiscard]] std::size_t dropped_on_open() const noexcept { return dropped_on_open_; }

private:
    MMapFile write_file_;
    MMapFile read_file_;
    std::size_t dropped_on_open_{0};

    // Empty on success, else the reason (the files are closed again). Never throws QuarkError.
    std::string open(std::string_view read_shm_path, std::string_view write_shm_path, bool create) {
        std::string error = open_files(read_shm_path, write_shm_path, create);
        if (!error.empty()) {
            write_file_.reset();
            read_file_.reset();
            return error;
        }
        while (try_read([](auto&&) {})) ++dropped_on_open_;
        return {};
    }

    std::string open_files(std::string_view read_shm_path, std::string_view write_shm_path, bool create) {
        for (const std::string_view path : {write_shm_path, read_shm_path}) {
            if (path.empty()) return "empty shm path";
            if (create) {
                if (std::string error = create_dir(path); !error.empty()) return error;
            }
            if (std::string error = check_filesystem(path, create); !error.empty()) return error;
        }
        if (std::string error = open_queue(write_file_, write_shm_path, create); !error.empty()) return error;
        if (std::string error = open_queue(read_file_, read_shm_path, create); !error.empty()) return error;

        if (create) {
            if (std::string error = set_permission(write_shm_path); !error.empty()) return error;
            if (std::string error = set_permission(read_shm_path); !error.empty()) return error;
        }
        if (::flock(write_file_.fd(), LOCK_EX | LOCK_NB) != 0) {
            return fmtlib::format("flock failed, likely occupied by other programs. path: {}, fd: {}, error: {}",
                                  write_shm_path, write_file_.fd(), std::strerror(errno));
        }
        return {};
    }

    // tmpfs or hugetlbfs only, checked on the directory before creating, on the file before opening
    static std::string check_filesystem(std::string_view file, bool create) {
        constexpr long kTmpfsMagic = 0x01021994;     // linux/magic.h TMPFS_MAGIC
        constexpr long kHugetlbfsMagic = 0x958458f6;  // linux/magic.h HUGETLBFS_MAGIC
        std::filesystem::path where{file};
        if (create) {
            where = where.parent_path();
            if (where.empty()) where = ".";
        }
        struct statfs s {};
        if (::statfs(where.c_str(), &s) != 0) {
            return fmtlib::format("cannot statfs {}: {}", where.string(), std::strerror(errno));
        }
        const long type = static_cast<long>(s.f_type);
        if (type != kTmpfsMagic && type != kHugetlbfsMagic) {
            return fmtlib::format("{} is on a filesystem of type {:#x}, not tmpfs or hugetlbfs: queue pages would "
                                  "page-fault after every writeback. Use a directory under {}",
                                  where.string(), static_cast<unsigned long>(type), kDefaultDir);
        }
        return {};
    }

    static std::string set_permission(std::string_view file) {
        namespace fs = std::filesystem;
        std::error_code ec;  // error_code overloads: no exception, also without QUARK_NO_EXCEPTIONS
        fs::permissions(file,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::group_write | fs::perms::others_read | fs::perms::others_write,
                        fs::perm_options::replace, ec);
        if (ec) {
            return fmtlib::format("failed to set 0666 to file: {}, error: {}", file, ec.message());
        }
        return {};
    }

    static std::string open_queue(MMapFile& file, std::string_view filename, bool create) {
        MMapFile::Options options;
        options.filename = filename;
        options.writable = true;
        options.lock = true;
        options.create = create;
        options.mode = 0666;
        options.size = sizeof(Queue);
        bool truncated = false;
        std::string error;
        if (!file.try_initialize(options, truncated, &error)) {
            return fmtlib::format("cannot open shm file {}: {}", filename, error);
        }
        if (create && truncated) {  // only the side that creates the file lays the queue out
            new (reinterpret_cast<Queue*>(file.addr())) Queue;
        }
        return {};
    }

    static std::string create_dir(std::string_view filename) {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto path = fs::path(filename);
        if (const auto parent_path = path.parent_path(); !parent_path.empty()) {
            fs::create_directories(parent_path, ec);
            if (ec) {
                return fmtlib::format("cannot create {}: {}", parent_path.string(), ec.message());
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
                return fmtlib::format("cannot stat {}: {}", path.string(), ec.message());
            }
            const fs::perms new_p = p | fs::perms::others_exec | fs::perms::group_exec | fs::perms::owner_exec;
            if (p != new_p) {
                fs::permissions(path, new_p, ec);
                if (ec) {
                    return fmtlib::format("cannot chmod {}: {}", path.string(), ec.message());
                }
            }
        }
        return {};
    }

    QUARK_ALWAYS_INLINE Queue *write_queue() {
        return reinterpret_cast<Queue *>(write_file_.addr());
    }

    QUARK_ALWAYS_INLINE Queue *read_queue() {
        return reinterpret_cast<Queue *>(read_file_.addr());
    }
};

QUARK_END_NAMESPACE
