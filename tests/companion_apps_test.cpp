#include "companion_apps.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
int main() {
    char name[] = "/tmp/frameyap-companions-XXXXXX";
    const auto root = fs::path(::mkdtemp(name));
    const auto home = root / "home", local = home / ".local/bin", path_dir = root / "path";
    fs::create_directories(local); fs::create_directories(path_dir);
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("PATH", path_dir.c_str(), 1);
    assert(!frameyap::find_companion("tnkplan"));
    assert(!frameyap::find_companion("tnkboard"));
    assert(!frameyap::find_companion("tnkdraw"));
    assert(!frameyap::find_companion("other"));
    const auto plan = local / "tnkplan", keyboard = path_dir / "tnkboard", draw = local / "tnkdraw";
    std::ofstream(plan) << "#!/bin/sh\nexit 0\n";
    std::ofstream(keyboard) << "#!/bin/sh\nexit 0\n";
    std::ofstream(draw) << "#!/bin/sh\nexit 0\n";
    assert(!frameyap::find_companion("tnkplan")); // a file is not yet executable
    assert(!frameyap::find_companion("tnkdraw"));
    ::chmod(plan.c_str(), 0700);
    ::chmod(keyboard.c_str(), 0700);
    ::chmod(draw.c_str(), 0700);
    assert(frameyap::find_companion("tnkplan") == plan);
    assert(frameyap::find_companion("tnkboard") == keyboard);
    assert(frameyap::find_companion("tnkdraw") == draw);
    ::setenv("PATH", (":" + path_dir.string()).c_str(), 1);
    assert(frameyap::find_companion("tnkboard") == keyboard);
    const auto marker = root / "launched";
    std::ofstream(plan) << "#!/bin/sh\nprintf launched > '" << marker.string() << "'\n";
    assert(frameyap::launch_companion(plan));
    for (int i = 0; i < 100 && !fs::exists(marker); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(fs::exists(marker));
    const auto argv_marker = root / "argv";
    std::ofstream(keyboard) << "#!/bin/sh\nprintf '%s\\n' \"$#\" \"$1\" > '" << argv_marker.string() << "'\n";
    const auto expect_argv = [&](frameyap::CompanionCommand command, const std::string& expected) {
        fs::remove(argv_marker);
        assert(frameyap::launch_companion(keyboard, command));
        std::string count, argument;
        for (int i = 0; i < 100; ++i) {
            std::ifstream in(argv_marker);
            if (std::getline(in, count) && std::getline(in, argument)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(count == (expected.empty() ? "0" : "1"));
        assert(argument == expected);
    };
    expect_argv(frameyap::CompanionCommand::Default, "");
    expect_argv(frameyap::CompanionCommand::Show, "--show");
    expect_argv(frameyap::CompanionCommand::Recenter, "--recenter");
    fs::remove(plan);
    assert(!frameyap::launch_companion(plan));
    fs::remove_all(root);
}
