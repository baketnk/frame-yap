#include "model_status.hpp"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace frameyap {
int model_status(const CliOptions& options, const char* executable) {
#if defined(__unix__) || defined(__APPLE__)
    namespace fs = std::filesystem;
    // Installed layouts put scripts/, python/ and assets/ beside bin/.
    // Resolve the actual binary rather than cwd or an untrusted PATH script.
    fs::path binary;
#if defined(__linux__)
    std::error_code error;
    binary = fs::read_symlink("/proc/self/exe", error);
#endif
    if (binary.empty()) binary = fs::absolute(executable);
    fs::path script = binary.parent_path().parent_path() / "scripts/model-status.py";
    // Out-of-tree builds can be nested arbitrarily; only the actual configured
    // build executable may fall back to the source script. Installed binaries
    // must carry their own scripts, not silently use a developer checkout.
    if (!fs::is_regular_file(script) && binary.parent_path() == fs::path(FRAMEYAP_BUILD_ROOT))
        script = fs::path(FRAMEYAP_SOURCE_ROOT) / "scripts/model-status.py";
    if (!fs::is_regular_file(script))
        throw std::runtime_error("model-status.py missing beside application layout: " + script.string());
    std::vector<std::string> args{"python3", script.string()};
    if (options.mode == CliMode::ListModels) args.emplace_back("--list-models");
    else { args.emplace_back("--check-model"); args.push_back(options.model_id); }
    if (!options.model_dir.empty()) { args.emplace_back("--model-dir"); args.push_back(options.model_dir); }
    if (!options.manifest_dir.empty()) { args.emplace_back("--manifest-dir"); args.push_back(options.manifest_dir); }
    if (options.json) args.emplace_back("--json");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("could not start offline model verifier");
    if (child == 0) {
        execvp(argv[0], argv.data());
        constexpr char missing[] = "FrameYap: python3 required for offline model status\n";
        (void)write(STDERR_FILENO, missing, sizeof(missing) - 1);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("could not wait for model verifier");
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#else
    (void)options; (void)executable;
    throw std::runtime_error("model status requires a POSIX host and Python 3.10+");
#endif
}
} // namespace frameyap
