#include "overlay.hpp"
#include "gestures.hpp"
#include "panel_surface.hpp"

#include <openvr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace frameyap {
namespace {
// The ARM64 SDK defaults to /data/work on Frame. Respect explicit overrides,
// otherwise use the user's standard OpenVR registry when it exists.
void configure_registry() {
    if (std::getenv("VR_PATHREG_OVERRIDE")) return;
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if ((!config || !*config) && !home) return;
    auto root = std::filesystem::path(config && *config ? config : std::string(home) + "/.config");
    auto registry = root / "openvr/openvrpaths.vrpath";
    if (root.is_absolute() && std::filesystem::is_regular_file(registry))
        ::setenv("VR_PATHREG_OVERRIDE", registry.c_str(), 0);
}
constexpr int W = PanelSurface::width, H = PanelSurface::height;
void overlay_check(vr::EVROverlayError err, vr::IVROverlay* api, const char* op) {
    if (err != vr::VROverlayError_None)
        throw std::runtime_error(std::string(op) + ": " + api->GetOverlayErrorNameFromEnum(err));
}
std::filesystem::path absolute_file(const std::filesystem::path& p) {
    auto result = std::filesystem::absolute(p);
    if (!std::filesystem::is_regular_file(result)) throw std::runtime_error("Missing file: " + result.string());
    return result;
}
} // namespace

struct Overlay::Impl {
    vr::IVRSystem* system = nullptr;
    vr::IVROverlay* overlay = nullptr;
    vr::IVRInput* input = nullptr;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    vr::VRActionSetHandle_t action_set = vr::k_ulInvalidActionSetHandle;
    std::array<vr::VRActionHandle_t, 6> actions{};
    std::filesystem::path settings_path;
    Mount mount;
    PanelSurface surface;
    Panel panel;
    bool save_failed = false;
    bool world_ready = false, placed = false, has_texture = false, shown = false;
    vr::HmdMatrix34_t world_transform{};
    std::optional<Mount> applied_mount;
    vr::TrackedDeviceIndex_t anchor = vr::k_unTrackedDeviceIndexInvalid;
    DoubleTap left;
    GripRecord right;
    std::array<NeutralEdge, 4> edges{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    bool grip_capture = false, ptt_capture = false;
    bool focus = true;
    bool persist_mount = true;
    vr::EVRInputError action_update_error = vr::VRInputError_None;
    unsigned pointer_downs = 0, pointer_ups = 0, pointer_actions = 0, pointer_resets = 0;
    std::string last_pointer_event = "none";

    Impl(const std::string& assets, const std::string& font, std::optional<Mount> requested, bool persist)
        : settings_path(default_mount_settings_path()),
          mount(requested ? *requested : load_mount(settings_path)),
          surface(font.empty() ? (std::filesystem::path(assets) / "fonts/Inconsolata-Regular.ttf").string() : font, mount),
          persist_mount(persist) {
        const auto action_path = absolute_file(std::filesystem::path(assets) / "actions.json");
        absolute_file(std::filesystem::path(assets) / "bindings_knuckles.json");
        try {
            configure_registry();
            vr::EVRInitError err = vr::VRInitError_None;
            system = vr::VR_Init(&err, vr::VRApplication_Overlay);
            if (err != vr::VRInitError_None || !system)
                throw std::runtime_error(std::string("OpenVR overlay init: ") + vr::VR_GetVRInitErrorAsEnglishDescription(err));
            overlay = vr::VROverlay();
            input = vr::VRInput();
            if (!overlay || !input) throw std::runtime_error("OpenVR overlay/input interface unavailable");
            // The manifest launches a shell launcher, then execs this binary.
            // Associate manually launched instances with our registered app key too.
            auto* applications = vr::VRApplications();
            if (applications && applications->IsApplicationInstalled("local.frameyap.overlay") &&
                applications->IdentifyApplication(static_cast<uint32_t>(::getpid()), "local.frameyap.overlay") != vr::VRApplicationError_None)
                throw std::runtime_error("Could not identify the registered FrameYap application");
            if (input->SetActionManifestPath(action_path.c_str()) != vr::VRInputError_None)
                throw std::runtime_error("Could not set OpenVR action manifest path");
            if (input->GetActionSetHandle("/actions/frameyap", &action_set) != vr::VRInputError_None)
                throw std::runtime_error("Could not find FrameYap action set");
            constexpr std::array<const char*, 6> names{{"left_grip", "right_grip", "ptt", "cancel", "insert", "enter"}};
            for (size_t i = 0; i < names.size(); ++i)
                if (input->GetActionHandle((std::string("/actions/frameyap/in/") + names[i]).c_str(), &actions[i]) != vr::VRInputError_None)
                    throw std::runtime_error(std::string("Could not find action ") + names[i]);
            overlay_check(overlay->CreateOverlay("local.frameyap.overlay.panel", "FrameYap", &handle), overlay, "CreateOverlay");
            overlay_check(overlay->SetOverlayWidthInMeters(handle, 0.85f), overlay, "SetOverlayWidthInMeters");
            overlay_check(overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse), overlay, "SetOverlayInputMethod");
            // Let dashboard lasers reach our controls without forcing global
            // laser-mouse mode over a running scene.
            overlay_check(overlay->SetOverlayFlag(handle, vr::VROverlayFlags_VisibleInDashboard, true), overlay, "VisibleInDashboard");
            vr::HmdVector2_t mouse_scale{{float(W), float(H)}};
            overlay_check(overlay->SetOverlayMouseScale(handle, &mouse_scale), overlay, "SetOverlayMouseScale");
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(), uint32_t(poses.size()));
            place();
            draw(Panel{"Disabled", "", "Record to start local worker", false, false});
        } catch (...) { cleanup(); throw; }
    }
    ~Impl() { cleanup(); }
    void cleanup() {
        if (overlay && handle != vr::k_ulOverlayHandleInvalid) {
            overlay->HideOverlay(handle);
            overlay->DestroyOverlay(handle);
            handle = vr::k_ulOverlayHandleInvalid;
        }
        if (system) { vr::VR_Shutdown(); system = nullptr; overlay = nullptr; input = nullptr; }
    }
    void visibility() {
        const bool wanted = placed && has_texture;
        if (wanted == shown) return;
        overlay_check(wanted ? overlay->ShowOverlay(handle) : overlay->HideOverlay(handle), overlay, "Overlay visibility");
        shown = wanted;
    }
    void place() {
        Mount effective = mount;
        vr::TrackedDeviceIndex_t target = vr::k_unTrackedDeviceIndex_Hmd;
        if (mount == Mount::LeftWrist || mount == Mount::RightWrist) {
            target = system->GetTrackedDeviceIndexForControllerRole(mount == Mount::LeftWrist
                ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
            if (target >= poses.size() || !poses[target].bPoseIsValid || !system->IsTrackedDeviceConnected(target))
                effective = Mount::World;
        }
        if (effective == Mount::World && applied_mount && *applied_mount != Mount::World)
            world_ready = false; // a fresh world fallback near the wearer, not an old room location
        std::string note = save_failed ? "Preference could not be saved; using it for this session." : "";
        if (effective != mount) note = save_failed ? "Wrist untracked; world fallback. Preference not saved." :
                                                     "Wrist not tracked - using world space until it returns.";
        if (effective == Mount::World && !world_ready) {
            const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
            Matrix34 pose{};
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) pose[r][c] = hmd.mDeviceToAbsoluteTracking.m[r][c];
            const auto world = hmd.bPoseIsValid && hmd.bDeviceIsConnected ? world_mount_pose(pose) : std::nullopt;
            if (!world) {
                placed = false;
                surface.set_placement_note("Waiting for headset tracking to place the menu.");
                visibility();
                return;
            }
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) world_transform.m[r][c] = (*world)[r][c];
            world_ready = true;
            applied_mount.reset();
        }
        surface.set_placement_note(note);
        if (applied_mount != effective || (effective != Mount::World && anchor != target)) {
            surface.reset_pointers(); // a release on a relocated surface cannot activate an old press
            if (effective == Mount::World) {
                overlay_check(overlay->SetOverlayTransformAbsolute(handle, vr::TrackingUniverseStanding, &world_transform), overlay,
                              "SetOverlayTransformAbsolute");
            } else {
                const auto pose = relative_mount_pose(effective);
                vr::HmdMatrix34_t transform{};
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) transform.m[r][c] = pose[r][c];
                overlay_check(overlay->SetOverlayTransformTrackedDeviceRelative(handle, target, &transform), overlay,
                              "SetOverlayTransformTrackedDeviceRelative");
            }
            overlay_check(overlay->SetOverlayWidthInMeters(handle, mount_width(effective)), overlay, "SetOverlayWidthInMeters");
            applied_mount = effective;
            anchor = target;
        }
        placed = true;
        visibility();
    }
    bool available(UiAction action) const { return surface.available(action); }
    void draw(const Panel& p) {
        panel = p;
        if (surface.render(p)) {
            // OpenVR's API takes void*, but does not modify the submitted RGBA bytes.
            overlay_check(overlay->SetOverlayRaw(handle, const_cast<unsigned char*>(surface.pixels().data()), W, H, 4), overlay, "SetOverlayRaw");
            has_texture = true;
        }
        visibility();
    }
    void reset_input(std::vector<UiAction>& result) {
        left.reset();
        const bool lost_grip = right.reset() == GripRecord::Change::Cancel || grip_capture;
        grip_capture = false;
        const bool lost_ptt = edges[0].reset() || ptt_capture;
        ptt_capture = false;
        if (lost_grip || lost_ptt) result.push_back(UiAction::Cancel);
        for (size_t i = 1; i < edges.size(); ++i) edges[i].reset();
        surface.reset_pointers();
    }
    // Bound actions are accepted only with a connected tracked source. A held input
    // following loss of activity must return to neutral before generating an edge.
    std::pair<bool, bool> digital(size_t index) {
        vr::InputDigitalActionData_t data{};
        if (input->GetDigitalActionData(actions[index], &data, sizeof(data), vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None ||
            !data.bActive || data.activeOrigin == vr::k_ulInvalidInputValueHandle) return {false, false};
        vr::InputOriginInfo_t origin{};
        if (input->GetOriginTrackedDeviceInfo(data.activeOrigin, &origin, sizeof(origin)) != vr::VRInputError_None ||
            origin.trackedDeviceIndex == vr::k_unTrackedDeviceIndexInvalid ||
            origin.trackedDeviceIndex >= poses.size() ||
            !system->IsTrackedDeviceConnected(origin.trackedDeviceIndex) ||
            !poses[origin.trackedDeviceIndex].bPoseIsValid) return {false, false};
        if (index < 2) {
            const auto role = index == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            if (system->GetTrackedDeviceIndexForControllerRole(role) != origin.trackedDeviceIndex) return {false, false};
        }
        return {true, data.bState};
    }
    std::vector<UiAction> poll() {
        std::vector<UiAction> result;
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(), uint32_t(poses.size()));
        place();
        vr::VREvent_t event{};
        while (system->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) result.push_back(UiAction::Quit);
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset || event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                world_ready = false; applied_mount.reset(); surface.reset_pointers();
            }
            if (event.eventType == vr::VREvent_TrackedDeviceDeactivated ||
                event.eventType == vr::VREvent_InputFocusCaptured) reset_input(result);
        }
        while (overlay->PollNextOverlayEvent(handle, &event, sizeof(event))) {
            switch (event.eventType) {
            case vr::VREvent_Quit: case vr::VREvent_OverlayClosed:
                result.push_back(UiAction::Quit); break;
            case vr::VREvent_OverlayHidden:
                focus = false; reset_input(result);
                if (panel.recording) result.push_back(UiAction::Cancel);
                break;
            case vr::VREvent_OverlayShown:
                focus = true; reset_input(result); break;
            case vr::VREvent_OverlayGamepadFocusLost:
                surface.reset_pointers(); ++pointer_resets;
                last_pointer_event = "gamepad focus lost"; break;
            case vr::VREvent_OverlayFocusChanged:
                // This global focus notification also fires when the dashboard
                // laser enters our overlay. Do not erase a press between down/up;
                // a release must still hit the same enabled control.
                last_pointer_event = "overlay focus changed"; break;
            case vr::VREvent_MouseMove:
                surface.pointer_move(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y);
                break;
            case vr::VREvent_MouseButtonDown:
                ++pointer_downs;
                last_pointer_event = "down button=" + std::to_string(event.data.mouse.button);
                if (event.data.mouse.button == vr::VRMouseButton_Left)
                    surface.pointer_down(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y);
                break;
            case vr::VREvent_MouseButtonUp:
                ++pointer_ups;
                last_pointer_event = "up button=" + std::to_string(event.data.mouse.button);
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    auto event_result = surface.pointer_up(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y);
                    if (event_result.action || event_result.mount || event_result.recenter) ++pointer_actions;
                    if (event_result.action) result.push_back(*event_result.action);
                    if (event_result.mount) {
                        mount = *event_result.mount;
                        save_failed = persist_mount && !save_mount(settings_path, mount);
                        world_ready = false;
                        applied_mount.reset();
                    }
                    if (event_result.recenter) { world_ready = false; applied_mount.reset(); }
                }
                break;
            default: break;
            }
        }
        place();
        vr::VRActiveActionSet_t set{};
        set.ulActionSet = action_set;
        set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        set.nPriority = 0; // normal priority; no experimental scene-input overrides
        action_update_error = input->UpdateActionState(&set, sizeof(set), 1);
        if (action_update_error != vr::VRInputError_None) {
            reset_input(result); return result;
        }
        auto [la, ld] = digital(0);
        auto [ra, rd] = digital(1);
        const auto now = DoubleTap::Clock::now();
        if (left.update(la && focus, ld, available(UiAction::Enter), now)) result.push_back(UiAction::Enter);
        switch (right.update(ra && focus, rd, now)) {
        case GripRecord::Change::Begin: grip_capture = true; result.push_back(UiAction::BeginRecord); break;
        case GripRecord::Change::End: grip_capture = false; result.push_back(UiAction::EndRecord); break;
        case GripRecord::Change::Cancel: grip_capture = false; result.push_back(UiAction::Cancel); break;
        case GripRecord::Change::None: break;
        }
        constexpr std::array<UiAction, 4> mapped{{UiAction::Record, UiAction::Cancel, UiAction::Insert, UiAction::Enter}};
        for (size_t i = 0; i < edges.size(); ++i) {
            auto [active, down] = digital(i + 2);
            if (i == 0 && (!active || !focus || !panel.enabled) && edges[i].reset()) {
                ptt_capture = false; result.push_back(UiAction::Cancel);
            }
            auto change = edges[i].update(active && focus && (panel.enabled || i == 1), down);
            if (i == 0) {
                if (change == NeutralEdge::Change::Down) {
                    ptt_capture = true; result.push_back(UiAction::BeginRecord);
                } else if (change == NeutralEdge::Change::Up) {
                    ptt_capture = false; result.push_back(UiAction::EndRecord);
                }
            } else if (change == NeutralEdge::Change::Down && available(mapped[i])) {
                result.push_back(mapped[i]);
            }
        }
        return result;
    }
};

Overlay::Overlay(const std::string& assets, const std::string& font, std::optional<Mount> mount, bool persist_mount)
    : impl_(std::make_unique<Impl>(assets, font, mount, persist_mount)) {}
Overlay::~Overlay() = default;
std::vector<UiAction> Overlay::poll() { return impl_->poll(); }
void Overlay::draw(const Panel& panel) { impl_->draw(panel); }
std::string Overlay::controls_status() {
    // Diagnostic only: distinguish SteamVR binding/activity from our stricter
    // pose/role gate. Raw legacy state is read-only and may be unavailable.
    std::string result = "SteamVR update=" + std::to_string(int(impl_->action_update_error));
    for (size_t i = 0; i < 2; ++i) {
        const auto role = i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
        const auto device = impl_->system->GetTrackedDeviceIndexForControllerRole(role);
        const bool tracked = device < impl_->poses.size() && impl_->poses[device].bPoseIsValid &&
                             impl_->system->IsTrackedDeviceConnected(device);
        vr::InputDigitalActionData_t data{};
        const auto error = impl_->input->GetDigitalActionData(impl_->actions[i], &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
        vr::InputOriginInfo_t origin{};
        const bool origin_ok = error == vr::VRInputError_None && data.activeOrigin != vr::k_ulInvalidInputValueHandle &&
            impl_->input->GetOriginTrackedDeviceInfo(data.activeOrigin, &origin, sizeof(origin)) == vr::VRInputError_None &&
            origin.trackedDeviceIndex == device;
        vr::VRControllerState_t raw{};
        const bool raw_ok = device < impl_->poses.size() && impl_->system->GetControllerState(device, &raw, sizeof(raw));
        result += std::string(i == 0 ? "\nLeft:" : "\nRight:") +
            " err=" + std::to_string(int(error)) + " active=" + (data.bActive ? "Y" : "N") +
            " down=" + (data.bState ? "Y" : "N") + " pose=" + (tracked ? "Y" : "N") +
            " origin=" + (origin_ok ? "Y" : "N") +
            " raw=" + (raw_ok ? ((raw.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) ? "down" : "up") : "n/a");
    }
    // The default Frame binding maps right X to the named hold-to-talk action.
    // Report the action's own activity separately from the stricter pose gate;
    // remapped bindings may intentionally have a different origin.
    vr::InputDigitalActionData_t ptt{};
    const auto ptt_error = impl_->input->GetDigitalActionData(impl_->actions[2], &ptt, sizeof(ptt), vr::k_ulInvalidInputValueHandle);
    const auto [accepted, down] = impl_->digital(2);
    result += "\nPTT (default right X): err=" + std::to_string(int(ptt_error)) +
              " active=" + (ptt.bActive ? "Y" : "N") + " down=" + (ptt.bState ? "Y" : "N") +
              " accepted=" + (accepted ? "Y" : "N") + " gated-down=" + (down ? "Y" : "N");
    return result;
}
std::string Overlay::pointer_status() const {
    return "Pointer down=" + std::to_string(impl_->pointer_downs) + " up=" + std::to_string(impl_->pointer_ups) +
           " hits=" + std::to_string(impl_->pointer_actions) + " resets=" + std::to_string(impl_->pointer_resets) +
           " last=" + impl_->last_pointer_event;
}

int registration(const std::string& manifest, bool remove, bool autostart) {
    try {
        const auto path = absolute_file(manifest);
        // Utility initialization does not load hardware drivers. Call only for an
        // explicitly requested registration, never at static initialization.
        configure_registry();
        if (!vr::VR_IsRuntimeInstalled()) throw std::runtime_error("OpenVR runtime not installed");
        vr::EVRInitError err = vr::VRInitError_None;
        auto* system = vr::VR_Init(&err, vr::VRApplication_Utility);
        if (err != vr::VRInitError_None || !system)
            throw std::runtime_error(std::string("OpenVR utility init: ") + vr::VR_GetVRInitErrorAsEnglishDescription(err));
        struct Shutdown { ~Shutdown() { vr::VR_Shutdown(); } } shutdown;
        auto* apps = vr::VRApplications();
        if (!apps) throw std::runtime_error("OpenVR applications interface unavailable");
        constexpr const char* key = "local.frameyap.overlay";
        if (remove) {
            // Only remove the supplied manifest; never touch other applications.
            auto e = apps->RemoveApplicationManifest(path.c_str());
            if (e != vr::VRApplicationError_None) throw std::runtime_error(apps->GetApplicationsErrorNameFromEnum(e));
        } else {
            auto e = apps->AddApplicationManifest(path.c_str(), false);
            if (e != vr::VRApplicationError_None) throw std::runtime_error(apps->GetApplicationsErrorNameFromEnum(e));
            if (autostart) {
                e = apps->SetApplicationAutoLaunch(key, true);
                if (e != vr::VRApplicationError_None) throw std::runtime_error(apps->GetApplicationsErrorNameFromEnum(e));
            }
        }
        if (apps->IsApplicationInstalled(key) == remove)
            throw std::runtime_error("OpenVR registration state did not match the requested operation");
        std::cout << (remove ? "Unregistered " : "Registered ") << key;
        if (!remove) std::cout << " (autolaunch=" << (apps->GetApplicationAutoLaunch(key) ? "on" : "off") << ")";
        std::cout << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FrameYap registration: " << e.what() << '\n';
        return 1;
    }
}
} // namespace frameyap
