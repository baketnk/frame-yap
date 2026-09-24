#include "laser_setting.hpp"
#include "mount.hpp"

#include <atomic>
#include <cerrno>
#include <fstream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace frameyap {
namespace {
std::atomic<unsigned long> next_temp{0};
}

std::filesystem::path default_laser_settings_path() {
    auto mount_path = default_mount_settings_path();
    return mount_path.empty() ? mount_path : mount_path.parent_path() / "lasers-anytime";
}

bool load_lasers_anytime(const std::filesystem::path& path) {
    try {
        if (path.empty() || std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        char data[9]{};
        input.read(data, sizeof(data));
        const auto size = input.gcount();
        return size == 3 && std::string_view(data, 3) == "on\n";
    } catch (...) { return false; }
}

bool save_lasers_anytime(const std::filesystem::path& path, bool enabled) {
    std::filesystem::path temporary;
    int fd = -1;
    try {
        if (path.empty() || path.filename().empty() || std::filesystem::is_symlink(path)) return false;
        const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            temporary = dir / (path.filename().string() + ".tmp." + std::to_string(::getpid()) +
                               "." + std::to_string(next_temp.fetch_add(1)));
            fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) break;
            if (errno != EEXIST) return false;
        }
        if (fd < 0) return false;
        const std::string_view content = enabled ? "on\n" : "off\n";
        size_t written = 0;
        while (written < content.size()) {
            const auto n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ::close(fd); fd = -1; std::filesystem::remove(temporary, ec); return false; }
            written += static_cast<size_t>(n);
        }
        const bool synced = ::fsync(fd) == 0;
        const bool closed = ::close(fd) == 0;
        fd = -1;
        if (!synced || !closed) { std::filesystem::remove(temporary, ec); return false; }
        std::filesystem::rename(temporary, path, ec);
        if (ec) { std::filesystem::remove(temporary, ec); return false; }
        return true;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        if (!temporary.empty()) { std::error_code ec; std::filesystem::remove(temporary, ec); }
        return false;
    }
}
} // namespace frameyap
