#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>

namespace frameyap {
enum class CliMode { Help, Version, Run, Register, Unregister, CheckInput, CheckOverlay,
                     CheckControls, ListModels, CheckModel };
struct CliOptions {
    CliMode mode = CliMode::Help;
    std::string manifest, assets, font, mount, socket, worker, python, model, threads;
    std::string model_id, model_dir, manifest_dir, model_store, backend;
    bool head = false, autostart = false, json = false;
};
struct CliError : std::runtime_error { using std::runtime_error::runtime_error; };
// Lexical installation identity shared by CLI and runtime. No filesystem access.
std::filesystem::path asset_root(const std::string& assets);
bool is_managed_worker(const std::string& assets, const std::string& worker);
// Parse syntax only: never initializes hardware, accesses model files or opens sockets.
CliOptions parse_cli(int argc, const char* const argv[]);
} // namespace frameyap
