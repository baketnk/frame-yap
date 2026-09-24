#include "audio.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
SDL_AudioStream* current = nullptr;
int opens = 0, resumes = 0, closes = 0;
bool removed = false;
void feed(float value, int count) { assert(current); current->queued.insert(current->queued.end(), count, value); }
}
const char* SDL_GetError() { return "fake failure"; }
bool SDL_InitSubSystem(int) { return true; }
void SDL_QuitSubSystem(int) {}
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec*, void*, void*) {
    ++opens; return current = new SDL_AudioStream;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*) { ++resumes; return true; }
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream*) { return 42; }
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) { return int(stream->queued.size() * sizeof(float)); }
int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* data, int size) {
    int count = std::min(int(stream->queued.size()), size / int(sizeof(float)));
    std::memcpy(data, stream->queued.data(), count * sizeof(float));
    stream->queued.erase(stream->queued.begin(), stream->queued.begin() + count);
    return count * sizeof(float);
}
bool SDL_ClearAudioStream(SDL_AudioStream* stream) { stream->queued.clear(); return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* stream) { ++closes; current = nullptr; delete stream; }
bool SDL_PollEvent(SDL_Event* event) {
    if (!removed) return false;
    removed = false; *event = SDL_Event{SDL_EVENT_AUDIO_DEVICE_REMOVED, {true, 42}}; return true;
}
int main() {
    using frameyap::Audio;
    {
        Audio audio;
        audio.prepare(); audio.prepare();
        assert(opens == 1 && resumes == 1 && audio.open() && !audio.recording());
        feed(0.7f, 10000); audio.poll();
        assert(current->queued.empty());
        feed(0.8f, 200); audio.start(); // queued idle speech cannot leak
        feed(0.1f, 3200); audio.poll();
        feed(0.2f, 3200);
        auto clip = audio.finish();
        assert(clip.size() == 6400 && clip.front() == 0.1f && clip.back() == 0.2f);
        assert(audio.open() && !audio.recording() && opens == 1 && closes == 0 && resumes == 1);
        feed(0.5f, 200); audio.start(); feed(0.3f, 4000);
        audio.cancel(); assert(current->queued.empty());
        feed(0.9f, 100); audio.start(); feed(0.4f, 3200);
        clip = audio.finish(); assert(clip.size() == 3200 && clip.front() == 0.4f);
        assert(opens == 1 && closes == 0 && resumes == 1);
        // Real float capture can exceed full scale. Both poll and finish must
        // saturate finite peaks without changing ordinary samples or clip size.
        audio.start();
        feed(1.01f, 1600); feed(-1.25f, 1600); audio.poll();
        feed(std::numeric_limits<float>::max(), 1);
        feed(-std::numeric_limits<float>::max(), 1);
        feed(1.f, 1); feed(-1.f, 1); feed(.25f, 1); feed(0.f, 1);
        clip = audio.finish();
        assert(clip.size() == 3206);
        assert(std::all_of(clip.begin(), clip.begin() + 1600, [](float x) { return x == 1.f; }));
        assert(std::all_of(clip.begin() + 1600, clip.begin() + 3200, [](float x) { return x == -1.f; }));
        assert(clip[3200] == 1.f && clip[3201] == -1.f && clip[3202] == 1.f && clip[3203] == -1.f);
        assert(clip[3204] == .25f && clip[3205] == 0.f);
        for (float invalid : {std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
            audio.start(); feed(.2f, 3200); feed(invalid, 1);
            bool rejected = false;
            try { (void)audio.finish(); } catch (const std::runtime_error&) { rejected = true; }
            assert(rejected && !audio.recording() && current->queued.empty());
            audio.start(); feed(.3f, 3200);
            clip = audio.finish();
            assert(clip.size() == 3200 && clip.front() == .3f); // no failed clip tail leaks
        }
        assert(opens == 1 && closes == 0 && resumes == 1);
        removed = true;
        bool failed = false;
        try { audio.poll(); } catch (const std::runtime_error&) { failed = true; }
        assert(failed);
        audio.close(); assert(closes == 1 && !audio.open());
        audio.prepare(); assert(opens == 2 && resumes == 2);
    }
    assert(closes == 2);
    std::cout << "audio lifecycle checks passed (fake device only)\n";
}
