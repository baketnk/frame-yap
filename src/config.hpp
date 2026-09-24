#pragma once
#include "mount.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <string>

namespace frameyap {
using Rgba = std::array<unsigned char, 4>;
struct Theme {
    Rgba background{12, 16, 27, 255}, card{20, 28, 43, 255};
    Rgba ink{230, 240, 249, 255}, muted{151, 173, 193, 255};
    Rgba accent{31, 240, 164, 255}, warning{255, 110, 135, 255};
    Rgba frame_start{31, 255, 145, 255}, frame_end{31, 112, 255, 255};
};
enum class DateFormat { Off, MonthDayYear, DayMonthYear, Iso };
struct Config {
    Theme theme;
    std::string font; // absolute TTF/OTF path; empty uses the bundled face
    // Requests OpenVR's experimental global action priority; SteamVR must allow it too.
    bool experimental_input_priority = false;
    bool advanced_debug = false; // opt-in full diagnostic logging; never raw audio recording
    bool auto_insert = false; // opt-in; runtime also requires uninterrupted verified Xwayland focus
    bool clock_24h = false;
    DateFormat date_format = DateFormat::MonthDayYear;
    WristPlacement wrist;
    // OpenVR action name -> physical Frame controller input path; empty disables it.
    std::map<std::string, std::string> buttons;
};
std::filesystem::path default_config_path();
Config load_config(const std::filesystem::path& path);
// Update only the selected boolean in an existing valid config; false on invalid/unwritable paths.
// Other user customizations and formatting are retained; creates a minimal config if absent.
bool save_advanced_debug(const std::filesystem::path& path, bool enabled) noexcept;
bool save_auto_insert(const std::filesystem::path& path, bool enabled) noexcept;
bool save_clock_24h(const std::filesystem::path& path, bool enabled) noexcept;
bool save_date_format(const std::filesystem::path& path, DateFormat format) noexcept;
std::string resolve_font(const std::string& assets, const std::string& requested);
// No writes or OpenVR access when no custom button mappings are specified.
// When customized, build a generated manifest and bindings under XDG cache.
std::filesystem::path action_manifest(const std::string& assets, const Config& config);
} // namespace frameyap
