#include "mount.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace frameyap {
namespace {
constexpr Matrix34 identity{{{{1.f, 0.f, 0.f, 0.f}},
                              {{0.f, 1.f, 0.f, 0.f}},
                              {{0.f, 0.f, 1.f, 0.f}}}};
constexpr std::size_t max_preference_bytes = 32;
std::atomic<unsigned long> next_temp{0};
} // namespace

std::string_view mount_name(Mount mount) {
    switch (mount) {
    case Mount::World: return "world";
    case Mount::LeftWrist: return "left-wrist";
    case Mount::RightWrist: return "right-wrist";
    case Mount::Head: return "head";
    }
    return {};
}

std::optional<Mount> parse_mount(std::string_view name) {
    if (name == "world") return Mount::World;
    if (name == "left-wrist") return Mount::LeftWrist;
    if (name == "right-wrist") return Mount::RightWrist;
    if (name == "head") return Mount::Head;
    return std::nullopt;
}

std::optional<Matrix34> world_mount_pose(const Matrix34& hmd) {
    for (const auto& row : hmd)
        for (float value : row)
            if (!std::isfinite(value)) return std::nullopt;

    // The HMD's -Z is its forward direction. Discard pitch and roll, so that
    // the panel stays level even when the wearer tilts their head.
    const double fx = -static_cast<double>(hmd[0][2]);
    const double fz = -static_cast<double>(hmd[2][2]);
    const double horizontal = std::hypot(fx, fz);
    if (!std::isfinite(horizontal) || horizontal < 1e-4) return std::nullopt;
    const double x = fx / horizontal, z = fz / horizontal;
    const double px = static_cast<double>(hmd[0][3]) + 1.05 * x;
    const double py = static_cast<double>(hmd[1][3]) - 0.16;
    const double pz = static_cast<double>(hmd[2][3]) + 1.05 * z;
    const Matrix34 pose{{{{static_cast<float>(-z), 0.f, static_cast<float>(-x), static_cast<float>(px)}},
                         {{0.f, 1.f, 0.f, static_cast<float>(py)}},
                         {{static_cast<float>(x), 0.f, static_cast<float>(-z), static_cast<float>(pz)}}}};
    for (const auto& row : pose)
        for (float value : row)
            if (!std::isfinite(value)) return std::nullopt;
    return pose;
}

Matrix34 relative_mount_pose(Mount mount, const WristPlacement& wrist) {
    auto pose = identity;
    switch (mount) {
    case Mount::Head: pose[1][3] = -0.16f; pose[2][3] = -1.05f; break;
    case Mount::LeftWrist: case Mount::RightWrist: {
        // VR Workspace's fallback wrist frame maps +Y toward fingers (-Z of
        // controller), +Z out of the back of the hand (+Y of controller).
        // Its HUD has right=fingers, up=back of hand, front=right x up.
        // The 0.089 m Z is the 0.054 m wrist calibration plus 0.035 m
        // backward from the fingers. Y also accounts for the HUD canvas's
        // bottom anchor: .12 m wrist lift + .09 m anchor - ~.03 m panel center.
        const float angle = wrist.roll_degrees * (std::acos(-1.f) / 180.f);
        const float c = std::cos(angle), s = std::sin(angle);
        pose[0] = {0.f, s, c, c * wrist.x + s * wrist.y};
        pose[1] = {0.f, c, -s, -s * wrist.x + c * wrist.y};
        pose[2] = {-1.f, 0.f, 0.f, wrist.z};
        if (mount == Mount::RightWrist) {
            // The wearer views the right wrist from the opposite side. Rotate
            // 180 degrees about panel-up, not a reflection or upside-down roll:
            // both right and front reverse, while up and center stay unchanged.
            for (auto& row : pose) { row[0] = -row[0]; row[2] = -row[2]; }
        }
        break;
    }
    case Mount::World: break;
    }
    return pose;
}

float mount_width(Mount mount, const WristPlacement& wrist) {
    return mount == Mount::LeftWrist || mount == Mount::RightWrist ? wrist.width : 0.85f;
}

std::filesystem::path default_mount_settings_path() {
    try {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg && std::filesystem::path(xdg).is_absolute())
            return std::filesystem::path(xdg) / "frameyap/mount";
        const char* home = std::getenv("HOME");
        if (home && *home && std::filesystem::path(home).is_absolute())
            return std::filesystem::path(home) / ".config/frameyap/mount";
    } catch (...) { return {}; }
    return {};
}

Mount load_mount(const std::filesystem::path& path) {
    try {
        if (path.empty() || !std::filesystem::is_regular_file(path)) return Mount::World;
        std::ifstream input(path, std::ios::binary);
        if (!input) return Mount::World;
        char bytes[max_preference_bytes + 1];
        input.read(bytes, sizeof(bytes));
        const auto size = input.gcount();
        if (size <= 0 || size > static_cast<std::streamsize>(max_preference_bytes)) return Mount::World;
        std::string_view token(bytes, static_cast<std::size_t>(size));
        if (token.back() == '\n') token.remove_suffix(1);
        return parse_mount(token).value_or(Mount::World);
    } catch (...) { return Mount::World; }
}

bool save_mount(const std::filesystem::path& path, Mount mount) {
    std::filesystem::path temporary;
    bool created = false;
    int fd = -1;
    try {
        const auto name = mount_name(mount);
        if (path.empty() || path.filename().empty() || name.empty()) return false;
        const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            temporary = dir / (path.filename().string() + ".tmp." + std::to_string(::getpid()) +
                               "." + std::to_string(next_temp.fetch_add(1)));
            fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) { created = true; break; }
            if (errno != EEXIST) return false;
        }
        if (!created) return false;
        const std::string content = std::string(name) + '\n';
        std::size_t written = 0;
        while (written < content.size()) {
            const auto n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ::close(fd); fd = -1; std::filesystem::remove(temporary, ec); return false; }
            written += static_cast<std::size_t>(n);
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
        if (created) { std::error_code ec; std::filesystem::remove(temporary, ec); }
        return false;
    }
}

} // namespace frameyap
