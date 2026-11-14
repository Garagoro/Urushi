#include "carfx.h"
#include "carfx_internal.h"
#include "logger.h"

#include <wincodec.h>
#include <algorithm>
#include <cstring>

namespace CarFX {
using namespace CarFX::Internal;

namespace {

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

} // namespace

namespace Internal {

bool CompileEffectMemory(int targetIdx, const char* srcData, size_t srcLen, const std::string& narrowPath, const std::wstring& includeDir, const FILETIME& ft, std::string* outErr) {
    if (!ValidTarget(targetIdx) || !g_device || !g_pfnD3DCompile) {
        if (outErr) *outErr = "CarFX or d3dcompiler_47.dll not initialized";
        return false;
    }

    if (!srcData || srcLen == 0) {
        if (outErr) *outErr = "Shader source is empty";
        return false;
    }

    ShaderDecl decl{};
    ParseShaderDecl(srcData, srcLen, &decl);

    ShaderIncludeHandler incl(includeDir, g_shadersDir);
    ID3DBlob* code = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = g_pfnD3DCompile(
        srcData, srcLen,
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

        TargetSlotState& ts = g_targets[targetIdx];
        std::lock_guard<std::mutex> lk(ts.publishMtx);
        ts.failures++;
        ts.lastError = msg;
        if (outErr) *outErr = msg;
        Logger::Error("CarFX compile error (" + narrowPath + "): " + msg);
        return false;
    }
    if (errBlob) errBlob->Release();

    ID3D11PixelShader* newPs = nullptr;
    hr = g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &newPs);
    if (FAILED(hr) || !newPs) {
        code->Release();
        if (outErr) *outErr = "CreatePixelShader failed";
        return false;
    }

    bool readsScene = false, readsDepth = false, readsLivery = false;
    if (g_pfnD3DReflect) {
        ID3D11ShaderReflection* refl = nullptr;
        if (SUCCEEDED(g_pfnD3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
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

    TargetSlotState& ts = g_targets[targetIdx];
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

bool CompileEffectFile(int targetIdx, const std::wstring& fullPath, std::string* outErr) {
    if (!ValidTarget(targetIdx) || !g_device || !g_pfnD3DCompile) {
        if (outErr) *outErr = "CarFX or d3dcompiler_47.dll not initialized";
        return false;
    }

    std::vector<char> src;
    FILETIME ft = {};
    if (!ReadFileUtf8(fullPath, src, ft)) {
        if (outErr) *outErr = "Cannot read shader file: " + WideToUtf8(fullPath);
        return false;
    }

    return CompileEffectMemory(targetIdx, src.data(), src.size(), WideToUtf8(fullPath), DirectoryOf(fullPath), ft, outErr);
}

} // namespace Internal

// ================= Shader Compilation, Hot-Reload & Textures =================

bool LoadShader(int target, const std::string& pathUtf8, std::string* outError) {
    if (!ValidTarget(target) || pathUtf8.empty()) return false;
    std::wstring full = ResolvePathW(pathUtf8);
    return CompileEffectFile(target, full, outError);
}

bool CompileShaderSource(int target, const std::string& source, const std::string& name, std::string* outError) {
    if (!ValidTarget(target) || source.empty()) {
        if (outError) *outError = "Invalid target or empty shader source";
        return false;
    }
    std::string vName = name.empty() ? "lua_shader.hlsl" : name;
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return CompileEffectMemory(target, source.data(), source.size(), vName, g_shadersDir, ft, outError);
}

void UnloadShader(int target) {
    if (!ValidTarget(target)) return;
    TargetSlotState& ts = g_targets[target];
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
        std::lock_guard<std::mutex> lk(g_targets[target].publishMtx);
        if (g_targets[target].shaderPath.empty()) return false;
        path = Utf8ToWide(g_targets[target].shaderPath);
    }
    return CompileEffectFile(target, path, outError);
}

void PollHotReload() {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    for (int t = 0; t < kTargetCount; ++t) {
        std::wstring path;
        FILETIME prevFt = {};
        {
            std::lock_guard<std::mutex> lk(g_targets[t].publishMtx);
            if (g_targets[t].shaderPath.empty()) continue;
            path = Utf8ToWide(g_targets[t].shaderPath);
            prevFt = g_targets[t].lastWriteTime;
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
    if (target == kTargetBody && !g_enabled.load(std::memory_order_acquire)) return cfg;

    TargetSlotState& ts = g_targets[target];
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

    TargetSlotState& ts = g_targets[target];
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
        if (g_targets[dstTarget].ps.load(std::memory_order_acquire) != nullptr) {
            UnloadShader(dstTarget);
        }
        return;
    }

    std::string srcPath;
    uint32_t srcCompiles = 0;
    std::string srcTex[kTextures];
    {
        std::lock_guard<std::mutex> lk(g_targets[srcTarget].publishMtx);
        srcPath = g_targets[srcTarget].shaderPath;
        srcCompiles = g_targets[srcTarget].compiles;
        for (int i = 0; i < kTextures; ++i) srcTex[i] = g_targets[srcTarget].userTexPaths[i];
    }
    if (srcPath.empty()) {
        UnloadShader(dstTarget);
        return;
    }

    bool needLoad = false;
    {
        std::lock_guard<std::mutex> lk(g_targets[dstTarget].publishMtx);
        if (g_targets[dstTarget].shaderPath != srcPath ||
            g_targets[dstTarget].syncedSrcCompiles != srcCompiles ||
            g_targets[dstTarget].ps.load(std::memory_order_relaxed) == nullptr) {
            needLoad = true;
        }
    }

    if (needLoad) {
        if (!CompileEffectFile(dstTarget, Utf8ToWide(srcPath), nullptr)) {
            return;
        }
        std::lock_guard<std::mutex> lk(g_targets[dstTarget].publishMtx);
        g_targets[dstTarget].syncedSrcCompiles = srcCompiles;
    }

    TargetSlotState& src = g_targets[srcTarget];
    TargetSlotState& dst = g_targets[dstTarget];
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
        AcquireSRWLockExclusive(&g_spatialLock[t]);
        g_spatialInput[t] = SpatialProximityInput{};
        ReleaseSRWLockExclusive(&g_spatialLock[t]);
        for (int i = 0; i < kGameInputs; ++i) {
            g_gameInputs[t][i].store(0.0f, std::memory_order_relaxed);
        }
    }
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void SetTargetEnabled(int target, bool enabled) {
    if (!ValidTarget(target)) return;
    g_targets[target].slotEnabled.store(enabled, std::memory_order_release);
    UpdateGhostShaderMask();
}

bool IsTargetCompiled(int target) {
    return ValidTarget(target) && (g_targets[target].ps.load(std::memory_order_acquire) != nullptr);
}

bool IsTargetActive(int target) {
    return IsTargetActiveInternal(target);
}

bool ShouldBypassDepthPrePass(int target) {
    return g_initialized.load(std::memory_order_acquire) &&
           IsTargetActiveInternal(target) &&
           g_targets[target].usesScene.load(std::memory_order_acquire);
}

void SetOverrideMode(int target, int overrideMode) {
    if (!ValidTarget(target)) return;
    if (overrideMode < kModeAuto || overrideMode > kModeOverlay) overrideMode = kModeAuto;
    g_targets[target].overrideMode.store(overrideMode, std::memory_order_release);
}

int GetOverrideMode(int target) {
    return ValidTarget(target) ? g_targets[target].overrideMode.load(std::memory_order_acquire) : kModeAuto;
}

void SetOverrideStealth(int target, int overrideStealth) {
    if (!ValidTarget(target)) return;
    if (overrideStealth < kStealthAuto || overrideStealth > kStealthOn) overrideStealth = kStealthAuto;
    g_targets[target].overrideStealth.store(overrideStealth, std::memory_order_release);
}

int GetOverrideStealth(int target) {
    return ValidTarget(target) ? g_targets[target].overrideStealth.load(std::memory_order_acquire) : kStealthAuto;
}

bool SetParamByName(int target, const char* name, const float* values, int count) {
    if (!ValidTarget(target) || !name || !values || count <= 0) return false;
    TargetSlotState& ts = g_targets[target];
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
    TargetSlotState& ts = g_targets[target];
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
        g_targets[target].param[slot][i].store(values[i], std::memory_order_relaxed);
    }
    return true;
}

bool GetParamSlot(int target, int slot, float out[4]) {
    if (!ValidTarget(target) || slot < 0 || slot >= kParams || !out) return false;
    for (int i = 0; i < 4; ++i) {
        out[i] = g_targets[target].param[slot][i].load(std::memory_order_relaxed);
    }
    return true;
}

bool SetUserTexture(int target, int texSlot, const std::string& imagePathUtf8, std::string* outError) {
    if (!ValidTarget(target) || texSlot < 0 || texSlot >= kTextures || !g_device) return false;
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
    if (FAILED(g_device->CreateTexture2D(&td, &initData, &tex)) || !tex) {
        if (outError) *outError = "CreateTexture2D failed";
        return false;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) {
        if (outError) *outError = "CreateShaderResourceView failed";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_targets[target].publishMtx);
        g_targets[target].userTexPaths[texSlot] = imagePathUtf8;
    }
    ID3D11ShaderResourceView* old = g_targets[target].userTex[texSlot].exchange(srv, std::memory_order_release);
    if (old) Retire(old);
    if (outError) outError->clear();
    Logger::Info("CarFX loaded user texture[" + std::to_string(texSlot) + "] (" +
                 std::to_string(w) + "x" + std::to_string(h) + "): " + WideToUtf8(full));
    return true;
}

void ClearUserTexture(int target, int texSlot) {
    if (!ValidTarget(target) || texSlot < 0 || texSlot >= kTextures) return;
    {
        std::lock_guard<std::mutex> lk(g_targets[target].publishMtx);
        g_targets[target].userTexPaths[texSlot].clear();
    }
    ID3D11ShaderResourceView* old = g_targets[target].userTex[texSlot].exchange(nullptr, std::memory_order_release);
    if (old) Retire(old);
}

void SetGameInput(int index, float value, int target) {
    if (ValidTarget(target) && index >= 0 && index < kGameInputs) {
        g_gameInputs[target][index].store(value, std::memory_order_relaxed);
    }
}

float GetGameInput(int index, int target) {
    return (ValidTarget(target) && index >= 0 && index < kGameInputs)
        ? g_gameInputs[target][index].load(std::memory_order_relaxed) : 0.0f;
}

void SetSpatialProximity(const SpatialProximityInput& input, int target) {
    if (!ValidTarget(target)) return;
    AcquireSRWLockExclusive(&g_spatialLock[target]);
    g_spatialInput[target] = input;
    ReleaseSRWLockExclusive(&g_spatialLock[target]);
}

SpatialProximityInput GetSpatialProximity(int target) {
    if (!ValidTarget(target)) return SpatialProximityInput{};
    AcquireSRWLockShared(&g_spatialLock[target]);
    SpatialProximityInput copy = g_spatialInput[target];
    ReleaseSRWLockShared(&g_spatialLock[target]);
    return copy;
}

std::vector<std::string> ListAvailableShaders() {
    std::vector<std::string> out;
    if (g_shadersDir.empty()) return out;
    std::wstring pat = g_shadersDir + L"\\*.hlsl";
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
    HANDLE hd = FindFirstFileW((g_shadersDir + L"\\cloud\\*").c_str(), &dd);
    if (hd != INVALID_HANDLE_VALUE) {
        do {
            if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (dd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (dd.cFileName[0] == L'.') continue;
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileW((g_shadersDir + L"\\cloud\\" + dd.cFileName + L"\\*.hlsl").c_str(), &fd2);
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
    return g_shadersDir;
}

} // namespace CarFX
