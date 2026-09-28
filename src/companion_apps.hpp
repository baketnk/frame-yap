#pragma once
#include <filesystem>
#include <optional>
#include <string_view>

namespace frameyap {
// Resolve an installed launcher once at boot; never search the source checkout.
std::optional<std::filesystem::path> find_companion(std::string_view name);
// Launch the installed wrapper (which toggles an existing overlay) without a shell.
// Returns false if fork or exec fails; does not wait for the overlay to exit.
bool launch_companion(const std::filesystem::path& launcher);
}
