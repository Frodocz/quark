#pragma once

#include "quark/Attributes.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

QUARK_BEGIN_NAMESPACE

/// What a liveness probe found. Unknown: the probe itself failed (e.g. no fd left), which says nothing
/// about the client: don't treat it as Dead.
enum class Liveness : uint8_t { Dead, Alive, Unknown };

/// The server side of FullDuplexShm sessions kept in one directory. Each client creates two files,
/// `<name><client_ext>` (client -> server, the client holds an exclusive flock on it while it runs)
/// and `<name><server_ext>` (server -> client). The server opens a session with
///   FullDuplexShm<N>(dir.client_path(name), dir.server_path(name), /*create=*/false)
/// and drops it once is_alive(name) turns false. A crashed client releases its lock with its fd.
///
/// Directory scans and flock probes are syscalls: call alive() from a timer, not per step.
///
/// Failures are reported, never folded into "no clients": a scan that fails (no fd left, a permission
/// error) returns nullopt, and the caller keeps its sessions as they are until a scan succeeds.
class ShmDirectory {
public:
    ShmDirectory(std::string dir, std::string client_ext, std::string server_ext)
        : dir_{std::move(dir)}, client_ext_{std::move(client_ext)}, server_ext_{std::move(server_ext)} {}

    [[nodiscard]] std::string client_path(std::string_view name) const { return path(name, client_ext_); }
    [[nodiscard]] std::string server_path(std::string_view name) const { return path(name, server_ext_); }

    /// Whether the client `name` runs: Alive if someone holds the exclusive lock on its file, Dead if
    /// nobody does or the file doesn't exist, Unknown if the probe failed.
    [[nodiscard]] Liveness liveness(std::string_view name) const {
        const int fd = ::open(client_path(name).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return errno == ENOENT ? Liveness::Dead : Liveness::Unknown;
        Liveness result = Liveness::Dead;  // we got the shared lock: no client holds LOCK_EX
        if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
            result = errno == EWOULDBLOCK ? Liveness::Alive : Liveness::Unknown;
        }
        ::close(fd);  // also drops our shared lock if we got it
        return result;
    }

    /// Names of the clients running now, sorted, both files present. Empty if the directory doesn't
    /// exist yet; nullopt if the scan failed (then nothing can be concluded about any client).
    [[nodiscard]] std::optional<std::vector<std::string>> alive() const {
        namespace fs = std::filesystem;
        std::vector<std::string> names;
        std::error_code ec;
        fs::directory_iterator it(dir_, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) return names;
            return std::nullopt;
        }
        // explicit increments: the range-for one throws filesystem_error on an error
        for (const fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) return std::nullopt;
            const std::string file = it->path().filename().string();
            if (!file.ends_with(client_ext_)) continue;
            const std::string name = file.substr(0, file.size() - client_ext_.size());
            if (name.empty()) continue;
            const bool paired = fs::exists(server_path(name), ec);
            if (ec) return std::nullopt;
            if (!paired) continue;
            switch (liveness(name)) {
                case Liveness::Alive: names.push_back(name); break;
                case Liveness::Dead: break;
                case Liveness::Unknown: return std::nullopt;
            }
        }
        if (ec) return std::nullopt;
        std::sort(names.begin(), names.end());
        return names;
    }

private:
    std::string dir_;
    std::string client_ext_;
    std::string server_ext_;

    [[nodiscard]] std::string path(std::string_view name, std::string_view ext) const {
        return (std::filesystem::path(dir_) / (std::string(name) + std::string(ext))).string();
    }
};

QUARK_END_NAMESPACE
