#include "focus_guard.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using frameyap::FocusGuard;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #condition); } while (false)

class DisplaySetting {
public:
    DisplaySetting() {
        if (const char* current = std::getenv("DISPLAY")) {
            previous_ = current;
            had_previous_ = true;
        }
        if (setenv("DISPLAY", "frameyap-no-such-display:9876", 1) != 0)
            throw std::runtime_error("could not set isolated DISPLAY");
    }
    ~DisplaySetting() {
        if (had_previous_) setenv("DISPLAY", previous_.c_str(), 1);
        else unsetenv("DISPLAY");
    }
    DisplaySetting(const DisplaySetting&) = delete;
    DisplaySetting& operator=(const DisplaySetting&) = delete;
private:
    std::string previous_;
    bool had_previous_ = false;
};

int main() {
    try {
        DisplaySetting isolated_display;
        FocusGuard guard; // Construction must not connect to X.
        CHECK(!guard.valid());
        CHECK(!guard.arm()); // No server: fail closed.
        CHECK(!guard.valid());
        CHECK(!guard.arm()); // No reconnect/rearm attempt.
        guard.invalidate();
        CHECK(!guard.valid());
        FocusGuard explicitly_invalidated;
        explicitly_invalidated.invalidate();
        CHECK(!explicitly_invalidated.arm());
        CHECK(!explicitly_invalidated.valid());
        std::cout << "focus_guard fail-closed policy checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "focus_guard_test: " << e.what() << '\n';
        return 1;
    }
}
