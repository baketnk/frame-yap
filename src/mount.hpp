#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

namespace frameyap {

enum class Mount { World, LeftWrist, RightWrist, Head };

std::string_view mount_name(Mount mount);
std::optional<Mount> parse_mount(std::string_view name);

// Row-major 3x4 affine transform (OpenVR axes: +Y up, -Z forward).
using Matrix34 = std::array<std::array<float, 4>, 3>;

// Controller-local watch placement. Defaults follow VR Workspace's fallback
// wrist calibration and HUD pose (not its avatar-specific calibration).
struct WristPlacement {
    float x = 0.f, y = 0.18f, z = 0.089f; // meters in controller coordinates
    float width = 0.30f; // meters
    float roll_degrees = 0.f; // about controller -Z before placing the panel
};

// World-space, yaw-only pose in front of the HMD, upright and facing the viewer.
// Invalid/nonfinite HMD transforms or a vanishing horizontal forward return nullopt.
std::optional<Matrix34> world_mount_pose(const Matrix34& hmd);
// Device-relative transform; World has no relative anchor and returns identity.
Matrix34 relative_mount_pose(Mount mount, const WristPlacement& wrist = {});
float mount_width(Mount mount, const WristPlacement& wrist = {});

// Empty when neither XDG_CONFIG_HOME nor HOME specifies an absolute base.
std::filesystem::path default_mount_settings_path();
// A missing, unreadable, overlong or malformed preference defaults to World.
Mount load_mount(const std::filesystem::path& path);
// Create local parent directories and atomically replace a single mount preference.
// Never stores any audio or transcript; failures return false without throwing.
bool save_mount(const std::filesystem::path& path, Mount mount);

} // namespace frameyap
