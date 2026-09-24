#pragma once
#include <filesystem>

namespace frameyap {
// Opt-in, user-local preference. Missing or malformed files always mean off.
std::filesystem::path default_laser_settings_path();
bool load_lasers_anytime(const std::filesystem::path& path);
bool save_lasers_anytime(const std::filesystem::path& path, bool enabled);
} // namespace frameyap
