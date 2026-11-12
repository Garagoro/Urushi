#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>
#include "carfx_shader_decl.h"

namespace CarFX {

constexpr UINT kCbSlot = 13;        // register(b13)
constexpr UINT kSceneSlot = 32;     // t32..t38 (scene, tex0..3, depth, livery)
constexpr UINT kViewCount = 7;      // 32:scene, 33..36:tex0..3, 37:depth, 38:livery
constexpr int kMaxGhostTargets = 8;

enum Target : int {
    kTargetNone      = -1,
    kTargetBody      = 0,
    kTargetInternals = 1,   // engine / suspension / interior parts of the player car (matched by their textures)
    kTargetGhostBase = 2,
    kTargetCount     = kTargetGhostBase + kMaxGhostTargets
};

inline bool IsGhostTarget(int target) {
    return target >= kTargetGhostBase && target < kTargetCount;
}

inline int GhostTargetFromSlot(int slotIdx) {
    return (slotIdx >= 0 && slotIdx < kMaxGhostTargets) ? (kTargetGhostBase + slotIdx) : kTargetNone;
}

struct ReplayShaderConfig {
    bool enabled = false;
    int overrideMode = 0;     // OverrideMode
    int overrideStealth = 0;  // OverrideStealth
    std::string shaderPath;
    float params[8][4] = {};
    std::string texPaths[4];
};

enum OverrideMode : int {
    kModeAuto    = 0,  // Use @mode from shader (defaults to replace if omitted)
    kModeReplace = 1,
    kModeOverlay = 2
};

enum OverrideStealth : int {
    kStealthAuto = 0,  // Use @stealth 1 from shader
    kStealthOff  = 1,
    kStealthOn   = 2
};

struct TargetTexturesInput {
    void* nativeAlbedoPtr = nullptr;    // GetNativeTexturePtr_Injected return value (SRV or Resource)
    void* nativePackedPtr = nullptr;
    uint32_t albedoTexId = 0;
    uint32_t packedTexId = 0;
    bool albedoIsDefault = true;
    bool packedIsDefault = true;
    bool valid = false;
};

struct SpatialProximityInput {
    float carPos[4] = { 0.0f, 0.0f, 0.0f, 0.95f };         // xyz: world origin, w: halfWidth
    float carRight[4] = { 1.0f, 0.0f, 0.0f, 2.20f };       // xyz: world right, w: halfLength
    float carUp[4] = { 0.0f, 1.0f, 0.0f, 1.30f };          // xyz: world up, w: height
    float carFwd[4] = { 0.0f, 0.0f, 1.0f, 0.0f };          // xyz: world forward, w: spatialValid (0/1)
    float camPos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };          // xyz: main camera world pos, w: camValid (0/1)

    float nearCarPos[4] = { 0.0f, 0.0f, 0.0f, 999.0f };    // xyz: nearest car OBB center, w: car gap (m)
    float nearCarRight[4] = { 1.0f, 0.0f, 0.0f, 0.96f };   // xyz: right, w: halfWidth
    float nearCarFwd[4] = { 0.0f, 0.0f, 1.0f, 2.25f };     // xyz: forward, w: halfLength
    float nearCarClosest[4] = { 0.0f, 0.0f, 0.0f, 0.65f }; // xyz: closest point on other car OBB, w: halfHeight

    float wallDirDist[4] = { 999.0f, 999.0f, 999.0f, 999.0f }; // x:Front, y:Rear, z:Left, w:Right gap (m)
    float wallNearestHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };    // xyz: closest wall point, w: min gap (m)
    float wallNearestNormal[4] = { 0.0f, 1.0f, 0.0f, 0.0f };   // xyz: wall normal, w: hitMask (F=1,B=2,L=4,R=8)
    float wallFrontHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallRearHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallLeftHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallRightHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallDoorLeftHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };   // xyz: left door (between wheels), w: gap (m)
    float wallDoorRightHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };  // xyz: right door (between wheels), w: gap (m)
    float wallFrontLeftHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallFrontRightHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallRearLeftHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float wallRearRightHit[4] = { 0.0f, 0.0f, 0.0f, 999.0f };
    float driftState[4] = { 0.0f, 0.0f, 0.0f, 0.0f };        // x: slipDeg, y: signedSlipDeg, z: norm65, w: speedKmh
    float camRight[4] = { 1.0f, 0.0f, 0.0f, 0.0f };          // xyz: main camera right axis (world)
    float camUp[4]    = { 0.0f, 1.0f, 0.0f, 0.0f };          // xyz: main camera up axis
    float camFwd[4]   = { 0.0f, 0.0f, 1.0f, 0.0f };          // xyz: main camera forward axis, w: basis valid (0/1)
};

struct DiagnosticStatus {
    bool initialized = false;
    bool enabled = false;
    bool hookedDraw = false;
    bool compiled = false;
    int activeMode = kModeAuto;         // OverrideMode
    int resolvedMode = kDeclModeReplace;
    int activeStealth = kStealthAuto;   // OverrideStealth
    bool declStealth = false;
    bool resolvedStealth = false;
    bool usesScene = false;
    bool usesDepth = false;
    bool usesLivery = false;

    uint32_t totalDrawsFrame = 0;
    uint32_t matchedDrawsFrame = 0;
    uint32_t matchedDrawsLastFrame = 0;
    uint32_t sceneCopiesTotal = 0;
    uint32_t depthCopiesTotal = 0;
    uint32_t compilesTotal = 0;
    uint32_t compileFailures = 0;

    bool bodyTargetsValid = false;
    bool bodyAlbedoIsDefault = true;
    bool bodyPackedIsDefault = true;
    uint32_t bodyAlbedoTexId = 0;
    uint32_t bodyPackedTexId = 0;
    uintptr_t bodyAlbedoSrv = 0;
    uintptr_t bodyPackedSrv = 0;

    uint32_t lastSlotMask = 0;
    uint32_t lastRtWidth = 0;
    uint32_t lastRtHeight = 0;
    uint32_t lastRtFormat = 0;
    uintptr_t lastGamePs = 0;

    std::string shaderPath;
    std::string lastError;
    ShaderDecl decl{};
};

using DrawIssueFn = void (*)(void* ctx);

// Initialize / shutdown on D3D11 device lifetime
bool Init(ID3D11Device* device, ID3D11DeviceContext* immediateContext, const std::wstring& shadersDir);
void Shutdown();

// Frame tick (called from D3D11Hook::Hooked_Present)
void OnNewFrame(ID3D11DeviceContext* context);
void SetHookedDrawFlag(bool hooked);

// Update player / ghost car texture SRVs from Unity Main Thread
void SetPlayerTargetTextures(int target, const TargetTexturesInput& input);
void ClearTargetTextures(int target);

// Shader management (thread-safe)
bool LoadShader(int target, const std::string& pathUtf8, std::string* outError = nullptr);
void UnloadShader(int target);
bool ReloadShader(int target, std::string* outError = nullptr);
void PollHotReload();
ReplayShaderConfig CaptureActiveConfig(int target = kTargetBody);
bool ApplyConfigToTarget(int target, const ReplayShaderConfig& cfg, std::string* outError = nullptr);
void SyncTargetFromSource(int dstTarget, int srcTarget);
void ResetGhostTargets();

// Runtime controls
void SetEnabled(bool enabled);
bool IsEnabled();
void SetTargetEnabled(int target, bool enabled);
bool IsTargetCompiled(int target);
bool IsTargetActive(int target);
bool ShouldBypassDepthPrePass(int target = kTargetBody);
bool ShouldHideCarInternals(int target = kTargetBody);
void SetStealthDepthWrite(bool on);
// Native texture pointers (albedo) of the player car internals; draws using any of them go to kTargetInternals.
void SetInternalsTextures(const std::vector<void*>& nativePtrs);   // body replace draws also write depth (body is drawn before the internals)
void SetOverrideMode(int target, int overrideMode);
int GetOverrideMode(int target);
void SetOverrideStealth(int target, int overrideStealth);
int GetOverrideStealth(int target);

// Parameter & texture control
bool SetParamByName(int target, const char* name, const float* values, int count);
bool GetParamByName(int target, const char* name, float* outValues, int maxCount);
bool SetParamSlot(int target, int slot, const float* values, int count);
bool GetParamSlot(int target, int slot, float out[4]);
bool SetUserTexture(int target, int texSlot, const std::string& imagePathUtf8, std::string* outError = nullptr);
void ClearUserTexture(int target, int texSlot);
void SetGameInput(int index, float value, int target = kTargetBody);
float GetGameInput(int index, int target = kTargetBody);
void SetSpatialProximity(const SpatialProximityInput& input, int target = kTargetBody);
SpatialProximityInput GetSpatialProximity(int target = kTargetBody);

// Draw interception entry point (called from D3D11 DrawIndexed / DrawIndexedInstanced hooks)
// Returns true if CarFX handled & issued the draw(s); false if caller should invoke original as normal.
bool TryHandleDraw(ID3D11DeviceContext* context, UINT countPerInstance, DrawIssueFn issueFn, void* issueCtx);

// Inspection for Lua / ImGui
DiagnosticStatus GetStatus(int target = kTargetBody);
std::vector<std::string> ListAvailableShaders();
std::wstring GetShadersDirectory();

} // namespace CarFX
