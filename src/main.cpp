#include <iostream>
#include <string_view>
#include <string>
#include <cstdlib>
#ifdef FRAMEYAP_NATIVE
#include "runtime.hpp"
#include "overlay.hpp"
#include "text_input.hpp"
#include "instance_lock.hpp"
#include <openvr.h>
#include <chrono>
#include <thread>
#endif
namespace {
void help() {
    std::cout << "FrameYap — standalone Steam Frame voice typing POC\n"
                 "Usage: frameyap [--help | --version]\n"
                 "  --run --assets DIR [--font FILE] --worker FILE --model DIR\n"
                 "        [--python FILE] [--threads 2|4] [--socket gamescope-0] [--mount MODE]\n"
                 "  --register MANIFEST [--autostart] | --unregister MANIFEST\n"
                 "  --check-input [--socket gamescope-0] (discovery only, no typing)\n"
                 "  --check-overlay --assets DIR [--font FILE] [--mount MODE] (5s, no mic/input)\n"
                 "  --check-controls --assets DIR [--font FILE] [--mount MODE] (30s, gestures only)\n\n"
                 "Mount: world (first-launch default), left-wrist, right-wrist, head.\n"
                 "Settings save the mount; --mount overrides it for this launch. --head is an alias.\n"
                 "Theme, font and Frame button mappings: $XDG_CONFIG_HOME/frameyap/config.json\n"
                 "(or ~/.config/frameyap/config.json). CLI --font overrides config; missing fonts\n"
                 "fall back to bundled Inconsolata, then a system DejaVu face.\n"
                 "Right X: hold to speak, release to review (default Frame binding).\n"
                 "Grip gestures are remappable but may be unavailable in the dashboard.\n"
                 "Left grip: double-tap for explicit Enter when active.\n"
                 "Review first: Insert approves current focus; never automatic Enter.\n"
                 "No device access unless an explicit runtime/check/registration mode is used.\n";
#ifndef FRAMEYAP_NATIVE
    std::cout << "This offline build has no hardware backends; enable FRAMEYAP_NATIVE to run.\n";
#endif
}
}
int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) { help(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "frameyap " << FRAMEYAP_VERSION << "\n"; return 0;
    }
#ifdef FRAMEYAP_NATIVE
    try {
        const std::string mode = argv[1];
        if (mode == "--register" || mode == "--unregister") {
            if (argc == 3 || (mode == "--register" && argc == 4 && std::string_view(argv[3]) == "--autostart")) {
                frameyap::InstanceLock lock;
                return frameyap::registration(argv[2], mode == "--unregister", argc == 4);
            }
        } else if (mode == "--run" || mode == "--check-input" || mode == "--check-overlay" || mode == "--check-controls") {
            frameyap::Options options;
            const char* socket = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
            options.socket = socket && *socket ? socket : "gamescope-0";
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                if (arg == "--head" && mode != "--check-input") { options.mount = frameyap::Mount::Head; continue; }
                if (i + 1 == argc) throw std::runtime_error("Missing option value");
                std::string value = argv[++i];
                if (arg == "--socket" && mode != "--check-overlay") options.socket = value;
                else if (arg == "--assets" && mode != "--check-input") options.assets = value;
                else if (arg == "--font" && mode != "--check-input") options.font = value;
                else if (arg == "--mount" && mode != "--check-input") {
                    options.mount = frameyap::parse_mount(value);
                    if (!options.mount) throw std::runtime_error("Mount must be world, left-wrist, right-wrist or head");
                }
                else if (arg == "--worker" && mode == "--run") options.worker = value;
                else if (arg == "--python" && mode == "--run") options.python = value;
                else if (arg == "--model" && mode == "--run") options.model = value;
                else if (arg == "--threads" && mode == "--run" && (value == "2" || value == "4")) options.threads = value == "2" ? 2 : 4;
                else throw std::runtime_error("Unsupported option/value");
            }
            if (mode == "--check-input") {
                frameyap::InstanceLock lock;
                frameyap::TextInput input(options.socket);
                std::cout << "Gamescope IME v2 ready; no input delivered.\n"; return 0;
            }
            if (options.assets.empty()) throw std::runtime_error("--assets required");
            if (mode == "--check-overlay" || mode == "--check-controls") {
                frameyap::InstanceLock lock;
                frameyap::Overlay overlay(options.assets, options.font, options.mount, false);
                const frameyap::Panel check_panel = mode == "--check-controls"
                    ? frameyap::Panel{"Controls check - watch terminal for events", "No audio captured. No text or Enter delivered.",
                                      "Click Record, Cancel or tabs. Actions are diagnostic only.", true, false}
                    : frameyap::Panel{"FrameYap five-second visual check", "No audio captured. No text or Enter delivered.",
                                      "Controls inactive during this check.", false, false};
                overlay.draw(check_panel);
                for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                    if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
                    char type[256]{}, profile[512]{};
                    vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof(type));
                    vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_InputProfilePath_String, profile, sizeof(profile));
                    std::cout << "Controller type=" << type << " profile=" << profile << '\n';
                }
                if (mode == "--check-controls") {
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    std::string status;
                    std::string pointer_status;
                    bool quit_check = false;
                    auto action_name = [](frameyap::UiAction action) {
                        switch (action) {
                        case frameyap::UiAction::BeginRecord: return "BeginRecord";
                        case frameyap::UiAction::EndRecord: return "EndRecord";
                        case frameyap::UiAction::Record: return "Record";
                        case frameyap::UiAction::Cancel: return "Cancel";
                        case frameyap::UiAction::Insert: return "Insert";
                        case frameyap::UiAction::Enter: return "Enter";
                        case frameyap::UiAction::Quit: return "Quit";
                        case frameyap::UiAction::Toggle: return "Toggle";
                        }
                        return "Unknown";
                    };
                    while (!quit_check && std::chrono::steady_clock::now() < end) {
                        for (auto action : overlay.poll()) {
                            if (action == frameyap::UiAction::Quit) { quit_check = true; break; }
                            std::cout << action_name(action) << " received; diagnostic only." << std::endl;
                        }
                        auto next = overlay.controls_status();
                        if (next != status) { status = next; std::cout << status << std::endl; }
                        auto pointer = overlay.pointer_status();
                        if (pointer != pointer_status) { pointer_status = pointer; std::cout << pointer_status << std::endl; }
                        // Keep the canvas fixed for action clicks: otherwise the
                        // changing counters cause SetOverlayRaw on every down/up.
                        // Tab/placement changes still redraw the correct controls.
                        overlay.draw(check_panel);
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    std::cout << "Final " << overlay.pointer_status() << std::endl;
                } else {
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (std::chrono::steady_clock::now() < end) {
                        for (auto action : overlay.poll()) if (action == frameyap::UiAction::Quit) return 0;
                        overlay.draw(check_panel);
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                std::cout << "Overlay APIs accepted panel; physical visibility requires human confirmation.\n"; return 0;
            }
            if (options.worker.empty() || options.model.empty()) throw std::runtime_error("--worker and --model required");
            return frameyap::run(options);
        }
    } catch (const std::exception& e) { std::cerr << "FrameYap: " << e.what() << '\n'; return 1; }
#endif
    std::cerr << "Unsupported arguments or runtime disabled in this build. Use --help.\n";
    return 2;
}
