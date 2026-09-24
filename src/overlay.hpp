#pragma once
#include "mount.hpp"
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace frameyap {
enum class UiAction { Toggle, Record, BeginRecord, EndRecord, Cancel, Insert, Enter, QuickChat, Quit };
struct Panel {
    Panel() = default;
    Panel(std::string status, std::string transcript, std::string detail,
          bool enabled, bool recording, bool record_available = true)
        : status(std::move(status)), transcript(std::move(transcript)), detail(std::move(detail)),
          enabled(enabled), recording(recording), record_available(record_available) {}
    std::string status;
    std::string transcript;
    std::string detail;
    bool enabled = false;
    bool recording = false;
    bool record_available = true;
    bool quick_open = false;
    size_t quick_selected = 0;
    std::vector<std::string> quick_inputs;
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
    bool advanced_debug() const;
    bool auto_insert() const;
    const std::vector<std::string>& quick_inputs() const;
    std::string controls_status(); // diagnostic only, no input delivery
    std::string pointer_status() const; // diagnostic counters, no input delivery
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Explicit registration only; zero on success, nonzero on failure.
int registration(const std::string& manifest, bool remove, bool autostart);
} // namespace frameyap
