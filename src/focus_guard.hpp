#pragma once

#include <memory>

namespace frameyap {

// One-shot observer for an explicitly armed Xwayland focus target. A guard may
// never be re-armed after arm() has been attempted or invalidate() is called.
class FocusGuard {
public:
    FocusGuard();
    ~FocusGuard();
    FocusGuard(const FocusGuard&) = delete;
    FocusGuard& operator=(const FocusGuard&) = delete;
    FocusGuard(FocusGuard&&) = delete;
    FocusGuard& operator=(FocusGuard&&) = delete;

    bool arm();
    bool valid();
    void invalidate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace frameyap
