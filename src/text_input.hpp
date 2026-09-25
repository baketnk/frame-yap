#pragma once
#include <memory>
#include <string>
namespace frameyap {
// Single-use authorization for an explicit user action. Sends retain the IME
// connection between leases: a compositor sync does not drain synthetic
// key events in the destination. This temporarily excludes other seat IMEs.
// Construction checks readiness without delivering input.
class TextInput {
public:
    explicit TextInput(const std::string& socket);
    ~TextInput();
    TextInput(const TextInput&) = delete;
    TextInput& operator=(const TextInput&) = delete;
    void text(const std::string& literal); // one validated batch, at most 24 codepoints
    void enter();
    // Release the retained, idle connection when disabling/stopping FrameYap.
    // Must have no active lease. Shutdown may wait the remaining <=150ms cooldown;
    // normal paced idle release has already waited. This is NOT a delivery ack.
    static void release_idle();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    bool used_ = false;
};
}
