#include "check.hpp"
#ifdef FRAMEYAP_NATIVE
#include "instance_lock.hpp"
#include "overlay.hpp"
#include "text_input.hpp"
#include <openvr.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace frameyap {
namespace {
const char* action_name(UiAction action) {
    switch (action) {
    case UiAction::BeginRecord: return "BeginRecord";
    case UiAction::EndRecord: return "EndRecord";
    case UiAction::Record: return "Record";
    case UiAction::Cancel: return "Cancel";
    case UiAction::Insert: return "Type";
    case UiAction::Enter: return "Type + Enter";
    case UiAction::QuickChat: return "Quick phrases";
    case UiAction::Quit: return "Quit";
    case UiAction::Toggle: return "Toggle";
    }
    return "Unknown";
}
} // namespace
int check_input(const std::string& socket) {
    InstanceLock lock;
    TextInput input(socket);
    std::cout << "Gamescope IME v2 ready; no input delivered.\n";
    return 0;
}
int check_overlay(const std::string& assets, const std::string& font,
                  std::optional<Mount> mount, bool controls) {
    InstanceLock lock;
    Overlay overlay(assets, font, mount, false);
    const Panel panel = controls
        ? Panel{"Controls check - watch terminal for events", "No audio captured. No text or Enter delivered.",
                "Click Record, Cancel or tabs. Actions are diagnostic only.", true, false}
        : Panel{"FrameYap five-second visual check", "No audio captured. No text or Enter delivered.",
                "Controls inactive during this check.", false, false};
    overlay.draw(panel);
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
        char type[256]{}, profile[512]{};
        vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof(type));
        vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_InputProfilePath_String, profile, sizeof(profile));
        std::cout << "Controller type=" << type << " profile=" << profile << '\n';
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(controls ? 30 : 5);
    std::string status, pointer_status;
    bool quit_check = false;
    while (!quit_check && std::chrono::steady_clock::now() < end) {
        for (auto action : overlay.poll()) {
            if (action == UiAction::Quit) { quit_check = true; break; }
            if (controls) std::cout << action_name(action) << " received; diagnostic only." << std::endl;
        }
        if (controls) {
            auto next = overlay.controls_status();
            if (next != status) { status = next; std::cout << status << std::endl; }
            auto pointer = overlay.pointer_status();
            if (pointer != pointer_status) { pointer_status = pointer; std::cout << pointer_status << std::endl; }
        }
        // Keep the canvas fixed for action clicks; placement changes still redraw.
        overlay.draw(panel);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (controls) std::cout << "Final " << overlay.pointer_status() << std::endl;
    std::cout << "Overlay APIs accepted panel; physical visibility requires human confirmation.\n";
    return 0;
}
} // namespace frameyap
#endif
