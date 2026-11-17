#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace AudioPlayer {

struct PlayOptions {
    bool loop = false;
    float volume = 1.0f;
};

bool Init();
void Shutdown();
void OnLuaReload();

std::vector<std::string> ListAudioFiles();
uint32_t Play(const std::string& filenameUtf8, const PlayOptions& options, std::string* outError = nullptr);
bool Stop(uint32_t voiceId);
void StopAll();
bool SetVolume(uint32_t voiceId, float volume);
bool IsPlaying(uint32_t voiceId);

} // namespace AudioPlayer
