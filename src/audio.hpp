#pragma once
#include <chrono>
#include <vector>
struct SDL_AudioStream;
namespace frameyap {
class Audio {
public:
    Audio() = default;
    ~Audio();
    void prepare(); // open/resume once, while app is ready; never retain idle samples
    void start(); // arm the already-running stream without touching the device
    bool poll(); // drain even while idle; true at 20s limit; throws on loss/stall
    std::vector<float> finish(); // finite PCM saturated to [-1, 1] for the ASR API
    void cancel(); // discard current clip, leave the device running
    void close(); // release device on shutdown or capture failure
    bool recording() const { return recording_; }
    bool open() const { return stream_ != nullptr; }
    int seconds() const;
private:
    void drain();
    SDL_AudioStream* stream_ = nullptr;
    bool initialized_ = false;
    bool recording_ = false;
    std::vector<float> pcm_;
    std::chrono::steady_clock::time_point started_, last_data_;
};
}
