#include "mount.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace frameyap;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)

namespace {
constexpr Matrix34 identity{{{{1.f, 0.f, 0.f, 0.f}},
                              {{0.f, 1.f, 0.f, 0.f}},
                              {{0.f, 0.f, 1.f, 0.f}}}};
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }
void check_basis(const Matrix34& pose, float right_x, float right_z, float back_x, float back_z) {
    CHECK(near(pose[0][0], right_x)); CHECK(near(pose[1][0], 0)); CHECK(near(pose[2][0], right_z));
    CHECK(near(pose[0][1], 0)); CHECK(near(pose[1][1], 1)); CHECK(near(pose[2][1], 0));
    CHECK(near(pose[0][2], back_x)); CHECK(near(pose[1][2], 0)); CHECK(near(pose[2][2], back_z));
}
struct TempDir {
    fs::path path;
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "frameyap-mount-test-XXXXXX").string();
        auto* dir = ::mkdtemp(pattern.data());
        CHECK(dir != nullptr);
        path = dir;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
void put(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(bool(out)); out << content; CHECK(bool(out));
}
struct EnvRestore {
    std::string key;
    bool had;
    std::string original;
    explicit EnvRestore(const char* k) : key(k), had(std::getenv(k) != nullptr), original(had ? std::getenv(k) : "") {}
    ~EnvRestore() { if (had) ::setenv(key.c_str(), original.c_str(), 1); else ::unsetenv(key.c_str()); }
};
} // namespace

int main() {
    const std::array<Mount, 4> mounts{Mount::World, Mount::LeftWrist, Mount::RightWrist, Mount::Head};
    const std::array<std::string_view, 4> names{"world", "left-wrist", "right-wrist", "head"};
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        CHECK(mount_name(mounts[i]) == names[i]);
        CHECK(parse_mount(names[i]) == mounts[i]);
    }
    CHECK(!parse_mount("World")); CHECK(!parse_mount("head\n")); CHECK(!parse_mount("left_wrist"));
    CHECK(!parse_mount("")); CHECK(!parse_mount("world extra"));

    CHECK(near(mount_width(Mount::World), .85f)); CHECK(near(mount_width(Mount::Head), .85f));
    CHECK(near(mount_width(Mount::LeftWrist), .30f)); CHECK(near(mount_width(Mount::RightWrist), .30f));
    CHECK(relative_mount_pose(Mount::World) == identity);
    const auto head = relative_mount_pose(Mount::Head);
    check_basis(head, 1, 0, 0, 1);
    CHECK(near(head[0][3], 0)); CHECK(near(head[1][3], -.16f)); CHECK(near(head[2][3], -1.05f));
    const auto left = relative_mount_pose(Mount::LeftWrist), right = relative_mount_pose(Mount::RightWrist);
    check_basis(right, 0, 1, -1, 0); // right wrist faces inward, not away from wearer
    for (int r = 0; r < 3; ++r) {
        CHECK(near(right[r][3], left[r][3])); // position and up unchanged
        CHECK(near(right[r][1], left[r][1]));
        CHECK(near(right[r][0], -left[r][0]));
        CHECK(near(right[r][2], -left[r][2]));
    }
    CHECK(near(left[0][0], 0)); CHECK(near(left[1][0], 0)); CHECK(near(left[2][0], -1));
    CHECK(near(left[0][1], 0)); CHECK(near(left[1][1], 1)); CHECK(near(left[2][1], 0));
    CHECK(near(left[0][2], 1)); CHECK(near(left[1][2], 0)); CHECK(near(left[2][2], 0));
    CHECK(near(left[0][3], 0)); CHECK(near(left[1][3], .18f)); CHECK(near(left[2][3], .089f));
    WristPlacement tuned{.02f, .1f, .07f, .25f, 90.f};
    const auto rotated = relative_mount_pose(Mount::RightWrist, tuned);
    CHECK(near(rotated[0][1], 1)); CHECK(near(rotated[1][1], 0));
    CHECK(near(rotated[0][2], 0)); CHECK(near(rotated[1][2], 1));
    // Every roll remains an orthonormal, positive-determinant rotation: no mirror.
    for (float roll : {-180.f, -90.f, -35.f, 0.f, 90.f, 180.f}) {
        tuned.roll_degrees = roll;
        for (auto hand : {Mount::LeftWrist, Mount::RightWrist}) {
            const auto m = relative_mount_pose(hand, tuned);
            for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
                float dot = 0;
                for (int r = 0; r < 3; ++r) dot += m[r][a] * m[r][b];
                CHECK(near(dot, a == b ? 1.f : 0.f));
            }
            for (int r = 0; r < 3; ++r) {
                const int a = (r + 1) % 3, b = (r + 2) % 3;
                CHECK(near(m[a][0] * m[b][1] - m[b][0] * m[a][1], m[r][2]));
            }
        }
    }
    CHECK(near(rotated[0][3], .1f)); CHECK(near(rotated[1][3], -.02f));
    CHECK(near(rotated[2][3], .07f)); CHECK(near(mount_width(Mount::RightWrist, tuned), .25f));
    CHECK(relative_mount_pose(Mount::Head, tuned) == head);
    CHECK(near(mount_width(Mount::World, tuned), .85f));

    auto hmd = identity;
    hmd[0][3] = 2; hmd[1][3] = 1.7f; hmd[2][3] = 3;
    auto pose = world_mount_pose(hmd);
    CHECK(pose); check_basis(*pose, 1, 0, 0, 1);
    CHECK(near((*pose)[0][3], 2)); CHECK(near((*pose)[1][3], 1.54f)); CHECK(near((*pose)[2][3], 1.95f));
    // 90 degree yaw: HMD looks +X. The panel's +Z points back toward -X.
    hmd[0][0] = 0; hmd[0][2] = -1; hmd[2][0] = 1; hmd[2][2] = 0;
    pose = world_mount_pose(hmd);
    CHECK(pose); check_basis(*pose, 0, 1, -1, 0);
    CHECK(near((*pose)[0][3], 3.05f)); CHECK(near((*pose)[2][3], 3));
    // Pitch must not tilt the panel or change its upright orientation.
    hmd = identity;
    hmd[1][1] = .8f; hmd[1][2] = -.6f;
    hmd[2][1] = .6f; hmd[2][2] = .8f;
    pose = world_mount_pose(hmd);
    CHECK(pose); check_basis(*pose, 1, 0, 0, 1);
    CHECK(near((*pose)[1][3], -.16f)); CHECK(near((*pose)[2][3], -1.05f));
    hmd[0][2] = 0; hmd[2][2] = 0; // forward is vertical, yaw undefined
    CHECK(!world_mount_pose(hmd));
    hmd[2][2] = 1e-6f; CHECK(!world_mount_pose(hmd));
    hmd = identity; hmd[0][3] = std::numeric_limits<float>::infinity(); CHECK(!world_mount_pose(hmd));
    hmd = identity; hmd[0][0] = std::numeric_limits<float>::quiet_NaN(); CHECK(!world_mount_pose(hmd));

    TempDir tmp;
    const auto file = tmp.path / "nested" / "mount";
    CHECK(load_mount(file) == Mount::World); CHECK(!save_mount({}, Mount::Head));
    for (auto mount : mounts) {
        CHECK(save_mount(file, mount));
        CHECK(load_mount(file) == mount);
        std::ifstream in(file, std::ios::binary);
        CHECK(bool(in));
        const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        CHECK(data == std::string(mount_name(mount)) + "\n");
    }
    put(file, "other\n"); CHECK(load_mount(file) == Mount::World);
    put(file, "head\nextra\n"); CHECK(load_mount(file) == Mount::World);
    put(file, std::string(128, 'a')); CHECK(load_mount(file) == Mount::World);
    put(file, std::string("head\0\n", 6)); CHECK(load_mount(file) == Mount::World);
    CHECK(!save_mount(tmp.path, Mount::Head)); // cannot rename over an existing directory
    CHECK(!save_mount(file, static_cast<Mount>(-1)));
    CHECK(load_mount(tmp.path) == Mount::World);

    EnvRestore restore_xdg("XDG_CONFIG_HOME"), restore_home("HOME");
    ::setenv("XDG_CONFIG_HOME", tmp.path.c_str(), 1);
    ::setenv("HOME", "/other-absolute-home", 1);
    CHECK(default_mount_settings_path() == tmp.path / "frameyap/mount");
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    CHECK(default_mount_settings_path() == fs::path("/other-absolute-home/.config/frameyap/mount"));
    ::setenv("HOME", "relative", 1);
    CHECK(default_mount_settings_path().empty());
    ::unsetenv("HOME"); ::unsetenv("XDG_CONFIG_HOME");
    CHECK(default_mount_settings_path().empty());
    std::cout << "mount checks passed\n";
}
