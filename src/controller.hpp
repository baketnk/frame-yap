#pragma once
#include "core.hpp"
#include "paced_delivery.hpp"
#include "overlay.hpp"
#include "worker.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace frameyap {
// Hardware-free ownership boundaries. Implementations may open devices only in
// these explicitly requested methods; constructing a Controller has no side effects.
class ControllerAudio {
public:
    virtual ~ControllerAudio() = default;
    virtual bool open() const = 0;
    virtual void prepare() = 0;
    virtual void start() = 0;
    virtual bool poll() = 0;
    virtual std::vector<float> finish() = 0;
    virtual void cancel() = 0;
    virtual void close() = 0;
    virtual int seconds() const = 0;
};
class ControllerWorker {
public:
    virtual ~ControllerWorker() = default;
    virtual void start(bool advanced_debug) = 0;
    virtual bool ready() const = 0;
    virtual void submit(uint64_t id, const std::vector<float>& pcm) = 0;
    virtual std::optional<WorkerReply> poll() = 0;
    virtual void stop() = 0;
};
class ControllerFocus {
public:
    virtual ~ControllerFocus() = default;
    virtual bool arm() = 0;
    virtual bool valid() = 0;
};
using FocusFactory = std::function<std::unique_ptr<ControllerFocus>()>;

class Controller {
public:
    Controller(ControllerAudio& audio, ControllerWorker& worker,
               DeliveryFactory delivery, FocusFactory focus,
               std::vector<std::string> quick_inputs, bool auto_insert = false,
               bool advanced_debug = false, bool close_mic_when_idle = false,
               PacedDelivery* paced = nullptr);
    // Called explicitly by runtime, after setting up the UI. A failure is shown
    // in Panel and can be retried with Record.
    void initialize();
    // Invalidate clip, preview and focus before switching manifest/worker.
    // A missing/unverified backend cannot be retried by the Record button.
    void backend_changed(bool verified, std::string detail = {});
    void tick(); // nonblocking poll of focus, worker, microphone; no UI or hardware setup
    Panel panel() const;
    void settings(bool auto_insert, bool advanced_debug, bool close_mic_when_idle);
    void action(UiAction action);
    bool quitting() const { return quit_; }
    void shutdown(); // releases only injected resources; safe to call repeatedly
    State state() const { return session_.state(); }
private:
    void warm();
    void start_record();
    void stop_record();
    void request_failed(const std::string& error);
    void delivery_detail(DeliveryResult result, bool submit);
    void queue_paced(std::string text, bool enter, bool review,
                     std::shared_ptr<ControllerFocus> focus, bool automatic = false);
    std::shared_ptr<ControllerFocus> manual_focus();
    ControllerAudio& audio_;
    ControllerWorker& worker_;
    DeliveryFactory delivery_;
    PacedDelivery* paced_ = nullptr; // optional: native tick-driven delivery
    bool paced_auto_ = false, paced_submit_ = false;
    FocusFactory focus_factory_;
    std::vector<std::string> quick_inputs_;
    Session session_;
    std::unique_ptr<ControllerFocus> armed_focus_;
    std::string detail_, status_note_;
    bool quit_ = false, quick_open_ = false;
    size_t quick_selected_ = 0;
    bool auto_insert_, advanced_debug_, close_mic_when_idle_;
    bool backend_available_ = true;
};
} // namespace frameyap
