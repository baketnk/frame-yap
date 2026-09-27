#include "placement.hpp"
#include "panel_drag.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>

using namespace frameyap;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)

namespace {
constexpr Matrix34 identity{{{{1.f, 0.f, 0.f, 0.f}},
                              {{0.f, 1.f, 0.f, 0.f}},
                              {{0.f, 0.f, 1.f, 0.f}}}};
void put(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    CHECK(bool(file)); file << bytes; CHECK(bool(file));
}
std::string get(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
bool near(float a, float b) { return std::abs(a - b) < 1e-6f; }
} // namespace

int main() {
    std::string pattern = (fs::temp_directory_path() / "frameyap-placement-test-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()) != nullptr);
    const fs::path dir = pattern;
    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    const auto left = default_placement_path(Mount::LeftWrist);
    const auto right = default_placement_path(Mount::RightWrist);
    const auto head = default_placement_path(Mount::Head);
    CHECK(left == dir / "frameyap/placement-left-wrist");
    CHECK(right == dir / "frameyap/placement-right-wrist");
    CHECK(head == dir / "frameyap/placement-head");
    CHECK(default_placement_path(Mount::World).empty());
    CHECK(!load_relative_placement(left, Mount::LeftWrist));

    RelativePlacement placed{relative_mount_pose(Mount::LeftWrist), 1.37f};
    // A freely rotated canvas with both depth and lateral offsets, rather than
    // only the editable roll angle in config.json.
    const auto turn = relative_mount_pose(Mount::RightWrist);
    placed.canvas_pose = compose_pose(placed.canvas_pose, turn);
    placed.canvas_pose[0][3] += .31f;
    placed.canvas_pose[1][3] -= .14f;
    placed.canvas_pose[2][3] += .22f;
    CHECK(valid_relative_placement(placed));
    CHECK(save_relative_placement(left, Mount::LeftWrist, placed));
    CHECK((fs::status(left).permissions() & fs::perms::group_read) == fs::perms::none);
    auto restored = load_relative_placement(left, Mount::LeftWrist);
    CHECK(restored && near(restored->scale, placed.scale));
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c)
        CHECK(near(restored->canvas_pose[r][c], placed.canvas_pose[r][c]));
    CHECK(!load_relative_placement(left, Mount::RightWrist));
    CHECK(!load_relative_placement(right, Mount::RightWrist));
    CHECK(save_relative_placement(right, Mount::RightWrist, RelativePlacement{relative_mount_pose(Mount::RightWrist), .7f}));
    CHECK(load_relative_placement(right, Mount::RightWrist)->scale == .7f);
    CHECK(load_relative_placement(left, Mount::LeftWrist)->scale == placed.scale);
    CHECK(save_relative_placement(head, Mount::Head, RelativePlacement{relative_mount_pose(Mount::Head), 2.f}));
    CHECK(load_relative_placement(head, Mount::Head)->scale == 2.f);
    CHECK(!save_relative_placement(left, Mount::World, placed));

    const auto good = get(left);
    for (const auto& bad : {std::string("frameyap-placement-v2 left-wrist 1\n"),
                            good + "junk", std::string(513, 'x')}) {
        put(left, bad);
        CHECK(!load_relative_placement(left, Mount::LeftWrist));
    }
    put(left, good);
    auto invalid = placed;
    invalid.scale = 0.f;
    CHECK(!save_relative_placement(left, Mount::LeftWrist, invalid));
    invalid = placed; invalid.scale = std::numeric_limits<float>::quiet_NaN();
    CHECK(!save_relative_placement(left, Mount::LeftWrist, invalid));
    invalid = placed; invalid.canvas_pose[0][0] *= 1.1f;
    CHECK(!save_relative_placement(left, Mount::LeftWrist, invalid));
    invalid = placed; invalid.canvas_pose[2][3] = 6.f;
    CHECK(!save_relative_placement(left, Mount::LeftWrist, invalid));
    CHECK(get(left) == good); // invalid writes cannot corrupt a prior placement

    const auto linked = dir / "linked-placement";
    fs::create_symlink(left, linked);
    CHECK(!load_relative_placement(linked, Mount::LeftWrist));
    CHECK(!save_relative_placement(linked, Mount::LeftWrist, placed));
    CHECK(get(left) == good);
    fs::create_directory(dir / "directory-placement");
    CHECK(!save_relative_placement(dir / "directory-placement", Mount::Head, placed));
    fs::create_directory_symlink(left.parent_path(), dir / "linked-directory");
    CHECK(!save_relative_placement(dir / "linked-directory/placement-left-wrist", Mount::LeftWrist, placed));
    fs::remove_all(dir);
    std::cout << "relative placement persistence checks passed\n";
}
