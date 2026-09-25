#include "battery.hpp"
#include <cassert>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace frameyap;
namespace fs = std::filesystem;
namespace {
void put(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value << '\n';
}
}
int main() {
    const auto root = fs::temp_directory_path() / ("frameyap-battery-" + std::to_string(::getpid()));
    fs::remove_all(root);
    assert(!system_battery(root)); // missing tree
    fs::create_directories(root);
    put(root / "AC/type", "Mains");
    put(root / "AC/online", "1");
    put(root / "hid-controller/type", "Battery"); // paired peripheral, not the headset
    put(root / "hid-controller/scope", "Device");
    put(root / "hid-controller/capacity", "12");
    assert(!system_battery(root));
    put(root / "BAT0/type", "Battery");
    put(root / "BAT0/capacity", "250"); // implausible values are ignored
    assert(!system_battery(root));
    put(root / "BAT0/capacity", "64");
    put(root / "BAT0/status", "Discharging");
    assert((system_battery(root) == BatteryLevel{64, false}));
    put(root / "BAT0/status", "Charging");
    assert((system_battery(root) == BatteryLevel{64, true}));
    put(root / "BAT0/capacity", "x1");
    assert(!system_battery(root));
    fs::remove_all(root);
}
