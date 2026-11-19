#pragma once

#include "quark/Attributes.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

QUARK_BEGIN_NAMESPACE

/// The server side of FullDuplexShm sessions kept in one directory. Each client creates two files,
/// `<name><client_ext>` (client -> server, the client holds an exclusive flock on it while it runs)
/// and `<name><server_ext>` (server -> client). The server opens a session with
///   FullDuplexShm<N>(dir.client_path(name), dir.server_path(name), /*create=*/false)
/// and drops it once is_alive(name) turns false. A crashed client releases its lock with its fd.
///
/// Directory scans and flock probes are syscalls: call alive() from a timer, not per step.
class ShmDirectory {
public:
    ShmDirectory(std::string dir, std::string client_ext, std::string server_ext)
        : dir_{std::move(dir)}, client_ext_{std::move(client_ext)}, server_ext_{std::move(server_ext)} {}

    [[nodiscard]] std::string client_path(std::string_view name) const { return path(name, client_ext_); }
    [[nodiscard]] std::string server_path(std::string_view name) const { return path(name, server_ext_); }

    /// Whether the client `name` runs: someone holds the exclusive lock on its file.
    [[nodiscard]] bool is_alive(std::string_view name) const {
        const int fd = ::open(client_path(name).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool locked = ::flock(fd, LOCK_SH | LOCK_NB) != 0;  // fails only against the client's LOCK_EX
        ::close(fd);  // also drops our shared lock if we got it
        return locked;
    }

    /// Names of the clients running now, sorted, both files present.
    [[nodiscard]] std::vector<std::string> alive() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string file = entry.path().filename().string();
            if (!file.ends_with(client_ext_)) continue;
            const std::string name = file.substr(0, file.size() - client_ext_.size());
            if (!name.empty() && std::filesystem::exists(server_path(name), ec) && is_alive(name)) {
                names.push_back(name);
            }
        }
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
