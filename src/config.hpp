#pragma once
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
struct Config {
    Theme theme;
    std::string font; // absolute TTF/OTF path; empty uses the bundled face
    // Requests OpenVR's experimental global action priority; SteamVR must allow it too.
    bool experimental_input_priority = false;
    // OpenVR action name -> physical Frame controller input path; empty disables it.
    std::map<std::string, std::string> buttons;
};
std::filesystem::path default_config_path();
Config load_config(const std::filesystem::path& path);
std::string resolve_font(const std::string& assets, const std::string& requested);
// No writes or OpenVR access when no custom button mappings are specified.
// When customized, build a generated manifest and bindings under XDG cache.
std::filesystem::path action_manifest(const std::string& assets, const Config& config);
} // namespace frameyap
