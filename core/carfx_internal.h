#pragma once

#include "carfx.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace CarFX {
namespace Internal {

constexpr DWORD kMaxSourceBytes = 1u << 20;
constexpr ULONGLONG kRetireDelayMs = 2000;
constexpr UINT kMaxTexSide = 8192;
constexpr int kScanStartSlot = 12;
constexpr int kScanSlotCount = 20;

constexpr GUID kIidShaderReflection = {
    0x8D536CA1, 0x0CCA, 0x4956, { 0xA8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 }
};
using D3DReflectFn = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

struct FxConstants {
    float time[4];
    float screen[4];
    float param[kParams][4];
    float frameInfo[4];
    float game[kGameInputs / 4][4];

    float carPos[4];
    float carRight[4];
    float carUp[4];
    float carFwd[4];
    float camPos[4];

    float nearCarPos[4];
    float nearCarRight[4];
    float nearCarFwd[4];
    float nearCarClosest[4];

    float wallDirDist[4];
    float wallNearestHit[4];
    float wallNearestNormal[4];
    float wallFrontHit[4];
    float wallRearHit[4];
    float wallLeftHit[4];
    float wallRightHit[4];
    float wallDoorLeftHit[4];
    float wallDoorRightHit[4];
    float wallFrontLeftHit[4];
    float wallFrontRightHit[4];
    float wallRearLeftHit[4];
    float wallRearRightHit[4];
    float driftState[4];
    float camRight[4];
    float camUp[4];
    float camFwd[4];
};
static_assert(sizeof(FxConstants) % 16 == 0, "CarFx constant buffer must align to 16 bytes");

struct BoundState {
    bool active = false;
    bool restoreOm = false;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* srvs[kViewCount] = {};
    ID3D11SamplerState* samplers[2] = {};
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* ds = nullptr;
    UINT stencilRef = 0;
};

struct RetiredObject {
    IUnknown* obj = nullptr;
    ULONGLONG releaseAt = 0;
};

struct CompiledEffect {
    ID3D11PixelShader* ps = nullptr;
    ShaderDecl decl{};
    bool usesScene = false;
    bool usesDepth = false;
    bool usesLivery = false;
    bool usesTex[kTextures] = {};
    ULONGLONG refDummy = 0;
};

struct TargetSlotState {
    std::mutex publishMtx;
    CompiledEffect* published = nullptr;
    std::atomic<ID3D11PixelShader*> ps{ nullptr };
    std::atomic<bool> slotEnabled{ true };
    std::atomic<int> declMode{ kDeclModeReplace };
    std::atomic<int> overrideMode{ kModeAuto };
    std::atomic<bool> declStealth{ false };
    std::atomic<int> overrideStealth{ kStealthAuto };
    std::atomic<bool> usesScene{ false };
    std::atomic<bool> usesDepth{ false };
    std::atomic<bool> usesLivery{ false };
    std::atomic<float> param[kParams][4]{};
    std::atomic<ID3D11ShaderResourceView*> userTex[kTextures]{};
    std::string userTexPaths[kTextures];

    std::string shaderPath;
    FILETIME lastWriteTime{};
    ShaderDecl decl{};
    std::string lastError;
    uint32_t compiles = 0;
    uint32_t failures = 0;
    uint32_t syncedSrcCompiles = 0;
    LARGE_INTEGER loadQpc{};
};

struct GameSrvSnapshot {
    struct ExtraResPair {
        uintptr_t albedoPtrRaw = 0;
        uintptr_t packedPtrRaw = 0;
        ID3D11ShaderResourceView* albedoSrv = nullptr;
        ID3D11ShaderResourceView* packedSrv = nullptr;
        ID3D11Resource* albedoRes = nullptr;
        ID3D11Resource* packedRes = nullptr;
        bool albedoIsDefault = true;
        bool packedIsDefault = true;
    };

    uintptr_t albedoPtrRaw = 0;
    uintptr_t packedPtrRaw = 0;
    ID3D11ShaderResourceView* albedoSrv = nullptr;
    ID3D11ShaderResourceView* packedSrv = nullptr;
    ID3D11Resource* albedoRes = nullptr;
    ID3D11Resource* packedRes = nullptr;
    uint32_t albedoTexId = 0;
    uint32_t packedTexId = 0;
    bool albedoIsDefault = true;
    bool packedIsDefault = true;
    bool valid = false;
    std::vector<ExtraResPair> extraPairs;
};

struct InternalsTexSet {
    std::vector<ID3D11ShaderResourceView*> srvs;
    std::vector<ID3D11Resource*> res;
    std::vector<uintptr_t> raw;
};

inline bool ValidTarget(int t) { return t >= 0 && t < kTargetCount; }

// Shared module state (defined in carfx.cpp)
extern ID3D11Device* g_device;
extern ID3D11DeviceContext* g_immediate;
extern std::wstring g_shadersDir;
extern std::atomic<bool> g_initialized;
extern std::atomic<bool> g_enabled;
extern std::atomic<bool> g_hookedDraw;

extern HMODULE g_d3dCompilerMod;
extern pD3DCompile g_pfnD3DCompile;
extern D3DReflectFn g_pfnD3DReflect;

extern ID3D11Buffer* g_cb;
extern ID3D11SamplerState* g_samplerLinear;
extern ID3D11SamplerState* g_samplerPoint;
extern ID3D11BlendState* g_overlayBlend;
extern ID3D11DepthStencilState* g_overlayDs;
extern ID3D11DepthStencilState* g_bodyDepthDs;
extern std::atomic<bool> g_stealthDepthWrite;

extern ID3D11Texture2D* g_sceneTex;
extern ID3D11ShaderResourceView* g_sceneSrv;
extern D3D11_TEXTURE2D_DESC g_sceneDesc;

extern ID3D11Texture2D* g_depthTex;
extern ID3D11ShaderResourceView* g_depthSrv;
extern D3D11_TEXTURE2D_DESC g_depthDesc;

extern TargetSlotState g_targets[kTargetCount];
extern std::atomic<uint32_t> g_activeGhostShaderMask;

extern SRWLOCK g_gameTexLock;
extern GameSrvSnapshot g_gameTex[kTargetCount];
extern std::atomic<uint64_t> g_gameTexGen;

extern InternalsTexSet g_internals;
extern std::atomic<uint64_t> g_internalsGen;

extern std::atomic<float> g_gameInputs[kTargetCount][kGameInputs];

extern SRWLOCK g_spatialLock[kTargetCount];
extern SpatialProximityInput g_spatialInput[kTargetCount];

extern std::mutex g_graveMtx;
extern std::vector<RetiredObject> g_graveyard;

extern LARGE_INTEGER g_qpcFreq;
extern std::atomic<int64_t> g_lastDrawQpc;
extern std::atomic<uint32_t> g_frameCounter;
extern uint32_t g_sceneCopiedFrame;
extern uint32_t g_depthCopiedFrame;

extern std::atomic<uint32_t> g_totalDrawsFrame;
extern std::atomic<uint32_t> g_totalDrawsLastFrame;
extern std::atomic<uint32_t> g_matchedDrawsFrame;
extern std::atomic<uint32_t> g_matchedDrawsLastFrame;
extern std::atomic<uint32_t> g_targetMatchedFrame[kTargetCount];
extern std::atomic<uint32_t> g_targetMatchedLastFrame[kTargetCount];
extern std::atomic<uint32_t> g_sceneCopiesTotal;
extern std::atomic<uint32_t> g_depthCopiesTotal;
extern std::atomic<uint32_t> g_lastSlotMask;
extern std::atomic<uint32_t> g_lastRtWidth;
extern std::atomic<uint32_t> g_lastRtHeight;
extern std::atomic<uint32_t> g_lastRtFormat;
extern std::atomic<uintptr_t> g_lastGamePs;
extern std::atomic<int> g_hintSlot;

extern ULONGLONG g_nextHotReloadCheckMs;

// Core SEH / lifecycle helpers (implemented in carfx.cpp)
template <typename T>
inline HRESULT SafeQueryInterfaceRaw(void* rawPtr, REFIID iid, T** outObj) {
    if (!rawPtr || !outObj) return E_INVALIDARG;
    *outObj = nullptr;
    const uintptr_t p = reinterpret_cast<uintptr_t>(rawPtr);
    if (p < 0x10000) return E_POINTER;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(rawPtr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return E_POINTER;
    }
    __try {
        IUnknown* unk = reinterpret_cast<IUnknown*>(rawPtr);
        void* vtbl = *reinterpret_cast<void**>(rawPtr);
        if (!vtbl || reinterpret_cast<uintptr_t>(vtbl) < 0x10000) return E_POINTER;
        return unk->QueryInterface(iid, reinterpret_cast<void**>(outObj));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outObj = nullptr;
        return E_FAIL;
    }
}

void SafeReleaseRaw(IUnknown* unk);
void UpdateGhostShaderMask();
bool IsTargetActiveInternal(int target);
void Retire(IUnknown* obj);
void SweepGraveyard(bool releaseAll);
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& w);
std::wstring ResolvePathRawW(const std::string& pathUtf8);
std::wstring ResolvePathW(const std::string& pathUtf8);
std::wstring DirectoryOf(const std::wstring& filePath);
bool ReadFileUtf8(const std::wstring& path, std::vector<char>& outData, FILETIME& outTime);

// Shader compile helpers (implemented in carfx_shader.cpp)
bool CompileEffectMemory(int targetIdx, const char* srcData, size_t srcLen,
                         const std::string& narrowPath, const std::wstring& includeDir,
                         const FILETIME& ft, std::string* outErr);
bool CompileEffectFile(int targetIdx, const std::wstring& fullPath, std::string* outErr);

// Draw / RT helpers (implemented in carfx_draw.cpp)
void ReleaseSceneCopy();
void ReleaseDepthCopy();

} // namespace Internal
} // namespace CarFX
