#pragma once
#include "overlay.hpp"
#include "battery.hpp"
#include "config.hpp"
#include "panel_drag.hpp"
#include <ctime>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frameyap {
struct SurfaceEvent {
    std::optional<UiAction> action;
    std::optional<Mount> mount;
    std::optional<bool> lasers_anytime;
    std::optional<bool> advanced_debug;
    std::optional<bool> auto_insert;
    std::optional<bool> close_mic_when_idle;
    std::optional<bool> lock_layout;
    std::optional<bool> clock_24h;
    std::optional<DateFormat> date_format;
    bool recenter = false;
    bool open_bindings = false;
    std::optional<ModelAction> model_action;
};
// Header indicators. Absent values are hidden, never shown as zero or as a guess.
struct StatusIndicators {
    std::optional<bool> dashboard_open; // SteamVR dashboard: controller bindings paused
    std::optional<BatteryLevel> left, head, right;
    bool operator==(const StatusIndicators&) const = default;
};
// One CPU RGBA canvas, independent of OpenVR. Settings replace the review area;
// status and safety controls remain on the same surface.
class PanelSurface {
public:
    struct Bounds { int x, y, w, h; };
    static constexpr int width = 1080, height = 780;
    static constexpr Bounds body{0, 0, 1000, 680};
    static constexpr Bounds grab{370, 698, 260, 52};
    static constexpr Bounds scale{996, 680, 68, 68};
    using Clock = std::chrono::steady_clock;
    PanelSurface(const std::string& font, Mount mount, Theme theme = {}, GradientConfig gradient = {});
    ~PanelSurface();
    PanelSurface(const PanelSurface&) = delete;
    PanelSurface& operator=(const PanelSurface&) = delete;
    // Monotonic time is injectable for offline loop/cadence checks. Hidden
    // overlays still process UI changes, but do not schedule animation repaints.
    bool render(const Panel& panel, Clock::time_point now = Clock::now(), bool animate = true);
    const std::vector<unsigned char>& pixels() const;
    // Handles capture one cursor and never authorize a UI action on release.
    static constexpr auto quit_hold = std::chrono::milliseconds(900);
    std::optional<PanelDragKind> pointer_down(unsigned cursor, float x, float y, Clock::time_point now = Clock::now());
    SurfaceEvent pointer_up(unsigned cursor, float x, float y, Clock::time_point now = Clock::now());
    bool dragging(unsigned cursor) const;
    // OpenVR intersection masks use top-left coordinates, unlike mouse events.
    std::vector<Bounds> input_regions() const;
    void set_layout_locked(bool locked);
    void reset_pointers();
    void set_placement_note(std::string note);
    void set_lasers_anytime(bool enabled);
    void set_advanced_debug(bool enabled);
    void set_auto_insert(bool enabled);
    void set_close_mic_when_idle(bool enabled);
    void set_clock_24h(bool enabled);
    void set_date_format(DateFormat format);
    void set_clock_time(std::time_t now);
    void set_binding_note(std::string note);
    void set_indicators(const StatusIndicators& indicators);
    bool available(UiAction action) const;
    // Exact text rows currently shown in the consent viewport (empty outside it).
    // OpenVR has no screen-reader accessibility channel for this canvas.
    std::vector<std::string> visible_model_review_lines() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace frameyap
