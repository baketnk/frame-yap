#pragma once
#include <cstdint>
#include <vector>
#define SDL_INIT_AUDIO 1
#define SDL_AUDIO_F32 2
#define SDL_AUDIO_DEVICE_DEFAULT_RECORDING 3
#define SDL_EVENT_AUDIO_DEVICE_REMOVED 4
using SDL_AudioDeviceID = unsigned;
struct SDL_AudioSpec { int format, channels, freq; };
struct SDL_AudioStream { std::vector<float> queued; };
struct SDL_Event { int type; struct { bool recording; SDL_AudioDeviceID which; } adevice; };
const char* SDL_GetError();
bool SDL_InitSubSystem(int);
void SDL_QuitSubSystem(int);
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec*, void*, void*);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream*);
int SDL_GetAudioStreamAvailable(SDL_AudioStream*);
int SDL_GetAudioStreamData(SDL_AudioStream*, void*, int);
bool SDL_ClearAudioStream(SDL_AudioStream*);
void SDL_DestroyAudioStream(SDL_AudioStream*);
bool SDL_PollEvent(SDL_Event*);
