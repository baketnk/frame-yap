#pragma once

#include "mount.hpp"

#include <cmath>
#include <optional>

namespace frameyap {

// Inputs are rigid poses in the SAME coordinate frame (world or the same
// mounting device); the 3x3 blocks are proper orthonormal rotations. +X is
// panel right, +Y panel up, and the translation is the panel center.
enum class PanelDragKind { Grab, Scale };
struct PanelDragUpdate { float dx, dy, factor; };

namespace panel_drag_detail {
struct Vec3 { double x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline Vec3 column(const Matrix34& m, int c) { return {m[0][c], m[1][c], m[2][c]}; }
inline Vec3 translation(const Matrix34& m) { return column(m, 3); }
inline Vec3 rotate(const Matrix34& m, Vec3 v) {
    return column(m, 0) * v.x + column(m, 1) * v.y + column(m, 2) * v.z;
}
inline Vec3 inverse_rotate(const Matrix34& m, Vec3 v) {
    return {dot(column(m, 0), v), dot(column(m, 1), v), dot(column(m, 2), v)};
}
inline bool rigid(const Matrix34& m) {
    for (const auto& row : m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    const Vec3 x = column(m, 0), y = column(m, 1), z = column(m, 2);
    constexpr double tolerance = 1e-3;
    return std::abs(dot(x, x) - 1) < tolerance &&
           std::abs(dot(y, y) - 1) < tolerance &&
           std::abs(dot(z, z) - 1) < tolerance &&
           std::abs(dot(x, y)) < tolerance &&
           std::abs(dot(x, z)) < tolerance &&
           std::abs(dot(y, z)) < tolerance &&
           std::abs(dot(cross(x, y), z) - 1) < tolerance;
}
} // namespace panel_drag_detail

class PanelDrag {
public:
    bool begin(PanelDragKind kind, const Matrix34& panel_pose, float width, float height,
               float hit_x, float hit_y, const Matrix34& controller_pose) {
        using namespace panel_drag_detail;
        reset();
        if ((kind != PanelDragKind::Grab && kind != PanelDragKind::Scale) ||
            !rigid(panel_pose) || !rigid(controller_pose) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            !std::isfinite(hit_x) || !std::isfinite(hit_y) ||
            hit_x < 0 || hit_x > 1 || hit_y < 0 || hit_y > 1) return false;

        const Vec3 center = translation(panel_pose);
        const Vec3 right = column(panel_pose, 0), up = column(panel_pose, 1);
        const Vec3 normal = column(panel_pose, 2);
        const Vec3 anchor = center - right * (static_cast<double>(width) * .5) +
                            up * (static_cast<double>(height) * .5);
        const Vec3 down_hit = anchor + right * (static_cast<double>(width) * hit_x) -
                              up * (static_cast<double>(height) * hit_y);
        const Vec3 down_vector = down_hit - translation(controller_pose);
        const double length = std::sqrt(dot(down_vector, down_vector));
        if (!std::isfinite(length) || length < 1e-6 || length > 10) return false;
        const Vec3 direction = down_vector * (1.0 / length);
        const Vec3 baseline = down_hit - anchor;
        const double baseline_sq = dot(baseline, baseline);
        if (!finite(anchor) || !finite(down_hit) || !finite(direction) ||
            std::abs(dot(direction, normal)) < 1e-5 ||
            (kind == PanelDragKind::Scale && (!std::isfinite(baseline_sq) || baseline_sq < 1e-16)))
            return false;

        kind_ = kind;
        center_ = center;
        right_ = right;
        up_ = up;
        normal_ = normal;
        anchor_ = anchor;
        down_hit_ = down_hit;
        baseline_sq_ = baseline_sq;
        local_ray_ = inverse_rotate(controller_pose, direction);
        active_ = true;
        return true;
    }

    std::optional<PanelDragUpdate> update(const Matrix34& controller_pose) const {
        using namespace panel_drag_detail;
        if (!active_ || !rigid(controller_pose)) return std::nullopt;
        const Vec3 origin = translation(controller_pose);
        const Vec3 ray = rotate(controller_pose, local_ray_);
        const double denominator = dot(ray, normal_);
        if (!finite(ray) || !std::isfinite(denominator) || std::abs(denominator) < 1e-5)
            return std::nullopt;
        const double distance = dot(center_ - origin, normal_) / denominator;
        if (!std::isfinite(distance) || distance <= 0 || distance > 10) return std::nullopt;
        const Vec3 hit = origin + ray * distance;
        const Vec3 delta = hit - down_hit_;
        const double dx = kind_ == PanelDragKind::Grab ? dot(delta, right_) : 0;
        const double dy = kind_ == PanelDragKind::Grab ? dot(delta, up_) : 0;
        const double factor = kind_ == PanelDragKind::Scale ?
            dot(hit - anchor_, down_hit_ - anchor_) / baseline_sq_ : 1;
        if (!finite(hit) || !std::isfinite(dx) || !std::isfinite(dy) ||
            !std::isfinite(factor) || !std::isfinite(static_cast<float>(dx)) ||
            !std::isfinite(static_cast<float>(dy)) || !std::isfinite(static_cast<float>(factor)))
            return std::nullopt;
        return PanelDragUpdate{static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(factor)};
    }

    void reset() { active_ = false; }
    bool active() const { return active_; }

private:
    using Vec3 = panel_drag_detail::Vec3;
    bool active_ = false;
    PanelDragKind kind_ = PanelDragKind::Grab;
    Vec3 center_{}, right_{}, up_{}, normal_{}, anchor_{}, down_hit_{}, local_ray_{};
    double baseline_sq_ = 0;
};

} // namespace frameyap
