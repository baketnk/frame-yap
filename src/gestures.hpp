#pragma once

#include <chrono>

namespace frameyap {
// Feed monotonic samples. Inactive, tracking/focus loss and reconnect require an
// observed neutral sample before any new press can be interpreted.
class DoubleTap {
public:
    using Clock = std::chrono::steady_clock;
    bool update(bool active, bool down, bool enabled, Clock::time_point now) {
        if (!active || !enabled) { reset(); return false; }
        if (!armed_) {
            if (!down) { armed_ = true; previous_ = false; }
            return false;
        }
        if (down == previous_) return false;
        previous_ = down;
        if (down) {
            pressed_ = now;
            if (waiting_ && now >= released_ && now - released_ <= std::chrono::milliseconds(350)) {
                waiting_ = false;
                // The second tap must itself be short; report on its release.
                second_ = true;
            } else { waiting_ = false; second_ = false; }
            return false;
        }
        if (now < pressed_ || now - pressed_ > std::chrono::milliseconds(250)) {
            waiting_ = second_ = false;
            return false;
        }
        if (second_) { second_ = false; waiting_ = false; return true; }
        waiting_ = true;
        released_ = now;
        return false;
    }
    void reset() { armed_ = previous_ = waiting_ = second_ = false; }
private:
    bool armed_ = false, previous_ = false, waiting_ = false, second_ = false;
    Clock::time_point pressed_{}, released_{};
};

// First short right-grip tap (<=250ms), then second down within 350ms
// begins capture immediately; second release ends it, regardless of its length.
// Any loss during capture cancels, and reconnect requires an observed neutral.
class GripRecord {
public:
    using Clock = std::chrono::steady_clock;
    enum class Change { None, Begin, End, Cancel };
    Change update(bool active, bool down, Clock::time_point now) {
        if (!active) return reset();
        if (!armed_) {
            if (!down) { armed_ = true; previous_ = false; }
            return Change::None;
        }
        if (down == previous_) return Change::None;
        previous_ = down;
        if (down) {
            if (waiting_ && now >= released_ && now - released_ <= std::chrono::milliseconds(350)) {
                waiting_ = false;
                holding_ = true;
                return Change::Begin;
            }
            waiting_ = false;
            pressed_ = now;
            return Change::None;
        }
        if (holding_) { holding_ = false; return Change::End; }
        if (now >= pressed_ && now - pressed_ <= std::chrono::milliseconds(250)) {
            waiting_ = true;
            released_ = now;
        } else waiting_ = false;
        return Change::None;
    }
    Change reset() {
        bool was_holding = holding_;
        armed_ = previous_ = waiting_ = holding_ = false;
        return was_holding ? Change::Cancel : Change::None;
    }
private:
    bool armed_ = false, previous_ = false, waiting_ = false, holding_ = false;
    Clock::time_point pressed_{}, released_{};
};

// Edge detector for hold-to-talk: first observed active state never starts recording.
class NeutralEdge {
public:
    enum class Change { None, Down, Up };
    Change update(bool active, bool down) {
        if (!active) { reset(); return Change::None; }
        if (!armed_) {
            if (!down) armed_ = true;
            return Change::None;
        }
        if (down == previous_) return Change::None;
        previous_ = down;
        return down ? Change::Down : Change::Up;
    }
    bool reset() {
        const bool was_down = armed_ && previous_;
        armed_ = previous_ = false;
        return was_down;
    }
private:
    bool armed_ = false, previous_ = false;
};
} // namespace frameyap
