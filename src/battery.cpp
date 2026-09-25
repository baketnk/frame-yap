#include "battery.hpp"
#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace frameyap {
namespace {
std::string first_line(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    if (in) std::getline(in, line);
    return line;
}
} // namespace

std::optional<BatteryLevel> system_battery(const std::filesystem::path& root) {
    std::error_code error;
    std::vector<std::filesystem::path> supplies;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) supplies.push_back(entry.path());
    std::sort(supplies.begin(), supplies.end()); // deterministic choice among several
    for (const auto& supply : supplies) {
        if (first_line(supply / "type") != "Battery" || first_line(supply / "scope") == "Device") continue;
        const auto capacity = first_line(supply / "capacity");
        if (capacity.empty() || capacity.size() > 3 ||
            !std::all_of(capacity.begin(), capacity.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const int percent = std::stoi(capacity);
        if (percent > 100) continue;
        return BatteryLevel{percent, first_line(supply / "status") == "Charging"};
    }
    return std::nullopt;
}
} // namespace frameyap
