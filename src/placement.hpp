#pragma once

#include "mount.hpp"
#include <filesystem>
#include <optional>

namespace frameyap {
// The exact OpenVR canvas transform in the selected tracked device's local
// coordinate frame, including the transparent grab/scale margins.
struct RelativePlacement {
    Matrix34 canvas_pose;
    float scale = 1.f;
};

// World-space placement is deliberately not persisted across tracking origins.
std::filesystem::path default_placement_path(Mount mount);
bool valid_relative_placement(const RelativePlacement& placement);
std::optional<RelativePlacement> load_relative_placement(const std::filesystem::path& path, Mount mount);
// Atomic owner-only write; does not follow a symlink at path or its parent.
bool save_relative_placement(const std::filesystem::path& path, Mount mount,
                             const RelativePlacement& placement);
} // namespace frameyap
