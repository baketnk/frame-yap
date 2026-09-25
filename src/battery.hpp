#pragma once
#include <filesystem>
#include <optional>

namespace frameyap {
struct BatteryLevel {
    int percent = 0; // 0..100
    bool charging = false;
    bool operator==(const BatteryLevel&) const = default;
};
// The device's own battery from a Linux power_supply tree. Peripheral batteries
// (scope=Device, such as paired controllers) are skipped. Read-only; no sudo.
std::optional<BatteryLevel> system_battery(const std::filesystem::path& root = "/sys/class/power_supply");
} // namespace frameyap
