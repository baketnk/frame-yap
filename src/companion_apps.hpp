#pragma once
#include <filesystem>
#include <optional>
#include <string_view>

namespace frameyap {
// Resolve an installed launcher once at boot; never search the source checkout.
std::optional<std::filesystem::path> find_companion(std::string_view name);
// Fixed control arguments only; never pass pointer/UI text to a shell.
enum class CompanionCommand { Default, Show, Recenter };
// Returns false if fork or exec fails; does not wait for the overlay to exit.
bool launch_companion(const std::filesystem::path& launcher, CompanionCommand command = CompanionCommand::Default);
}
