#include "angle_fade.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace frameyap;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
namespace {
constexpr Matrix34 identity{{{{1.f, 0.f, 0.f, 0.f}},
                              {{0.f, 1.f, 0.f, 0.f}},
                              {{0.f, 0.f, 1.f, 0.f}}}};
Matrix34 pitch(float degrees) {
    auto pose = identity;
    const float a = degrees * 3.14159265358979323846f / 180.f;
    pose[1][1] = pose[2][2] = std::cos(a);
    pose[1][2] = -std::sin(a); pose[2][1] = std::sin(a);
    return pose;
}
Matrix34 roll(float degrees) {
    auto pose = identity;
    const float a = degrees * 3.14159265358979323846f / 180.f;
    pose[0][0] = pose[1][1] = std::cos(a);
    pose[0][1] = -std::sin(a); pose[1][0] = std::sin(a);
    return pose;
}
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }
} // namespace
int main() {
    auto head = identity;
    head[2][3] = 1.f; // facing +Z of the panel
    CHECK(near(wrist_view_opacity(identity, head), 1.f));
    CHECK(near(wrist_view_opacity(pitch(60.f), head), 1.f));
    CHECK(near(wrist_view_opacity(pitch(67.5f), head), .5f));
    CHECK(near(wrist_view_opacity(pitch(75.f), head), 0.f));
    CHECK(near(wrist_view_opacity(pitch(-67.5f), head), .5f));
    CHECK(near(wrist_view_opacity(roll(67.5f), head), .5f)); // no front-only shortcut
    CHECK(near(wrist_view_opacity(roll(180.f), head), 0.f));
    auto anchor = identity;
    anchor[0] = {0.f, 0.f, 1.f, 3.f};
    anchor[2] = {-1.f, 0.f, 0.f, 2.f};
    auto turned_head = anchor;
    turned_head[0][3] = 4.f;
    CHECK(near(wrist_view_opacity_relative(identity, anchor, turned_head), 1.f));
    CHECK(near(wrist_view_opacity_relative(pitch(67.5f), anchor, turned_head), .5f));
    const auto tilted = pitch(55.f);
    auto combined = tilted;
    // Add a roll about the viewer axis: each axis alone is readable, combined is not.
    auto r = roll(55.f);
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        float value = 0;
        for (int k = 0; k < 3; ++k) value += r[row][k] * tilted[k][col];
        combined[row][col] = value;
    }
    CHECK(near(wrist_view_opacity(combined, head), 0.f));
    auto behind = head;
    behind[2][3] = -1.f;
    CHECK(near(wrist_view_opacity(identity, behind), 0.f));
    auto offset = head;
    offset[0][3] = 2.f; // camera moved aside, not rotated
    CHECK(wrist_view_opacity(identity, offset) < 1.f);
    CHECK(near(wrist_view_opacity(identity, identity), 0.f));
    auto invalid = identity;
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(near(wrist_view_opacity(invalid, head), 0.f));
    std::cout << "wrist angle fade checks passed\n";
}
