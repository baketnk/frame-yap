#include "placement.hpp"
#include "panel_drag.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace frameyap {
namespace {
constexpr std::size_t max_placement_bytes = 512;
std::atomic<unsigned long> next_temp{0};
bool relative_mount(Mount mount) {
    return mount == Mount::LeftWrist || mount == Mount::RightWrist || mount == Mount::Head;
}
} // namespace

std::filesystem::path default_placement_path(Mount mount) {
    if (!relative_mount(mount)) return {};
    const auto preference = default_mount_settings_path();
    if (preference.empty()) return {};
    return preference.parent_path() / ("placement-" + std::string(mount_name(mount)));
}

bool valid_relative_placement(const RelativePlacement& placement) {
    if (!panel_drag_detail::rigid(placement.canvas_pose) ||
        !std::isfinite(placement.scale) || placement.scale < .5f || placement.scale > 2.f) return false;
    for (int row = 0; row < 3; ++row)
        if (std::abs(placement.canvas_pose[row][3]) > 5.f) return false;
    return true;
}

std::optional<RelativePlacement> load_relative_placement(const std::filesystem::path& path, Mount mount) {
    try {
        if (!relative_mount(mount) || path.empty() || std::filesystem::is_symlink(path) ||
            !std::filesystem::is_regular_file(path)) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        char bytes[max_placement_bytes + 1];
        file.read(bytes, sizeof(bytes));
        const auto count = file.gcount();
        if (count <= 0 || count > static_cast<std::streamsize>(max_placement_bytes)) return {};
        std::istringstream input(std::string(bytes, static_cast<std::size_t>(count)));
        input.imbue(std::locale::classic());
        std::string magic, name;
        RelativePlacement placement{};
        if (!(input >> magic >> name >> placement.scale) || magic != "frameyap-placement-v1" ||
            name != mount_name(mount)) return {};
        for (auto& row : placement.canvas_pose)
            for (auto& value : row)
                if (!(input >> value)) return {};
        input >> std::ws;
        if (!input.eof() || !valid_relative_placement(placement)) return {};
        return placement;
    } catch (...) { return {}; }
}

bool save_relative_placement(const std::filesystem::path& path, Mount mount,
                             const RelativePlacement& placement) {
    if (!relative_mount(mount) || path.empty() || !path.is_absolute() ||
        !valid_relative_placement(placement)) return false;
    std::filesystem::path temporary;
    int fd = -1;
    try {
        const auto dir = path.parent_path();
        if (std::filesystem::is_symlink(dir) || std::filesystem::is_symlink(path) ||
            (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path))) return false;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "frameyap-placement-v1 " << mount_name(mount) << ' ' <<
            std::setprecision(std::numeric_limits<float>::max_digits10) << placement.scale;
        for (const auto& row : placement.canvas_pose)
            for (float value : row) out << ' ' << value;
        out << '\n';
        const auto content = out.str();
        if (content.size() > max_placement_bytes) return false;
        std::filesystem::create_directories(dir);
        if (std::filesystem::is_symlink(dir)) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            temporary = dir / (path.filename().string() + ".tmp." + std::to_string(::getpid()) +
                               "." + std::to_string(next_temp.fetch_add(1)));
            fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) break;
            if (errno != EEXIST) return false;
        }
        if (fd < 0) return false;
        std::size_t written = 0;
        while (written < content.size()) {
            const auto n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ::close(fd); fd = -1; std::filesystem::remove(temporary); return false; }
            written += static_cast<std::size_t>(n);
        }
        const bool synced = ::fsync(fd) == 0;
        const bool closed = ::close(fd) == 0;
        fd = -1;
        if (!synced || !closed) { std::filesystem::remove(temporary); return false; }
        std::filesystem::rename(temporary, path);
        return true;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
        return false;
    }
}
} // namespace frameyap
