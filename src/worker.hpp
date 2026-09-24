#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frameyap {

struct WorkerReply {
    uint64_t id;
    std::string text;
    std::string error;
};

class Worker {
public:
    explicit Worker(std::chrono::milliseconds warmup_timeout = std::chrono::seconds(120),
                    std::chrono::milliseconds request_timeout = std::chrono::seconds(60));
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void start(const std::string& python, const std::string& script,
               const std::string& model, int threads = 2, bool advanced_debug = false);
    bool ready() const;
    void submit(uint64_t id, const std::vector<float>& pcm);
    std::optional<WorkerReply> poll();
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace frameyap
