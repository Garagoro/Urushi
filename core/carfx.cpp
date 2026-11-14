#include "carfx.h"
#include "carfx_internal.h"
#include "logger.h"
#include "native_carx.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace CarFX {
using namespace CarFX::Internal;

namespace Internal {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_immediate = nullptr;
std::wstring g_shadersDir;
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_enabled{ true };
std::atomic<bool> g_hookedDraw{ false };

HMODULE g_d3dCompilerMod = nullptr;
pD3DCompile g_pfnD3DCompile = nullptr;
D3DReflectFn g_pfnD3DReflect = nullptr;

ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_samplerLinear = nullptr;
ID3D11SamplerState* g_samplerPoint = nullptr;
ID3D11BlendState* g_overlayBlend = nullptr;
ID3D11DepthStencilState* g_overlayDs = nullptr;
ID3D11DepthStencilState* g_bodyDepthDs = nullptr;
std::atomic<bool> g_stealthDepthWrite{ false };

// Scene & Depth copy cache (render thread)
ID3D11Texture2D* g_sceneTex = nullptr;
ID3D11ShaderResourceView* g_sceneSrv = nullptr;
D3D11_TEXTURE2D_DESC g_sceneDesc{};

ID3D11Texture2D* g_depthTex = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
D3D11_TEXTURE2D_DESC g_depthDesc{};

TargetSlotState g_targets[kTargetCount];
std::atomic<uint32_t> g_activeGhostShaderMask{ 0 };

// Game SRVs double-check lock (main thread writes, render thread reads)
SRWLOCK g_gameTexLock = SRWLOCK_INIT;
GameSrvSnapshot g_gameTex[kTargetCount];
std::atomic<uint64_t> g_gameTexGen{ 1 };

// Internals target: set of textures of the car's non-body parts (guarded by g_gameTexLock; render thread keeps a cached copy)
InternalsTexSet g_internals;
std::atomic<uint64_t> g_internalsGen{ 1 };

std::atomic<float> g_gameInputs[kTargetCount][kGameInputs]{};

SRWLOCK g_spatialLock[kTargetCount] = {};
SpatialProximityInput g_spatialInput[kTargetCount]{};


std::mutex g_graveMtx;
std::vector<RetiredObject> g_graveyard;

LARGE_INTEGER g_qpcFreq{};
std::atomic<int64_t> g_lastDrawQpc{ 0 };
std::atomic<uint32_t> g_frameCounter{ 0 };
uint32_t g_sceneCopiedFrame = ~0u;
uint32_t g_depthCopiedFrame = ~0u;

// Diagnostics
std::atomic<uint32_t> g_totalDrawsFrame{ 0 };
std::atomic<uint32_t> g_totalDrawsLastFrame{ 0 };
std::atomic<uint32_t> g_matchedDrawsFrame{ 0 };
std::atomic<uint32_t> g_matchedDrawsLastFrame{ 0 };
std::atomic<uint32_t> g_targetMatchedFrame[kTargetCount]{};
std::atomic<uint32_t> g_targetMatchedLastFrame[kTargetCount]{};
std::atomic<uint32_t> g_sceneCopiesTotal{ 0 };
std::atomic<uint32_t> g_depthCopiesTotal{ 0 };
std::atomic<uint32_t> g_lastSlotMask{ 0 };
std::atomic<uint32_t> g_lastRtWidth{ 0 };
std::atomic<uint32_t> g_lastRtHeight{ 0 };
std::atomic<uint32_t> g_lastRtFormat{ 0 };
std::atomic<uintptr_t> g_lastGamePs{ 0 };
std::atomic<int> g_hintSlot{ 15 };

ULONGLONG g_nextHotReloadCheckMs = 0;


void UpdateGhostShaderMask() {
    uint32_t m = 0;
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        if (g_targets[t].slotEnabled.load(std::memory_order_acquire) &&
            g_targets[t].ps.load(std::memory_order_acquire) != nullptr) {
            m |= (1u << (t - kTargetGhostBase));
        }
    }
    g_activeGhostShaderMask.store(m, std::memory_order_release);
}

bool IsTargetActiveInternal(int target) {
    if (!ValidTarget(target)) return false;
    if (IsGhostTarget(target)) {
        return g_targets[target].slotEnabled.load(std::memory_order_acquire) &&
               (g_targets[target].ps.load(std::memory_order_acquire) != nullptr);
    }
    return g_enabled.load(std::memory_order_acquire) &&
           g_targets[target].slotEnabled.load(std::memory_order_acquire) &&
           (g_targets[target].ps.load(std::memory_order_acquire) != nullptr);
}

void SafeReleaseRaw(IUnknown* unk) {
    if (!unk) return;
    __try {
        unk->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Retire(IUnknown* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_graveMtx);
    g_graveyard.push_back({ obj, GetTickCount64() + kRetireDelayMs });
}

void SweepGraveyard(bool releaseAll) {
    std::vector<IUnknown*> toRelease;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_graveMtx);
        for (size_t i = 0; i < g_graveyard.size();) {
            if (releaseAll || now >= g_graveyard[i].releaseAt) {
                toRelease.push_back(g_graveyard[i].obj);
                g_graveyard[i] = g_graveyard.back();
                g_graveyard.pop_back();
            } else {
                ++i;
            }
        }
    }
    for (IUnknown* p : toRelease) {
        SafeReleaseRaw(p);
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring ResolvePathRawW(const std::string& pathUtf8) {
    std::wstring w = Utf8ToWide(pathUtf8);
    if (w.empty()) return {};
    // Absolute if drive letter or UNC
    if ((w.size() >= 2 && w[1] == L':') || (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\')) {
        return w;
    }
    if (!g_shadersDir.empty()) {
        // Strip leading "shaders/" or "shaders\" if caller passed relative to game root
        if (w.rfind(L"shaders\\", 0) == 0 || w.rfind(L"shaders/", 0) == 0) {
            return g_shadersDir.substr(0, g_shadersDir.rfind(L"shaders")) + w;
        }
        return g_shadersDir + L"\\" + w;
    }
    return w;
}

// Paths come from Lua (carfx.load / carfx.set_tex): only files inside the game shaders folder are reachable,
// so a script can neither probe nor compile/read arbitrary files.
std::wstring ResolvePathW(const std::string& pathUtf8) {
    std::wstring resolved = ResolvePathRawW(pathUtf8);
    if (resolved.empty() || g_shadersDir.empty()) return {};
    wchar_t full[MAX_PATH * 2] = {};
    wchar_t base[MAX_PATH * 2] = {};
    const DWORD n1 = GetFullPathNameW(resolved.c_str(), MAX_PATH * 2, full, nullptr);
    const DWORD n2 = GetFullPathNameW(g_shadersDir.c_str(), MAX_PATH * 2, base, nullptr);
    if (n1 == 0 || n1 >= MAX_PATH * 2 || n2 == 0 || n2 >= MAX_PATH * 2) return {};
    std::wstring f(full), b(base);
    if (!b.empty() && b.back() != L'\\') b += L'\\';
    if (f.size() <= b.size() || _wcsnicmp(f.c_str(), b.c_str(), b.size()) != 0) return {};
    return f;
}

std::wstring DirectoryOf(const std::wstring& filePath) {
    size_t pos = filePath.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring{} : filePath.substr(0, pos);
}

bool ReadFileUtf8(const std::wstring& path, std::vector<char>& outData, FILETIME& outTime) {
    outData.clear();
    outTime = {};
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = {};
    GetFileTime(f, nullptr, nullptr, &ft);
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > kMaxSourceBytes) {
        CloseHandle(f);
        return false;
    }
    DWORD want = static_cast<DWORD>(sz.QuadPart);
    outData.resize(want);
    DWORD got = 0;
    bool ok = ReadFile(f, outData.data(), want, &got, nullptr) && got == want;
    CloseHandle(f);
    if (!ok) {
        outData.clear();
        return false;
    }
    outTime = ft;
    return true;
}

} // namespace Internal

// ================= Public Core & Target Texture API =================

bool Init(ID3D11Device* device, ID3D11DeviceContext* immediateContext, const std::wstring& shadersDir) {
    if (!device || !immediateContext) return false;
    if (g_initialized.load(std::memory_order_acquire)) return true;

    g_device = device;
    g_device->AddRef();
    g_immediate = immediateContext;
    g_immediate->AddRef();
    g_shadersDir = shadersDir;
    QueryPerformanceFrequency(&g_qpcFreq);

    CreateDirectoryW(g_shadersDir.c_str(), nullptr);

    if (!g_d3dCompilerMod) {
        g_d3dCompilerMod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (g_d3dCompilerMod) {
            g_pfnD3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(g_d3dCompilerMod, "D3DCompile"));
            g_pfnD3DReflect = reinterpret_cast<D3DReflectFn>(GetProcAddress(g_d3dCompilerMod, "D3DReflect"));
        }
    }
    if (!g_pfnD3DCompile) {
        Logger::Warn("CarFX: d3dcompiler_47.dll not found — runtime HLSL compilation disabled.");
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(FxConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&cbd, nullptr, &g_cb)) || !g_cb) {
        Logger::Error("CarFX: failed to create CarFx constant buffer.");
        Shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC sdLin = {};
    sdLin.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdLin.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sdLin.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sdLin.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sdLin.MaxAnisotropy = 1;
    sdLin.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sdLin.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sdLin, &g_samplerLinear);

    D3D11_SAMPLER_DESC sdPt = {};
    sdPt.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sdPt.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.MaxAnisotropy = 1;
    sdPt.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sdPt.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sdPt, &g_samplerPoint);

    // Premultiplied alpha blend (ONE / INV_SRC_ALPHA, preserving target alpha)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_device->CreateBlendState(&bd, &g_overlayBlend))) {
        Logger::Warn("CarFX: CreateBlendState failed.");
    }

    // Overlay depth state: test LESS_EQUAL without writing depth
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsd.StencilEnable = FALSE;
    {
        D3D11_DEPTH_STENCIL_DESC wd = dsd;
        wd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        g_device->CreateDepthStencilState(&wd, &g_bodyDepthDs);
    }
    if (FAILED(g_device->CreateDepthStencilState(&dsd, &g_overlayDs))) {
        Logger::Warn("CarFX: CreateDepthStencilState failed.");
    }

    g_initialized.store(true, std::memory_order_release);
    Logger::Info("CarFX initialized. Shaders dir: " + WideToUtf8(g_shadersDir));
    return true;
}

void Shutdown() {
    g_initialized.store(false, std::memory_order_release);

    for (int t = 0; t < kTargetCount; ++t) {
        UnloadShader(t);
        for (int i = 0; i < kTextures; ++i) {
            ClearUserTexture(t, i);
        }
    }

    AcquireSRWLockExclusive(&g_gameTexLock);
    for (int t = 0; t < kTargetCount; ++t) {
        SafeReleaseRaw(g_gameTex[t].albedoSrv);
        SafeReleaseRaw(g_gameTex[t].packedSrv);
        SafeReleaseRaw(g_gameTex[t].albedoRes);
        SafeReleaseRaw(g_gameTex[t].packedRes);
        g_gameTex[t] = {};
    }
    ReleaseSRWLockExclusive(&g_gameTexLock);

    SweepGraveyard(true);
    ReleaseSceneCopy();
    ReleaseDepthCopy();

    if (g_overlayDs) { g_overlayDs->Release(); g_overlayDs = nullptr; }
    if (g_bodyDepthDs) { g_bodyDepthDs->Release(); g_bodyDepthDs = nullptr; }
    if (g_overlayBlend) { g_overlayBlend->Release(); g_overlayBlend = nullptr; }
    if (g_samplerPoint) { g_samplerPoint->Release(); g_samplerPoint = nullptr; }
    if (g_samplerLinear) { g_samplerLinear->Release(); g_samplerLinear = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_immediate) { g_immediate->Release(); g_immediate = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }

    Logger::Info("CarFX shut down.");
}

void OnNewFrame(ID3D11DeviceContext* /*context*/) {
    if (!g_initialized.load(std::memory_order_acquire)) return;

    const uint32_t prevFrame = g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    const uint32_t totalPrev = g_totalDrawsFrame.exchange(0, std::memory_order_relaxed);
    const uint32_t matchedPrev = g_matchedDrawsFrame.exchange(0, std::memory_order_relaxed);
    uint32_t targetPrev[kTargetCount]{};
    for (int t = 0; t < kTargetCount; ++t) {
        targetPrev[t] = g_targetMatchedFrame[t].exchange(0, std::memory_order_relaxed);
    }
    g_totalDrawsLastFrame.store(totalPrev, std::memory_order_relaxed);
    const bool active = g_enabled.load(std::memory_order_relaxed) &&
                        (g_targets[kTargetBody].ps.load(std::memory_order_relaxed) != nullptr);
    const bool ghostActive = (g_activeGhostShaderMask.load(std::memory_order_relaxed) != 0);
    if (active || ghostActive || (prevFrame % 30) == 0) {
        g_matchedDrawsLastFrame.store(matchedPrev, std::memory_order_relaxed);
        for (int t = 0; t < kTargetCount; ++t) {
            g_targetMatchedLastFrame[t].store(targetPrev[t], std::memory_order_relaxed);
        }
    }

    SweepGraveyard(false);

    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextHotReloadCheckMs) {
        g_nextHotReloadCheckMs = now + 250;
        PollHotReload();
    }
}

void SetHookedDrawFlag(bool hooked) {
    g_hookedDraw.store(hooked, std::memory_order_release);
}

void SetPlayerTargetTextures(int target, const TargetTexturesInput& input) {
    if (!ValidTarget(target) || !g_initialized.load(std::memory_order_acquire)) return;

    // Quickly skip if identical raw handles already resolved
    {
        bool unchanged = false;
        AcquireSRWLockShared(&g_gameTexLock);
        const GameSrvSnapshot& cur = g_gameTex[target];
        if (cur.valid == input.valid &&
            cur.albedoPtrRaw == reinterpret_cast<uintptr_t>(input.nativeAlbedoPtr) &&
            cur.packedPtrRaw == reinterpret_cast<uintptr_t>(input.nativePackedPtr) &&
            cur.albedoIsDefault == input.albedoIsDefault &&
            cur.packedIsDefault == input.packedIsDefault &&
            cur.extraPairs.size() == input.extraPairs.size()) {
            bool extrasSame = true;
            for (size_t i = 0; i < cur.extraPairs.size(); ++i) {
                if (cur.extraPairs[i].albedoPtrRaw != reinterpret_cast<uintptr_t>(input.extraPairs[i].nativeAlbedoPtr) ||
                    cur.extraPairs[i].packedPtrRaw != reinterpret_cast<uintptr_t>(input.extraPairs[i].nativePackedPtr)) {
                    extrasSame = false;
                    break;
                }
            }
            unchanged = extrasSame;
        }
        ReleaseSRWLockShared(&g_gameTexLock);
        if (unchanged) return;
    }

    GameSrvSnapshot next{};
    next.valid = input.valid;
    next.albedoPtrRaw = reinterpret_cast<uintptr_t>(input.nativeAlbedoPtr);
    next.packedPtrRaw = reinterpret_cast<uintptr_t>(input.nativePackedPtr);
    next.albedoTexId = input.albedoTexId;
    next.packedTexId = input.packedTexId;
    next.albedoIsDefault = input.albedoIsDefault;
    next.packedIsDefault = input.packedIsDefault;

    if (input.valid && input.nativeAlbedoPtr) {
        if (SUCCEEDED(SafeQueryInterfaceRaw(input.nativeAlbedoPtr, __uuidof(ID3D11ShaderResourceView), &next.albedoSrv)) && next.albedoSrv) {
            next.albedoSrv->GetResource(&next.albedoRes);
        } else {
            SafeQueryInterfaceRaw(input.nativeAlbedoPtr, __uuidof(ID3D11Resource), &next.albedoRes);
        }
    }
    if (input.valid && input.nativePackedPtr) {
        if (SUCCEEDED(SafeQueryInterfaceRaw(input.nativePackedPtr, __uuidof(ID3D11ShaderResourceView), &next.packedSrv)) && next.packedSrv) {
            next.packedSrv->GetResource(&next.packedRes);
        } else {
            SafeQueryInterfaceRaw(input.nativePackedPtr, __uuidof(ID3D11Resource), &next.packedRes);
        }
    }

    if (input.valid && !input.extraPairs.empty()) {
        for (const auto& ep : input.extraPairs) {
            GameSrvSnapshot::ExtraResPair r{};
            r.albedoPtrRaw = reinterpret_cast<uintptr_t>(ep.nativeAlbedoPtr);
            r.packedPtrRaw = reinterpret_cast<uintptr_t>(ep.nativePackedPtr);
            r.albedoIsDefault = ep.albedoIsDefault;
            r.packedIsDefault = ep.packedIsDefault;
            if (ep.nativeAlbedoPtr) {
                if (SUCCEEDED(SafeQueryInterfaceRaw(ep.nativeAlbedoPtr, __uuidof(ID3D11ShaderResourceView), &r.albedoSrv)) && r.albedoSrv) {
                    r.albedoSrv->GetResource(&r.albedoRes);
                } else {
                    SafeQueryInterfaceRaw(ep.nativeAlbedoPtr, __uuidof(ID3D11Resource), &r.albedoRes);
                }
            }
            if (ep.nativePackedPtr) {
                if (SUCCEEDED(SafeQueryInterfaceRaw(ep.nativePackedPtr, __uuidof(ID3D11ShaderResourceView), &r.packedSrv)) && r.packedSrv) {
                    r.packedSrv->GetResource(&r.packedRes);
                } else {
                    SafeQueryInterfaceRaw(ep.nativePackedPtr, __uuidof(ID3D11Resource), &r.packedRes);
                }
            }
            if (r.albedoSrv || r.albedoRes || r.packedSrv || r.packedRes) {
                next.extraPairs.push_back(r);
            }
        }
    }

    GameSrvSnapshot old{};
    AcquireSRWLockExclusive(&g_gameTexLock);
    old = std::move(g_gameTex[target]);
    g_gameTex[target] = std::move(next);
    g_gameTexGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_gameTexLock);

    if (old.albedoSrv) Retire(old.albedoSrv);
    if (old.packedSrv) Retire(old.packedSrv);
    if (old.albedoRes) Retire(old.albedoRes);
    if (old.packedRes) Retire(old.packedRes);
    for (const auto& ep : old.extraPairs) {
        if (ep.albedoSrv) Retire(ep.albedoSrv);
        if (ep.packedSrv) Retire(ep.packedSrv);
        if (ep.albedoRes) Retire(ep.albedoRes);
        if (ep.packedRes) Retire(ep.packedRes);
    }

    static bool s_loggedOnce[kTargetCount] = {};
    if (next.valid && (!s_loggedOnce[target] ||
        next.albedoSrv != old.albedoSrv || next.packedSrv != old.packedSrv)) {
        s_loggedOnce[target] = true;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "CarFX target[%d] textures updated: albedo=(srv=%p, id=0x%X, def=%d) packed=(srv=%p, id=0x%X, def=%d)",
                    target, next.albedoSrv, next.albedoTexId, (int)next.albedoIsDefault,
                    next.packedSrv, next.packedTexId, (int)next.packedIsDefault);
        Logger::Info(buf);
    }
}

void ClearTargetTextures(int target) {
    if (!ValidTarget(target)) return;
    TargetTexturesInput empty{};
    SetPlayerTargetTextures(target, empty);
}

static std::atomic<uint32_t> s_internalsPartMask{ kPartsDefault };
void SetInternalsPartMask(uint32_t mask) { s_internalsPartMask.store(mask & 0x1Fu, std::memory_order_relaxed); }
uint32_t GetInternalsPartMask() { return s_internalsPartMask.load(std::memory_order_relaxed); }

void SetInternalsTextures(const std::vector<void*>& nativePtrs) {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    std::vector<uintptr_t> raw;
    raw.reserve(nativePtrs.size());
    for (void* p : nativePtrs) raw.push_back(reinterpret_cast<uintptr_t>(p));
    std::sort(raw.begin(), raw.end());

    {
        AcquireSRWLockShared(&g_gameTexLock);
        const bool same = (g_internals.raw == raw);
        ReleaseSRWLockShared(&g_gameTexLock);
        if (same) return;
    }

    InternalsTexSet next;
    next.raw = raw;
    for (uintptr_t r : raw) {
        if (!r) continue;
        void* p = reinterpret_cast<void*>(r);
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Resource* res = nullptr;
        if (SUCCEEDED(SafeQueryInterfaceRaw(p, __uuidof(ID3D11ShaderResourceView), &srv)) && srv) {
            next.srvs.push_back(srv);
            srv->GetResource(&res);
            if (res) next.res.push_back(res);   // a bound SRV of another view on the same texture still matches
        } else if (SUCCEEDED(SafeQueryInterfaceRaw(p, __uuidof(ID3D11Resource), &res)) && res) {
            next.res.push_back(res);
        }
    }

    InternalsTexSet old;
    AcquireSRWLockExclusive(&g_gameTexLock);
    old = std::move(g_internals);
    g_internals = std::move(next);
    g_internalsGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_gameTexLock);

    for (auto* s : old.srvs) Retire(s);
    for (auto* r : old.res) Retire(r);

    static size_t s_lastSrvCount = static_cast<size_t>(-1);
    static size_t s_lastResCount = static_cast<size_t>(-1);
    if (g_internals.srvs.size() != s_lastSrvCount || g_internals.res.size() != s_lastResCount) {
        s_lastSrvCount = g_internals.srvs.size();
        s_lastResCount = g_internals.res.size();
        Logger::Info("CarFX internals textures: " + std::to_string(g_internals.srvs.size()) + " views, " +
                     std::to_string(g_internals.res.size()) + " resources");
    }
}

void SetStealthDepthWrite(bool on) {
    g_stealthDepthWrite.store(on, std::memory_order_relaxed);
}

bool ShouldHideCarInternals(int target) {
    if (!g_initialized.load(std::memory_order_acquire) || !IsTargetActiveInternal(target)) {
        return false;
    }
    const int ov = g_targets[target].overrideStealth.load(std::memory_order_acquire);
    if (ov == kStealthOn) return true;
    if (ov == kStealthOff) return false;
    return g_targets[target].declStealth.load(std::memory_order_acquire);
}


} // namespace CarFX
