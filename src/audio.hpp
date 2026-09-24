#pragma once
#include <chrono>
#include <vector>
struct SDL_AudioStream;
namespace frameyap {
class Audio {
public:
    Audio() = default;
    ~Audio();
    void start();
    bool poll(); // true at 20s limit; throws if capture was lost/stalled
    std::vector<float> finish();
    void cancel();
    bool recording() const { return stream_ != nullptr; }
    int seconds() const;
private:
    void drain();
    SDL_AudioStream* stream_ = nullptr;
    bool initialized_ = false;
    std::vector<float> pcm_;
    std::chrono::steady_clock::time_point started_, last_data_;
};
}
