#include "paced_delivery.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace frameyap {
PacedDelivery::PacedDelivery(DeliveryFactory acquire, Clock clock,
                             std::function<void()> release_idle, Guard permitted)
    : acquire_(std::move(acquire)), clock_(std::move(clock)),
      release_idle_(std::move(release_idle)), permitted_(std::move(permitted)) {}

void PacedDelivery::trace(const std::string& line) const {
    if (trace_) { try { trace_(line); } catch (...) {} }
}
bool PacedDelivery::valid_guard() const {
    try { return (!permitted_ || permitted_()) && task_->guard && task_->guard(); }
    catch (...) { return false; }
}
void PacedDelivery::start(std::string text, bool enter, Guard guard,
                          std::function<void()> on_first_send) {
    if (task_) throw std::runtime_error("Input already being paced");
    if (text.empty() && !enter) throw std::runtime_error("Empty input action");
    if (!text.empty() && (literal_text(text) != text))
        throw std::runtime_error("Input must be a validated literal");
    auto candidate = std::make_unique<Task>();
    candidate->text = std::move(text);
    candidate->enter = enter;
    candidate->guard = std::move(guard);
    candidate->on_first_send = std::move(on_first_send);
    // Acquiring an IME is not delivery. Even after acquisition, focus must
    // still match the authorization at this action's start.
    try {
        if ((permitted_ && !permitted_()) || !candidate->guard || !candidate->guard())
            throw std::runtime_error("Input focus unavailable");
        candidate->first_lease = acquire_();
        if (!candidate->first_lease || (permitted_ && !permitted_()) || !candidate->guard())
            throw std::runtime_error("Input focus changed during acquisition");
    } catch (const std::exception& e) {
        trace(std::string("start rejected: ") + e.what());
        std::fill(candidate->text.begin(), candidate->text.end(), '\0');
        throw;
    } catch (...) {
        trace("start rejected: unknown");
        std::fill(candidate->text.begin(), candidate->text.end(), '\0');
        throw;
    }
    trace("start bytes=" + std::to_string(candidate->text.size()) +
          " enter=" + (candidate->enter ? "1" : "0"));
    retained_ = true;
    task_ = std::move(candidate);
}
PacedDelivery::Outcome PacedDelivery::finish(DeliveryResult result, bool focus_lost) {
    Outcome out{result, task_->began, focus_lost};
    trace("finish result=" + std::to_string(static_cast<int>(result)) + " began=" +
          (task_->began ? "1" : "0") + " focus_lost=" + (focus_lost ? "1" : "0") +
          " offset=" + std::to_string(task_->offset) + "/" + std::to_string(task_->text.size()));
    std::fill(task_->text.begin(), task_->text.end(), '\0');
    task_.reset();
    return out;
}
void PacedDelivery::release_if_idle() {
    if (retained_ && !task_ && (!last_commit_ || clock_() - *last_commit_ >= gap)) {
        if (release_idle_) {
            try { release_idle_(); } catch (...) { return; } // retry next idle tick
        }
        retained_ = false;
    }
}
std::optional<PacedDelivery::Outcome> PacedDelivery::tick() {
    if (!task_) { release_if_idle(); return std::nullopt; }
    if (last_commit_ && clock_() - *last_commit_ < gap) return std::nullopt;
    auto& task = *task_;
    // A focus loss before ANY send leaves the review unconsumed. After the
    // first possible send, the remainder is discarded, not retargeted/retried.
    if (!valid_guard()) {
        trace("guard failed before acquire");
        return finish(task.began ? DeliveryResult::TextUncertain : DeliveryResult::Ignored, true);
    }
    std::unique_ptr<DeliveryLease> lease = std::move(task.first_lease);
    if (!lease) {
        try { lease = acquire_(); }
        catch (const std::exception& e) {
            trace(std::string("acquire failed: ") + e.what());
            return finish(task.began ? (task.offset == task.text.size() ?
            DeliveryResult::TextQueuedEnterUnavailable : DeliveryResult::TextUncertain) : DeliveryResult::Ignored); }
        catch (...) { trace("acquire failed: unknown"); return finish(task.began ? (task.offset == task.text.size() ?
            DeliveryResult::TextQueuedEnterUnavailable : DeliveryResult::TextUncertain) : DeliveryResult::Ignored); }
        if (!lease) return finish(task.began ? DeliveryResult::TextUncertain : DeliveryResult::Ignored);
    }
    if (!valid_guard()) {
        trace("guard failed after acquire");
        return finish(task.began ? DeliveryResult::TextUncertain : DeliveryResult::Ignored, true);
    }
    if (!task.began) {
        // A callback consumes review immediately before a possibly ambiguous send.
        try { if (task.on_first_send) task.on_first_send(); }
        catch (...) { return finish(DeliveryResult::Ignored); }
        task.began = true;
    }
    const bool sending_text = task.offset < task.text.size();
    // Mark even a throwing send as attempted. Do not retry or submit Enter.
    last_commit_ = clock_();
    if (sending_text) {
        std::size_t end = task.offset;
        for (std::size_t count = 0; end < task.text.size() && count < max_codepoints; ++count) {
            auto byte = static_cast<unsigned char>(task.text[end]);
            end += byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        }
        std::string chunk;
        try {
            chunk = task.text.substr(task.offset, end - task.offset);
            lease->text(chunk);
        } catch (const std::exception& e) {
            trace("text send failed at offset=" + std::to_string(task.offset) + ": " + e.what());
            last_commit_ = clock_();
            std::fill(chunk.begin(), chunk.end(), '\0');
            return finish(DeliveryResult::TextUncertain);
        } catch (...) {
            trace("text send failed at offset=" + std::to_string(task.offset) + ": unknown");
            last_commit_ = clock_();
            std::fill(chunk.begin(), chunk.end(), '\0');
            return finish(DeliveryResult::TextUncertain);
        }
        last_commit_ = clock_();
        trace("sent offset=" + std::to_string(task.offset) + " bytes=" + std::to_string(end - task.offset));
        std::fill(chunk.begin(), chunk.end(), '\0');
        task.offset = end;
        if (end == task.text.size() && !task.enter) return finish(DeliveryResult::TextQueued);
    } else {
        try { lease->enter(); trace("sent enter"); }
        catch (const std::exception& e) { trace(std::string("enter failed: ") + e.what()); last_commit_ = clock_(); return finish(DeliveryResult::EnterUncertain); }
        catch (...) { last_commit_ = clock_(); return finish(DeliveryResult::EnterUncertain); }
        last_commit_ = clock_();
        return finish(DeliveryResult::EnterQueued);
    }
    return std::nullopt;
}
void PacedDelivery::cancel() {
    if (task_) {
        std::fill(task_->text.begin(), task_->text.end(), '\0');
        task_.reset();
    }
    release_if_idle();
}
} // namespace frameyap
