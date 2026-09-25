#include "cli.hpp"
#include "check.hpp"
#include "model_status.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#ifdef FRAMEYAP_NATIVE
#include "runtime.hpp"
#include "instance_lock.hpp"
#include "overlay.hpp"
#endif

namespace {
void help() {
    std::cout << "FrameYap — standalone Steam Frame voice typing\n"
                 "Usage: frameyap [--help | --version | --list-models | --check-model ID]\n"
                 "  --list-models [--model-dir STORE] [--manifest-dir DIR] [--json]\n"
                 "  --check-model ID --model-dir DIR [--manifest-dir DIR] [--json]\n"
                 "  --run --assets DIR [--font FILE] [--backend ID] [--manifest-dir DIR]\n"
                 "        [--model-store DIR] [--model DIR] [--python FILE] [--threads 2|4]\n"
                 "        [--worker FILE] [--socket gamescope-0] [--mount MODE]\n"
                 "  --register MANIFEST [--autostart] | --unregister MANIFEST\n"
                 "  --check-input [--socket gamescope-0] (discovery only, no typing)\n"
                 "  --check-overlay --assets DIR [--font FILE] [--mount MODE] (5s, no mic/input)\n"
                 "  --check-controls --assets DIR [--font FILE] [--mount MODE] (30s, gestures only)\n\n"
                 "Model status is offline: --check-model hashes pinned local files and never loads inference.\n"
                 "Run without --worker/--model to use the manifest-managed Models UI; install requires consent.\n"
                 "Custom --worker scripts still require --model; --manifest-dir and --model-store\n"
                 "run overrides must be absolute local paths. No model or runtime downloads on startup.\n"
                 "Mount: world (first-launch default), left-wrist, right-wrist, head.\n"
                 "Settings save the mount, opt-in Lasers anytime mode and Auto insert (off by default).\n"
                 "--mount overrides placement for this launch. --head is an alias.\n"
                 "Theme, font and Frame button mappings: $XDG_CONFIG_HOME/frameyap/config.json\n"
                 "(or ~/.config/frameyap/config.json). CLI --font overrides config; missing fonts\n"
                 "fall back to bundled Inconsolata, then a system DejaVu face.\n"
                 "Config input_priority: normal (default) or experimental; also requires\n"
                 "SteamVR Developer setting Enable global input from overlays.\n"
                 "Right X: hold to speak, release to review (default Frame binding).\n"
                 "Grip gestures are remappable but may be unavailable in the dashboard.\n"
                 "Right B: cancel; A: Type (text + space, or Enter alone when no review); Y: Quick phrases.\n"
                 "Left grip: double-tap to Type + Enter (Enter alone when no review).\n"
                 "Review by default. Auto insert requires uninterrupted verified Xwayland focus.\n"
                 "Type approves current focus; Enter is never automatic.\n"
                 "No device access unless an explicit runtime/check/registration mode is used.\n";
#ifndef FRAMEYAP_NATIVE
    std::cout << "This offline build has no hardware backends; enable FRAMEYAP_NATIVE to run.\n";
#endif
}
#ifdef FRAMEYAP_NATIVE
int native_mode(const frameyap::CliOptions& cli) {
    using frameyap::CliMode;
    if (cli.mode == CliMode::Register || cli.mode == CliMode::Unregister) {
        frameyap::InstanceLock lock;
        return frameyap::registration(cli.manifest, cli.mode == CliMode::Unregister, cli.autostart);
    }
    frameyap::Options options;
    const char* socket = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    options.socket = !cli.socket.empty() ? cli.socket : socket && *socket ? socket : "gamescope-0";
    options.assets = cli.assets;
    options.font = cli.font;
    if (!cli.mount.empty()) {
        options.mount = frameyap::parse_mount(cli.mount);
        if (!options.mount) throw std::runtime_error("Mount must be world, left-wrist, right-wrist or head");
    }
    if (cli.head) options.mount = frameyap::Mount::Head;
    if (cli.mode == CliMode::CheckInput) return frameyap::check_input(options.socket);
    if (options.assets.empty()) throw frameyap::CliError("--assets required");
    if (cli.mode == CliMode::CheckOverlay || cli.mode == CliMode::CheckControls)
        return frameyap::check_overlay(options.assets, options.font, options.mount,
                                       cli.mode == CliMode::CheckControls);
    options.worker = cli.worker;
    options.model = cli.model;
    options.backend = cli.backend;
    options.manifest_dir = cli.manifest_dir;
    options.model_store = cli.model_store;
    if (!cli.python.empty()) options.python = cli.python;
    if (!cli.threads.empty()) options.threads = std::stoi(cli.threads);
    return frameyap::run(options);
}
#endif
} // namespace

int main(int argc, char** argv) {
    frameyap::CliOptions cli;
    try {
        cli = frameyap::parse_cli(argc, const_cast<const char* const*>(argv));
    } catch (const frameyap::CliError& error) {
        std::cerr << "FrameYap: " << error.what() << '\n';
        return 2;
    }
    if (cli.mode == frameyap::CliMode::Help) { help(); return 0; }
    if (cli.mode == frameyap::CliMode::Version) {
        std::cout << "frameyap " << FRAMEYAP_VERSION << '\n';
        if (std::string(FRAMEYAP_GIT_INFO).size()) std::cout << FRAMEYAP_GIT_INFO << '\n';
        return 0;
    }
    try {
        if (cli.mode == frameyap::CliMode::ListModels || cli.mode == frameyap::CliMode::CheckModel)
            return frameyap::model_status(cli, argv[0]);
#ifdef FRAMEYAP_NATIVE
        return native_mode(cli);
#else
        std::cerr << "Unsupported arguments or runtime disabled in this build. Use --help.\n";
        return 2;
#endif
    } catch (const frameyap::CliError& error) {
        std::cerr << "FrameYap: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "FrameYap: " << error.what() << '\n';
        return 1;
    }
}
