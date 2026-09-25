#pragma once
#include "mount.hpp"
#include <optional>
#include <string>
namespace frameyap {
struct Options {
    std::string assets, font, python = "python3", worker, model, socket;
    std::string backend, manifest_dir, model_store; // optional overrides; empty loads saved choice / XDG store
    int threads = 2;
    std::optional<Mount> mount; // saved preference, or world on first launch
};
int run(const Options& options);
}
