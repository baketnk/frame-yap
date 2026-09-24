#include "laser_setting.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    using namespace frameyap;
    const auto pattern = (std::filesystem::temp_directory_path() / "frameyap-laser-test-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = ::mkdtemp(buffer.data());
    assert(created);
    const std::filesystem::path root(created);
    assert(::setenv("XDG_CONFIG_HOME", root.c_str(), 1) == 0);
    assert(default_laser_settings_path() == root / "frameyap/lasers-anytime");
    const auto path = default_laser_settings_path();
    assert(!load_lasers_anytime(path));
    assert(save_lasers_anytime(path, true));
    assert(load_lasers_anytime(path));
    assert((std::filesystem::status(path).permissions() &
            (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
    assert(save_lasers_anytime(path, false));
    assert(!load_lasers_anytime(path));
    { std::ofstream out(path, std::ios::trunc); out << "anything else\n"; }
    assert(!load_lasers_anytime(path));
    const auto link = root / "link";
    std::filesystem::create_symlink(path, link);
    assert(!load_lasers_anytime(link));
    assert(!save_lasers_anytime(link, true));
    assert(!load_lasers_anytime(path));
    std::filesystem::remove_all(root);
}
