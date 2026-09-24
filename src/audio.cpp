#include "audio.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace frameyap {
namespace { void check(bool result) { if (!result) throw std::runtime_error(std::string("Microphone: ") + SDL_GetError()); } }
Audio::~Audio() {
    close();
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
void Audio::prepare() {
    if (stream_) return;
    if (!initialized_) { check(SDL_InitSubSystem(SDL_INIT_AUDIO)); initialized_ = true; }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 16000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!stream_) throw std::runtime_error(std::string("Microphone: ") + SDL_GetError());
    try { check(SDL_ResumeAudioStreamDevice(stream_)); }
    catch (...) { close(); throw; }
    last_data_ = std::chrono::steady_clock::now();
}
void Audio::start() {
    if (recording_) throw std::runtime_error("Already recording");
    if (!stream_) throw std::runtime_error("Microphone unavailable; retry capture");
    // Clear queued idle data at the PTT boundary; the device never pauses or
    // reopens. Polling also discards idle samples so the queue stays small.
    check(SDL_ClearAudioStream(stream_));
    std::fill(pcm_.begin(), pcm_.end(), 0.0f); pcm_.clear(); pcm_.reserve(320000);
    started_ = last_data_ = std::chrono::steady_clock::now();
    recording_ = true;
}
void Audio::drain() {
    std::array<float, 4096> chunk{};
    // During idle and after the 20s bound, consume and discard device samples.
    // Never let idle audio accumulate for the next utterance.
    while (true) {
        int available = SDL_GetAudioStreamAvailable(stream_);
        check(available >= 0);
        if (available < int(sizeof(float))) break;
        const int count = static_cast<int>(std::min(chunk.size(), std::size_t(available) / sizeof(float)));
        int bytes = SDL_GetAudioStreamData(stream_, chunk.data(), count * sizeof(float));
        check(bytes >= 0);
        if (bytes == 0) break;
        if (bytes % sizeof(float)) throw std::runtime_error("Misaligned microphone samples");
        if (recording_) {
            const auto retain = std::min(std::size_t(bytes) / sizeof(float), 320000 - pcm_.size());
            for (std::size_t i = 0; i < retain; ++i) {
                if (!std::isfinite(chunk[i])) throw std::runtime_error("Invalid microphone samples");
                // Floating-point capture/resampling can exceed full scale. The
                // ASR API requires normalized PCM; saturate peaks, never scale
                // the whole clip or silently turn nonfinite input into speech.
                chunk[i] = std::clamp(chunk[i], -1.0f, 1.0f);
            }
            pcm_.insert(pcm_.end(), chunk.begin(), chunk.begin() + retain);
        }
        last_data_ = std::chrono::steady_clock::now();
    }
}
bool Audio::poll() {
    if (!stream_) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED && event.adevice.recording &&
            event.adevice.which == SDL_GetAudioStreamDevice(stream_))
            throw std::runtime_error("Recording device removed; clip discarded");
    }
    drain();
    if (std::chrono::steady_clock::now() - last_data_ > std::chrono::seconds(2))
        throw std::runtime_error("Microphone stalled; clip discarded");
    return recording_ && (pcm_.size() == 320000 || seconds() >= 20);
}
std::vector<float> Audio::finish() {
    if (!recording_) return {};
    try {
        drain();
        recording_ = false;
        auto result = std::move(pcm_);
        pcm_.clear();
        // Discard any resampler remainder and never pause the capture device.
        check(SDL_ClearAudioStream(stream_));
        return result;
    } catch (...) { cancel(); throw; }
}
void Audio::cancel() {
    recording_ = false;
    std::fill(pcm_.begin(), pcm_.end(), 0.0f); pcm_.clear();
    if (stream_) SDL_ClearAudioStream(stream_); // best effort on cancellation
}
void Audio::close() {
    cancel();
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}
int Audio::seconds() const {
    return recording_ ? int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count()) : 0;
}
}
