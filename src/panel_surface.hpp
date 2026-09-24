#pragma once
#include "overlay.hpp"
#include "config.hpp"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frameyap {
struct SurfaceEvent {
    std::optional<UiAction> action;
    std::optional<Mount> mount;
    std::optional<bool> lasers_anytime;
    bool recenter = false;
    bool open_bindings = false;
};
// One CPU RGBA canvas, independent of OpenVR. Settings replace the review area;
// status and safety controls remain on the same surface.
class PanelSurface {
public:
    static constexpr int width = 1000, height = 680;
    PanelSurface(const std::string& font, Mount mount, Theme theme = {});
    ~PanelSurface();
    PanelSurface(const PanelSurface&) = delete;
    PanelSurface& operator=(const PanelSurface&) = delete;
    bool render(const Panel& panel);
    const std::vector<unsigned char>& pixels() const;
    void pointer_down(unsigned cursor, float x, float y);
    SurfaceEvent pointer_up(unsigned cursor, float x, float y);
    void pointer_move(unsigned cursor, float x, float y);
    void reset_pointers();
    void set_placement_note(std::string note);
    void set_lasers_anytime(bool enabled);
    // PTT, Cancel, Insert, Enter, left-grip gesture, right-grip gesture.
    void set_bindings(std::array<std::string, 6> labels);
    void set_binding_note(std::string note);
    bool bindings_visible() const;
    bool available(UiAction action) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace frameyap
