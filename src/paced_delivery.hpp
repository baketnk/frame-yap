#pragma once
#include "core.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace frameyap {
// One explicitly authorized action (text and optional Enter) at a time. All
// callbacks run on the UI thread; no sleeping, worker thread, or input replay.
class PacedDelivery {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Guard = std::function<bool()>;
    struct Outcome {
        DeliveryResult result;
        bool began;       // first send may have reached the compositor
        bool focus_lost;  // a check failed before the next send
    };
    static constexpr auto gap = std::chrono::milliseconds(150);
    static constexpr std::size_t max_codepoints = 24;
    PacedDelivery(DeliveryFactory acquire, Clock clock = std::chrono::steady_clock::now,
                  std::function<void()> release_idle = {}, Guard permitted = {});
    ~PacedDelivery() { cancel(); }
    // Validates/acquires before accepting a task. Failure throws without calling
    // on_first_send; an existing review can still be offered for explicit Type.
    // The first lease is held until the first due tick; subsequent leases are
    // acquired one at a time after the previous lease has been released.
    void start(std::string text, bool enter, Guard guard, std::function<void()> on_first_send);
    std::optional<Outcome> tick(); // at most ONE commit; returns outcome only at termination
    bool active() const { return bool(task_); }
    bool began() const { return task_ && task_->began; }
    void cancel(); // drops and scrubs the remaining literal; never sends Enter
private:
    struct Task {
        std::string text;
        std::size_t offset = 0;
        bool enter = false, began = false;
        Guard guard;
        std::function<void()> on_first_send;
        std::unique_ptr<DeliveryLease> first_lease;
    };
    bool valid_guard() const;
    Outcome finish(DeliveryResult result, bool focus_lost = false);
    void release_if_idle();
    DeliveryFactory acquire_;
    Clock clock_;
    std::function<void()> release_idle_;
    Guard permitted_;
    std::unique_ptr<Task> task_;
    std::optional<std::chrono::steady_clock::time_point> last_commit_;
    bool retained_ = false;
};
} // namespace frameyap
