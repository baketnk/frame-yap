#pragma once

#include "mount.hpp"

#include <algorithm>
#include <cmath>

namespace frameyap {
namespace angle_fade_detail {
struct Vec3 { double x, y, z; };
inline Vec3 axis(const Matrix34& m, int column) { return {m[0][column], m[1][column], m[2][column]}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool normalize(Vec3& v) {
    const double n = std::sqrt(dot(v, v));
    if (!std::isfinite(n) || n < 1e-5) return false;
    v = v * (1. / n);
    return true;
}
} // namespace angle_fade_detail

// Wrist-only visibility: compare the entire panel orientation (including roll)
// to an upright panel pointing at the viewer. Fade linearly between 60 and
// 75 degrees; the matrix math here has no engine dependency.
// Both poses must be in the same tracking space, with rigid orthonormal axes.
// Invalid geometry hides the panel rather than making it clickable at full alpha.
inline float wrist_view_opacity(const Matrix34& panel, const Matrix34& head) {
    using namespace angle_fade_detail;
    Vec3 front = axis(head, 3) - axis(panel, 3);
    if (!normalize(front)) return 0.f;
    Vec3 right = axis(head, 0);
    right = right - front * dot(right, front);
    if (!normalize(right)) {
        right = cross(axis(head, 1), front);
        if (!normalize(right)) return 0.f;
    }
    Vec3 up = cross(front, right);
    if (!normalize(up)) return 0.f;
    Vec3 actual_right = axis(panel, 0), actual_up = axis(panel, 1), actual_front = axis(panel, 2);
    if (!normalize(actual_right) || !normalize(actual_up) || !normalize(actual_front)) return 0.f;
    // Trace of readable^T * actual is 1 + 2 cos(angle) for rotations.
    // Unlike a front-normal dot product, this also rejects upside-down text.
    const double cosine = std::clamp((dot(right, actual_right) + dot(up, actual_up) +
                                      dot(front, actual_front) - 1.) * .5, -1., 1.);
    if (!std::isfinite(cosine)) return 0.f;
    constexpr double pi = 3.14159265358979323846;
    const double angle = std::acos(cosine);
    return static_cast<float>(1. - std::clamp((angle - pi / 3.) / (pi / 12.), 0., 1.));
}

// OpenVR wrist transforms are controller-relative; tracking poses are absolute.
// Compose into the headset's tracking space before comparing orientations.
inline float wrist_view_opacity_relative(const Matrix34& local_panel, const Matrix34& anchor,
                                         const Matrix34& head) {
    Matrix34 world{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) {
        world[r][c] = c == 3 ? anchor[r][3] : 0.f;
        for (int k = 0; k < 3; ++k) world[r][c] += anchor[r][k] * local_panel[k][c];
    }
    return wrist_view_opacity(world, head);
}

} // namespace frameyap
