#pragma once
#include "overlay.hpp"
#include "config.hpp"
#include "panel_drag.hpp"
#include <ctime>
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
    std::optional<bool> clock_24h;
    std::optional<DateFormat> date_format;
    bool recenter = false;
    bool open_bindings = false;
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
    PanelSurface(const std::string& font, Mount mount, Theme theme = {});
    ~PanelSurface();
    PanelSurface(const PanelSurface&) = delete;
    PanelSurface& operator=(const PanelSurface&) = delete;
    bool render(const Panel& panel);
    const std::vector<unsigned char>& pixels() const;
    // Handles capture one cursor and never authorize a UI action on release.
    std::optional<PanelDragKind> pointer_down(unsigned cursor, float x, float y);
    SurfaceEvent pointer_up(unsigned cursor, float x, float y);
    bool dragging(unsigned cursor) const;
    void reset_pointers();
    void set_placement_note(std::string note);
    void set_lasers_anytime(bool enabled);
    void set_advanced_debug(bool enabled);
    void set_auto_insert(bool enabled);
    void set_clock_24h(bool enabled);
    void set_date_format(DateFormat format);
    void set_clock_time(std::time_t now);
    void set_binding_note(std::string note);
    bool available(UiAction action) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace frameyap
