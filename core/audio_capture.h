#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace AudioCapture {

struct SourceInfo {
    DWORD pid = 0;
    std::string exeName;
    std::string displayName;
    bool isActive = false;
    bool isGame = false;
};

struct Levels {
    float level = 0.0f;
    float bass = 0.0f;
    float lowMid = 0.0f;
    float highMid = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
    float beat = 0.0f;
};

// Lifecycle
void Init();
void Shutdown();

// Feature support & source enumeration
bool IsProcessLoopbackSupported();
std::vector<SourceInfo> ListSources();

// Source selection ("system" or executable name like "Spotify.exe")
bool SetSource(const std::string& sourceName);
std::string GetSource();

// Master capture toggle (OFF by default)
void SetEnabled(bool enabled);
bool IsEnabled();

// Status reporting (e.g. "capturing", "waiting", "off", "error")
void GetStatus(std::string* outState, std::string* outMessage);

// Live smoothed metrics for Lua & UI
Levels GetLevels();
void GetSpectrum(float outSpectrum[32]);

// Tuning
void SetGain(float gain);
float GetGain();

void SetSmoothing(float attack, float release);
void GetSmoothing(float* outAttack, float* outRelease);

// Fast render-thread reader for CarFX constant buffer b13
void GetShaderAudioData(float audio[4], float audio2[4], float* spectrum32 = nullptr);

} // namespace AudioCapture
