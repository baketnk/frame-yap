#pragma once
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace frameyap {
struct UpdateResult {
    enum class State { Current, Available, Failed } state;
    std::string version; // only populated for a newer, validated release
};
// The only network path is an explicit start() call from Settings. The helper
// emits a tiny protocol; polling never blocks the OpenVR interaction loop.
class UpdateCheck {
public:
    UpdateCheck(std::filesystem::path helper, std::string current, std::string python = "python3");
    ~UpdateCheck();
    UpdateCheck(const UpdateCheck&) = delete;
    UpdateCheck& operator=(const UpdateCheck&) = delete;
    bool start();
    std::optional<UpdateResult> poll();
    bool busy() const { return pid_ > 0; }
private:
    void stop() noexcept;
    std::filesystem::path helper_;
    std::string current_, python_, output_;
    pid_t pid_ = -1;
    int fd_ = -1;
    bool eof_ = false;
    std::chrono::steady_clock::time_point deadline_{};
};
// A separate explicit click opens a desktop terminal. The terminal waits for
// the user to quit FrameYap and press Enter; it survives FrameYap's own exit.
bool open_update_terminal(const std::filesystem::path& helper, const std::string& version);
}
