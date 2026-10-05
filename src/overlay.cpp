#include "overlay.hpp"
#include "overlay_texture.hpp"
#include "texture_refresh.hpp"
#include "angle_fade.hpp"
#include "gestures.hpp"
#include "laser_setting.hpp"
#include "placement.hpp"
#include "panel_surface.hpp"
#include "companion_apps.hpp"
#include "update_check.hpp"

#include <openvr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
constexpr int CW = PanelSurface::body.w, CH = PanelSurface::body.h;
Matrix34 matrix(const vr::HmdMatrix34_t& value) {
    Matrix34 result{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) result[r][c] = value.m[r][c];
    return result;
}
constexpr std::array<const char*, 7> action_names{{"left_grip", "right_grip", "ptt", "cancel", "insert", "enter", "quick_chat"}};
void overlay_check(vr::EVROverlayError err, vr::IVROverlay* api, const char* op) {
    if (err != vr::VROverlayError_None)
        throw std::runtime_error(std::string(op) + ": " + api->GetOverlayErrorNameFromEnum(err));
}
std::filesystem::path absolute_file(const std::filesystem::path& p) {
    auto result = std::filesystem::absolute(p);
    if (!std::filesystem::is_regular_file(result)) throw std::runtime_error("Missing file: " + result.string());
    return result;
}
template<class Query> std::vector<std::string> vulkan_extensions(Query query) {
    const auto size = query(nullptr, 0);
    if (!size) return {};
    if (size > 65536) throw std::runtime_error("OpenVR Vulkan extension list is too large");
    std::vector<char> buffer(size, '\0');
    const auto written = query(buffer.data(), size);
    if (!written || written > size || buffer[written - 1] != '\0')
        throw std::runtime_error("OpenVR Vulkan extension list changed or is invalid");
    std::istringstream words(std::string(buffer.data(), written - 1));
    std::vector<std::string> result;
    for (std::string name; words >> name;) result.push_back(std::move(name));
    return result;
}
std::string battery_text(const std::optional<BatteryLevel>& level) {
    if (!level) return "n/a";
    return std::to_string(level->percent) + "%" + (level->charging ? " charging" : "");
}
} // namespace

struct Overlay::Impl {
    vr::IVRSystem* system = nullptr;
    vr::IVROverlay* overlay = nullptr;
    vr::IVRInput* input = nullptr;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    std::unique_ptr<OverlayTexture> gpu_texture;
    vr::VRActionSetHandle_t action_set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionSetHandle_t grab_action_set = vr::k_ulInvalidActionSetHandle;
    std::array<vr::VRInputValueHandle_t, 2> hands{};
    std::array<vr::VRActionHandle_t, 7> actions{};
    std::array<vr::VRActionHandle_t, 2> depth_actions{};
    struct Persistence {
        std::filesystem::path settings_path, laser_settings_path;
        bool save_failed = false, debug_save_failed = false, auto_save_failed = false;
        bool layout_save_failed = false, mic_save_failed = false, gradient_save_failed = false, laser_change_failed = false;
        bool placement_save_failed = false;
        bool persist_mount = true;
    } persistence;
    Mount mount;
    bool lasers_anytime = false;
    Config config;
    PanelSurface surface;
    std::optional<std::filesystem::path> plan_launcher, keyboard_launcher, draw_launcher;
    UpdateCheck updates;
    std::filesystem::path install_update_script;
    std::string update_version;
    Panel panel;
    std::vector<ModelAction> model_actions;
    StatusIndicators indicators;
    std::chrono::steady_clock::time_point batteries_read{};
    bool world_ready = false, placed = false, has_texture = false, shown = false;
    float published_alpha = -1.f;
    float size_scale = 1.f;
    bool placement_dirty = false;
    Matrix34 canvas_pose{};
    std::array<std::optional<RelativePlacement>, 4> saved_placements{};
    struct DragState {
        Matrix34 canvas{};
        PanelDrag panel;
        PanelDragKind kind = PanelDragKind::Grab;
        unsigned cursor = 0;
        vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid;
        bool trigger_observed = false;
        float scale = 1.f;
        std::chrono::steady_clock::time_point started{}, last_depth_sample{};
    } dragging;
    vr::HmdMatrix34_t world_transform{};
    std::optional<Mount> applied_mount;
    vr::TrackedDeviceIndex_t anchor = vr::k_unTrackedDeviceIndexInvalid;
    DoubleTap left;
    GripRecord right;
    std::array<NeutralEdge, 5> edges{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    bool grip_capture = false, ptt_capture = false;
    bool focus = true;
    std::array<std::optional<std::pair<float, float>>, 2> cursor_positions{};
    struct Diagnostics {
        vr::EVRInputError action_update_error = vr::VRInputError_None;
        unsigned pointer_downs = 0, pointer_ups = 0, pointer_actions = 0, pointer_resets = 0;
        unsigned texture_uploads = 0, show_calls = 0, hide_calls = 0;
        unsigned overlay_shown_events = 0, overlay_hidden_events = 0, image_loaded_events = 0, image_failed_events = 0;
        unsigned overlay_focus_events = 0, global_focus_events = 0, input_focus_captured_events = 0;
        std::string last_pointer_event = "none";
    } diagnostics;
    struct Performance {
        bool enabled = [] { const char* flag = std::getenv("FRAMEYAP_PROFILE");
                            return flag && std::string_view(flag) == "1"; }();
        uint64_t renders = 0, uploads = 0, motion_ticks = 0, retained_frames = 0, motion_redraws = 0, motion_uploads = 0;
        uint64_t depth_samples = 0, depth_axis_active = 0, depth_scroll_events = 0, depth_scroll_accepted = 0;
        double max_depth = 0;
        double render_ms = 0, upload_ms = 0, motion_ms = 0, motion_max_ms = 0;
        static double elapsed(std::chrono::steady_clock::time_point start) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        void report() const {
            if (!enabled) return;
            std::cout << "FrameYap profile: renders=" << renders << " render_ms=" << render_ms
                      << " uploads=" << uploads << " upload_ms=" << upload_ms
                      << " motion_ticks=" << motion_ticks << " motion_ms=" << motion_ms
                      << " motion_max_ms=" << motion_max_ms << " retained_frames=" << retained_frames
                      << " motion_redraws=" << motion_redraws << " motion_uploads=" << motion_uploads
                      << " depth_samples=" << depth_samples << " depth_axis_active=" << depth_axis_active
                      << " depth_scroll_events=" << depth_scroll_events << " depth_scroll_accepted=" << depth_scroll_accepted
                      << " max_depth=" << max_depth << std::endl;
        }
    } performance;

    Impl(const std::string& assets, const std::string& font, std::optional<Mount> requested, bool persist)
        : persistence{default_mount_settings_path(), default_laser_settings_path()},
          mount(requested ? *requested : load_mount(persistence.settings_path)),
          lasers_anytime(load_lasers_anytime(persistence.laser_settings_path)),
          config(load_config(default_config_path())),
          surface(resolve_font(assets, font.empty() ? config.font : font), mount, config.theme, config.gradient),
          updates((std::filesystem::absolute(assets) / "../scripts/check-update.py").lexically_normal(), FRAMEYAP_VERSION),
          install_update_script((std::filesystem::absolute(assets) / "../scripts/install-update.py").lexically_normal()) {
        persistence.persist_mount = persist;
        plan_launcher = find_companion("tnkplan");
        keyboard_launcher = find_companion("tnkboard");
        draw_launcher = find_companion("tnkdraw");
        surface.set_companions(plan_launcher.has_value(), keyboard_launcher.has_value(), draw_launcher.has_value());
        for (auto selected : {Mount::LeftWrist, Mount::RightWrist, Mount::Head})
            saved_placements[static_cast<size_t>(selected)] =
                load_relative_placement(default_placement_path(selected), selected);
        const auto action_path = absolute_file(action_manifest(assets, config));
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
            auto* compositor = vr::VRCompositor();
            if (!compositor) throw std::runtime_error("OpenVR Vulkan compositor interface unavailable");
            const auto extensions = vulkan_extensions([&](char* out, uint32_t size) {
                return compositor->GetVulkanInstanceExtensionsRequired(out, size);
            });
            gpu_texture = std::make_unique<OverlayTexture>(W, H, extensions,
                [&](VkInstance instance) {
                    uint64_t physical = 0;
                    system->GetOutputDevice(&physical, vr::TextureType_Vulkan, instance);
                    return reinterpret_cast<VkPhysicalDevice>(physical);
                }, [&](VkPhysicalDevice physical) {
                    return vulkan_extensions([&](char* out, uint32_t size) {
                        return compositor->GetVulkanDeviceExtensionsRequired(physical, out, size);
                    });
                });
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
            if (input->GetActionSetHandle("/actions/grab", &grab_action_set) != vr::VRInputError_None)
                throw std::runtime_error("Could not find grab action set");
            input->GetInputSourceHandle("/user/hand/left", &hands[0]);
            input->GetInputSourceHandle("/user/hand/right", &hands[1]);
            for (size_t i = 0; i < action_names.size(); ++i)
                if (input->GetActionHandle((std::string("/actions/frameyap/in/") + action_names[i]).c_str(), &actions[i]) != vr::VRInputError_None)
                    throw std::runtime_error(std::string("Could not find action ") + action_names[i]);
            for (size_t i = 0; i < depth_actions.size(); ++i) {
                const char* name = i == 0 ? "left_depth" : "right_depth";
                if (input->GetActionHandle((std::string("/actions/grab/in/") + name).c_str(), &depth_actions[i]) != vr::VRInputError_None)
                    throw std::runtime_error(std::string("Could not find action ") + name);
            }
            overlay_check(overlay->CreateOverlay("local.frameyap.overlay.panel", "FrameYap", &handle), overlay, "CreateOverlay");
            overlay_check(overlay->SetOverlayWidthInMeters(handle, 0.85f), overlay, "SetOverlayWidthInMeters");
            overlay_check(overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse), overlay, "SetOverlayInputMethod");
            // This separate preference requests system-wide laser mouse mode only while the
            // panel is visible; it may affect interaction with a running game.
            if (lasers_anytime && overlay->SetOverlayFlag(handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true) != vr::VROverlayError_None) {
                lasers_anytime = false;
                persistence.laser_change_failed = true;
            }
            overlay_check(overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true), overlay, "Laser scroll");
            // Normal UI scrolling uses discrete events. Request the compositor
            // smooth stream only while grabbing for the depth fallback.
            overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, false);
            surface.set_lasers_anytime(lasers_anytime);
            surface.set_wrist_world_fallback(config.wrist_world_fallback);
            surface.set_advanced_debug(config.advanced_debug);
            surface.set_auto_insert(config.auto_insert);
            surface.set_gradient_enabled(config.gradient.enabled);
            surface.set_close_mic_when_idle(config.close_mic_when_idle);
            surface.set_layout_locked(config.lock_layout);
            surface.set_clock_24h(config.clock_24h);
            surface.set_date_format(config.date_format);
            surface.set_version(std::string(FRAMEYAP_VERSION) +
                                (std::string(FRAMEYAP_GIT_INFO).empty() ? "" : " " FRAMEYAP_GIT_INFO));
            overlay_check(overlay->SetOverlayFlag(handle, vr::VROverlayFlags_VisibleInDashboard, true), overlay, "VisibleInDashboard");
            vr::HmdVector2_t mouse_scale{{float(W), float(H)}};
            overlay_check(overlay->SetOverlayMouseScale(handle, &mouse_scale), overlay, "SetOverlayMouseScale");
            update_intersection_mask();
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(), uint32_t(poses.size()));
            place();
            draw(Panel{"Disabled", "", "Record to start local worker", false, false});
            std::cout << input_mode_status() << std::endl;
        } catch (...) { cleanup(); throw; }
    }
    ~Impl() { performance.report(); cleanup(); }
    void update_intersection_mask() {
        const auto regions = surface.input_regions();
        std::vector<vr::VROverlayIntersectionMaskPrimitive_t> mask(regions.size());
        for (size_t i = 0; i < mask.size(); ++i) {
            const auto b = regions[i];
            mask[i].m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Rectangle;
            // Mask rectangles are TOP-left pixel coordinates. Mouse events are
            // bottom-left GL coordinates; flipping this mask makes the outside
            // scale corner unclickable while the body happens to cover Grab.
            mask[i].m_Primitive.m_Rectangle = {float(b.x), float(b.y), float(b.w), float(b.h)};
        }
        overlay_check(overlay->SetOverlayIntersectionMask(handle, mask.data(), uint32_t(mask.size())), overlay, "SetOverlayIntersectionMask");
    }
    void cleanup() {
        if (overlay && handle != vr::k_ulOverlayHandleInvalid) {
            overlay->HideOverlay(handle);
            overlay->DestroyOverlay(handle);
            handle = vr::k_ulOverlayHandleInvalid;
        }
        if (system) { vr::VR_Shutdown(); system = nullptr; overlay = nullptr; input = nullptr; }
        // OpenVR retains client-side resources for submitted Vulkan images.
        // Its shutdown must finish before their device/instance are destroyed.
        gpu_texture.reset();
    }
    void visibility() {
        float alpha = 1.f;
        if (placed && applied_mount && (*applied_mount == Mount::LeftWrist || *applied_mount == Mount::RightWrist)) {
            alpha = tracked(anchor) && tracked(vr::k_unTrackedDeviceIndex_Hmd)
                ? wrist_view_opacity_relative(canvas_pose, matrix(poses[anchor].mDeviceToAbsoluteTracking),
                                              matrix(poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking))
                : 0.f;
        }
        // Keep geometry/texture stationary; opacity is a compositor property,
        // not an upload. At zero, hide the interactive overlay as well.
        alpha = std::round(alpha * 255.f) / 255.f;
        if (alpha != published_alpha) {
            overlay_check(overlay->SetOverlayAlpha(handle, alpha), overlay, "SetOverlayAlpha");
            published_alpha = alpha;
        }
        const bool wanted = placed && has_texture && alpha > 0.f;
        if (wanted == shown) return;
        overlay_check(wanted ? overlay->ShowOverlay(handle) : overlay->HideOverlay(handle), overlay, "Overlay visibility");
        if (wanted) ++diagnostics.show_calls; else ++diagnostics.hide_calls;
        shown = wanted;
    }
    void place() {
        vr::TrackedDeviceIndex_t target = vr::k_unTrackedDeviceIndex_Hmd;
        if (mount == Mount::LeftWrist || mount == Mount::RightWrist) {
            target = system->GetTrackedDeviceIndexForControllerRole(mount == Mount::LeftWrist
                ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        }
        const bool wrist_tracked = target < poses.size() && poses[target].bPoseIsValid &&
            system->IsTrackedDeviceConnected(target);
        const auto selected = effective_mount(mount, wrist_tracked, config.wrist_world_fallback);
        if (!selected) {
            if (placed) { surface.reset_pointers(); dragging.panel.reset(); }
            placed = false;
            applied_mount.reset();
            visibility();
            return;
        }
        const Mount effective = *selected;
        if (effective == Mount::World && applied_mount && *applied_mount != Mount::World)
            world_ready = false; // a fresh world fallback near the wearer, not an old room location
        std::string note = persistence.placement_save_failed ? "Placement not saved; using it only for this session." :
                           persistence.mic_save_failed ? "Mic preference not saved; using it only for this session." :
                           persistence.gradient_save_failed ? "Could not save background setting." :
                           persistence.layout_save_failed ? "Layout lock not saved; using it only for this session." :
                           persistence.auto_save_failed ? "Auto insert preference not saved; using it only for this session." :
                           persistence.debug_save_failed ? "Debug preference not saved; using it only for this session." :
                           persistence.laser_change_failed ? "SteamVR declined the laser mode change." :
                           persistence.save_failed ? "Preference could not be saved; using it for this session." : "";
        if (effective != mount) note = persistence.save_failed ? "Wrist untracked; world fallback. Preference not saved." :
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
        const bool relocated = applied_mount != effective || anchor != target;
        if (relocated || placement_dirty) {
            const float base_width = mount_width(effective, config.wrist);
            if (relocated) {
                surface.reset_pointers(); dragging.panel.reset();
                const auto& remembered = saved_placements[static_cast<size_t>(effective)];
                if (effective != Mount::World && remembered) {
                    // Saved canvas coordinates already include transparent margins.
                    size_scale = remembered->scale;
                    canvas_pose = remembered->canvas_pose;
                } else {
                    if (effective != Mount::World) size_scale = 1.f;
                    auto pose = effective == Mount::World ? matrix(world_transform) : relative_mount_pose(effective, config.wrist);
                    pose = resized_mount_pose(pose, base_width, size_scale, float(CH) / CW);
                    // Preserve main-panel dimensions when adding transparent margins.
                    const float meters_per_pixel = base_width * size_scale / CW;
                    const float dx = (W - CW) * .5f * meters_per_pixel;
                    const float dy = -(H - CH) * .5f * meters_per_pixel;
                    for (int r = 0; r < 3; ++r) pose[r][3] += pose[r][0] * dx + pose[r][1] * dy;
                    canvas_pose = pose;
                }
            }
            // After a grab, keep the full released pose. Never rebuild it from
            // a planar offset or configured orientation on the next poll.
            vr::HmdMatrix34_t transform{};
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) transform.m[r][c] = canvas_pose[r][c];
            if (effective == Mount::World)
                overlay_check(overlay->SetOverlayTransformAbsolute(handle, vr::TrackingUniverseStanding, &transform), overlay,
                              "SetOverlayTransformAbsolute");
            else
                overlay_check(overlay->SetOverlayTransformTrackedDeviceRelative(handle, target, &transform), overlay,
                              "SetOverlayTransformTrackedDeviceRelative");
            overlay_check(overlay->SetOverlayWidthInMeters(handle, base_width * size_scale * W / CW), overlay, "SetOverlayWidthInMeters");
            applied_mount = effective;
            anchor = target;
            placement_dirty = false;
        }
        placed = true;
        visibility();
    }
    bool tracked(vr::TrackedDeviceIndex_t device) const {
        return device < poses.size() && poses[device].bPoseIsValid && poses[device].bDeviceIsConnected;
    }
    std::optional<Matrix34> drag_source() const {
        if (!tracked(dragging.device) || !applied_mount) return {};
        auto pose = matrix(poses[dragging.device].mDeviceToAbsoluteTracking);
        if (*applied_mount != Mount::World) {
            if (!tracked(anchor)) return {};
            pose = relative_pose(matrix(poses[anchor].mDeviceToAbsoluteTracking), pose);
        }
        return pose;
    }
    void begin_drag(PanelDragKind kind, const vr::VREvent_t& event) {
        if (config.lock_layout) { surface.reset_pointers(); dragging.panel.reset(); return; }
        dragging.device = event.trackedDeviceIndex;
        dragging.cursor = event.data.mouse.cursorIndex;
        // Single-cursor overlay: some runtime mouse events omit the source.
        // The dashboard's primary device is the only supported fallback, never
        // a guessed left/right hand or a source chosen by proximity.
        if (dragging.cursor == 0 && (dragging.device == vr::k_unTrackedDeviceIndexInvalid ||
                                     dragging.device == vr::k_unTrackedDeviceIndex_Hmd))
            dragging.device = overlay->GetPrimaryDashboardDevice();
        dragging.kind = kind;
        const auto source = drag_source();
        const float w = applied_mount ? mount_width(*applied_mount, config.wrist) * size_scale * W / CW : 0.f;
        if (!source || system->GetTrackedDeviceClass(dragging.device) != vr::TrackedDeviceClass_Controller ||
            !dragging.panel.begin(kind, canvas_pose, w, w * H / W, event.data.mouse.x / W,
                                  1.f - event.data.mouse.y / H, *source)) {
            surface.reset_pointers(); dragging.panel.reset();
            diagnostics.last_pointer_event = "drag source unavailable";
            return;
        }
        dragging.scale = size_scale; dragging.canvas = canvas_pose;
        dragging.started = dragging.last_depth_sample = std::chrono::steady_clock::now();
        if (kind == PanelDragKind::Grab)
            overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
        vr::VRControllerState_t state{};
        dragging.trigger_observed = system->GetControllerState(dragging.device, &state, sizeof(state)) &&
            (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger));
        diagnostics.last_pointer_event = std::string(kind == PanelDragKind::Grab ? "grab" : "scale") +
            " device=" + std::to_string(dragging.device) + " trigger-watch=" + (dragging.trigger_observed ? "Y" : "N");
    }
    void finish_drag(bool released) {
        overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, false);
        if (!released && applied_mount && *applied_mount == mount) {
            canvas_pose = dragging.canvas;
            size_scale = dragging.scale;
            placement_dirty = true;
        }
        if (released && applied_mount && *applied_mount == mount && mount != Mount::World &&
            (size_scale != dragging.scale || canvas_pose != dragging.canvas)) {
            const RelativePlacement current{canvas_pose, size_scale};
            if (valid_relative_placement(current)) {
                saved_placements[static_cast<size_t>(mount)] = current;
                persistence.placement_save_failed = persistence.persist_mount &&
                    !save_relative_placement(default_placement_path(mount), mount, current);
            } else persistence.placement_save_failed = true;
        }
        surface.reset_pointers(); dragging.panel.reset();
    }
    std::optional<float> depth_axis() const {
        const auto left = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        const auto right = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        const size_t hand = dragging.device == left ? 0 : dragging.device == right ? 1 : depth_actions.size();
        if (hand == depth_actions.size()) return {};
        vr::InputAnalogActionData_t data{};
        if (input->GetAnalogActionData(depth_actions[hand], &data, sizeof(data), hands[hand]) != vr::VRInputError_None ||
            !data.bActive || data.activeOrigin == vr::k_ulInvalidInputValueHandle || !std::isfinite(data.y)) return {};
        vr::InputOriginInfo_t origin{};
        if (input->GetOriginTrackedDeviceInfo(data.activeOrigin, &origin, sizeof(origin)) != vr::VRInputError_None ||
            origin.trackedDeviceIndex != dragging.device) return {};
        return data.y;
    }
    void update_drag() {
        if (!dragging.panel.active()) return;
        const auto source = drag_source();
        vr::VRControllerState_t state{};
        const bool trigger_released = dragging.trigger_observed &&
            (!system->GetControllerState(dragging.device, &state, sizeof(state)) ||
             !(state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)));
        // Without a readable release watchdog, never keep manipulating after
        // the pointer leaves our hit region: an outside MouseUp is not assured.
        const bool lost_unwatched_pointer = !dragging.trigger_observed && !overlay->IsHoverTargetOverlay(handle);
        const bool timed_out = std::chrono::steady_clock::now() - dragging.started > std::chrono::seconds(15);
        if (!surface.dragging(dragging.cursor) || !shown || !focus || !source || trigger_released || lost_unwatched_pointer || timed_out) {
            finish_drag((!surface.dragging(dragging.cursor) || trigger_released) &&
                        shown && focus && bool(source) && !lost_unwatched_pointer && !timed_out);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - dragging.last_depth_sample).count();
        dragging.last_depth_sample = now;
        if (dragging.kind == PanelDragKind::Grab) {
            const auto axis = diagnostics.action_update_error == vr::VRInputError_None ? depth_axis() : std::nullopt;
            if (performance.enabled) {
                ++performance.depth_samples;
                performance.depth_axis_active += bool(axis);
            }
            dragging.panel.update_depth(axis, dt);
            if (performance.enabled)
                performance.max_depth = std::max(performance.max_depth, std::abs(dragging.panel.depth()));
        }
        const auto change = dragging.panel.update(*source);
        if (!change) { finish_drag(false); return; }
        const float scale = dragging.kind == PanelDragKind::Scale ? std::clamp(dragging.scale * change->factor, .5f, 2.f) : size_scale;
        const auto pose = dragging.kind == PanelDragKind::Grab ? change->pose :
            resized_mount_pose(dragging.canvas, mount_width(*applied_mount, config.wrist) * dragging.scale * W / CW,
                               scale / dragging.scale, float(H) / W);
        if (scale != size_scale || pose != canvas_pose) {
            size_scale = scale; canvas_pose = pose; placement_dirty = true;
        }
    }
    bool available(UiAction action) const { return surface.available(action); }
    std::optional<BatteryLevel> device_battery(vr::TrackedDeviceIndex_t index) const {
        if (index == vr::k_unTrackedDeviceIndexInvalid || !system->IsTrackedDeviceConnected(index)) return {};
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        if (!system->GetBoolTrackedDeviceProperty(index, vr::Prop_DeviceProvidesBatteryStatus_Bool, &error) ||
            error != vr::TrackedProp_Success) return {};
        const float level = system->GetFloatTrackedDeviceProperty(index, vr::Prop_DeviceBatteryPercentage_Float, &error);
        if (error != vr::TrackedProp_Success || !std::isfinite(level) || level < 0.f || level > 1.f) return {};
        const bool charging = system->GetBoolTrackedDeviceProperty(index, vr::Prop_DeviceIsCharging_Bool, &error);
        return BatteryLevel{int(std::lround(level * 100.f)), error == vr::TrackedProp_Success && charging};
    }
    void update_indicators() {
        indicators.dashboard_open = overlay->IsDashboardVisible();
        const auto now = std::chrono::steady_clock::now();
        if (now - batteries_read >= std::chrono::seconds(5) || batteries_read == decltype(now){}) {
            batteries_read = now;
            indicators.left = device_battery(system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand));
            indicators.right = device_battery(system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand));
            // A standalone headset may report its battery only through Linux.
            indicators.head = device_battery(vr::k_unTrackedDeviceIndex_Hmd);
            if (!indicators.head) indicators.head = system_battery();
        }
        surface.set_indicators(indicators);
    }
    void draw(const Panel& p) {
        panel = p;
        surface.set_clock_time(std::time(nullptr));
        update_indicators();
        // Placement updates reuse the initialized image. Defer cosmetic
        // rendering/uploads (including gradient ticks) until after the drag;
        // keep panel and indicator state live for the next render.
        const bool initialized_motion = dragging.panel.active() && has_texture;
        if (performance.enabled && initialized_motion) ++performance.retained_frames;
        refresh_texture(dragging.panel.active(), has_texture,
                        [&] {
            const auto start = performance.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const bool rendered = surface.render(p, PanelSurface::Clock::now(), shown);
            if (performance.enabled && rendered) { if (initialized_motion) ++performance.motion_redraws; ++performance.renders; performance.render_ms += Performance::elapsed(start); }
            return rendered;
        }, [&] {
            const auto start = performance.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            gpu_texture->upload(surface.pixels());
            auto texture = gpu_texture->texture();
            overlay_check(overlay->SetOverlayTexture(handle, &texture), overlay, "SetOverlayTexture (Vulkan)");
            ++diagnostics.texture_uploads;
            if (performance.enabled) { if (initialized_motion) ++performance.motion_uploads; ++performance.uploads; performance.upload_ms += Performance::elapsed(start); }
            has_texture = true;
        });
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
    int32_t action_priority() const {
        return config.experimental_input_priority ? vr::k_nActionSetOverlayGlobalPriorityMin : 0;
    }
    std::string input_mode_status() const {
        // Read the runtime permission separately: requesting a priority does not
        // enable SteamVR's global setting or prove delivery of controller input.
        vr::EVRSettingsError settings_error = vr::VRSettingsError_None;
        auto* settings = vr::VRSettings();
        const bool allowed = settings && settings->GetBool(vr::k_pch_SteamVR_Section,
            vr::k_pch_SteamVR_AllowGlobalActionSetPriority, &settings_error);
        const char* permission = !settings || settings_error != vr::VRSettingsError_None ? "unavailable" :
                                 allowed ? "enabled" : "disabled";
        bool laser_flag = false;
        const auto laser_error = overlay->GetOverlayFlag(handle,
            vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, &laser_flag);
        return std::string("Input priority-request=") + (config.experimental_input_priority ? "experimental" : "normal") +
            " (" + std::to_string(action_priority()) + ") SteamVR-global-input=" + permission +
            "\nMode dashboard=" + (overlay->IsDashboardVisible() ? "Y" : "N") +
            " lasers-anytime=" + (laser_error == vr::VROverlayError_None ? (laser_flag ? "Y" : "N") : "n/a") +
            " system-input-available=" + (system->IsInputAvailable() ? "Y" : "N") +
            " panel-shown=" + (shown ? "Y" : "N") + " focus-gate=" + (focus ? "Y" : "N") +
            "\nBattery L=" + battery_text(indicators.left) + " HMD=" + battery_text(indicators.head) +
            " R=" + battery_text(indicators.right);
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
        const auto open_companion = [&](SurfaceEvent::Companion selected) {
            const auto& launcher = selected == SurfaceEvent::Companion::Plan ? plan_launcher :
                selected == SurfaceEvent::Companion::Draw ? draw_launcher : keyboard_launcher;
            const auto command = selected == SurfaceEvent::Companion::KeyboardRecenter ? CompanionCommand::Recenter :
                selected == SurfaceEvent::Companion::Keyboard ? CompanionCommand::Show : CompanionCommand::Default;
            if (launcher && !launch_companion(*launcher, command))
                surface.set_binding_note("Could not open companion app.");
            else surface.set_binding_note("");
        };
        if (auto update = updates.poll()) {
            update_version = std::move(update->version);
            surface.set_update_status(update->state == UpdateResult::State::Available ? UpdateStatus::Available :
                update->state == UpdateResult::State::Current ? UpdateStatus::Current : UpdateStatus::Failed,
                update_version);
        }
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(), uint32_t(poses.size()));
        place();
        vr::VREvent_t event{};
        while (system->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) result.push_back(UiAction::Quit);
            if (event.eventType == vr::VREvent_OverlayFocusChanged) ++diagnostics.global_focus_events;
            if (event.eventType == vr::VREvent_InputFocusCaptured) ++diagnostics.input_focus_captured_events;
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
                ++diagnostics.overlay_hidden_events;
                focus = false; cursor_positions.fill(std::nullopt); reset_input(result);
                if (panel.recording) result.push_back(UiAction::Cancel);
                break;
            case vr::VREvent_OverlayShown:
                ++diagnostics.overlay_shown_events;
                focus = true; reset_input(result); break;
            case vr::VREvent_ImageLoaded: ++diagnostics.image_loaded_events; break;
            case vr::VREvent_ImageFailed: ++diagnostics.image_failed_events; break;
            case vr::VREvent_OverlayGamepadFocusLost:
                cursor_positions.fill(std::nullopt);
                surface.reset_pointers(); ++diagnostics.pointer_resets;
                diagnostics.last_pointer_event = "gamepad focus lost"; break;
            case vr::VREvent_OverlayFocusChanged:
                ++diagnostics.overlay_focus_events;
                // This global focus notification also fires when the dashboard
                // laser enters our overlay. Do not erase a press between down/up;
                // a release must still hit the same enabled control.
                diagnostics.last_pointer_event = "overlay focus changed"; break;
            case vr::VREvent_MouseMove:
                // Hover drives scrolling and hold cancellation. Manipulation uses
                // the captured controller ray, not changing overlay coordinates.
                if (event.data.mouse.cursorIndex < cursor_positions.size()) {
                    cursor_positions[event.data.mouse.cursorIndex] = {event.data.mouse.x, H - event.data.mouse.y};
                    surface.pointer_move(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y);
                }
                break;
            case vr::VREvent_ScrollDiscrete: case vr::VREvent_ScrollSmooth:
                if (dragging.panel.active()) {
                    if (event.eventType == vr::VREvent_ScrollSmooth && dragging.kind == PanelDragKind::Grab) {
                        const bool accepted = dragging.panel.queue_scroll_depth(event.trackedDeviceIndex,
                                                                                dragging.device, event.data.scroll.ydelta);
                        if (performance.enabled) {
                            ++performance.depth_scroll_events;
                            performance.depth_scroll_accepted += accepted;
                        }
                    }
                    break; // depth consumes the grab input; never scroll the UI
                }
                if (!dragging.panel.active() && event.data.scroll.cursorIndex < cursor_positions.size() &&
                    cursor_positions[event.data.scroll.cursorIndex]) {
                    const auto [x, y] = *cursor_positions[event.data.scroll.cursorIndex];
                    surface.scroll(x, y, event.data.scroll.ydelta);
                }
                break;
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.cursorIndex < cursor_positions.size())
                    cursor_positions[event.data.mouse.cursorIndex] = {event.data.mouse.x, H - event.data.mouse.y};
                ++diagnostics.pointer_downs;
                diagnostics.last_pointer_event = "down button=" + std::to_string(event.data.mouse.button);
                if (event.data.mouse.button == vr::VRMouseButton_Left)
                    if (auto kind = surface.pointer_down(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y))
                        begin_drag(*kind, event);
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.cursorIndex < cursor_positions.size())
                    cursor_positions[event.data.mouse.cursorIndex] = {event.data.mouse.x, H - event.data.mouse.y};
                ++diagnostics.pointer_ups;
                diagnostics.last_pointer_event = "up button=" + std::to_string(event.data.mouse.button);
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    auto event_result = surface.pointer_up(event.data.mouse.cursorIndex, event.data.mouse.x, H - event.data.mouse.y);
                    if (event_result.action || event_result.mount || event_result.recenter || event_result.lasers_anytime || event_result.open_bindings || event_result.launch_companion || event_result.check_updates || event_result.install_update || event_result.advanced_debug || event_result.auto_insert || event_result.gradient_enabled || event_result.close_mic_when_idle || event_result.lock_layout || event_result.wrist_world_fallback || event_result.clock_24h || event_result.date_format || event_result.model_action) ++diagnostics.pointer_actions;
                    if (event_result.action) result.push_back(*event_result.action);
                    if (event_result.check_updates) {
                        update_version.clear();
                        surface.set_update_status(updates.start() ? UpdateStatus::Checking : UpdateStatus::Failed);
                    }
                    if (event_result.install_update && !update_version.empty() &&
                        !open_update_terminal(install_update_script, update_version))
                        surface.set_update_status(UpdateStatus::TerminalFailed);
                    if (event_result.launch_companion) open_companion(*event_result.launch_companion);
                    if (event_result.model_action) {
                        model_actions.push_back(*event_result.model_action);
                        reset_input(result); // revoke held PTT, delivery and stale pointer approval
                        return result;
                    }
                    if (event_result.clock_24h) {
                        config.clock_24h = *event_result.clock_24h;
                        persistence.save_failed = persistence.persist_mount && !save_clock_24h(default_config_path(), config.clock_24h);
                        surface.set_clock_24h(config.clock_24h);
                        reset_input(result);
                        return result;
                    }
                    if (event_result.date_format) {
                        config.date_format = *event_result.date_format;
                        persistence.save_failed = persistence.persist_mount && !save_date_format(default_config_path(), config.date_format);
                        surface.set_date_format(config.date_format);
                        reset_input(result);
                        return result;
                    }
                    if (event_result.lock_layout) {
                        config.lock_layout = *event_result.lock_layout;
                        surface.set_layout_locked(config.lock_layout);
                        update_intersection_mask();
                        persistence.layout_save_failed = persistence.persist_mount && !save_lock_layout(default_config_path(), config.lock_layout);
                        reset_input(result); dragging.panel.reset();
                        return result;
                    }
                    if (event_result.wrist_world_fallback) {
                        config.wrist_world_fallback = *event_result.wrist_world_fallback;
                        persistence.save_failed = persistence.persist_mount &&
                            !save_wrist_world_fallback(default_config_path(), config.wrist_world_fallback);
                        surface.set_wrist_world_fallback(config.wrist_world_fallback);
                        reset_input(result);
                        place(); // hide or show immediately if the wrist is currently untracked
                        return result;
                    }
                    if (event_result.close_mic_when_idle) {
                        config.close_mic_when_idle = *event_result.close_mic_when_idle;
                        persistence.mic_save_failed = persistence.persist_mount &&
                            !save_close_mic_when_idle(default_config_path(), config.close_mic_when_idle);
                        surface.set_close_mic_when_idle(config.close_mic_when_idle);
                        reset_input(result); // a setting change invalidates held actions
                        return result;
                    }
                    if (event_result.gradient_enabled) {
                        config.gradient.enabled = *event_result.gradient_enabled;
                        persistence.gradient_save_failed = persistence.persist_mount &&
                            !save_gradient_enabled(default_config_path(), config.gradient.enabled);
                        surface.set_gradient_enabled(config.gradient.enabled);
                        reset_input(result);
                        return result;
                    }
                    if (event_result.auto_insert) {
                        config.auto_insert = *event_result.auto_insert;
                        persistence.auto_save_failed = persistence.persist_mount && !save_auto_insert(default_config_path(), config.auto_insert);
                        surface.set_auto_insert(config.auto_insert);
                        reset_input(result); // setting change invalidates held actions
                        return result;
                    }
                    if (event_result.advanced_debug) {
                        config.advanced_debug = *event_result.advanced_debug;
                        persistence.debug_save_failed = persistence.persist_mount && !save_advanced_debug(default_config_path(), config.advanced_debug);
                        surface.set_advanced_debug(config.advanced_debug);
                        reset_input(result);
                        // Runtime observes the change before handling this batch,
                        // restarts its worker and invalidates pending work/actions.
                        return result;
                    }
                    if (event_result.open_bindings) {
                        // The editor changes input ownership. Invalidate held gestures
                        // and pointer presses; never turn the returning release into input.
                        reset_input(result);
                        const auto error = input->OpenBindingUI(nullptr, action_set, vr::k_ulInvalidInputValueHandle, false);
                        surface.set_binding_note(error == vr::VRInputError_None
                            ? "SteamVR binding editor requested."
                            : "SteamVR could not open bindings. Try its controller settings.");
                    }
                    if (event_result.lasers_anytime) {
                        const bool enabled = *event_result.lasers_anytime;
                        if (overlay->SetOverlayFlag(handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
                                                    enabled) == vr::VROverlayError_None) {
                            lasers_anytime = enabled;
                            persistence.laser_change_failed = false;
                            surface.set_lasers_anytime(enabled);
                            persistence.save_failed = persistence.persist_mount && !save_lasers_anytime(persistence.laser_settings_path, enabled);
                        } else {
                            persistence.laser_change_failed = true;
                        }
                    }
                    if (event_result.mount) {
                        mount = *event_result.mount;
                        persistence.save_failed = persistence.persist_mount && !save_mount(persistence.settings_path, mount);
                        world_ready = false;
                        applied_mount.reset();
                    }
                    if (event_result.recenter) { world_ready = false; applied_mount.reset(); }
                }
                break;
            default: break;
            }
        }
        vr::VRActiveActionSet_t set{};
        set.ulActionSet = action_set;
        set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        // This is a request, not proof of delivery through the dashboard/game.
        set.nPriority = action_priority();
        std::array<vr::VRActiveActionSet_t, 2> sets{set, {}};
        uint32_t count = 1;
        if (dragging.panel.active() && dragging.kind == PanelDragKind::Grab && tracked(dragging.device)) {
            const auto role = system->GetControllerRoleForTrackedDeviceIndex(dragging.device);
            const auto hand = role == vr::TrackedControllerRole_LeftHand ? hands[0] :
                              role == vr::TrackedControllerRole_RightHand ? hands[1] : vr::k_ulInvalidInputValueHandle;
            if (hand != vr::k_ulInvalidInputValueHandle) {
                // Depth priority must never promote recording/typing bindings.
                sets[1].ulActionSet = grab_action_set;
                sets[1].ulRestrictedToDevice = hand;
                sets[1].nPriority = vr::k_nActionSetOverlayGlobalPriorityMin;
                count = 2;
            }
        }
        diagnostics.action_update_error = input->UpdateActionState(sets.data(), sizeof(sets[0]), count);
        const bool measuring_motion = performance.enabled && dragging.panel.active();
        const auto motion_start = measuring_motion ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Event handlers can launch a companion or update a setting. Refresh
        // tracking immediately before motion submission after that work.
        if (dragging.panel.active())
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses.data(), uint32_t(poses.size()));
        update_drag();
        place();
        if (measuring_motion) {
            ++performance.motion_ticks;
            const auto elapsed = Performance::elapsed(motion_start);
            performance.motion_ms += elapsed;
            performance.motion_max_ms = std::max(performance.motion_max_ms, elapsed);
        }
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!shown || !focus || !placed || !hmd.bPoseIsValid || !hmd.bDeviceIsConnected ||
            !overlay->IsHoverTargetOverlay(handle)) {
            // Tracking/visibility/hover loss revokes the hold even if OpenVR never sends MouseUp.
            surface.reset_pointers();
        } else if (auto command = surface.poll_keyboard_hold()) {
            ++diagnostics.pointer_actions;
            open_companion(*command);
        }
        if (dragging.panel.active()) {
            // A controller used to manipulate the panel must not also authorize
            // Record/Insert/Enter. Require neutral rearm after the drag ends.
            left.reset(); right.reset();
            if (grip_capture || ptt_capture) result.push_back(UiAction::Cancel);
            grip_capture = ptt_capture = false;
            for (auto& edge : edges) edge.reset();
            return result;
        }
        if (diagnostics.action_update_error != vr::VRInputError_None) {
            reset_input(result); return result;
        }
        auto [la, ld] = digital(0);
        auto [ra, rd] = digital(1);
        const auto now = DoubleTap::Clock::now();
        if (left.update(la && focus, ld, available(UiAction::Enter), now)) result.push_back(UiAction::Enter);
        switch (right.update(ra && focus && !panel.quick_open, rd, now)) {
        case GripRecord::Change::Begin: grip_capture = true; result.push_back(UiAction::BeginRecord); break;
        case GripRecord::Change::End: grip_capture = false; result.push_back(UiAction::EndRecord); break;
        case GripRecord::Change::Cancel: grip_capture = false; result.push_back(UiAction::Cancel); break;
        case GripRecord::Change::None: break;
        }
        constexpr std::array<UiAction, 5> mapped{{UiAction::Record, UiAction::Cancel, UiAction::Insert, UiAction::Enter, UiAction::QuickChat}};
        for (size_t i = 0; i < edges.size(); ++i) {
            auto [active, down] = digital(i + 2);
            if (i == 0 && (!active || !focus || !panel.enabled || panel.quick_open) && edges[i].reset()) {
                ptt_capture = false; result.push_back(UiAction::Cancel);
            }
            auto change = edges[i].update(active && focus && ((panel.enabled && (i != 0 || !panel.quick_open)) || i == 1), down);
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
std::vector<ModelAction> Overlay::take_model_actions() {
    auto result = std::move(impl_->model_actions);
    impl_->model_actions.clear();
    return result;
}
void Overlay::draw(const Panel& panel) { impl_->draw(panel); }
bool Overlay::advanced_debug() const { return impl_->config.advanced_debug; }
bool Overlay::auto_insert() const { return impl_->config.auto_insert; }
bool Overlay::close_mic_when_idle() const { return impl_->config.close_mic_when_idle; }
const std::vector<std::string>& Overlay::quick_inputs() const { return impl_->config.quick_inputs; }
std::string Overlay::controls_status() {
    // Compare the same actions across modes, before and after our pose/role gate.
    // IsInputAvailable and a successful UpdateActionState are not delivery proof.
    std::string result = impl_->input_mode_status() +
        " update=" + std::to_string(int(impl_->diagnostics.action_update_error));
    for (size_t i = 0; i < action_names.size(); ++i) {
        vr::InputDigitalActionData_t data{};
        const auto error = impl_->input->GetDigitalActionData(impl_->actions[i], &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
        const auto [accepted, down] = impl_->digital(i);
        result += std::string("\nAction ") + action_names[i] +
            " err=" + std::to_string(int(error)) + " active=" + (data.bActive ? "Y" : "N") +
            " down=" + (data.bState ? "Y" : "N") +
            " pose-role-accepted=" + (accepted ? "Y" : "N") + " gated-down=" + (down ? "Y" : "N");
    }
    return result;
}
std::string Overlay::pointer_status() const {
    return "Pointer down=" + std::to_string(impl_->diagnostics.pointer_downs) + " up=" + std::to_string(impl_->diagnostics.pointer_ups) +
           " hits=" + std::to_string(impl_->diagnostics.pointer_actions) + " resets=" + std::to_string(impl_->diagnostics.pointer_resets) +
           " last=" + impl_->diagnostics.last_pointer_event +
           "\nOverlay renderer=Vulkan textureUploads=" + std::to_string(impl_->diagnostics.texture_uploads) +
           " showCalls=" + std::to_string(impl_->diagnostics.show_calls) + " hideCalls=" + std::to_string(impl_->diagnostics.hide_calls) +
           " shownEvents=" + std::to_string(impl_->diagnostics.overlay_shown_events) +
           " hiddenEvents=" + std::to_string(impl_->diagnostics.overlay_hidden_events) +
           " imageLoaded=" + std::to_string(impl_->diagnostics.image_loaded_events) +
           " imageFailed=" + std::to_string(impl_->diagnostics.image_failed_events) +
           " overlayFocus=" + std::to_string(impl_->diagnostics.overlay_focus_events) +
           " globalFocus=" + std::to_string(impl_->diagnostics.global_focus_events) +
           " inputCaptured=" + std::to_string(impl_->diagnostics.input_focus_captured_events);
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
