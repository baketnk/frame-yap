#include "audio.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace frameyap {
namespace { void check(bool result) { if (!result) throw std::runtime_error(std::string("Microphone: ") + SDL_GetError()); } }
Audio::~Audio() { cancel(); }
void Audio::start() {
    if (stream_) throw std::runtime_error("Already recording");
    check(SDL_InitSubSystem(SDL_INIT_AUDIO)); initialized_ = true;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 16000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!stream_) { cancel(); check(false); }
    pcm_.clear(); pcm_.reserve(320000);
    started_ = last_data_ = std::chrono::steady_clock::now();
    try { check(SDL_ResumeAudioStreamDevice(stream_)); }
    catch (...) { cancel(); throw; }
}
void Audio::drain() {
    std::array<float, 4096> chunk{};
    while (pcm_.size() < 320000) {
        int available = SDL_GetAudioStreamAvailable(stream_);
        check(available >= 0);
        if (available < int(sizeof(float))) break;
        const int count = static_cast<int>(std::min({chunk.size(), std::size_t(available) / sizeof(float), 320000 - pcm_.size()}));
        int bytes = SDL_GetAudioStreamData(stream_, chunk.data(), count * sizeof(float));
        check(bytes >= 0);
        if (bytes == 0) break;
        if (bytes % sizeof(float)) throw std::runtime_error("Misaligned microphone samples");
        for (int i = 0; i < bytes / int(sizeof(float)); ++i)
            if (!std::isfinite(chunk[i])) throw std::runtime_error("Invalid microphone samples");
        pcm_.insert(pcm_.end(), chunk.begin(), chunk.begin() + bytes / sizeof(float));
        last_data_ = std::chrono::steady_clock::now();
    }
}
bool Audio::poll() {
    if (!stream_) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED && event.adevice.recording)
            throw std::runtime_error("Recording device removed; clip discarded");
    }
    drain();
    if (std::chrono::steady_clock::now() - last_data_ > std::chrono::seconds(2))
        throw std::runtime_error("Microphone stalled; clip discarded");
    return pcm_.size() == 320000 || seconds() >= 20;
}
std::vector<float> Audio::finish() {
    if (!stream_) return {};
    check(SDL_PauseAudioStreamDevice(stream_));
    check(SDL_FlushAudioStream(stream_));
    drain();
    auto result = std::move(pcm_);
    cancel();
    return result;
}
void Audio::cancel() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    // Best effort clearing of the owned buffer; no persistent recording/logging.
    std::fill(pcm_.begin(), pcm_.end(), 0.0f); pcm_.clear();
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    initialized_ = false;
}
int Audio::seconds() const {
    return stream_ ? int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count()) : 0;
}
}
