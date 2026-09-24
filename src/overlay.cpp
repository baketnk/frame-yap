#include "overlay.hpp"
#include "gestures.hpp"

#include <openvr.h>
#include <ft2build.h>
#include FT_FREETYPE_H

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
constexpr int W = 900, H = 500;
struct Rect { int x, y, w, h; bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
} };
struct Button { Rect bounds; const char* label; UiAction action; };
constexpr std::array<Button, 6> buttons{{
    {{24, 422, 130, 58}, "Prev", UiAction::Toggle}, // handled as page navigation, not a toggle
    {{168, 422, 130, 58}, "Record", UiAction::Record},
    {{312, 422, 130, 58}, "Cancel", UiAction::Cancel},
    {{456, 422, 130, 58}, "Insert", UiAction::Insert},
    {{600, 422, 130, 58}, "Enter", UiAction::Enter},
    {{744, 422, 130, 58}, "Quit", UiAction::Quit},
}};
void overlay_check(vr::EVROverlayError err, vr::IVROverlay* api, const char* op) {
    if (err != vr::VROverlayError_None)
        throw std::runtime_error(std::string(op) + ": " + api->GetOverlayErrorNameFromEnum(err));
}
std::filesystem::path absolute_file(const std::filesystem::path& p) {
    auto result = std::filesystem::absolute(p);
    if (!std::filesystem::is_regular_file(result)) throw std::runtime_error("Missing file: " + result.string());
    return result;
}
// A strict, bounded UTF-8 decoder: malformed input is shown as replacement glyphs.
uint32_t next_codepoint(std::string_view s, size_t& i) {
    const unsigned char a = static_cast<unsigned char>(s[i++]);
    if (a < 0x80) return a;
    int count = a >= 0xc2 && a <= 0xdf ? 1 : a >= 0xe0 && a <= 0xef ? 2 : a >= 0xf0 && a <= 0xf4 ? 3 : 0;
    if (!count || i + count > s.size()) return 0xfffd;
    uint32_t value = a & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    for (int k = 0; k < count; ++k) {
        unsigned char b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (b & 0x3f);
    }
    if ((count == 1 && value < 0x80) || (count == 2 && value < 0x800) ||
        (count == 3 && value < 0x10000) || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
        return 0xfffd;
    i += count;
    return value;
}
} // namespace

struct Overlay::Impl {
    vr::IVRSystem* system = nullptr;
    vr::IVROverlay* overlay = nullptr;
    vr::IVRInput* input = nullptr;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    vr::VRActionSetHandle_t action_set = vr::k_ulInvalidActionSetHandle;
    std::array<vr::VRActionHandle_t, 6> actions{};
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::vector<unsigned char> pixels = std::vector<unsigned char>(W * H * 4);
    Panel panel;
    bool hand = false;
    vr::TrackedDeviceIndex_t anchor = vr::k_unTrackedDeviceIndexInvalid;
    DoubleTap left;
    GripRecord right;
    std::array<NeutralEdge, 4> edges{};
    std::array<int, 2> pressed_button{{-1, -1}};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    std::vector<std::string> pages;
    size_t page = 0;
    bool drawn = false;
    bool grip_capture = false, ptt_capture = false;
    bool focus = true;
    static constexpr Rect next_page{754, 292, 112, 30};

    Impl(const std::string& assets, const std::string& font, bool attach) : hand(attach) {
        const auto action_path = absolute_file(std::filesystem::path(assets) / "actions.json");
        absolute_file(std::filesystem::path(assets) / "bindings_knuckles.json");
        const auto font_path = absolute_file(font);
        if (FT_Init_FreeType(&library)) throw std::runtime_error("FreeType initialization failed");
        try {
            if (FT_New_Face(library, font_path.c_str(), 0, &face) || FT_Set_Pixel_Sizes(face, 0, 31))
                throw std::runtime_error("Could not load specified font");
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
        if (face) { FT_Done_Face(face); face = nullptr; }
        if (library) { FT_Done_FreeType(library); library = nullptr; }
    }
    void place() {
        vr::TrackedDeviceIndex_t target = vr::k_unTrackedDeviceIndex_Hmd;
        if (hand) {
            const auto left_index = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            if (left_index != vr::k_unTrackedDeviceIndexInvalid && left_index < poses.size() &&
                poses[left_index].bPoseIsValid && system->IsTrackedDeviceConnected(left_index))
                target = left_index;
        }
        if (anchor == target) return;
        vr::HmdMatrix34_t transform{{{1.f, 0.f, 0.f, target == vr::k_unTrackedDeviceIndex_Hmd ? 0.f : 0.13f},
                                      {0.f, 1.f, 0.f, target == vr::k_unTrackedDeviceIndex_Hmd ? -0.16f : 0.12f},
                                      {0.f, 0.f, 1.f, target == vr::k_unTrackedDeviceIndex_Hmd ? -1.05f : -0.18f}}};
        overlay_check(overlay->SetOverlayTransformTrackedDeviceRelative(handle, target, &transform), overlay, "SetOverlayTransformTrackedDeviceRelative");
        anchor = target;
    }
    void rect(Rect r, std::array<unsigned char, 4> c) {
        for (int y = std::max(r.y, 0); y < std::min(H, r.y + r.h); ++y)
            for (int x = std::max(r.x, 0); x < std::min(W, r.x + r.w); ++x) {
                const size_t p = (size_t(y) * W + x) * 4;
                std::copy(c.begin(), c.end(), pixels.begin() + p);
            }
    }
    void text(std::string_view s, int x, int baseline, int max_x, int max_y, int max_bytes) {
        size_t i = 0;
        int used = 0;
        while (i < s.size() && used < max_bytes && baseline < max_y) {
            const size_t before = i;
            const auto cp = next_codepoint(s, i);
            used += int(i - before);
            if (cp == '\n') { x = 28; baseline += 39; continue; }
            if (cp == '\r' || cp == '\t') continue;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) continue;
            const auto glyph = face->glyph;
            if (x + (glyph->advance.x >> 6) >= max_x) { x = 28; baseline += 39; }
            if (baseline >= max_y) break;
            const auto& b = glyph->bitmap;
            for (unsigned row = 0; row < b.rows; ++row)
                for (unsigned col = 0; col < b.width; ++col) {
                    int px = x + glyph->bitmap_left + int(col);
                    int py = baseline - glyph->bitmap_top + int(row);
                    if (px < 0 || px >= W || py < 0 || py >= max_y) continue;
                    const unsigned char alpha = b.buffer[int(row) * b.pitch + int(col)];
                    auto* dst = pixels.data() + (size_t(py) * W + px) * 4;
                    for (int c = 0; c < 3; ++c)
                        dst[c] = static_cast<unsigned char>((unsigned(dst[c]) * (255 - alpha) + 240u * alpha) / 255);
                }
            x += glyph->advance.x >> 6;
        }
    }
    bool available(UiAction a) const {
        switch (a) {
        case UiAction::Toggle: return page > 0;
        case UiAction::Quit: case UiAction::Record: case UiAction::Cancel: return true;
        case UiAction::BeginRecord: case UiAction::EndRecord: return true;
        case UiAction::Enter: return panel.enabled;
        case UiAction::Insert: return panel.enabled && !panel.transcript.empty();
        }
        return false;
    }
    // Split by rendered glyph advances, never by byte count or arbitrary characters.
    // UTF-8 slices remain intact; all bytes (including invalid UTF-8) stay visible.
    std::vector<std::string> paginate(std::string_view s, int lines, int right) {
        std::vector<std::string> out;
        size_t start = 0, i = 0;
        int x = 28, line = 0;
        while (i < s.size()) {
            size_t before = i;
            auto cp = next_codepoint(s, i);
            if (cp == '\n') {
                if (++line == lines) {
                    out.emplace_back(s.substr(start, i - start));
                    start = i; line = 0;
                }
                x = 28;
                continue;
            }
            int advance = 0;
            if (cp != '\r' && cp != '\t' && !FT_Load_Char(face, cp, FT_LOAD_DEFAULT))
                advance = face->glyph->advance.x >> 6;
            if (x + advance >= right && x != 28) {
                if (++line == lines) {
                    out.emplace_back(s.substr(start, before - start));
                    start = before; line = 0;
                } else {
                    // Add a line break in the displayed page while keeping source intact.
                    // The renderer's own width wrapping uses the same advance rule.
                }
                x = 28;
            }
            x += advance;
        }
        if (start < s.size() || out.empty()) out.emplace_back(s.substr(start));
        return out;
    }
    void draw(const Panel& p) {
        if (p.transcript != panel.transcript || pages.empty()) {
            pages = paginate(p.transcript.empty() ? "Transcript preview" : p.transcript, 3, W - 28);
            page = 0;
            drawn = false;
        }
        if (drawn && p.status == panel.status && p.transcript == panel.transcript &&
            p.detail == panel.detail && p.enabled == panel.enabled && p.recording == panel.recording) return;
        panel = p;
        rect({0, 0, W, H}, {20, 26, 39, 255});
        rect({0, 0, W, 12}, {static_cast<unsigned char>(p.recording ? 220 : 55),
                              static_cast<unsigned char>(p.recording ? 82 : 135), 72, 255});
        text(p.enabled ? "FrameYap  |  Enabled" : "FrameYap  |  Disabled", 28, 52, W - 20, 72, 128);
        const auto status_lines = paginate(p.status, 2, 620);
        text(status_lines[0], 28, 96, 620, 150, int(status_lines[0].size()));
        if (status_lines.size() > 1) text("[status truncated]", 635, 140, W - 28, 150, 32);
        rect({20, 153, W - 40, 171}, {37, 46, 63, 255});
        text(pages[page], 28, 190, W - 28, 290, int(pages[page].size()));
        const std::string page_label = "Page " + std::to_string(page + 1) + " / " + std::to_string(pages.size());
        text(page_label, 383, 315, 720, 324, 64);
        rect(next_page, page + 1 < pages.size() ? std::array<unsigned char, 4>{56, 94, 129, 255}
                                                 : std::array<unsigned char, 4>{49, 52, 60, 255});
        text("Next", 767, 316, 860, 324, 8);
        const auto detail_lines = paginate(p.detail, 2, 620);
        text(detail_lines[0], 28, 355, 620, 412, int(detail_lines[0].size()));
        if (detail_lines.size() > 1)
            text("[detail truncated]", 635, 405, W - 28, 412, 32);
        for (const auto& button : buttons) {
            const bool enabled = available(button.action);
            rect(button.bounds, enabled ? std::array<unsigned char, 4>{56, 94, 129, 255}
                                        : std::array<unsigned char, 4>{49, 52, 60, 255});
            text(button.label, button.bounds.x + 10, button.bounds.y + 38,
                 button.bounds.x + button.bounds.w - 5, H, 64);
        }
        overlay_check(overlay->SetOverlayRaw(handle, pixels.data(), W, H, 4), overlay, "SetOverlayRaw");
        overlay_check(overlay->ShowOverlay(handle), overlay, "ShowOverlay");
        drawn = true;
    }
    void reset_input(std::vector<UiAction>& result) {
        left.reset();
        const bool lost_grip = right.reset() == GripRecord::Change::Cancel || grip_capture;
        grip_capture = false;
        const bool lost_ptt = edges[0].reset() || ptt_capture;
        ptt_capture = false;
        if (lost_grip || lost_ptt) result.push_back(UiAction::Cancel);
        for (size_t i = 1; i < edges.size(); ++i) edges[i].reset();
        pressed_button.fill(-1);
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
            case vr::VREvent_OverlayFocusChanged:
                // Pointer/gamepad overlay focus is not OS keyboard focus or action
                // activity. Cancel the old gesture, then require neutral rearm;
                // do not permanently latch global grip actions off.
                reset_input(result); break;
            case vr::VREvent_MouseButtonDown: {
                const auto& m = event.data.mouse;
                if (m.button != vr::VRMouseButton_Left || m.cursorIndex >= pressed_button.size()) break;
                const int x = int(m.x), y = H - int(m.y);
                pressed_button[m.cursorIndex] = -1;
                for (size_t i = 0; i < buttons.size(); ++i)
                    if (buttons[i].bounds.contains(x, y) && available(buttons[i].action)) {
                        pressed_button[m.cursorIndex] = int(i); break;
                    }
                if (next_page.contains(x, y) && page + 1 < pages.size())
                    pressed_button[m.cursorIndex] = int(buttons.size());
                break;
            }
            case vr::VREvent_MouseButtonUp: {
                const auto& m = event.data.mouse;
                if (m.button != vr::VRMouseButton_Left || m.cursorIndex >= pressed_button.size()) break;
                const int index = pressed_button[m.cursorIndex];
                pressed_button[m.cursorIndex] = -1;
                if (index == int(buttons.size()) && next_page.contains(int(m.x), H - int(m.y)) &&
                    page + 1 < pages.size()) { ++page; drawn = false; }
                else if (index == 0 && buttons[0].bounds.contains(int(m.x), H - int(m.y)) && page > 0) {
                    --page; drawn = false;
                } else if (index > 0 && index < int(buttons.size()) &&
                    buttons[index].bounds.contains(int(m.x), H - int(m.y)) &&
                    available(buttons[index].action)) result.push_back(buttons[index].action);
                break;
            }
            default: break;
            }
        }
        vr::VRActiveActionSet_t set{};
        set.ulActionSet = action_set;
        set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        set.nPriority = 0; // normal priority; no experimental scene-input overrides
        if (input->UpdateActionState(&set, sizeof(set), 1) != vr::VRInputError_None) {
            reset_input(result); return result;
        }
        auto [la, ld] = digital(0);
        auto [ra, rd] = digital(1);
        const auto now = DoubleTap::Clock::now();
        if (left.update(la && focus, ld, panel.enabled, now)) result.push_back(UiAction::Enter);
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

Overlay::Overlay(const std::string& assets, const std::string& font, bool hand)
    : impl_(std::make_unique<Impl>(assets, font, hand)) {}
Overlay::~Overlay() = default;
std::vector<UiAction> Overlay::poll() { return impl_->poll(); }
void Overlay::draw(const Panel& panel) { impl_->draw(panel); }
std::string Overlay::controls_status() {
    auto left = impl_->digital(0), right = impl_->digital(1);
    return std::string("Grip actions tracked/active: left=") + (left.first ? "yes" : "no") +
           " right=" + (right.first ? "yes" : "no");
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
