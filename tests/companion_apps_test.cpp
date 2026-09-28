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
    assert(!frameyap::find_companion("other"));
    const auto plan = local / "tnkplan", keyboard = path_dir / "tnkboard";
    std::ofstream(plan) << "#!/bin/sh\nexit 0\n";
    std::ofstream(keyboard) << "#!/bin/sh\nexit 0\n";
    assert(!frameyap::find_companion("tnkplan")); // a file is not yet executable
    ::chmod(plan.c_str(), 0700);
    ::chmod(keyboard.c_str(), 0700);
    assert(frameyap::find_companion("tnkplan") == plan);
    assert(frameyap::find_companion("tnkboard") == keyboard);
    ::setenv("PATH", (":" + path_dir.string()).c_str(), 1);
    assert(frameyap::find_companion("tnkboard") == keyboard);
    const auto marker = root / "launched";
    std::ofstream(plan) << "#!/bin/sh\nprintf launched > '" << marker.string() << "'\n";
    assert(frameyap::launch_companion(plan));
    for (int i = 0; i < 100 && !fs::exists(marker); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(fs::exists(marker));
    fs::remove(plan);
    assert(!frameyap::launch_companion(plan));
    fs::remove_all(root);
}
