#include "carfx.h"
#include "logger.h"
#include "native_carx.h"

#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wincodec.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace CarFX {
namespace {

constexpr DWORD kMaxSourceBytes = 1u << 20;
constexpr ULONGLONG kRetireDelayMs = 2000;
constexpr UINT kMaxTexSide = 8192;
constexpr int kScanStartSlot = 12;   // t12..t31 (CarX MAT_CAR_PAINT_3_0 binds u_sTexture at t15..t21 across quality presets)
constexpr int kScanSlotCount = 20;

// IID_ID3D11ShaderReflection ({8D536CA1-0CCA-4956-A837-786963755584})
constexpr GUID kIidShaderReflection = {
    0x8D536CA1, 0x0CCA, 0x4956, { 0xA8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 }
};
using D3DReflectFn = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

struct FxConstants {
    float time[4];                   // x: sec since load, y: dt since prev draw, z: matched draw count, w: target index
    float screen[4];                 // x: vp width, y: vp height, z: 1/rt width, w: 1/rt height
    float param[kParams][4];         // p0..p7
    float frameInfo[4];              // x: livery bound (0/1), y: tex0 bound, z: tex1 bound, w: mode (1=replace, 2=overlay)
    float game[kGameInputs / 4][4];  // g_fxGame[4]

    // Spatial + Proximity vectors populated from MainThread Unity/Physics
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
};

// Custom #include handler resolving relative to current file directory first, then global shaders/ dir
class ShaderIncludeHandler final : public ID3DInclude {
public:
    ShaderIncludeHandler(std::wstring localDir, std::wstring rootDir)
        : m_localDir(std::move(localDir)), m_rootDir(std::move(rootDir)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID /*pParentData*/,
                           LPCVOID* ppData, UINT* pBytes) override {
        if (!pFileName || !ppData || !pBytes) return E_INVALIDARG;
        *ppData = nullptr;
        *pBytes = 0;

        wchar_t wName[MAX_PATH] = {};
        if (!MultiByteToWideChar(CP_UTF8, 0, pFileName, -1, wName, MAX_PATH)) return E_FAIL;

        std::vector<std::wstring> candidates;
        if (IncludeType == D3D_INCLUDE_LOCAL && !m_localDir.empty()) {
            candidates.push_back(m_localDir + L"\\" + wName);
        }
        if (!m_rootDir.empty()) {
            candidates.push_back(m_rootDir + L"\\" + wName);
        }
        if (!m_localDir.empty() && IncludeType != D3D_INCLUDE_LOCAL) {
            candidates.push_back(m_localDir + L"\\" + wName);
        }

        // #include may only reach files inside the shaders folder (cloud shaders must not read arbitrary files)
        std::wstring rootFull;
        {
            wchar_t rb[MAX_PATH * 2] = {};
            const DWORD rn = GetFullPathNameW(m_rootDir.c_str(), MAX_PATH * 2, rb, nullptr);
            if (rn == 0 || rn >= MAX_PATH * 2) return E_ACCESSDENIED;
            rootFull = rb;
            if (rootFull.empty() || rootFull.back() != L'\\') rootFull += L'\\';
        }
        if (wcschr(wName, L':') != nullptr) return E_ACCESSDENIED;

        for (const auto& path : candidates) {
            wchar_t pb[MAX_PATH * 2] = {};
            const DWORD pn = GetFullPathNameW(path.c_str(), MAX_PATH * 2, pb, nullptr);
            if (pn == 0 || pn >= MAX_PATH * 2 || _wcsnicmp(pb, rootFull.c_str(), rootFull.size()) != 0) continue;
            HANDLE f = CreateFileW(pb, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz = {};
            if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > kMaxSourceBytes) {
                CloseHandle(f);
                continue;
            }
            DWORD want = static_cast<DWORD>(sz.QuadPart);
            char* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, want));
            DWORD got = 0;
            if (buf && ReadFile(f, buf, want, &got, nullptr) && got == want) {
                CloseHandle(f);
                *ppData = buf;
                *pBytes = got;
                return S_OK;
            }
            if (buf) HeapFree(GetProcessHeap(), 0, buf);
            CloseHandle(f);
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    HRESULT __stdcall Close(LPCVOID pData) override {
        if (pData) HeapFree(GetProcessHeap(), 0, const_cast<void*>(pData));
        return S_OK;
    }

private:
    std::wstring m_localDir;
    std::wstring m_rootDir;
};

// ================= Module State =================

ID3D11Device* s_device = nullptr;
ID3D11DeviceContext* s_immediate = nullptr;
std::wstring s_shadersDir;
std::atomic<bool> s_initialized{ false };
std::atomic<bool> s_enabled{ true };
std::atomic<bool> s_hookedDraw{ false };

HMODULE s_d3dCompilerMod = nullptr;
pD3DCompile s_pfnD3DCompile = nullptr;
D3DReflectFn s_pfnD3DReflect = nullptr;

ID3D11Buffer* s_cb = nullptr;
ID3D11SamplerState* s_samplerLinear = nullptr;
ID3D11SamplerState* s_samplerPoint = nullptr;
ID3D11BlendState* s_overlayBlend = nullptr;
ID3D11DepthStencilState* s_overlayDs = nullptr;
ID3D11DepthStencilState* s_bodyDepthDs = nullptr;
std::atomic<bool> s_stealthDepthWrite{ false };

// Scene & Depth copy cache (render thread)
ID3D11Texture2D* s_sceneTex = nullptr;
ID3D11ShaderResourceView* s_sceneSrv = nullptr;
D3D11_TEXTURE2D_DESC s_sceneDesc{};

ID3D11Texture2D* s_depthTex = nullptr;
ID3D11ShaderResourceView* s_depthSrv = nullptr;
D3D11_TEXTURE2D_DESC s_depthDesc{};

TargetSlotState s_targets[kTargetCount];
std::atomic<uint32_t> s_activeGhostShaderMask{ 0 };

// Game SRVs double-check lock (main thread writes, render thread reads)
SRWLOCK s_gameTexLock = SRWLOCK_INIT;
GameSrvSnapshot s_gameTex[kTargetCount];
std::atomic<uint64_t> s_gameTexGen{ 1 };

std::atomic<float> s_gameInputs[kTargetCount][kGameInputs]{};

SRWLOCK s_spatialLock[kTargetCount] = {};
SpatialProximityInput s_spatialInput[kTargetCount]{};

inline bool ValidTarget(int t) { return t >= 0 && t < kTargetCount; }

void UpdateGhostShaderMask() {
    uint32_t m = 0;
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        if (s_targets[t].slotEnabled.load(std::memory_order_acquire) &&
            s_targets[t].ps.load(std::memory_order_acquire) != nullptr) {
            m |= (1u << (t - kTargetGhostBase));
        }
    }
    s_activeGhostShaderMask.store(m, std::memory_order_release);
}

bool IsTargetActiveInternal(int target) {
    if (!ValidTarget(target)) return false;
    if (IsGhostTarget(target)) {
        return s_targets[target].slotEnabled.load(std::memory_order_acquire) &&
               (s_targets[target].ps.load(std::memory_order_acquire) != nullptr);
    }
    return s_enabled.load(std::memory_order_acquire) &&
           s_targets[target].slotEnabled.load(std::memory_order_acquire) &&
           (s_targets[target].ps.load(std::memory_order_acquire) != nullptr);
}

std::mutex s_graveMtx;
std::vector<RetiredObject> s_graveyard;

LARGE_INTEGER s_qpcFreq{};
std::atomic<int64_t> s_lastDrawQpc{ 0 };
std::atomic<uint32_t> s_frameCounter{ 0 };
uint32_t s_sceneCopiedFrame = ~0u;
uint32_t s_depthCopiedFrame = ~0u;

// Diagnostics
std::atomic<uint32_t> s_totalDrawsFrame{ 0 };
std::atomic<uint32_t> s_totalDrawsLastFrame{ 0 };
std::atomic<uint32_t> s_matchedDrawsFrame{ 0 };
std::atomic<uint32_t> s_matchedDrawsLastFrame{ 0 };
std::atomic<uint32_t> s_targetMatchedFrame[kTargetCount]{};
std::atomic<uint32_t> s_targetMatchedLastFrame[kTargetCount]{};
std::atomic<uint32_t> s_sceneCopiesTotal{ 0 };
std::atomic<uint32_t> s_depthCopiesTotal{ 0 };
std::atomic<uint32_t> s_lastSlotMask{ 0 };
std::atomic<uint32_t> s_lastRtWidth{ 0 };
std::atomic<uint32_t> s_lastRtHeight{ 0 };
std::atomic<uint32_t> s_lastRtFormat{ 0 };
std::atomic<uintptr_t> s_lastGamePs{ 0 };
std::atomic<int> s_hintSlot{ 15 };

ULONGLONG s_nextHotReloadCheckMs = 0;

// ================= Leaf SEH / Helpers =================

template <typename T>
static HRESULT SafeQueryInterfaceRaw(void* rawPtr, REFIID iid, T** outObj) {
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

static void SafeReleaseRaw(IUnknown* unk) {
    if (!unk) return;
    __try {
        unk->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Retire(IUnknown* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(s_graveMtx);
    s_graveyard.push_back({ obj, GetTickCount64() + kRetireDelayMs });
}

void SweepGraveyard(bool releaseAll) {
    std::vector<IUnknown*> toRelease;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(s_graveMtx);
        for (size_t i = 0; i < s_graveyard.size();) {
            if (releaseAll || now >= s_graveyard[i].releaseAt) {
                toRelease.push_back(s_graveyard[i].obj);
                s_graveyard[i] = s_graveyard.back();
                s_graveyard.pop_back();
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
    if (!s_shadersDir.empty()) {
        // Strip leading "shaders/" or "shaders\" if caller passed relative to game root
        if (w.rfind(L"shaders\\", 0) == 0 || w.rfind(L"shaders/", 0) == 0) {
            return s_shadersDir.substr(0, s_shadersDir.rfind(L"shaders")) + w;
        }
        return s_shadersDir + L"\\" + w;
    }
    return w;
}

// Paths come from Lua (carfx.load / carfx.set_tex): only files inside the game shaders folder are reachable,
// so a script can neither probe nor compile/read arbitrary files.
std::wstring ResolvePathW(const std::string& pathUtf8) {
    std::wstring resolved = ResolvePathRawW(pathUtf8);
    if (resolved.empty() || s_shadersDir.empty()) return {};
    wchar_t full[MAX_PATH * 2] = {};
    wchar_t base[MAX_PATH * 2] = {};
    const DWORD n1 = GetFullPathNameW(resolved.c_str(), MAX_PATH * 2, full, nullptr);
    const DWORD n2 = GetFullPathNameW(s_shadersDir.c_str(), MAX_PATH * 2, base, nullptr);
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

void ReleaseSceneCopy() {
    if (s_sceneSrv) { s_sceneSrv->Release(); s_sceneSrv = nullptr; }
    if (s_sceneTex) { s_sceneTex->Release(); s_sceneTex = nullptr; }
    memset(&s_sceneDesc, 0, sizeof(s_sceneDesc));
}

void ReleaseDepthCopy() {
    if (s_depthSrv) { s_depthSrv->Release(); s_depthSrv = nullptr; }
    if (s_depthTex) { s_depthTex->Release(); s_depthTex = nullptr; }
    memset(&s_depthDesc, 0, sizeof(s_depthDesc));
}

DXGI_FORMAT UntypelessColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return fmt;
    }
}

bool IsDepthFormatMapping(DXGI_FORMAT dsvFmt, DXGI_FORMAT* texFmt, DXGI_FORMAT* srvFmt) {
    switch (dsvFmt) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        *texFmt = DXGI_FORMAT_R32_TYPELESS;
        *srvFmt = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *texFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
        *srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *texFmt = DXGI_FORMAT_R24G8_TYPELESS;
        *srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *texFmt = DXGI_FORMAT_R16_TYPELESS;
        *srvFmt = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

bool EnsureSceneCopy(ID3D11DeviceContext* c, ID3D11Resource* rtRes) {
    if (!rtRes || !s_device) return false;
    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(rtRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex))) || !srcTex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    DXGI_FORMAT fmt = UntypelessColorFormat(td.Format);

    if (!s_sceneTex || s_sceneDesc.Width != td.Width || s_sceneDesc.Height != td.Height || s_sceneDesc.Format != fmt) {
        ReleaseSceneCopy();
        D3D11_TEXTURE2D_DESC dstDesc = {};
        dstDesc.Width = td.Width;
        dstDesc.Height = td.Height;
        dstDesc.MipLevels = 1;
        dstDesc.ArraySize = 1;
        dstDesc.Format = fmt;
        dstDesc.SampleDesc.Count = 1;
        dstDesc.SampleDesc.Quality = 0;
        dstDesc.Usage = D3D11_USAGE_DEFAULT;
        dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s_device->CreateTexture2D(&dstDesc, nullptr, &s_sceneTex)) || !s_sceneTex) {
            srcTex->Release();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = fmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(s_device->CreateShaderResourceView(s_sceneTex, &srvDesc, &s_sceneSrv)) || !s_sceneSrv) {
            ReleaseSceneCopy();
            srcTex->Release();
            return false;
        }
        s_sceneDesc = dstDesc;
    }

    if (td.SampleDesc.Count > 1) {
        c->ResolveSubresource(s_sceneTex, 0, srcTex, 0, fmt);
    } else if (td.MipLevels > 1 || td.ArraySize > 1) {
        c->CopySubresourceRegion(s_sceneTex, 0, 0, 0, 0, srcTex, 0, nullptr);
    } else {
        c->CopyResource(s_sceneTex, srcTex);
    }
    srcTex->Release();
    s_sceneCopiesTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EnsureDepthCopy(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv) {
    if (!dsv || !s_device) return false;
    ID3D11Resource* dRes = nullptr;
    dsv->GetResource(&dRes);
    if (!dRes) return false;

    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(dRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex))) || !srcTex) {
        dRes->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsv->GetDesc(&dsvDesc);

    DXGI_FORMAT copyTexFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT copySrvFmt = DXGI_FORMAT_UNKNOWN;
    bool fmtOk = IsDepthFormatMapping(dsvDesc.Format, &copyTexFmt, &copySrvFmt) ||
                 IsDepthFormatMapping(td.Format, &copyTexFmt, &copySrvFmt);
    if (!fmtOk || td.SampleDesc.Count > 1) {
        srcTex->Release();
        dRes->Release();
        return false;
    }

    if (!s_depthTex || s_depthDesc.Width != td.Width || s_depthDesc.Height != td.Height || s_depthDesc.Format != copyTexFmt) {
        ReleaseDepthCopy();
        D3D11_TEXTURE2D_DESC dstDesc = {};
        dstDesc.Width = td.Width;
        dstDesc.Height = td.Height;
        dstDesc.MipLevels = 1;
        dstDesc.ArraySize = 1;
        dstDesc.Format = copyTexFmt;
        dstDesc.SampleDesc.Count = 1;
        dstDesc.Usage = D3D11_USAGE_DEFAULT;
        dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s_device->CreateTexture2D(&dstDesc, nullptr, &s_depthTex)) || !s_depthTex) {
            srcTex->Release();
            dRes->Release();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = copySrvFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(s_device->CreateShaderResourceView(s_depthTex, &srvDesc, &s_depthSrv)) || !s_depthSrv) {
            ReleaseDepthCopy();
            srcTex->Release();
            dRes->Release();
            return false;
        }
        s_depthDesc = dstDesc;
    }

    if (td.MipLevels > 1 || td.ArraySize > 1) {
        c->CopySubresourceRegion(s_depthTex, 0, 0, 0, 0, srcTex, 0, nullptr);
    } else {
        c->CopyResource(s_depthTex, srcTex);
    }
    srcTex->Release();
    dRes->Release();
    s_depthCopiesTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool CompileEffectFile(int targetIdx, const std::wstring& fullPath, std::string* outErr) {
    if (!ValidTarget(targetIdx) || !s_device || !s_pfnD3DCompile) {
        if (outErr) *outErr = "CarFX or d3dcompiler_47.dll not initialized";
        return false;
    }

    std::vector<char> src;
    FILETIME ft = {};
    if (!ReadFileUtf8(fullPath, src, ft)) {
        if (outErr) *outErr = "Cannot read shader file: " + WideToUtf8(fullPath);
        return false;
    }

    ShaderDecl decl{};
    ParseShaderDecl(src.data(), src.size(), &decl);

    std::string narrowPath = WideToUtf8(fullPath);
    ShaderIncludeHandler incl(DirectoryOf(fullPath), s_shadersDir);
    ID3DBlob* code = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = s_pfnD3DCompile(
        src.data(), src.size(),
        narrowPath.c_str(),
        nullptr,
        &incl,
        "main",
        "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &errBlob
    );

    if (FAILED(hr) || !code) {
        std::string msg = "D3DCompile failed";
        if (errBlob && errBlob->GetBufferPointer() && errBlob->GetBufferSize() > 0) {
            msg.assign(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
        }
        if (errBlob) errBlob->Release();
        if (code) code->Release();

        TargetSlotState& ts = s_targets[targetIdx];
        std::lock_guard<std::mutex> lk(ts.publishMtx);
        ts.failures++;
        ts.lastError = msg;
        if (outErr) *outErr = msg;
        Logger::Error("CarFX compile error (" + narrowPath + "): " + msg);
        return false;
    }
    if (errBlob) errBlob->Release();

    ID3D11PixelShader* newPs = nullptr;
    hr = s_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &newPs);
    if (FAILED(hr) || !newPs) {
        code->Release();
        if (outErr) *outErr = "CreatePixelShader failed";
        return false;
    }

    bool readsScene = false, readsDepth = false, readsLivery = false;
    if (s_pfnD3DReflect) {
        ID3D11ShaderReflection* refl = nullptr;
        if (SUCCEEDED(s_pfnD3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                                      kIidShaderReflection, reinterpret_cast<void**>(&refl))) && refl) {
            D3D11_SHADER_DESC sd = {};
            if (SUCCEEDED(refl->GetDesc(&sd))) {
                for (UINT i = 0; i < sd.BoundResources; ++i) {
                    D3D11_SHADER_INPUT_BIND_DESC rbd = {};
                    if (SUCCEEDED(refl->GetResourceBindingDesc(i, &rbd)) && rbd.Name) {
                        if (_stricmp(rbd.Name, "g_fxScene") == 0) readsScene = true;
                        else if (_stricmp(rbd.Name, "g_fxDepth") == 0) readsDepth = true;
                        else if (_stricmp(rbd.Name, "g_fxLivery") == 0) readsLivery = true;
                    }
                }
            }
            refl->Release();
        }
    }
    code->Release();

    TargetSlotState& ts = s_targets[targetIdx];
    std::lock_guard<std::mutex> lk(ts.publishMtx);

    bool isSameFile = (ts.shaderPath == narrowPath && ts.compiles > 0);
    float defaults[kParams][4] = {};
    DeclDefaults(decl, defaults);
    if (!isSameFile) {
        for (int p = 0; p < kParams; ++p) {
            for (int c = 0; c < 4; ++c) {
                ts.param[p][c].store(defaults[p][c], std::memory_order_relaxed);
            }
        }
        QueryPerformanceCounter(&ts.loadQpc);
    } else {
        // On hot reload, keep user values for existing param names/slots and apply defaults for newly declared ones
        for (int i = 0; i < decl.paramCount; ++i) {
            const ParamDecl& np = decl.param[i];
            const ParamDecl* op = FindParam(ts.decl, np.name);
            bool samePlace = op && op->slot == np.slot && op->first == np.first && op->count == np.count;
            if (!samePlace) {
                for (int c = 0; c < np.count; ++c) {
                    ts.param[np.slot][np.first + c].store(np.def[c], std::memory_order_relaxed);
                }
            } else {
                for (int c = 0; c < np.count; ++c) {
                    float cur = ts.param[np.slot][np.first + c].load(std::memory_order_relaxed);
                    ts.param[np.slot][np.first + c].store(ClampToDecl(np, cur), std::memory_order_relaxed);
                }
            }
        }
    }

    ID3D11PixelShader* oldPs = ts.ps.exchange(newPs, std::memory_order_release);
    if (oldPs) Retire(oldPs);

    ts.declMode.store(decl.mode != kDeclModeNone ? decl.mode : kDeclModeReplace, std::memory_order_release);
    ts.declStealth.store(decl.stealth, std::memory_order_release);
    ts.usesScene.store(readsScene, std::memory_order_release);
    ts.usesDepth.store(readsDepth, std::memory_order_release);
    ts.usesLivery.store(readsLivery, std::memory_order_release);
    ts.shaderPath = narrowPath;
    ts.lastWriteTime = ft;
    ts.decl = decl;
    ts.lastError.clear();
    ts.compiles++;
    UpdateGhostShaderMask();

    if (outErr) outErr->clear();
    Logger::Info("CarFX compiled [" + narrowPath + "] mode=" +
                 std::string(ts.declMode.load() == kDeclModeOverlay ? "overlay" : "replace") +
                 " stealth=" + (decl.stealth ? "1" : "0") +
                 " scene=" + (readsScene ? "1" : "0") +
                 " depth=" + (readsDepth ? "1" : "0") +
                 " livery=" + (readsLivery ? "1" : "0"));
    return true;
}

bool IsTwoChannelOrDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

bool MatchTargetAndCheckRT(ID3D11DeviceContext* c, int* outTarget, uint32_t* outMask,
                           ID3D11ShaderResourceView** outAlbedoSrv,
                           ID3D11Resource** outRtRes, UINT* outRtW, UINT* outRtH, DXGI_FORMAT* outRtFmt,
                           ID3D11DepthStencilView** outDsv) {
    *outTarget = kTargetNone;
    *outMask = 0;
    *outAlbedoSrv = nullptr;

    // Render-thread cache keyed by s_gameTexGen: avoids SRWLock + 20-slot GetResource on every draw call!
    static uint64_t s_rtCachedGen = 0;
    static GameSrvSnapshot s_rtSnap[kTargetCount]{};
    static int s_rtKnownAlbSlot = -1;
    static int s_rtKnownPackSlot = -1;
    static uint32_t s_rtRediscoveryFrame = ~0u;

    const uint64_t curGen = s_gameTexGen.load(std::memory_order_acquire);
    if (curGen != s_rtCachedGen) {
        AcquireSRWLockShared(&s_gameTexLock);
        for (int i = 0; i < kTargetCount; ++i) s_rtSnap[i] = s_gameTex[i];
        s_rtCachedGen = s_gameTexGen.load(std::memory_order_relaxed);
        ReleaseSRWLockShared(&s_gameTexLock);
        s_rtRediscoveryFrame = ~0u;
    }

    auto targetHasMatchableSrv = [](const GameSrvSnapshot& gs) -> bool {
        return gs.valid && (!gs.albedoIsDefault || !gs.packedIsDefault);
    };

    bool anyGhostTex = false;
    bool needAlbSlot = !s_rtSnap[kTargetBody].albedoIsDefault && s_rtSnap[kTargetBody].valid;
    bool needPackSlot = false;
    if (s_rtSnap[kTargetBody].valid && !s_rtSnap[kTargetBody].packedIsDefault) {
        needPackSlot = true;
    }
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        if (targetHasMatchableSrv(s_rtSnap[t])) {
            anyGhostTex = true;
            if (!s_rtSnap[t].packedIsDefault) needPackSlot = true;
            if (!s_rtSnap[t].albedoIsDefault) needAlbSlot = true;
        }
    }

    const GameSrvSnapshot& body = s_rtSnap[kTargetBody];
    const bool playerCanMatch = targetHasMatchableSrv(body);
    if (!playerCanMatch && !anyGhostTex) return false;

    int matchedTarget = kTargetNone;
    uint32_t mask = 0;
    ID3D11ShaderResourceView* matchedAlbedoSrv = nullptr;
    int discoveredAlbSlot = -1;
    int discoveredPackSlot = -1;

    const bool bodyNeedBoth = playerCanMatch && !body.albedoIsDefault && !body.packedIsDefault &&
                              (body.packedSrv || body.packedRes) && (body.albedoRes != body.packedRes);

    auto srvMatches = [](ID3D11ShaderResourceView* bound, ID3D11Resource** cachedRes,
                         ID3D11ShaderResourceView* targetSrv, ID3D11Resource* targetRes) -> bool {
        if (!bound) return false;
        if (targetSrv) return bound == targetSrv;
        if (!targetRes) return false;
        if (!*cachedRes) bound->GetResource(cachedRes);
        return (*cachedRes == targetRes);
    };

    const bool haveFastSlots =
        (!needAlbSlot || s_rtKnownAlbSlot >= kScanStartSlot) &&
        (!needPackSlot || s_rtKnownPackSlot >= kScanStartSlot);

    if (haveFastSlots) {
        ID3D11ShaderResourceView* vAlb = nullptr;
        ID3D11ShaderResourceView* vPack = nullptr;
        ID3D11Resource* resAlb = nullptr;
        ID3D11Resource* resPack = nullptr;
        if (needAlbSlot && s_rtKnownAlbSlot >= kScanStartSlot) {
            c->PSGetShaderResources(static_cast<UINT>(s_rtKnownAlbSlot), 1, &vAlb);
        }
        if (needPackSlot && s_rtKnownPackSlot >= kScanStartSlot) {
            c->PSGetShaderResources(static_cast<UINT>(s_rtKnownPackSlot), 1, &vPack);
        }

        // Check active ghost targets FIRST so isolated ghost clones never leak into player target
        if (anyGhostTex) {
            for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
                const GameSrvSnapshot& gs = s_rtSnap[t];
                if (!targetHasMatchableSrv(gs)) continue;
                bool match = false;
                if (!gs.packedIsDefault && (gs.packedSrv || gs.packedRes)) {
                    match = srvMatches(vPack, &resPack, gs.packedSrv, gs.packedRes);
                } else if (!gs.albedoIsDefault && (gs.albedoSrv || gs.albedoRes)) {
                    match = srvMatches(vAlb, &resAlb, gs.albedoSrv, gs.albedoRes);
                }
                if (match) {
                    matchedTarget = t;
                    if (!gs.packedIsDefault && s_rtKnownPackSlot >= kScanStartSlot &&
                        srvMatches(vPack, &resPack, gs.packedSrv, gs.packedRes)) {
                        mask |= (1u << s_rtKnownPackSlot);
                    }
                    if (!gs.albedoIsDefault && s_rtKnownAlbSlot >= kScanStartSlot &&
                        srvMatches(vAlb, &resAlb, gs.albedoSrv, gs.albedoRes)) {
                        mask |= (1u << s_rtKnownAlbSlot);
                        matchedAlbedoSrv = vAlb;
                        matchedAlbedoSrv->AddRef();
                    }
                    break;
                }
            }
        }

        if (matchedTarget == kTargetNone && playerCanMatch) {
            bool match = false;
            const bool albMatch = !body.albedoIsDefault && (body.albedoSrv || body.albedoRes) &&
                                  srvMatches(vAlb, &resAlb, body.albedoSrv, body.albedoRes);
            const bool packMatch = !body.packedIsDefault && (body.packedSrv || body.packedRes) &&
                                   srvMatches(vPack, &resPack, body.packedSrv, body.packedRes);
            if (!body.albedoIsDefault && (body.albedoSrv || body.albedoRes)) {
                match = albMatch && (!bodyNeedBoth || packMatch);
            } else if (!body.packedIsDefault && (body.packedSrv || body.packedRes)) {
                match = packMatch;
            }
            if (match) {
                matchedTarget = kTargetBody;
                if (albMatch && vAlb) {
                    mask |= (1u << s_rtKnownAlbSlot);
                    matchedAlbedoSrv = vAlb;
                    matchedAlbedoSrv->AddRef();
                }
                if (packMatch && s_rtKnownPackSlot >= kScanStartSlot) {
                    mask |= (1u << s_rtKnownPackSlot);
                }
            }
        }

        if (resAlb) resAlb->Release();
        if (resPack) resPack->Release();
        if (vAlb) vAlb->Release();
        if (vPack) vPack->Release();

        if (matchedTarget == kTargetNone) {
            const uint32_t frameId = s_frameCounter.load(std::memory_order_relaxed);
            if (s_matchedDrawsLastFrame.load(std::memory_order_relaxed) == 0 && (frameId - s_rtRediscoveryFrame) >= 60) {
                s_rtRediscoveryFrame = frameId;
                s_rtKnownAlbSlot = -1;
                s_rtKnownPackSlot = -1;
            }
            return false;
        }
    } else {
        ID3D11ShaderResourceView* srvs[kScanSlotCount] = {};
        c->PSGetShaderResources(kScanStartSlot, kScanSlotCount, srvs);
        ID3D11Resource* cachedRes[kScanSlotCount] = {};

        auto scanMatchForTarget = [&](int t, bool isGhost, bool requireBoth) -> bool {
            const GameSrvSnapshot& gs = s_rtSnap[t];
            if (!targetHasMatchableSrv(gs)) return false;

            ID3D11ShaderResourceView* foundAlb = nullptr;
            int foundAlbIdx = -1;
            int foundPackIdx = -1;

            for (int i = 0; i < kScanSlotCount; ++i) {
                ID3D11ShaderResourceView* v = srvs[i];
                if (!v) continue;

                const bool isAlb = !gs.albedoIsDefault && srvMatches(v, &cachedRes[i], gs.albedoSrv, gs.albedoRes);
                const bool isPack = !gs.packedIsDefault && srvMatches(v, &cachedRes[i], gs.packedSrv, gs.packedRes);
                if (isAlb && !foundAlb) {
                    foundAlb = v;
                    foundAlbIdx = kScanStartSlot + i;
                }
                if (isPack && foundPackIdx < 0) {
                    foundPackIdx = kScanStartSlot + i;
                }
            }

            bool ok = false;
            if (isGhost) {
                if (!gs.packedIsDefault) ok = (foundPackIdx >= 0);
                else if (!gs.albedoIsDefault) ok = (foundAlb != nullptr);
            } else {
                ok = requireBoth ? (foundAlb != nullptr && foundPackIdx >= 0)
                                 : (!gs.albedoIsDefault ? (foundAlb != nullptr) : (foundPackIdx >= 0));
            }
            if (!ok) return false;

            matchedTarget = t;
            if (foundAlbIdx >= 0) {
                discoveredAlbSlot = foundAlbIdx;
                mask |= (1u << foundAlbIdx);
                matchedAlbedoSrv = foundAlb;
                matchedAlbedoSrv->AddRef();
            }
            if (foundPackIdx >= 0) {
                discoveredPackSlot = foundPackIdx;
                mask |= (1u << foundPackIdx);
            }
            return true;
        };

        if (anyGhostTex) {
            for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
                if (scanMatchForTarget(t, true, false)) break;
            }
        }
        if (matchedTarget == kTargetNone && playerCanMatch) {
            scanMatchForTarget(kTargetBody, false, bodyNeedBoth);
        }

        for (int i = 0; i < kScanSlotCount; ++i) {
            if (cachedRes[i]) cachedRes[i]->Release();
            if (srvs[i]) srvs[i]->Release();
        }
        if (matchedTarget == kTargetNone) {
            return false;
        }
    }

    // Verify main color RTV (rejects DepthOnly, ShadowCaster, NORMALS_BUFFER, tiny reflection cubemaps)
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) {
        if (dsv) dsv->Release();
        if (matchedAlbedoSrv) matchedAlbedoSrv->Release();
        return false;
    }

    ID3D11Resource* rtRes = nullptr;
    rtv->GetResource(&rtRes);
    rtv->Release();

    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFmt = DXGI_FORMAT_UNKNOWN;
    if (rtRes) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(rtRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            rtW = td.Width;
            rtH = td.Height;
            rtFmt = td.Format;
            tex->Release();
        }
    }

    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    bool validRt = rtRes && rtW > 0 && rtH > 0 && !IsTwoChannelOrDepthFormat(rtFmt);
    if (validRt && nvp > 0 && vp.Width > 64.0f && vp.Height > 64.0f) {
        if (rtW < static_cast<UINT>(vp.Width * 0.5f) || rtH < static_cast<UINT>(vp.Height * 0.5f)) {
            validRt = false;
        }
    }

    if (!validRt) {
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        if (matchedAlbedoSrv) matchedAlbedoSrv->Release();
        return false;
    }

    // Latch discovered slot indices only after confirming main color RT!
    if (discoveredAlbSlot >= kScanStartSlot) {
        s_rtKnownAlbSlot = discoveredAlbSlot;
    }
    if (discoveredPackSlot >= kScanStartSlot) {
        s_rtKnownPackSlot = discoveredPackSlot;
    }

    *outTarget = matchedTarget;
    *outMask = mask;
    *outAlbedoSrv = matchedAlbedoSrv;   // caller owns ref
    *outRtRes = rtRes;                  // caller owns ref
    *outRtW = rtW;
    *outRtH = rtH;
    *outRtFmt = rtFmt;
    *outDsv = dsv;                      // caller owns ref
    return true;
}

void RestoreState(ID3D11DeviceContext* c, BoundState& b) {
    if (!b.active) return;
    c->PSSetShader(b.ps, nullptr, 0);
    c->PSSetConstantBuffers(kCbSlot, 1, &b.cb);
    c->PSSetShaderResources(kSceneSlot, kViewCount, b.srvs);
    c->PSSetSamplers(14, 2, b.samplers);
    if (b.restoreOm) {
        c->OMSetBlendState(b.blend, b.blendFactor, b.sampleMask);
        c->OMSetDepthStencilState(b.ds, b.stencilRef);
    }

    if (b.ps) b.ps->Release();
    if (b.cb) b.cb->Release();
    for (ID3D11ShaderResourceView*& v : b.srvs) {
        if (v) v->Release();
    }
    for (ID3D11SamplerState*& s : b.samplers) {
        if (s) s->Release();
    }
    if (b.blend) b.blend->Release();
    if (b.ds) b.ds->Release();
    b = {};
}

} // namespace

// ================= Public API =================

bool Init(ID3D11Device* device, ID3D11DeviceContext* immediateContext, const std::wstring& shadersDir) {
    if (!device || !immediateContext) return false;
    if (s_initialized.load(std::memory_order_acquire)) return true;

    s_device = device;
    s_device->AddRef();
    s_immediate = immediateContext;
    s_immediate->AddRef();
    s_shadersDir = shadersDir;
    QueryPerformanceFrequency(&s_qpcFreq);

    CreateDirectoryW(s_shadersDir.c_str(), nullptr);

    if (!s_d3dCompilerMod) {
        s_d3dCompilerMod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (s_d3dCompilerMod) {
            s_pfnD3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(s_d3dCompilerMod, "D3DCompile"));
            s_pfnD3DReflect = reinterpret_cast<D3DReflectFn>(GetProcAddress(s_d3dCompilerMod, "D3DReflect"));
        }
    }
    if (!s_pfnD3DCompile) {
        Logger::Warn("CarFX: d3dcompiler_47.dll not found — runtime HLSL compilation disabled.");
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(FxConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(s_device->CreateBuffer(&cbd, nullptr, &s_cb)) || !s_cb) {
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
    s_device->CreateSamplerState(&sdLin, &s_samplerLinear);

    D3D11_SAMPLER_DESC sdPt = {};
    sdPt.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sdPt.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdPt.MaxAnisotropy = 1;
    sdPt.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sdPt.MaxLOD = D3D11_FLOAT32_MAX;
    s_device->CreateSamplerState(&sdPt, &s_samplerPoint);

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
    if (FAILED(s_device->CreateBlendState(&bd, &s_overlayBlend))) {
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
        s_device->CreateDepthStencilState(&wd, &s_bodyDepthDs);
    }
    if (FAILED(s_device->CreateDepthStencilState(&dsd, &s_overlayDs))) {
        Logger::Warn("CarFX: CreateDepthStencilState failed.");
    }

    s_initialized.store(true, std::memory_order_release);
    Logger::Info("CarFX initialized. Shaders dir: " + WideToUtf8(s_shadersDir));
    return true;
}

void Shutdown() {
    s_initialized.store(false, std::memory_order_release);

    for (int t = 0; t < kTargetCount; ++t) {
        UnloadShader(t);
        for (int i = 0; i < kTextures; ++i) {
            ClearUserTexture(t, i);
        }
    }

    AcquireSRWLockExclusive(&s_gameTexLock);
    for (int t = 0; t < kTargetCount; ++t) {
        SafeReleaseRaw(s_gameTex[t].albedoSrv);
        SafeReleaseRaw(s_gameTex[t].packedSrv);
        SafeReleaseRaw(s_gameTex[t].albedoRes);
        SafeReleaseRaw(s_gameTex[t].packedRes);
        s_gameTex[t] = {};
    }
    ReleaseSRWLockExclusive(&s_gameTexLock);

    SweepGraveyard(true);
    ReleaseSceneCopy();
    ReleaseDepthCopy();

    if (s_overlayDs) { s_overlayDs->Release(); s_overlayDs = nullptr; }
    if (s_bodyDepthDs) { s_bodyDepthDs->Release(); s_bodyDepthDs = nullptr; }
    if (s_overlayBlend) { s_overlayBlend->Release(); s_overlayBlend = nullptr; }
    if (s_samplerPoint) { s_samplerPoint->Release(); s_samplerPoint = nullptr; }
    if (s_samplerLinear) { s_samplerLinear->Release(); s_samplerLinear = nullptr; }
    if (s_cb) { s_cb->Release(); s_cb = nullptr; }
    if (s_immediate) { s_immediate->Release(); s_immediate = nullptr; }
    if (s_device) { s_device->Release(); s_device = nullptr; }

    Logger::Info("CarFX shut down.");
}

void OnNewFrame(ID3D11DeviceContext* /*context*/) {
    if (!s_initialized.load(std::memory_order_acquire)) return;

    const uint32_t prevFrame = s_frameCounter.fetch_add(1, std::memory_order_relaxed);
    const uint32_t totalPrev = s_totalDrawsFrame.exchange(0, std::memory_order_relaxed);
    const uint32_t matchedPrev = s_matchedDrawsFrame.exchange(0, std::memory_order_relaxed);
    uint32_t targetPrev[kTargetCount]{};
    for (int t = 0; t < kTargetCount; ++t) {
        targetPrev[t] = s_targetMatchedFrame[t].exchange(0, std::memory_order_relaxed);
    }
    s_totalDrawsLastFrame.store(totalPrev, std::memory_order_relaxed);
    const bool active = s_enabled.load(std::memory_order_relaxed) &&
                        (s_targets[kTargetBody].ps.load(std::memory_order_relaxed) != nullptr);
    const bool ghostActive = (s_activeGhostShaderMask.load(std::memory_order_relaxed) != 0);
    if (active || ghostActive || (prevFrame % 30) == 0) {
        s_matchedDrawsLastFrame.store(matchedPrev, std::memory_order_relaxed);
        for (int t = 0; t < kTargetCount; ++t) {
            s_targetMatchedLastFrame[t].store(targetPrev[t], std::memory_order_relaxed);
        }
    }

    SweepGraveyard(false);

    const ULONGLONG now = GetTickCount64();
    if (now >= s_nextHotReloadCheckMs) {
        s_nextHotReloadCheckMs = now + 250;
        PollHotReload();
    }
}

void SetHookedDrawFlag(bool hooked) {
    s_hookedDraw.store(hooked, std::memory_order_release);
}

void SetPlayerTargetTextures(int target, const TargetTexturesInput& input) {
    if (!ValidTarget(target) || !s_initialized.load(std::memory_order_acquire)) return;

    // Quickly skip if identical raw handles already resolved
    {
        bool unchanged = false;
        AcquireSRWLockShared(&s_gameTexLock);
        const GameSrvSnapshot& cur = s_gameTex[target];
        if (cur.valid == input.valid &&
            cur.albedoPtrRaw == reinterpret_cast<uintptr_t>(input.nativeAlbedoPtr) &&
            cur.packedPtrRaw == reinterpret_cast<uintptr_t>(input.nativePackedPtr) &&
            cur.albedoIsDefault == input.albedoIsDefault &&
            cur.packedIsDefault == input.packedIsDefault) {
            unchanged = true;
        }
        ReleaseSRWLockShared(&s_gameTexLock);
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

    GameSrvSnapshot old{};
    AcquireSRWLockExclusive(&s_gameTexLock);
    old = s_gameTex[target];
    s_gameTex[target] = next;
    s_gameTexGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&s_gameTexLock);

    if (old.albedoSrv) Retire(old.albedoSrv);
    if (old.packedSrv) Retire(old.packedSrv);
    if (old.albedoRes) Retire(old.albedoRes);
    if (old.packedRes) Retire(old.packedRes);

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

bool LoadShader(int target, const std::string& pathUtf8, std::string* outError) {
    if (!ValidTarget(target) || pathUtf8.empty()) return false;
    std::wstring full = ResolvePathW(pathUtf8);
    return CompileEffectFile(target, full, outError);
}

void UnloadShader(int target) {
    if (!ValidTarget(target)) return;
    TargetSlotState& ts = s_targets[target];
    {
        std::lock_guard<std::mutex> lk(ts.publishMtx);
        ID3D11PixelShader* oldPs = ts.ps.exchange(nullptr, std::memory_order_release);
        if (oldPs) Retire(oldPs);
        ts.shaderPath.clear();
        ts.lastWriteTime = {};
        ts.lastError.clear();
        ts.decl = {};
        ts.syncedSrcCompiles = 0;
        ts.declStealth.store(false, std::memory_order_relaxed);
        ts.usesScene.store(false, std::memory_order_relaxed);
        ts.usesDepth.store(false, std::memory_order_relaxed);
        ts.usesLivery.store(false, std::memory_order_relaxed);
    }
    UpdateGhostShaderMask();
}

bool ReloadShader(int target, std::string* outError) {
    if (!ValidTarget(target)) return false;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(s_targets[target].publishMtx);
        if (s_targets[target].shaderPath.empty()) return false;
        path = Utf8ToWide(s_targets[target].shaderPath);
    }
    return CompileEffectFile(target, path, outError);
}

void PollHotReload() {
    if (!s_initialized.load(std::memory_order_acquire)) return;
    for (int t = 0; t < kTargetCount; ++t) {
        std::wstring path;
        FILETIME prevFt = {};
        {
            std::lock_guard<std::mutex> lk(s_targets[t].publishMtx);
            if (s_targets[t].shaderPath.empty()) continue;
            path = Utf8ToWide(s_targets[t].shaderPath);
            prevFt = s_targets[t].lastWriteTime;
        }
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            if (CompareFileTime(&fad.ftLastWriteTime, &prevFt) != 0) {
                CompileEffectFile(t, path, nullptr);
            }
        }
    }
}

ReplayShaderConfig CaptureActiveConfig(int target) {
    ReplayShaderConfig cfg{};
    if (!ValidTarget(target)) return cfg;
    if (target == kTargetBody && !s_enabled.load(std::memory_order_acquire)) return cfg;

    TargetSlotState& ts = s_targets[target];
    if (!ts.slotEnabled.load(std::memory_order_acquire)) return cfg;
    if (ts.ps.load(std::memory_order_acquire) == nullptr) return cfg;

    std::lock_guard<std::mutex> lk(ts.publishMtx);
    if (ts.shaderPath.empty()) return cfg;

    size_t slashPos = ts.shaderPath.find_last_of("\\/");
    cfg.shaderPath = (slashPos != std::string::npos) ? ts.shaderPath.substr(slashPos + 1) : ts.shaderPath;
    cfg.enabled = true;
    cfg.overrideMode = ts.overrideMode.load(std::memory_order_relaxed);
    cfg.overrideStealth = ts.overrideStealth.load(std::memory_order_relaxed);
    for (int p = 0; p < kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            cfg.params[p][c] = ts.param[p][c].load(std::memory_order_relaxed);
        }
    }
    for (int i = 0; i < kTextures; ++i) {
        if (!ts.userTexPaths[i].empty()) {
            size_t tSlash = ts.userTexPaths[i].find_last_of("\\/");
            cfg.texPaths[i] = (tSlash != std::string::npos) ? ts.userTexPaths[i].substr(tSlash + 1) : ts.userTexPaths[i];
        }
    }
    return cfg;
}

bool ApplyConfigToTarget(int target, const ReplayShaderConfig& cfg, std::string* outError) {
    if (!ValidTarget(target)) return false;
    if (!cfg.enabled || cfg.shaderPath.empty()) {
        UnloadShader(target);
        return true;
    }

    if (!LoadShader(target, cfg.shaderPath, outError)) {
        return false;
    }

    TargetSlotState& ts = s_targets[target];
    ts.overrideMode.store(cfg.overrideMode, std::memory_order_release);
    ts.overrideStealth.store(cfg.overrideStealth, std::memory_order_release);
    for (int p = 0; p < kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            ts.param[p][c].store(cfg.params[p][c], std::memory_order_relaxed);
        }
    }
    for (int i = 0; i < kTextures; ++i) {
        if (!cfg.texPaths[i].empty()) {
            SetUserTexture(target, i, cfg.texPaths[i], nullptr);
        } else {
            ClearUserTexture(target, i);
        }
    }
    ts.slotEnabled.store(true, std::memory_order_release);
    UpdateGhostShaderMask();
    return true;
}

void SyncTargetFromSource(int dstTarget, int srcTarget) {
    if (!ValidTarget(dstTarget) || !ValidTarget(srcTarget) || dstTarget == srcTarget) return;

    if (!IsTargetActiveInternal(srcTarget)) {
        if (s_targets[dstTarget].ps.load(std::memory_order_acquire) != nullptr) {
            UnloadShader(dstTarget);
        }
        return;
    }

    std::string srcPath;
    uint32_t srcCompiles = 0;
    std::string srcTex[kTextures];
    {
        std::lock_guard<std::mutex> lk(s_targets[srcTarget].publishMtx);
        srcPath = s_targets[srcTarget].shaderPath;
        srcCompiles = s_targets[srcTarget].compiles;
        for (int i = 0; i < kTextures; ++i) srcTex[i] = s_targets[srcTarget].userTexPaths[i];
    }
    if (srcPath.empty()) {
        UnloadShader(dstTarget);
        return;
    }

    bool needLoad = false;
    {
        std::lock_guard<std::mutex> lk(s_targets[dstTarget].publishMtx);
        if (s_targets[dstTarget].shaderPath != srcPath ||
            s_targets[dstTarget].syncedSrcCompiles != srcCompiles ||
            s_targets[dstTarget].ps.load(std::memory_order_relaxed) == nullptr) {
            needLoad = true;
        }
    }

    if (needLoad) {
        if (!CompileEffectFile(dstTarget, Utf8ToWide(srcPath), nullptr)) {
            return;
        }
        std::lock_guard<std::mutex> lk(s_targets[dstTarget].publishMtx);
        s_targets[dstTarget].syncedSrcCompiles = srcCompiles;
    }

    TargetSlotState& src = s_targets[srcTarget];
    TargetSlotState& dst = s_targets[dstTarget];
    dst.overrideMode.store(src.overrideMode.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dst.overrideStealth.store(src.overrideStealth.load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int p = 0; p < kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            dst.param[p][c].store(src.param[p][c].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
    for (int i = 0; i < kTextures; ++i) {
        std::string dstTex;
        {
            std::lock_guard<std::mutex> lk(dst.publishMtx);
            dstTex = dst.userTexPaths[i];
        }
        if (dstTex != srcTex[i]) {
            if (!srcTex[i].empty()) SetUserTexture(dstTarget, i, srcTex[i], nullptr);
            else ClearUserTexture(dstTarget, i);
        }
    }
    dst.slotEnabled.store(true, std::memory_order_release);
    UpdateGhostShaderMask();
}

void ResetGhostTargets() {
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        UnloadShader(t);
        for (int i = 0; i < kTextures; ++i) {
            ClearUserTexture(t, i);
        }
        ClearTargetTextures(t);
        AcquireSRWLockExclusive(&s_spatialLock[t]);
        s_spatialInput[t] = SpatialProximityInput{};
        ReleaseSRWLockExclusive(&s_spatialLock[t]);
        for (int i = 0; i < kGameInputs; ++i) {
            s_gameInputs[t][i].store(0.0f, std::memory_order_relaxed);
        }
    }
}

void SetEnabled(bool enabled) {
    s_enabled.store(enabled, std::memory_order_release);
}

bool IsEnabled() {
    return s_enabled.load(std::memory_order_acquire);
}

void SetTargetEnabled(int target, bool enabled) {
    if (!ValidTarget(target)) return;
    s_targets[target].slotEnabled.store(enabled, std::memory_order_release);
    UpdateGhostShaderMask();
}

bool IsTargetCompiled(int target) {
    return ValidTarget(target) && (s_targets[target].ps.load(std::memory_order_acquire) != nullptr);
}

bool IsTargetActive(int target) {
    return IsTargetActiveInternal(target);
}

bool ShouldBypassDepthPrePass(int target) {
    return s_initialized.load(std::memory_order_acquire) &&
           IsTargetActiveInternal(target) &&
           s_targets[target].usesScene.load(std::memory_order_acquire);
}

void SetStealthDepthWrite(bool on) {
    s_stealthDepthWrite.store(on, std::memory_order_relaxed);
}

bool ShouldHideCarInternals(int target) {
    if (!s_initialized.load(std::memory_order_acquire) || !IsTargetActiveInternal(target)) {
        return false;
    }
    const int ov = s_targets[target].overrideStealth.load(std::memory_order_acquire);
    if (ov == kStealthOn) return true;
    if (ov == kStealthOff) return false;
    return s_targets[target].declStealth.load(std::memory_order_acquire);
}

void SetOverrideMode(int target, int overrideMode) {
    if (!ValidTarget(target)) return;
    if (overrideMode < kModeAuto || overrideMode > kModeOverlay) overrideMode = kModeAuto;
    s_targets[target].overrideMode.store(overrideMode, std::memory_order_release);
}

int GetOverrideMode(int target) {
    return ValidTarget(target) ? s_targets[target].overrideMode.load(std::memory_order_acquire) : kModeAuto;
}

void SetOverrideStealth(int target, int overrideStealth) {
    if (!ValidTarget(target)) return;
    if (overrideStealth < kStealthAuto || overrideStealth > kStealthOn) overrideStealth = kStealthAuto;
    s_targets[target].overrideStealth.store(overrideStealth, std::memory_order_release);
}

int GetOverrideStealth(int target) {
    return ValidTarget(target) ? s_targets[target].overrideStealth.load(std::memory_order_acquire) : kStealthAuto;
}

bool SetParamByName(int target, const char* name, const float* values, int count) {
    if (!ValidTarget(target) || !name || !values || count <= 0) return false;
    TargetSlotState& ts = s_targets[target];
    std::lock_guard<std::mutex> lk(ts.publishMtx);
    const ParamDecl* pd = FindParam(ts.decl, name);
    if (!pd) return false;
    int n = (std::min)(count, pd->count);
    for (int i = 0; i < n; ++i) {
        ts.param[pd->slot][pd->first + i].store(ClampToDecl(*pd, values[i]), std::memory_order_relaxed);
    }
    return true;
}

bool GetParamByName(int target, const char* name, float* outValues, int maxCount) {
    if (!ValidTarget(target) || !name || !outValues || maxCount <= 0) return false;
    TargetSlotState& ts = s_targets[target];
    std::lock_guard<std::mutex> lk(ts.publishMtx);
    const ParamDecl* pd = FindParam(ts.decl, name);
    if (!pd) return false;
    int n = (std::min)(maxCount, pd->count);
    for (int i = 0; i < n; ++i) {
        outValues[i] = ts.param[pd->slot][pd->first + i].load(std::memory_order_relaxed);
    }
    return true;
}

bool SetParamSlot(int target, int slot, const float* values, int count) {
    if (!ValidTarget(target) || slot < 0 || slot >= kParams || !values || count <= 0) return false;
    int n = (std::min)(count, 4);
    for (int i = 0; i < n; ++i) {
        s_targets[target].param[slot][i].store(values[i], std::memory_order_relaxed);
    }
    return true;
}

bool GetParamSlot(int target, int slot, float out[4]) {
    if (!ValidTarget(target) || slot < 0 || slot >= kParams || !out) return false;
    for (int i = 0; i < 4; ++i) {
        out[i] = s_targets[target].param[slot][i].load(std::memory_order_relaxed);
    }
    return true;
}

bool SetUserTexture(int target, int texSlot, const std::string& imagePathUtf8, std::string* outError) {
    if (!ValidTarget(target) || texSlot < 0 || texSlot >= kTextures || !s_device) return false;
    if (imagePathUtf8.empty()) {
        ClearUserTexture(target, texSlot);
        return true;
    }

    std::wstring full = ResolvePathW(imagePathUtf8);
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hrCom);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        if (needUninit) CoUninitialize();
        if (outError) *outError = "WIC factory failed";
        return false;
    }

    auto fail = [&](const char* msg) -> bool {
        if (factory) factory->Release();
        if (needUninit) CoUninitialize();
        if (outError) *outError = msg;
        return false;
    };

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(full.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder) {
        return fail("Failed to decode image");
    }
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
        decoder->Release();
        return fail("Failed to read image frame");
    }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > kMaxTexSide || h > kMaxTexSide) {
        frame->Release();
        decoder->Release();
        return fail("Invalid image dimensions");
    }

    IWICFormatConverter* conv = nullptr;
    if (FAILED(factory->CreateFormatConverter(&conv)) || !conv ||
        FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        if (conv) conv->Release();
        frame->Release();
        decoder->Release();
        return fail("WIC format conversion failed");
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (needUninit) CoUninitialize();
    if (FAILED(hr)) {
        if (outError) *outError = "CopyPixels failed";
        return false;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(s_device->CreateTexture2D(&td, &initData, &tex)) || !tex) {
        if (outError) *outError = "CreateTexture2D failed";
        return false;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    hr = s_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) {
        if (outError) *outError = "CreateShaderResourceView failed";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(s_targets[target].publishMtx);
        s_targets[target].userTexPaths[texSlot] = imagePathUtf8;
    }
    ID3D11ShaderResourceView* old = s_targets[target].userTex[texSlot].exchange(srv, std::memory_order_release);
    if (old) Retire(old);
    if (outError) outError->clear();
    Logger::Info("CarFX loaded user texture[" + std::to_string(texSlot) + "] (" +
                 std::to_string(w) + "x" + std::to_string(h) + "): " + WideToUtf8(full));
    return true;
}

void ClearUserTexture(int target, int texSlot) {
    if (!ValidTarget(target) || texSlot < 0 || texSlot >= kTextures) return;
    {
        std::lock_guard<std::mutex> lk(s_targets[target].publishMtx);
        s_targets[target].userTexPaths[texSlot].clear();
    }
    ID3D11ShaderResourceView* old = s_targets[target].userTex[texSlot].exchange(nullptr, std::memory_order_release);
    if (old) Retire(old);
}

void SetGameInput(int index, float value, int target) {
    if (ValidTarget(target) && index >= 0 && index < kGameInputs) {
        s_gameInputs[target][index].store(value, std::memory_order_relaxed);
    }
}

float GetGameInput(int index, int target) {
    return (ValidTarget(target) && index >= 0 && index < kGameInputs)
        ? s_gameInputs[target][index].load(std::memory_order_relaxed) : 0.0f;
}

void SetSpatialProximity(const SpatialProximityInput& input, int target) {
    if (!ValidTarget(target)) return;
    AcquireSRWLockExclusive(&s_spatialLock[target]);
    s_spatialInput[target] = input;
    ReleaseSRWLockExclusive(&s_spatialLock[target]);
}

SpatialProximityInput GetSpatialProximity(int target) {
    if (!ValidTarget(target)) return SpatialProximityInput{};
    AcquireSRWLockShared(&s_spatialLock[target]);
    SpatialProximityInput copy = s_spatialInput[target];
    ReleaseSRWLockShared(&s_spatialLock[target]);
    return copy;
}

bool TryHandleDraw(ID3D11DeviceContext* context, UINT /*countPerInstance*/, DrawIssueFn issueFn, void* issueCtx) {
    if (!context || !issueFn || !s_initialized.load(std::memory_order_acquire)) return false;
    if (!NativeCarX::IsInSessionFast()) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;

    s_totalDrawsFrame.fetch_add(1, std::memory_order_relaxed);

    const bool playerWant = IsTargetActiveInternal(kTargetBody);
    const bool anyGhostWant = (s_activeGhostShaderMask.load(std::memory_order_acquire) != 0);

    // Run detection when player or any ghost has an active shader, or every 30 frames for diagnostics
    const uint32_t frameId = s_frameCounter.load(std::memory_order_relaxed);
    const bool runDiagOnly = (!playerWant && !anyGhostWant) && ((frameId % 30) != 0);
    if (runDiagOnly) return false;

    int matchedTarget = kTargetNone;
    uint32_t slotMask = 0;
    ID3D11ShaderResourceView* albedoSrv = nullptr;
    ID3D11Resource* rtRes = nullptr;
    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11DepthStencilView* dsv = nullptr;

    if (!MatchTargetAndCheckRT(context, &matchedTarget, &slotMask, &albedoSrv,
                               &rtRes, &rtW, &rtH, &rtFmt, &dsv)) {
        const uintptr_t knownPs = s_lastGamePs.load(std::memory_order_relaxed);
        static uint32_t s_lastGhostMissDiagFrame = ~0u;
        if (anyGhostWant && knownPs && s_targetMatchedLastFrame[kTargetGhostBase].load(std::memory_order_relaxed) == 0 &&
            (frameId - s_lastGhostMissDiagFrame) >= 180) {
            ID3D11PixelShader* ps = nullptr;
            context->PSGetShader(&ps, nullptr, nullptr);
            if (ps) ps->Release();
            if (reinterpret_cast<uintptr_t>(ps) == knownPs) {
                ID3D11RenderTargetView* rtv = nullptr;
                context->OMGetRenderTargets(1, &rtv, nullptr);
                if (rtv) {
                    ID3D11Resource* rtR = nullptr;
                    rtv->GetResource(&rtR);
                    rtv->Release();
                    UINT w = 0, h = 0; DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
                    if (rtR) {
                        ID3D11Texture2D* tx = nullptr;
                        if (SUCCEEDED(rtR->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tx))) && tx) {
                            D3D11_TEXTURE2D_DESC td{}; tx->GetDesc(&td);
                            w = td.Width; h = td.Height; f = td.Format;
                            tx->Release();
                        }
                        rtR->Release();
                    }
                    if (w >= 640 && !IsTwoChannelOrDepthFormat(f)) {
                        s_lastGhostMissDiagFrame = frameId;
                        ID3D11ShaderResourceView* srvs[kScanSlotCount] = {};
                        context->PSGetShaderResources(kScanStartSlot, kScanSlotCount, srvs);
                        ID3D11Resource* gPackRes = nullptr;
                        ID3D11ShaderResourceView* gPackSrv = nullptr;
                        {
                            AcquireSRWLockShared(&s_gameTexLock);
                            gPackRes = s_gameTex[kTargetGhostBase].packedRes;
                            gPackSrv = s_gameTex[kTargetGhostBase].packedSrv;
                            ReleaseSRWLockShared(&s_gameTexLock);
                        }
                        char info[384] = {};
                        int off = _snprintf_s(info, sizeof(info), _TRUNCATE,
                            "Ghost miss-diag PS=%p RT=%ux%u(fmt=%u) gPack=(srv=%p,res=%p) bound:",
                            ps, w, h, (unsigned)f, gPackSrv, gPackRes);
                        for (int i = 0; i < kScanSlotCount; ++i) {
                            if (!srvs[i]) continue;
                            ID3D11Resource* r = nullptr;
                            srvs[i]->GetResource(&r);
                            if (off > 0 && off < (int)sizeof(info) - 32) {
                                off += _snprintf_s(info + off, sizeof(info) - off, _TRUNCATE,
                                    " t%d=%p/%p", kScanStartSlot + i, srvs[i], r);
                            }
                            if (r) r->Release();
                            srvs[i]->Release();
                        }
                        Logger::Info(info);
                    }
                }
            }
        }
        return false;
    }

    if (ValidTarget(matchedTarget)) {
        s_targetMatchedFrame[matchedTarget].fetch_add(1, std::memory_order_relaxed);
    }
    // Record telemetry for player diagnostics (and first-time ghost verification)
    if (matchedTarget == kTargetBody) {
        s_matchedDrawsFrame.fetch_add(1, std::memory_order_relaxed);
        s_lastSlotMask.store(slotMask, std::memory_order_relaxed);
        s_lastRtWidth.store(rtW, std::memory_order_relaxed);
        s_lastRtHeight.store(rtH, std::memory_order_relaxed);
        s_lastRtFormat.store(static_cast<uint32_t>(rtFmt), std::memory_order_relaxed);
    }

    ID3D11PixelShader* origPs = nullptr;
    context->PSGetShader(&origPs, nullptr, nullptr);
    if (matchedTarget == kTargetBody) {
        s_lastGamePs.store(reinterpret_cast<uintptr_t>(origPs), std::memory_order_relaxed);
    }

    static bool s_loggedPlayerDrawOnce = false;
    static bool s_loggedGhostDrawOnce = false;
    if (matchedTarget == kTargetBody && !s_loggedPlayerDrawOnce) {
        s_loggedPlayerDrawOnce = true;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "CarFX FIRST MATCHED PLAYER CAR DRAW: target=%d slots=0x%X rt=%ux%u fmt=%u origPs=%p",
                    matchedTarget, slotMask, rtW, rtH, static_cast<unsigned>(rtFmt), origPs);
        Logger::Info(buf);
    } else if (IsGhostTarget(matchedTarget) && !s_loggedGhostDrawOnce) {
        s_loggedGhostDrawOnce = true;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "CarFX FIRST MATCHED GHOST CAR DRAW: target=%d slots=0x%X rt=%ux%u fmt=%u origPs=%p",
                    matchedTarget, slotMask, rtW, rtH, static_cast<unsigned>(rtFmt), origPs);
        Logger::Info(buf);
    }

    TargetSlotState& ts = s_targets[matchedTarget];
    ID3D11PixelShader* customPs = ts.ps.load(std::memory_order_acquire);
    if (!IsTargetActiveInternal(matchedTarget) || !customPs || !s_cb) {
        if (origPs) origPs->Release();
        if (albedoSrv) albedoSrv->Release();
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        return false;
    }

    int ov = ts.overrideMode.load(std::memory_order_acquire);
    int mode = (ov != kModeAuto) ? ov : ts.declMode.load(std::memory_order_acquire);
    if (mode != kDeclModeOverlay) mode = kDeclModeReplace;

    // 1. Copy scene / depth ONCE per frame right before the first body part is overwritten (replace)
    //    or if overlay shader samples background/depth!
    const bool needScene = ts.usesScene.load(std::memory_order_acquire);
    const bool needDepth = ts.usesDepth.load(std::memory_order_acquire);
    if (needScene && s_sceneCopiedFrame != frameId) {
        if (EnsureSceneCopy(context, rtRes)) s_sceneCopiedFrame = frameId;
    }
    if (needDepth && s_depthCopiedFrame != frameId && dsv) {
        if (EnsureDepthCopy(context, dsv)) s_depthCopiedFrame = frameId;
    }

    // 2. Update CarFx constant buffer b13
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    context->RSGetViewports(&nvp, &vp);

    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);
    const int64_t prevQpc = s_lastDrawQpc.exchange(nowQpc.QuadPart, std::memory_order_relaxed);
    const double freq = s_qpcFreq.QuadPart ? static_cast<double>(s_qpcFreq.QuadPart) : 1.0;

    FxConstants k{};
    k.time[0] = static_cast<float>((nowQpc.QuadPart - ts.loadQpc.QuadPart) / freq);
    k.time[1] = prevQpc ? static_cast<float>((nowQpc.QuadPart - prevQpc) / freq) : 0.0f;
    k.time[2] = static_cast<float>(s_matchedDrawsFrame.load(std::memory_order_relaxed));
    k.time[3] = static_cast<float>(matchedTarget);

    k.screen[0] = (nvp && vp.Width > 0.0f) ? vp.Width : static_cast<float>(rtW);
    k.screen[1] = (nvp && vp.Height > 0.0f) ? vp.Height : static_cast<float>(rtH);
    k.screen[2] = rtW > 0 ? 1.0f / static_cast<float>(rtW) : 0.0f;
    k.screen[3] = rtH > 0 ? 1.0f / static_cast<float>(rtH) : 0.0f;

    for (int p = 0; p < kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            k.param[p][c] = ts.param[p][c].load(std::memory_order_relaxed);
        }
    }

    ID3D11ShaderResourceView* tex0 = ts.userTex[0].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex1 = ts.userTex[1].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex2 = ts.userTex[2].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex3 = ts.userTex[3].load(std::memory_order_acquire);

    k.frameInfo[0] = albedoSrv ? 1.0f : 0.0f;
    k.frameInfo[1] = tex0 ? 1.0f : 0.0f;
    k.frameInfo[2] = tex1 ? 1.0f : 0.0f;
    k.frameInfo[3] = static_cast<float>(mode);

    for (int i = 0; i < kGameInputs; ++i) {
        k.game[i / 4][i % 4] = s_gameInputs[matchedTarget][i].load(std::memory_order_relaxed);
    }

    {
        AcquireSRWLockShared(&s_spatialLock[matchedTarget]);
        const SpatialProximityInput sp = s_spatialInput[matchedTarget];
        ReleaseSRWLockShared(&s_spatialLock[matchedTarget]);
        memcpy(k.carPos, sp.carPos, sizeof(k.carPos));
        memcpy(k.carRight, sp.carRight, sizeof(k.carRight));
        memcpy(k.carUp, sp.carUp, sizeof(k.carUp));
        memcpy(k.carFwd, sp.carFwd, sizeof(k.carFwd));
        memcpy(k.camPos, sp.camPos, sizeof(k.camPos));
        memcpy(k.nearCarPos, sp.nearCarPos, sizeof(k.nearCarPos));
        memcpy(k.nearCarRight, sp.nearCarRight, sizeof(k.nearCarRight));
        memcpy(k.nearCarFwd, sp.nearCarFwd, sizeof(k.nearCarFwd));
        memcpy(k.nearCarClosest, sp.nearCarClosest, sizeof(k.nearCarClosest));
        memcpy(k.wallDirDist, sp.wallDirDist, sizeof(k.wallDirDist));
        memcpy(k.wallNearestHit, sp.wallNearestHit, sizeof(k.wallNearestHit));
        memcpy(k.wallNearestNormal, sp.wallNearestNormal, sizeof(k.wallNearestNormal));
        memcpy(k.wallFrontHit, sp.wallFrontHit, sizeof(k.wallFrontHit));
        memcpy(k.wallRearHit, sp.wallRearHit, sizeof(k.wallRearHit));
        memcpy(k.wallLeftHit, sp.wallLeftHit, sizeof(k.wallLeftHit));
        memcpy(k.wallRightHit, sp.wallRightHit, sizeof(k.wallRightHit));
        memcpy(k.wallDoorLeftHit, sp.wallDoorLeftHit, sizeof(k.wallDoorLeftHit));
        memcpy(k.wallDoorRightHit, sp.wallDoorRightHit, sizeof(k.wallDoorRightHit));
        memcpy(k.wallFrontLeftHit, sp.wallFrontLeftHit, sizeof(k.wallFrontLeftHit));
        memcpy(k.wallFrontRightHit, sp.wallFrontRightHit, sizeof(k.wallFrontRightHit));
        memcpy(k.wallRearLeftHit, sp.wallRearLeftHit, sizeof(k.wallRearLeftHit));
        memcpy(k.wallRearRightHit, sp.wallRearRightHit, sizeof(k.wallRearRightHit));
        memcpy(k.driftState, sp.driftState, sizeof(k.driftState));
        memcpy(k.camRight, sp.camRight, sizeof(k.camRight));
        memcpy(k.camUp, sp.camUp, sizeof(k.camUp));
        memcpy(k.camFwd, sp.camFwd, sizeof(k.camFwd));
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        if (origPs) origPs->Release();
        if (albedoSrv) albedoSrv->Release();
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        return false;
    }
    memcpy(mapped.pData, &k, sizeof(k));
    context->Unmap(s_cb, 0);

    // 3. In overlay mode, let the game draw base paint + baked livery + lighting FIRST!
    if (mode == kDeclModeOverlay) {
        issueFn(issueCtx);
    }

    // 4. Save game state, bind CarFX PS/CB/SRVs (and Blend/DS for overlay), draw, restore!
    BoundState saved{};
    saved.active = true;
    saved.ps = origPs;   // ownership transferred to saved
    context->PSGetConstantBuffers(kCbSlot, 1, &saved.cb);
    context->PSGetShaderResources(kSceneSlot, kViewCount, saved.srvs);
    context->PSGetSamplers(14, 2, saved.samplers);
    const bool depthWrite = (mode != kDeclModeOverlay) && s_stealthDepthWrite.load(std::memory_order_relaxed) &&
                            s_bodyDepthDs && matchedTarget == kTargetBody && ShouldHideCarInternals(matchedTarget);
    if (mode == kDeclModeOverlay || depthWrite) {
        saved.restoreOm = true;
        context->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
        context->OMGetDepthStencilState(&saved.ds, &saved.stencilRef);
    }

    ID3D11ShaderResourceView* views[kViewCount] = {
        needScene ? s_sceneSrv : nullptr,   // t32: g_fxScene
        tex0,                               // t33: g_fxTex0
        tex1,                               // t34: g_fxTex1
        tex2,                               // t35: g_fxTex2
        tex3,                               // t36: g_fxTex3
        needDepth ? s_depthSrv : nullptr,   // t37: g_fxDepth
        albedoSrv                           // t38: g_fxLivery
    };
    ID3D11SamplerState* samplers[2] = { s_samplerLinear, s_samplerPoint };

    context->PSSetShader(customPs, nullptr, 0);
    context->PSSetConstantBuffers(kCbSlot, 1, &s_cb);
    context->PSSetShaderResources(kSceneSlot, kViewCount, views);
    context->PSSetSamplers(14, 2, samplers);
    if (depthWrite) {
        context->OMSetDepthStencilState(s_bodyDepthDs, saved.stencilRef);
    }
    if (mode == kDeclModeOverlay) {
        if (s_overlayBlend) context->OMSetBlendState(s_overlayBlend, nullptr, 0xFFFFFFFFu);
        if (s_overlayDs) context->OMSetDepthStencilState(s_overlayDs, saved.stencilRef);
    }

    issueFn(issueCtx);

    RestoreState(context, saved);

    if (albedoSrv) albedoSrv->Release();
    if (rtRes) rtRes->Release();
    if (dsv) dsv->Release();
    return true;
}

DiagnosticStatus GetStatus(int target) {
    DiagnosticStatus st{};
    st.initialized = s_initialized.load(std::memory_order_acquire);
    st.enabled = IsGhostTarget(target)
        ? (ValidTarget(target) && s_targets[target].slotEnabled.load(std::memory_order_acquire))
        : s_enabled.load(std::memory_order_acquire);
    st.hookedDraw = s_hookedDraw.load(std::memory_order_acquire);

    const int tIdx = ValidTarget(target) ? target : kTargetBody;
    st.totalDrawsFrame = s_totalDrawsLastFrame.load(std::memory_order_relaxed);
    st.matchedDrawsFrame = s_targetMatchedFrame[tIdx].load(std::memory_order_relaxed);
    st.matchedDrawsLastFrame = s_targetMatchedLastFrame[tIdx].load(std::memory_order_relaxed);
    st.sceneCopiesTotal = s_sceneCopiesTotal.load(std::memory_order_relaxed);
    st.depthCopiesTotal = s_depthCopiesTotal.load(std::memory_order_relaxed);
    st.lastSlotMask = s_lastSlotMask.load(std::memory_order_relaxed);
    st.lastRtWidth = s_lastRtWidth.load(std::memory_order_relaxed);
    st.lastRtHeight = s_lastRtHeight.load(std::memory_order_relaxed);
    st.lastRtFormat = s_lastRtFormat.load(std::memory_order_relaxed);
    st.lastGamePs = s_lastGamePs.load(std::memory_order_relaxed);

    const int texIdx = ValidTarget(target) ? target : kTargetBody;
    AcquireSRWLockShared(&s_gameTexLock);
    const GameSrvSnapshot& b = s_gameTex[texIdx];
    st.bodyTargetsValid = b.valid;
    st.bodyAlbedoIsDefault = b.albedoIsDefault;
    st.bodyPackedIsDefault = b.packedIsDefault;
    st.bodyAlbedoTexId = b.albedoTexId;
    st.bodyPackedTexId = b.packedTexId;
    st.bodyAlbedoSrv = reinterpret_cast<uintptr_t>(b.albedoSrv);
    st.bodyPackedSrv = reinterpret_cast<uintptr_t>(b.packedSrv);
    ReleaseSRWLockShared(&s_gameTexLock);

    if (ValidTarget(target)) {
        TargetSlotState& ts = s_targets[target];
        std::lock_guard<std::mutex> lk(ts.publishMtx);
        st.compiled = (ts.ps.load(std::memory_order_relaxed) != nullptr);
        st.activeMode = ts.overrideMode.load(std::memory_order_relaxed);
        int eff = st.activeMode != kModeAuto ? st.activeMode : ts.declMode.load(std::memory_order_relaxed);
        st.resolvedMode = eff;
        st.activeStealth = ts.overrideStealth.load(std::memory_order_relaxed);
        st.declStealth = ts.declStealth.load(std::memory_order_relaxed);
        st.resolvedStealth = (st.activeStealth == kStealthOn) || (st.activeStealth == kStealthAuto && st.declStealth);
        st.usesScene = ts.usesScene.load(std::memory_order_relaxed);
        st.usesDepth = ts.usesDepth.load(std::memory_order_relaxed);
        st.usesLivery = ts.usesLivery.load(std::memory_order_relaxed);
        st.compilesTotal = ts.compiles;
        st.compileFailures = ts.failures;
        st.shaderPath = ts.shaderPath;
        st.lastError = ts.lastError;
        st.decl = ts.decl;
    }

    return st;
}

std::vector<std::string> ListAvailableShaders() {
    std::vector<std::string> out;
    if (s_shadersDir.empty()) return out;
    std::wstring pat = s_shadersDir + L"\\*.hlsl";
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                out.push_back(WideToUtf8(fd.cFileName));
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    // shaders installed from the cloud library: shaders/cloud/<id>/*.hlsl  ->  "cloud/<id>/<name>.hlsl"
    WIN32_FIND_DATAW dd{};
    HANDLE hd = FindFirstFileW((s_shadersDir + L"\\cloud\\*").c_str(), &dd);
    if (hd != INVALID_HANDLE_VALUE) {
        do {
            if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (dd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (dd.cFileName[0] == L'.') continue;
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileW((s_shadersDir + L"\\cloud\\" + dd.cFileName + L"\\*.hlsl").c_str(), &fd2);
            if (h2 == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    out.push_back("cloud/" + WideToUtf8(dd.cFileName) + "/" + WideToUtf8(fd2.cFileName));
                }
            } while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        } while (FindNextFileW(hd, &dd));
        FindClose(hd);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring GetShadersDirectory() {
    return s_shadersDir;
}

} // namespace CarFX
