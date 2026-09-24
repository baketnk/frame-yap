#pragma once
#include "mount.hpp"
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace frameyap {
enum class UiAction { Toggle, Record, BeginRecord, EndRecord, Cancel, Insert, Enter, Quit };
struct Panel {
    std::string status;
    std::string transcript;
    std::string detail;
    bool enabled = false;
    bool recording = false;
    bool record_available = true;
};
class Overlay {
public:
    Overlay(const std::string& assets, const std::string& font = {}, std::optional<Mount> mount = {},
            bool persist_mount = true);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    std::vector<UiAction> poll();
    void draw(const Panel& panel);
    std::string controls_status(); // diagnostic only, no input delivery
    std::string pointer_status() const; // diagnostic counters, no input delivery
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Explicit registration only; zero on success, nonzero on failure.
int registration(const std::string& manifest, bool remove, bool autostart);
} // namespace frameyap
