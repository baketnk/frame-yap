#include "cli.hpp"
#include <array>
#include <filesystem>
#include <string_view>

namespace frameyap {
namespace {
using Mode = CliMode;
constexpr unsigned bit(Mode mode) { return 1u << static_cast<unsigned>(mode); }
constexpr unsigned run = bit(Mode::Run), input = bit(Mode::CheckInput);
constexpr unsigned visual = bit(Mode::CheckOverlay) | bit(Mode::CheckControls);
constexpr unsigned models = bit(Mode::ListModels) | bit(Mode::CheckModel);
struct ModeSpec { std::string_view name; Mode mode; bool positional; };
constexpr std::array modes{
    ModeSpec{"--help", Mode::Help, false}, ModeSpec{"--version", Mode::Version, false},
    ModeSpec{"--run", Mode::Run, false}, ModeSpec{"--register", Mode::Register, true},
    ModeSpec{"--unregister", Mode::Unregister, true},
    ModeSpec{"--check-input", Mode::CheckInput, false},
    ModeSpec{"--check-overlay", Mode::CheckOverlay, false},
    ModeSpec{"--check-controls", Mode::CheckControls, false},
    ModeSpec{"--list-models", Mode::ListModels, false},
    ModeSpec{"--check-model", Mode::CheckModel, true},
};
struct OptionSpec { std::string_view name; unsigned allowed; std::string CliOptions::* field; bool flag; };
constexpr std::array options{
    OptionSpec{"--assets", run | visual, &CliOptions::assets, false},
    OptionSpec{"--font", run | visual, &CliOptions::font, false},
    OptionSpec{"--socket", run | input | bit(Mode::CheckControls), &CliOptions::socket, false},
    OptionSpec{"--mount", run | visual, &CliOptions::mount, false},
    OptionSpec{"--worker", run, &CliOptions::worker, false},
    OptionSpec{"--python", run, &CliOptions::python, false},
    OptionSpec{"--model", run, &CliOptions::model, false},
    OptionSpec{"--threads", run, &CliOptions::threads, false},
    OptionSpec{"--model-dir", models, &CliOptions::model_dir, false},
    OptionSpec{"--backend", run, &CliOptions::backend, false},
    OptionSpec{"--model-store", run, &CliOptions::model_store, false},
    OptionSpec{"--manifest-dir", models | run, &CliOptions::manifest_dir, false},
    OptionSpec{"--head", run | visual, nullptr, true},
    OptionSpec{"--autostart", bit(Mode::Register), nullptr, true},
    OptionSpec{"--json", models, nullptr, true},
};
// Matches python/frameyap/model_files.py's manifest ID grammar. Reject invalid
// IDs before runtime setup; no basename or option-looking ID can be selected.
bool valid_backend(std::string_view id) {
    if (id.empty() || id.size() > 48 || id.front() < 'a' || id.front() > 'z') return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool safe_absolute_path(const std::string& value) {
    const std::filesystem::path path(value);
    if (!path.is_absolute()) return false;
    for (const auto& part : path) if (part == "." || part == "..") return false;
    for (unsigned char c : value) if (c < 32 || c == 127) return false;
    return true;
}
} // namespace
std::filesystem::path asset_root(const std::string& assets) {
    if (assets.empty()) return {};
    auto path = std::filesystem::absolute(std::filesystem::path(assets)).lexically_normal();
    // path("assets/") has an empty final component on some standard libraries.
    while (path.filename().empty() && path.has_parent_path() && path != path.root_path())
        path = path.parent_path();
    return path.parent_path();
}
bool is_managed_worker(const std::string& assets, const std::string& worker) {
    if (assets.empty() || worker.empty()) return false;
    const auto expected = (asset_root(assets) / "python/frameyap/worker.py").lexically_normal();
    return std::filesystem::absolute(std::filesystem::path(worker)).lexically_normal() == expected;
}
CliOptions parse_cli(int argc, const char* const argv[]) {
    CliOptions result;
    if (argc <= 1) return result;
    const std::string_view first(argv[1]);
    const ModeSpec* spec = nullptr;
    for (const auto& candidate : modes) if (first == candidate.name) { spec = &candidate; break; }
    if (!spec) throw CliError("Unsupported arguments. Use --help.");
    result.mode = spec->mode;
    int i = 2;
    if (spec->positional) {
        if (i == argc || !argv[i][0] || argv[i][0] == '-')
            throw CliError("Missing value for " + std::string(first));
        if (result.mode == Mode::CheckModel) result.model_id = argv[i++];
        else result.manifest = argv[i++];
    }
    for (; i < argc; ++i) {
        std::string_view arg(argv[i]);
        const OptionSpec* option = nullptr;
        for (const auto& candidate : options) if (arg == candidate.name) { option = &candidate; break; }
        if (!option || !(option->allowed & bit(result.mode)))
            throw CliError("Unsupported arguments: " + std::string(arg));
        if (option->flag) {
            bool* target = arg == "--head" ? &result.head :
                           arg == "--json" ? &result.json : &result.autostart;
            if (*target) throw CliError("Duplicate option: " + std::string(arg));
            *target = true;
        } else {
            if (i + 1 == argc || !argv[i + 1][0] || argv[i + 1][0] == '-')
                throw CliError("Missing value for " + std::string(arg));
            auto& value = result.*(option->field);
            if (!value.empty()) throw CliError("Duplicate option: " + std::string(arg));
            value = argv[++i];
        }
    }
    if (result.head && !result.mount.empty()) throw CliError("--head and --mount cannot be combined");
    if (result.mode == Mode::Run && result.threads != "" && result.threads != "2" && result.threads != "4")
        throw CliError("--threads must be 2 or 4");
    if (result.mode == Mode::CheckModel && result.model_dir.empty())
        throw CliError("--check-model requires --model-dir");
    if (result.mode == Mode::CheckModel && !valid_backend(result.model_id))
        throw CliError("--check-model requires a manifest ID ([a-z][a-z0-9_-]{0,47})");
    if (result.mode == Mode::Run) {
        if (!result.backend.empty() && !valid_backend(result.backend))
            throw CliError("--backend must be a manifest ID ([a-z][a-z0-9_-]{0,47})");
        if (!result.manifest_dir.empty() && !safe_absolute_path(result.manifest_dir))
            throw CliError("--manifest-dir must be an absolute local directory without dot segments or controls");
        if (!result.model_store.empty() && !safe_absolute_path(result.model_store))
            throw CliError("--model-store must be an absolute local directory without dot segments or controls");
        // Only the worker at the app root inferred from --assets belongs to
        // the managed dispatcher. A namesake script elsewhere is custom.
        if (!result.worker.empty() && !is_managed_worker(result.assets, result.worker)) {
            if (!result.backend.empty()) throw CliError("--backend cannot be combined with a custom --worker");
            if (result.model.empty()) throw CliError("--model required with a custom --worker");
        }
    }
    return result;
}
} // namespace frameyap
