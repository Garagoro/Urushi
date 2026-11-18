#include "postfx.h"
#include "carfx.h"
#include "carfx_internal.h"
#include "logger.h"
#include "safe_mode.h"
#include "hook_manager.h"

#include <d3dcompiler.h>
#include <mutex>
#include <algorithm>
#include <cstring>

namespace PostFX {

namespace {

struct PostFxConstants {
    float time[4];       // x: time (s), y: deltaTime (s), z: frame, w: 0
    float screen[4];     // x: width, y: height, z: 1/width, w: 1/height
    float param[8][4];   // p0..p7
    float driftState[4]; // x: abs angle, y: signed angle, z: norm65, w: speed km/h
};
static_assert(sizeof(PostFxConstants) % 16 == 0, "PostFxConstants must align to 16 bytes");

DXGI_FORMAT UntypelessColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return fmt;
    }
}

struct SavedD3D11State {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT numScissorRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11Buffer* vertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blendState = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* dsState = nullptr;
    UINT stencilRef = 0;
    ID3D11Buffer* psConstantBuffers[14] = {};
    ID3D11ShaderResourceView* psSrvs[38] = {};
    ID3D11SamplerState* psSamplers[16] = {};
    ID3D11Buffer* vsConstantBuffers[2] = {};

    void Save(ID3D11DeviceContext* ctx) {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        ctx->RSGetViewports(&numViewports, viewports);
        ctx->RSGetScissorRects(&numScissorRects, scissorRects);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetInputLayout(&inputLayout);
        ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vertexBuffers, strides, offsets);
        ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->HSGetShader(&hs, nullptr, nullptr);
        ctx->DSGetShader(&ds, nullptr, nullptr);
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->RSGetState(&rs);
        ctx->OMGetBlendState(&blendState, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&dsState, &stencilRef);
        ctx->PSGetConstantBuffers(0, 14, psConstantBuffers);
        ctx->PSGetShaderResources(0, 38, psSrvs);
        ctx->PSGetSamplers(0, 16, psSamplers);
        ctx->VSGetConstantBuffers(0, 2, vsConstantBuffers);
    }

    void Restore(ID3D11DeviceContext* ctx) {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        for (auto& r : rtv) { if (r) { r->Release(); r = nullptr; } }
        if (dsv) { dsv->Release(); dsv = nullptr; }

        ctx->RSSetViewports(numViewports, viewports);
        ctx->RSSetScissorRects(numScissorRects, scissorRects);
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetInputLayout(inputLayout);
        if (inputLayout) { inputLayout->Release(); inputLayout = nullptr; }

        ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vertexBuffers, strides, offsets);
        for (auto& vb : vertexBuffers) { if (vb) { vb->Release(); vb = nullptr; } }

        ctx->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);
        if (indexBuffer) { indexBuffer->Release(); indexBuffer = nullptr; }

        ctx->VSSetShader(vs, nullptr, 0);
        if (vs) { vs->Release(); vs = nullptr; }

        ctx->PSSetShader(ps, nullptr, 0);
        if (ps) { ps->Release(); ps = nullptr; }

        ctx->GSSetShader(gs, nullptr, 0);
        if (gs) { gs->Release(); gs = nullptr; }

        ctx->HSSetShader(hs, nullptr, 0);
        if (hs) { hs->Release(); hs = nullptr; }

        ctx->DSSetShader(ds, nullptr, 0);
        if (ds) { ds->Release(); ds = nullptr; }

        ctx->CSSetShader(cs, nullptr, 0);
        if (cs) { cs->Release(); cs = nullptr; }

        ctx->RSSetState(rs);
        if (rs) { rs->Release(); rs = nullptr; }

        ctx->OMSetBlendState(blendState, blendFactor, sampleMask);
        if (blendState) { blendState->Release(); blendState = nullptr; }

        ctx->OMSetDepthStencilState(dsState, stencilRef);
        if (dsState) { dsState->Release(); dsState = nullptr; }

        ctx->PSSetConstantBuffers(0, 14, psConstantBuffers);
        for (auto& cb : psConstantBuffers) { if (cb) { cb->Release(); cb = nullptr; } }

        ctx->PSSetShaderResources(0, 38, psSrvs);
        for (auto& s : psSrvs) { if (s) { s->Release(); s = nullptr; } }

        ctx->PSSetSamplers(0, 16, psSamplers);
        for (auto& smp : psSamplers) { if (smp) { smp->Release(); smp = nullptr; } }

        ctx->VSSetConstantBuffers(0, 2, vsConstantBuffers);
        for (auto& cb : vsConstantBuffers) { if (cb) { cb->Release(); cb = nullptr; } }
    }
};

static ID3D11Device* s_device = nullptr;
static ID3D11DeviceContext* s_context = nullptr;
static ID3D11VertexShader* s_fullScreenVs = nullptr;
static ID3D11PixelShader* s_activePs = nullptr;
static ID3D11Buffer* s_cb = nullptr;
static ID3D11SamplerState* s_linearSampler = nullptr;
static ID3D11SamplerState* s_pointSampler = nullptr;
static ID3D11BlendState* s_opaqueBlend = nullptr;
static ID3D11DepthStencilState* s_noDepthDs = nullptr;
static ID3D11RasterizerState* s_rs = nullptr;

static ID3D11Texture2D* s_sceneCopyTex = nullptr;
static ID3D11ShaderResourceView* s_sceneCopySrv = nullptr;
static D3D11_TEXTURE2D_DESC s_sceneCopyDesc = {};

static std::wstring s_shadersDir;
static std::string s_activeShaderPath;
static FILETIME s_lastWriteTime = {};
static CarFX::ShaderDecl s_decl = {};
static std::atomic<bool> s_enabled{ false };
static std::atomic<float> s_params[8][4]{};
static std::mutex s_mutex;

static LARGE_INTEGER s_qpcFreq = {};
static LARGE_INTEGER s_startQpc = {};
static int64_t s_lastFrameQpc = 0;
static ULONGLONG s_nextHotReloadMs = 0;

static void ShutdownLocked() {
    UnloadShader();

    if (s_sceneCopySrv) { s_sceneCopySrv->Release(); s_sceneCopySrv = nullptr; }
    if (s_sceneCopyTex) { s_sceneCopyTex->Release(); s_sceneCopyTex = nullptr; }
    s_sceneCopyDesc = {};

    if (s_fullScreenVs) { s_fullScreenVs->Release(); s_fullScreenVs = nullptr; }
    if (s_cb) { s_cb->Release(); s_cb = nullptr; }
    if (s_linearSampler) { s_linearSampler->Release(); s_linearSampler = nullptr; }
    if (s_pointSampler) { s_pointSampler->Release(); s_pointSampler = nullptr; }
    if (s_opaqueBlend) { s_opaqueBlend->Release(); s_opaqueBlend = nullptr; }
    if (s_noDepthDs) { s_noDepthDs->Release(); s_noDepthDs = nullptr; }
    if (s_rs) { s_rs->Release(); s_rs = nullptr; }

    if (s_context) { s_context->Release(); s_context = nullptr; }
    if (s_device) { s_device->Release(); s_device = nullptr; }
}

} // namespace

bool Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& shadersDir) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!device || !context) return false;

    if (s_device) ShutdownLocked();

    s_device = device;
    s_device->AddRef();
    s_context = context;
    s_context->AddRef();
    s_shadersDir = shadersDir;

    QueryPerformanceFrequency(&s_qpcFreq);
    QueryPerformanceCounter(&s_startQpc);

    // Compile full-screen triangle vertex shader (VSMain generates coords directly from SV_VertexID)
    static const char kVsSource[] =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n";

    if (CarFX::Internal::g_pfnD3DCompile) {
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = CarFX::Internal::g_pfnD3DCompile(
            kVsSource, sizeof(kVsSource) - 1,
            "postfx_vs.hlsl", nullptr, nullptr,
            "main", "vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &vsBlob, &errBlob
        );
        if (SUCCEEDED(hr) && vsBlob) {
            s_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_fullScreenVs);
            vsBlob->Release();
        } else {
            if (errBlob) {
                Logger::Error("PostFX full-screen VS compile error: " +
                              std::string(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize()));
                errBlob->Release();
            }
        }
    }

    // Dynamic constant buffer for b0 / b13
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PostFxConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s_device->CreateBuffer(&cbd, nullptr, &s_cb);

    // Linear clamp sampler (s0 / s14)
    D3D11_SAMPLER_DESC smpDesc = {};
    smpDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smpDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smpDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smpDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smpDesc.MaxLOD = D3D11_FLOAT32_MAX;
    s_device->CreateSamplerState(&smpDesc, &s_linearSampler);

    // Point clamp sampler (s1 / s15)
    smpDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_device->CreateSamplerState(&smpDesc, &s_pointSampler);

    // Opaque overwrite blend state
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    s_device->CreateBlendState(&bld, &s_opaqueBlend);

    // Depth-stencil state: no depth test, no depth write
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    s_device->CreateDepthStencilState(&dsd, &s_noDepthDs);

    // Rasterizer state: no culling, solid fill
    D3D11_RASTERIZER_DESC rsd = {};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthClipEnable = FALSE;
    s_device->CreateRasterizerState(&rsd, &s_rs);

    Logger::Info("PostFX stage initialized.");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    ShutdownLocked();
}

void OnResizeBuffers() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sceneCopySrv) { s_sceneCopySrv->Release(); s_sceneCopySrv = nullptr; }
    if (s_sceneCopyTex) { s_sceneCopyTex->Release(); s_sceneCopyTex = nullptr; }
    s_sceneCopyDesc = {};
}

void OnNewFrame(ID3D11DeviceContext* /*context*/) {
    // Check hot-reload every ~500ms
    const ULONGLONG now = GetTickCount64();
    if (now >= s_nextHotReloadMs) {
        s_nextHotReloadMs = now + 500;
        ReloadShader(nullptr);
    }
}

void Render(IDXGISwapChain* swapChain, ID3D11RenderTargetView* backBufferRtv) {
    if (!s_enabled.load(std::memory_order_acquire) || !s_activePs || !swapChain || !s_context ||
        !s_device || !SafeMode::IsFull() || HookManager::IsUnloading()) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_activePs || !s_fullScreenVs || !s_cb) return;

    SavedD3D11State saved{};
    saved.Save(s_context);

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
    if (FAILED(hr) || !pBackBuffer) {
        saved.Restore(s_context);
        return;
    }

    D3D11_TEXTURE2D_DESC bbDesc = {};
    pBackBuffer->GetDesc(&bbDesc);
    DXGI_FORMAT copyFmt = UntypelessColorFormat(bbDesc.Format);

    // Reallocate scene copy texture if backbuffer size or format changed
    if (!s_sceneCopyTex || s_sceneCopyDesc.Width != bbDesc.Width || s_sceneCopyDesc.Height != bbDesc.Height || s_sceneCopyDesc.Format != copyFmt) {
        if (s_sceneCopySrv) { s_sceneCopySrv->Release(); s_sceneCopySrv = nullptr; }
        if (s_sceneCopyTex) { s_sceneCopyTex->Release(); s_sceneCopyTex = nullptr; }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = bbDesc.Width;
        td.Height = bbDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = copyFmt;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        hr = s_device->CreateTexture2D(&td, nullptr, &s_sceneCopyTex);
        if (FAILED(hr) || !s_sceneCopyTex) {
            pBackBuffer->Release();
            saved.Restore(s_context);
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = copyFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = s_device->CreateShaderResourceView(s_sceneCopyTex, &srvDesc, &s_sceneCopySrv);
        if (FAILED(hr) || !s_sceneCopySrv) {
            if (s_sceneCopyTex) { s_sceneCopyTex->Release(); s_sceneCopyTex = nullptr; }
            pBackBuffer->Release();
            saved.Restore(s_context);
            return;
        }
        s_sceneCopyDesc = td;
    }

    // Copy backbuffer contents into the scene snapshot texture
    if (bbDesc.SampleDesc.Count > 1) {
        s_context->ResolveSubresource(s_sceneCopyTex, 0, pBackBuffer, 0, copyFmt);
    } else {
        s_context->CopyResource(s_sceneCopyTex, pBackBuffer);
    }

    // Determine target backbuffer RTV
    ID3D11RenderTargetView* targetRtv = backBufferRtv;
    ID3D11RenderTargetView* tempRtv = nullptr;
    if (!targetRtv) {
        s_device->CreateRenderTargetView(pBackBuffer, nullptr, &tempRtv);
        targetRtv = tempRtv;
    }
    pBackBuffer->Release();
    if (!targetRtv) {
        saved.Restore(s_context);
        return;
    }

    // Update constant buffer
    LARGE_INTEGER nowQpc = {};
    QueryPerformanceCounter(&nowQpc);
    const double freq = s_qpcFreq.QuadPart ? static_cast<double>(s_qpcFreq.QuadPart) : 1.0;
    const float timeSec = static_cast<float>((nowQpc.QuadPart - s_startQpc.QuadPart) / freq);
    const float dtSec = s_lastFrameQpc ? static_cast<float>((nowQpc.QuadPart - s_lastFrameQpc) / freq) : 0.016f;
    s_lastFrameQpc = nowQpc.QuadPart;

    PostFxConstants cbData = {};
    cbData.time[0] = timeSec;
    cbData.time[1] = dtSec;
    cbData.time[2] = 0.0f;
    cbData.time[3] = 0.0f;

    cbData.screen[0] = static_cast<float>(bbDesc.Width);
    cbData.screen[1] = static_cast<float>(bbDesc.Height);
    cbData.screen[2] = bbDesc.Width > 0 ? (1.0f / static_cast<float>(bbDesc.Width)) : 0.0f;
    cbData.screen[3] = bbDesc.Height > 0 ? (1.0f / static_cast<float>(bbDesc.Height)) : 0.0f;

    for (int p = 0; p < 8; ++p) {
        for (int c = 0; c < 4; ++c) {
            cbData.param[p][c] = s_params[p][c].load(std::memory_order_relaxed);
        }
    }

    // Live drift metrics from CarFX spatial proximity
    CarFX::SpatialProximityInput sp = CarFX::GetSpatialProximity(CarFX::kTargetBody);
    cbData.driftState[0] = sp.driftState[0];
    cbData.driftState[1] = sp.driftState[1];
    cbData.driftState[2] = sp.driftState[2];
    cbData.driftState[3] = sp.driftState[3];

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
        memcpy(mapped.pData, &cbData, sizeof(cbData));
        s_context->Unmap(s_cb, 0);
    }

    // Setup pipeline state
    s_context->OMSetRenderTargets(1, &targetRtv, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height), 0.0f, 1.0f };
    s_context->RSSetViewports(1, &vp);
    s_context->RSSetScissorRects(0, nullptr);
    s_context->RSSetState(s_rs);
    s_context->OMSetBlendState(s_opaqueBlend, nullptr, 0xFFFFFFFF);
    s_context->OMSetDepthStencilState(s_noDepthDs, 0);
    s_context->IASetInputLayout(nullptr);
    s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    s_context->VSSetShader(s_fullScreenVs, nullptr, 0);
    s_context->PSSetShader(s_activePs, nullptr, 0);

    ID3D11Buffer* cbs[] = { s_cb };
    s_context->VSSetConstantBuffers(0, 1, cbs);
    s_context->PSSetConstantBuffers(0, 1, cbs);
    s_context->PSSetConstantBuffers(13, 1, cbs);

    ID3D11ShaderResourceView* depthSrv = CarFX::GetCurrentFrameDepthSRV();
    ID3D11ShaderResourceView* srvs0[] = { s_sceneCopySrv, depthSrv };
    s_context->PSSetShaderResources(0, 2, srvs0);
    s_context->PSSetShaderResources(32, 1, &s_sceneCopySrv);
    if (depthSrv) s_context->PSSetShaderResources(37, 1, &depthSrv);

    ID3D11SamplerState* samplers[] = { s_linearSampler, s_pointSampler };
    s_context->PSSetSamplers(0, 2, samplers);
    s_context->PSSetSamplers(14, 2, samplers);

    s_context->Draw(3, 0);

    // Unbind SRVs to prevent DX11 binding hazard with subsequent passes
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    s_context->PSSetShaderResources(0, 2, nullSrvs);
    s_context->PSSetShaderResources(32, 1, nullSrvs);
    s_context->PSSetShaderResources(37, 1, nullSrvs);

    if (tempRtv) tempRtv->Release();

    saved.Restore(s_context);
}

bool LoadShader(const std::string& filenameUtf8, std::string* outError) {
    std::lock_guard<std::mutex> lock(s_mutex);
    std::wstring fullPath = CarFX::ResolvePostShaderPath(filenameUtf8);
    if (fullPath.empty()) {
        if (outError) *outError = "Shader file not allowed or not found in shaders/post/";
        return false;
    }

    std::vector<char> src;
    FILETIME ft = {};
    if (!CarFX::Internal::ReadFileUtf8(fullPath, src, ft)) {
        if (outError) *outError = "Cannot read file: " + Logger::ToNarrow(fullPath);
        return false;
    }

    ID3D11PixelShader* newPs = nullptr;
    CarFX::ShaderDecl decl{};
    std::string narrowPath = Logger::ToNarrow(fullPath);
    std::wstring incDir = CarFX::Internal::DirectoryOf(fullPath);

    if (!CarFX::CompilePixelShader(src.data(), src.size(), narrowPath, incDir, &newPs, &decl, outError)) {
        return false;
    }

    if (s_activePs) s_activePs->Release();
    s_activePs = newPs;
    s_decl = decl;
    s_activeShaderPath = filenameUtf8;
    s_lastWriteTime = ft;

    // Apply defaults from annotations
    float defs[CarFX::kParams][4] = {};
    CarFX::DeclDefaults(s_decl, defs);
    for (int p = 0; p < CarFX::kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            s_params[p][c].store(defs[p][c], std::memory_order_relaxed);
        }
    }

    Logger::Info("PostFX loaded shader: " + filenameUtf8);
    return true;
}

void UnloadShader() {
    if (s_activePs) {
        s_activePs->Release();
        s_activePs = nullptr;
    }
    s_decl = {};
    s_activeShaderPath.clear();
    s_lastWriteTime = {};
}

bool ReloadShader(std::string* outError) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_activeShaderPath.empty()) return false;

    std::wstring fullPath = CarFX::ResolvePostShaderPath(s_activeShaderPath);
    if (fullPath.empty()) return false;

    HANDLE f = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = {};
    GetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);

    if (ft.dwLowDateTime == s_lastWriteTime.dwLowDateTime && ft.dwHighDateTime == s_lastWriteTime.dwHighDateTime) {
        return true;
    }

    std::vector<char> src;
    if (!CarFX::Internal::ReadFileUtf8(fullPath, src, ft)) return false;

    ID3D11PixelShader* newPs = nullptr;
    CarFX::ShaderDecl decl{};
    std::string narrowPath = Logger::ToNarrow(fullPath);
    std::wstring incDir = CarFX::Internal::DirectoryOf(fullPath);

    if (!CarFX::CompilePixelShader(src.data(), src.size(), narrowPath, incDir, &newPs, &decl, outError)) {
        return false;
    }

    if (s_activePs) s_activePs->Release();
    s_activePs = newPs;
    s_decl = decl;
    s_lastWriteTime = ft;

    // Clamp existing parameters to new bounds
    for (int i = 0; i < s_decl.paramCount; ++i) {
        const auto& np = s_decl.param[i];
        for (int c = 0; c < np.count; ++c) {
            float cur = s_params[np.slot][np.first + c].load(std::memory_order_relaxed);
            s_params[np.slot][np.first + c].store(CarFX::ClampToDecl(np, cur), std::memory_order_relaxed);
        }
    }

    Logger::Info("PostFX hot-reloaded: " + s_activeShaderPath);
    return true;
}

void SetEnabled(bool enabled) {
    s_enabled.store(enabled, std::memory_order_release);
}

bool IsEnabled() {
    return s_enabled.load(std::memory_order_acquire);
}

bool SetParamSlot(int slot, const float* values, int count) {
    if (slot < 0 || slot >= 8 || !values || count <= 0) return false;
    int n = (std::min)(count, 4);
    for (int c = 0; c < n; ++c) {
        s_params[slot][c].store(values[c], std::memory_order_relaxed);
    }
    return true;
}

bool SetParamByName(const char* name, const float* values, int count) {
    if (!name || !values || count <= 0) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    const CarFX::ParamDecl* pd = CarFX::FindParam(s_decl, name);
    if (!pd) return false;
    int n = (std::min)(count, pd->count);
    for (int c = 0; c < n; ++c) {
        s_params[pd->slot][pd->first + c].store(values[c], std::memory_order_relaxed);
    }
    return true;
}

bool GetParamSlot(int slot, float out[4]) {
    if (slot < 0 || slot >= 8 || !out) return false;
    for (int c = 0; c < 4; ++c) {
        out[c] = s_params[slot][c].load(std::memory_order_relaxed);
    }
    return true;
}

CarFX::ShaderDecl GetCurrentDecl() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_decl;
}

std::vector<std::string> ListShaders() {
    return CarFX::ListPostShaders();
}

std::wstring GetPostShadersDirectory() {
    return s_shadersDir + L"\\post";
}

} // namespace PostFX
