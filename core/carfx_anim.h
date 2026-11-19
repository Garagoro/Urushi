#pragma once

#include "carfx.h"
#include <d3d11.h>
#include <string>
#include <vector>

namespace CarFX {
namespace Anim {

// Frame resolution budgets
constexpr UINT kDefaultMaxDimension = 1024;    // Default maximum long-side resolution (downscaled if larger)
constexpr UINT kHardMaxDimension = 2048;       // Hard maximum long-side resolution
// Memory budget: 256 MB decoded memory cap per clip
constexpr size_t kMaxAnimMemoryBytes = 256 * 1024 * 1024;
// Frame count budget to prevent runaway memory allocation
constexpr UINT kMaxAnimFrameCount = 600;
// Default FPS for sequence folders
constexpr float kDefaultSequenceFps = 24.0f;

enum class State {
    None,
    Loading,
    Ready,
    Error
};

enum class LoopMode {
    Loop,
    PingPong,
    Once
};

struct PlaybackConfig {
    bool hasPlaying = false;
    bool playing = true;
    bool hasSpeed = false;
    float speed = 1.0f;
    bool hasMode = false;
    LoopMode mode = LoopMode::Loop;
    bool hasFps = false;
    float fps = 24.0f;
    bool hasFrame = false;
    int frame = 0;
};

struct SlotInfo {
    std::string file;
    bool animated = false;
    int frames = 0;
    float fps = 0.0f;
    float duration = 0.0f;
    int width = 0;
    int height = 0;
    bool playing = false;
    float speed = 1.0f;
    std::string mode = "loop";
    int frame = 0;
    std::string state = "ready"; // "loading" | "ready" | "error"
    std::string error;
};

// Lifecycle
void Init();
void Shutdown();
void OnNewFrame(ID3D11Device* device, double dtSec);
void OnSceneTransition();

// Loading & slot clearing
bool StartLoad(int target, int slot, const std::string& pathUtf8, const std::wstring& fullPathW, bool isDirectory, std::string* outError);
void StopAndClearSlot(int target, int slot);
void StopAndClearTarget(int target);

// Playback control & query
bool SetPlayback(int target, int slot, const PlaybackConfig& cfg);
SlotInfo GetInfo(int target, int slot);
bool IsSlotAnimated(int target, int slot);

// Fill b13 cbuffer texAnim array for a target
void GetConstantData(int target, float outData[kTextures][4]);

// Enumerate textures & sequence folders inside shaders/
std::vector<std::string> ListCandidateTextures();

} // namespace Anim
} // namespace CarFX
