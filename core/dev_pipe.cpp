#include "dev_pipe.h"
#include "urushi_ui.h"
#include "d3d11_hook.h"
#include "lua_engine.h"
#include "native_carx.h"
#include "car_sim_optimizer.h"
#include "carfx.h"
#include "logger.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\urushi_dev";
constexpr DWORD kPipeBufBytes = 256 * 1024;
constexpr int kRequestTimeoutMs = 6000;

std::wstring g_exeDir;
HANDLE g_pipeThread = nullptr;
HANDLE g_stopEvent = nullptr;
std::atomic<bool> g_running{ false };

// DX11 readback / WIC / offscreen simulated resolution state (render thread only)
ID3D11Texture2D*        g_stagingTex = nullptr;
ID3D11Texture2D*        g_resolveTex = nullptr;
ID3D11Texture2D*        g_simTex = nullptr;
ID3D11RenderTargetView* g_simRtv = nullptr;
UINT                    g_stageW = 0, g_stageH = 0;
DXGI_FORMAT             g_stageFmt = DXGI_FORMAT_UNKNOWN;
UINT                    g_resW = 0, g_resH = 0;
DXGI_FORMAT             g_resFmt = DXGI_FORMAT_UNKNOWN;
UINT                    g_simTexW = 0, g_simTexH = 0;
DXGI_FORMAT             g_simTexFmt = DXGI_FORMAT_UNKNOWN;
IWICImagingFactory*     g_wic = nullptr;

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Synthetic ImGui input queue (drained in BeforeNewFrame)
struct SynthInput {
    enum Type { Move, Click, Key } type = Move;
    float x = 0.0f, y = 0.0f;
    int button = 0;
    bool dbl = false;
    int vk = 0;
    bool down = false;
    int holdFrames = 0;
};
std::deque<SynthInput> g_inputQueue;
bool g_holdPosValid = false;
float g_holdX = 0.0f, g_holdY = 0.0f;
int g_holdPosFrames = 0;

struct DevCommand {
    std::string cmd;
    std::unordered_map<std::string, std::string> args;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool isShot = false;
    int shotWaitFrames = 0;
    std::string responseJson;
};

std::mutex g_queueMtx;
std::deque<std::shared_ptr<DevCommand>> g_cmdQueue;
std::shared_ptr<DevCommand> g_activeShot;

// -------------------- Minimal JSON & Base64 helpers --------------------

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < len) ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

struct JsonCursor {
    const char* p;
    const char* end;
};

void SkipWs(JsonCursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n')) ++c.p;
}

bool ParseJsonString(JsonCursor& c, std::string& out) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != '"') return false;
    ++c.p;
    out.clear();
    while (c.p < c.end) {
        char ch = *c.p++;
        if (ch == '"') return true;
        if (ch != '\\') { out += ch; continue; }
        if (c.p >= c.end) return false;
        char e = *c.p++;
        switch (e) {
        case '"':  out += '"'; break;
        case '\\': out += '\\'; break;
        case '/':  out += '/'; break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && c.p < c.end; ++k) {
                char h = *c.p++;
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
            }
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: out += e; break;
        }
    }
    return false;
}

bool ParseJsonScalarOrSkip(JsonCursor& c, std::string& outVal, bool& hasScalar) {
    SkipWs(c);
    hasScalar = false;
    if (c.p >= c.end) return false;
    if (*c.p == '"') {
        hasScalar = ParseJsonString(c, outVal);
        return hasScalar;
    }
    if (*c.p == '{' || *c.p == '[') {
        int depth = 0;
        bool inStr = false;
        while (c.p < c.end) {
            char ch = *c.p++;
            if (inStr) {
                if (ch == '\\' && c.p < c.end) ++c.p;
                else if (ch == '"') inStr = false;
            } else {
                if (ch == '"') inStr = true;
                else if (ch == '{' || ch == '[') ++depth;
                else if (ch == '}' || ch == ']') { if (--depth <= 0) return true; }
            }
        }
        return false;
    }
    const char* start = c.p;
    while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' &&
           *c.p != ' ' && *c.p != '\t' && *c.p != '\r' && *c.p != '\n') {
        ++c.p;
    }
    outVal.assign(start, c.p);
    hasScalar = !outVal.empty();
    return hasScalar;
}

bool ParseJsonArgsObject(JsonCursor& c, std::unordered_map<std::string, std::string>& args) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != '{') return false;
    ++c.p;
    for (;;) {
        SkipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == '}') { ++c.p; return true; }
        std::string key;
        if (!ParseJsonString(c, key)) return false;
        SkipWs(c);
        if (c.p >= c.end || *c.p != ':') return false;
        ++c.p;
        std::string val;
        bool okScalar = false;
        if (!ParseJsonScalarOrSkip(c, val, okScalar)) return false;
        if (okScalar) args[key] = val;
        SkipWs(c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
        return false;
    }
}

bool ParseRequestLine(const std::string& line, std::string& reqId, std::string& cmd,
                      std::unordered_map<std::string, std::string>& args) {
    JsonCursor c{ line.data(), line.data() + line.size() };
    SkipWs(c);
    if (c.p >= c.end || *c.p != '{') return false;
    ++c.p;
    reqId = "0";
    cmd.clear();
    args.clear();
    for (;;) {
        SkipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == '}') break;
        std::string key;
        if (!ParseJsonString(c, key)) return false;
        SkipWs(c);
        if (c.p >= c.end || *c.p != ':') return false;
        ++c.p;
        SkipWs(c);
        if (key == "args" && c.p < c.end && *c.p == '{') {
            if (!ParseJsonArgsObject(c, args)) return false;
        } else {
            std::string val;
            bool ok = false;
            if (!ParseJsonScalarOrSkip(c, val, ok)) return false;
            if (ok) {
                if (key == "id") reqId = val;
                else if (key == "cmd") cmd = val;
                else args[key] = val;
            }
        }
        SkipWs(c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == '}') break;
        return false;
    }
    return !cmd.empty();
}

std::string MakeErrorJson(const std::string& id, const std::string& msg) {
    return "{\"id\":" + id + ",\"ok\":false,\"error\":\"" + JsonEscape(msg) + "\"}";
}

std::string MakeOkJson(const std::string& id, const std::string& bodyFields) {
    if (bodyFields.empty()) return "{\"id\":" + id + ",\"ok\":true}";
    return "{\"id\":" + id + ",\"ok\":true," + bodyFields + "}";
}

std::string ArgStr(const std::unordered_map<std::string, std::string>& m, const char* k, const char* def = "") {
    auto it = m.find(k);
    return (it != m.end()) ? it->second : std::string(def);
}

int ArgInt(const std::unordered_map<std::string, std::string>& m, const char* k, int def = 0) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

float ArgFloat(const std::unordered_map<std::string, std::string>& m, const char* k, float def = 0.0f) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

bool ArgBool(const std::unordered_map<std::string, std::string>& m, const char* k, bool def = false) {
    auto it = m.find(k);
    if (it == m.end()) return def;
    return (it->second == "true" || it->second == "1");
}

// -------------------- Snapshot serializers --------------------

std::string SerializeWindowsArray(const UrushiUI::DebugSnapshot& snap) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < snap.windows.size(); ++i) {
        const auto& w = snap.windows[i];
        if (i > 0) ss << ",";
        ss << "{"
           << "\"id\":\"" << JsonEscape(w.id) << "\","
           << "\"title\":\"" << JsonEscape(w.title) << "\","
           << "\"icon\":\"" << JsonEscape(w.icon) << "\","
           << "\"order\":" << w.order << ","
           << "\"open\":" << (w.open ? "true" : "false") << ","
           << "\"pinned\":" << (w.pinned ? "true" : "false") << ","
           << "\"shaded\":" << (w.shaded ? "true" : "false") << ","
           << "\"hud_no_inputs\":" << (w.hudNoInputs ? "true" : "false") << ","
           << "\"scrollable\":" << (w.scrollable ? "true" : "false") << ","
           << "\"x\":" << static_cast<int>(w.x) << ","
           << "\"y\":" << static_cast<int>(w.y) << ","
           << "\"width\":" << static_cast<int>(w.width) << ","
           << "\"actual_w\":" << static_cast<int>(w.actualW) << ","
           << "\"actual_h\":" << static_cast<int>(w.actualH) << ","
           << "\"body_h\":" << static_cast<int>(w.bodyH)
           << "}";
    }
    ss << "]";
    return ss.str();
}

std::string SerializeStatePayload() {
    auto snap = UrushiUI::GetDebugSnapshot();
    const auto optStats = CarSimOptimizer::GetStats();
    const float fps = ImGui::GetCurrentContext() ? ImGui::GetIO().Framerate : 0.0f;
    const float dtMs = (fps > 0.1f) ? (1000.0f / fps) : 0.0f;

    std::ostringstream ss;
    ss << "\"pid\":" << GetCurrentProcessId() << ","
       << "\"menu_open\":" << (D3D11Hook::IsMenuOpen() ? "true" : "false") << ","
       << "\"in_session\":" << (NativeCarX::IsInSession() ? "true" : "false") << ","
       << "\"fps\":" << fps << ","
       << "\"frame_ms\":" << dtMs << ","
       << "\"accent\":\"" << JsonEscape(UrushiUI::GetAccentName()) << "\","
       << "\"net_cars\":" << optStats.totalNetCars << ","
       << "\"work_w\":" << static_cast<int>(snap.workW) << ","
       << "\"work_h\":" << static_cast<int>(snap.workH) << ","
       << "\"ui_scale\":" << snap.uiScale << ","
       << "\"auto_scale\":" << snap.autoScale << ","
       << "\"user_scale\":" << snap.userScale << ","
       << "\"dock\":{"
       << "\"x\":" << static_cast<int>(snap.dockX) << ","
       << "\"y\":" << static_cast<int>(snap.dockY) << ","
       << "\"w\":" << static_cast<int>(snap.dockW) << ","
       << "\"h\":" << static_cast<int>(snap.dockH) << "},"
       << "\"windows\":" << SerializeWindowsArray(snap);
    return ss.str();
}

// -------------------- DX11 BackBuffer + WIC PNG Capture --------------------

bool EnsureWic() {
    if (g_wic) return true;
    static bool tried = false;
    if (!tried) {
        tried = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (FAILED(hr) || !g_wic) {
        g_wic = nullptr;
        return false;
    }
    return true;
}

bool EnsureResolve(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_resolveTex && w == g_resW && h == g_resH && fmt == g_resFmt) return true;
    SafeRelease(g_resolveTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_resolveTex))) {
        return false;
    }
    g_resW = w; g_resH = h; g_resFmt = fmt;
    return true;
}

bool EnsureStaging(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_stagingTex && w == g_stageW && h == g_stageH && fmt == g_stageFmt) return true;
    SafeRelease(g_stagingTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_stagingTex))) {
        return false;
    }
    g_stageW = w; g_stageH = h; g_stageFmt = fmt;
    return true;
}

bool EncodeBgraToPng(const uint8_t* bgra, UINT w, UINT h, UINT stride, std::vector<uint8_t>& outPng) {
    outPng.clear();
    if (!EnsureWic()) return false;

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IStream* mem = SHCreateMemStream(nullptr, 0);
    if (!mem) return false;

    bool ok = false;
    do {
        if (FAILED(g_wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromIStream(mem))) break;
        if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(w, h))) break;

        WICPixelFormatGUID pixFmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&pixFmt))) break;
        if (!IsEqualGUID(pixFmt, GUID_WICPixelFormat32bppBGRA)) break;

        if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra)))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(enc->Commit())) break;

        STATSTG st{};
        if (FAILED(mem->Stat(&st, STATFLAG_NONAME))) break;
        const ULONGLONG sz = st.cbSize.QuadPart;
        if (sz == 0 || sz > (48u << 20)) break;

        LARGE_INTEGER zero{};
        mem->Seek(zero, STREAM_SEEK_SET, nullptr);
        outPng.resize(static_cast<size_t>(sz));
        ULONG got = 0;
        if (FAILED(mem->Read(outPng.data(), static_cast<ULONG>(sz), &got)) || got != sz) {
            outPng.clear();
            break;
        }
        ok = true;
    } while (false);

    SafeRelease(frame);
    SafeRelease(enc);
    SafeRelease(stream);
    SafeRelease(mem);
    return ok;
}

DXGI_FORMAT NormalizeTypelessColorFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

bool EnsureSimTarget(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_simTex && g_simRtv && w == g_simTexW && h == g_simTexH && fmt == g_simTexFmt) return true;
    SafeRelease(g_simRtv);
    SafeRelease(g_simTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_simTex)) || !g_simTex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_simTex, nullptr, &g_simRtv)) || !g_simRtv) {
        SafeRelease(g_simTex);
        return false;
    }
    g_simTexW = w; g_simTexH = h; g_simTexFmt = fmt;
    return true;
}

bool EncodeTextureRegionToPng(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                              ID3D11Texture2D* srcTex, UINT srcW, UINT srcH, DXGI_FORMAT rawFmt,
                              const std::unordered_map<std::string, std::string>& args,
                              std::vector<uint8_t>& outPng, UINT& outW, UINT& outH, std::string& errOut) {
    const DXGI_FORMAT fmt = NormalizeTypelessColorFormat(rawFmt);
    const bool isRgba8 = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool isBgra8 = (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    const bool isR10   = (fmt == DXGI_FORMAT_R10G10B10A2_UNORM);
    if (!isRgba8 && !isBgra8 && !isR10) {
        errOut = "Unsupported texture DXGI_FORMAT=" + std::to_string(static_cast<int>(rawFmt));
        return false;
    }

    if (!EnsureStaging(dev, srcW, srcH, rawFmt)) {
        errOut = "Staging texture creation failed";
        return false;
    }
    ctx->CopyResource(g_stagingTex, srcTex);

    int cx = 0, cy = 0;
    int cw = static_cast<int>(srcW);
    int ch = static_cast<int>(srcH);

    std::string winId = ArgStr(args, "window_id");
    if (!winId.empty()) {
        auto snap = UrushiUI::GetDebugSnapshot();
        for (const auto& w : snap.windows) {
            if (w.id == winId && w.open) {
                const int pad = 16;
                cx = static_cast<int>(w.x) - pad;
                cy = static_cast<int>(w.y) - pad;
                cw = static_cast<int>(w.actualW) + pad * 2;
                ch = static_cast<int>(w.actualH) + pad * 2;
                break;
            }
        }
        if (winId == "dock") {
            const int pad = 16;
            cx = static_cast<int>(snap.dockX) - pad;
            cy = static_cast<int>(snap.dockY) - pad;
            cw = static_cast<int>(snap.dockW) + pad * 2;
            ch = static_cast<int>(snap.dockH) + pad * 2;
        }
    } else if (ArgInt(args, "w", 0) > 0 && ArgInt(args, "h", 0) > 0) {
        cx = ArgInt(args, "x", 0);
        cy = ArgInt(args, "y", 0);
        cw = ArgInt(args, "w", static_cast<int>(srcW));
        ch = ArgInt(args, "h", static_cast<int>(srcH));
    }

    cx = std::clamp(cx, 0, static_cast<int>(srcW) - 1);
    cy = std::clamp(cy, 0, static_cast<int>(srcH) - 1);
    cw = std::clamp(cw, 1, static_cast<int>(srcW) - cx);
    ch = std::clamp(ch, 1, static_cast<int>(srcH) - cy);

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(g_stagingTex, 0, D3D11_MAP_READ, 0, &ms)) || !ms.pData) {
        errOut = "Staging Map failed";
        return false;
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4);
    for (int row = 0; row < ch; ++row) {
        const uint8_t* srcRow = static_cast<const uint8_t*>(ms.pData) +
                                static_cast<size_t>(cy + row) * ms.RowPitch +
                                static_cast<size_t>(cx) * 4;
        uint8_t* dstRow = bgra.data() + static_cast<size_t>(row) * static_cast<size_t>(cw) * 4;
        if (isBgra8) {
            for (int col = 0; col < cw; ++col) {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 0];
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
                dstRow[col * 4 + 2] = srcRow[col * 4 + 2];
                dstRow[col * 4 + 3] = 255;
            }
        } else if (isRgba8) {
            for (int col = 0; col < cw; ++col) {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 2];
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
                dstRow[col * 4 + 2] = srcRow[col * 4 + 0];
                dstRow[col * 4 + 3] = 255;
            }
        } else {
            const uint32_t* p32 = reinterpret_cast<const uint32_t*>(srcRow);
            for (int col = 0; col < cw; ++col) {
                uint32_t v = p32[col];
                dstRow[col * 4 + 2] = static_cast<uint8_t>((v & 0x3FFu) >> 2);
                dstRow[col * 4 + 1] = static_cast<uint8_t>(((v >> 10) & 0x3FFu) >> 2);
                dstRow[col * 4 + 0] = static_cast<uint8_t>(((v >> 20) & 0x3FFu) >> 2);
                dstRow[col * 4 + 3] = 255;
            }
        }
    }
    ctx->Unmap(g_stagingTex, 0);

    outW = static_cast<UINT>(cw);
    outH = static_cast<UINT>(ch);
    if (!EncodeBgraToPng(bgra.data(), outW, outH, outW * 4, outPng)) {
        errOut = "WIC PNG encoding failed";
        return false;
    }
    return true;
}

bool CaptureSwapChainPng(IDXGISwapChain* swapChain, ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         const std::unordered_map<std::string, std::string>& args,
                         std::vector<uint8_t>& outPng, UINT& outW, UINT& outH, std::string& errOut) {
    if (!swapChain || !dev || !ctx) {
        errOut = "DX11 device/swapchain not ready";
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
        errOut = "GetBuffer(0) failed";
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    const DXGI_FORMAT fmt = NormalizeTypelessColorFormat(td.Format);
    const bool isRgba8 = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool isBgra8 = (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    const bool isR10   = (fmt == DXGI_FORMAT_R10G10B10A2_UNORM);
    if (!isRgba8 && !isBgra8 && !isR10) {
        back->Release();
        errOut = "Unsupported backbuffer DXGI_FORMAT=" + std::to_string(static_cast<int>(td.Format));
        return false;
    }

    ID3D11Texture2D* srcTex = back;
    if (td.SampleDesc.Count > 1) {
        if (!EnsureResolve(dev, td.Width, td.Height, fmt)) {
            back->Release();
            errOut = "ResolveSubresource texture creation failed";
            return false;
        }
        ctx->ResolveSubresource(g_resolveTex, 0, back, 0, fmt);
        srcTex = g_resolveTex;
    }

    const int reqSimW = ArgInt(args, "sim_w", 0);
    const int reqSimH = ArgInt(args, "sim_h", 0);
    if (reqSimW > 0 && reqSimH > 0) {
        const UINT simW = static_cast<UINT>(std::clamp(reqSimW, 320, 7680));
        const UINT simH = static_cast<UINT>(std::clamp(reqSimH, 240, 4320));
        const DXGI_FORMAT simFmt = isRgba8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!EnsureSimTarget(dev, simW, simH, simFmt)) {
            back->Release();
            errOut = "Simulated RenderTargetView creation failed";
            return false;
        }

        const float clearCol[4] = { 0.065f, 0.070f, 0.085f, 1.0f };
        ctx->ClearRenderTargetView(g_simRtv, clearCol);

        // Blit clean game frame behind simulated ImGui overlay when format family matches
        if ((isRgba8 && simFmt == DXGI_FORMAT_R8G8B8A8_UNORM) ||
            (isBgra8 && simFmt == DXGI_FORMAT_B8G8R8A8_UNORM)) {
            const UINT copyW = (std::min)(td.Width, simW);
            const UINT copyH = (std::min)(td.Height, simH);
            const UINT srcX = (td.Width - copyW) / 2;
            const UINT srcY = (td.Height - copyH) / 2;
            const UINT dstX = (simW - copyW) / 2;
            const UINT dstY = (simH - copyH) / 2;
            D3D11_BOX box{ srcX, srcY, 0, srcX + copyW, srcY + copyH, 1 };
            ctx->CopySubresourceRegion(g_simTex, 0, dstX, dstY, 0, srcTex, 0, &box);
        }

        ID3D11RenderTargetView* prevRtv = nullptr;
        ID3D11DepthStencilView* prevDsv = nullptr;
        ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        UINT numVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT prevVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        ctx->RSGetViewports(&numVp, prevVps);

        ctx->OMSetRenderTargets(1, &g_simRtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(simW);
        vp.Height = static_cast<float>(simH);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        if (ImDrawData* dd = ImGui::GetDrawData()) {
            ImGui_ImplDX11_RenderDrawData(dd);
        }

        ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
        if (numVp > 0) ctx->RSSetViewports(numVp, prevVps);
        SafeRelease(prevRtv);
        SafeRelease(prevDsv);
        back->Release();

        return EncodeTextureRegionToPng(dev, ctx, g_simTex, simW, simH, simFmt, args, outPng, outW, outH, errOut);
    }

    const bool ok = EncodeTextureRegionToPng(dev, ctx, srcTex, td.Width, td.Height, td.Format, args, outPng, outW, outH, errOut);
    back->Release();
    return ok;
}

std::string ReadLogTail(int maxLines) {
    std::wstring path = g_exeDir + L"\\urushi.log";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > maxLines) lines.pop_front();
    }
    std::string joined;
    for (const auto& l : lines) {
        if (!joined.empty()) joined += '\n';
        joined += l;
    }
    return joined;
}

// -------------------- Render Thread Dispatch --------------------

std::string ExecuteInstantCommand(const std::string& reqId, const std::string& cmd,
                                  const std::unordered_map<std::string, std::string>& args) {
    if (cmd == "ping") {
        return MakeOkJson(reqId, "\"pid\":" + std::to_string(GetCurrentProcessId()));
    }
    if (cmd == "logs") {
        int n = std::clamp(ArgInt(args, "lines", 40), 1, 300);
        return MakeOkJson(reqId, "\"log\":\"" + JsonEscape(ReadLogTail(n)) + "\"");
    }
    return "";
}

void ExecuteBeforeFrameCommand(const std::shared_ptr<DevCommand>& c) {
    const auto& args = c->args;
    const std::string reqId = ArgStr(args, "__req_id", "0");

    if (c->cmd == "menu") {
        std::string act = ArgStr(args, "action", "toggle");
        if (act == "open") D3D11Hook::SetMenuOpen(true);
        else if (act == "close") D3D11Hook::SetMenuOpen(false);
        else D3D11Hook::ToggleMenu();
        c->responseJson = MakeOkJson(reqId, std::string("\"menu_open\":") + (D3D11Hook::IsMenuOpen() ? "true" : "false"));
    } else if (c->cmd == "window") {
        std::string act = ArgStr(args, "action", "toggle");
        std::string id = ArgStr(args, "id");
        if (id.empty() && act != "close_all") {
            c->responseJson = MakeErrorJson(reqId, "missing window 'id'");
        } else if (act == "close_all") {
            UrushiUI::CloseAllUnpinned();
            c->responseJson = MakeOkJson(reqId, SerializeStatePayload());
        } else {
            if (act == "open") {
                D3D11Hook::SetMenuOpen(true);
                UrushiUI::SetWindowOpen(id, true);
            } else if (act == "close") {
                UrushiUI::SetWindowOpen(id, false);
            } else if (act == "toggle") {
                UrushiUI::ToggleWindow(id);
            } else if (act == "solo") {
                D3D11Hook::SetMenuOpen(true);
                auto snap = UrushiUI::GetDebugSnapshot();
                for (const auto& w : snap.windows) {
                    UrushiUI::SetWindowOpen(w.id, w.id == id);
                }
            } else if (act == "pin") {
                UrushiUI::SetWindowPinned(id, true);
            } else if (act == "unpin") {
                UrushiUI::SetWindowPinned(id, false);
            } else if (act == "shade") {
                UrushiUI::SetWindowShaded(id, true);
            } else if (act == "unshade") {
                UrushiUI::SetWindowShaded(id, false);
            } else if (act == "move") {
                UrushiUI::SetWindowPos(id, ArgFloat(args, "x", 264.0f), ArgFloat(args, "y", 24.0f));
            }
            c->responseJson = MakeOkJson(reqId, "\"windows\":" + SerializeWindowsArray(UrushiUI::GetDebugSnapshot()));
        }
    } else if (c->cmd == "accent") {
        std::string name = ArgStr(args, "name", "amber");
        if (UrushiUI::SetAccentByName(name)) {
            c->responseJson = MakeOkJson(reqId, "\"accent\":\"" + JsonEscape(UrushiUI::GetAccentName()) + "\"");
        } else {
            c->responseJson = MakeErrorJson(reqId, "unknown accent (amber|vermilion|platinum|indigo)");
        }
    } else if (c->cmd == "reload_lua") {
        LuaEngine::Reload();
        c->responseJson = MakeOkJson(reqId, "\"reloaded\":true");
    } else if (c->cmd == "lua_eval") {
        std::string code = ArgStr(args, "code");
        std::string out, err;
        if (LuaEngine::Eval(code, out, err)) {
            c->responseJson = MakeOkJson(reqId, "\"output\":\"" + JsonEscape(out) + "\"");
        } else {
            c->responseJson = MakeErrorJson(reqId, err);
        }
    } else if (c->cmd == "click") {
        SynthInput s;
        s.type = SynthInput::Click;
        s.x = ArgFloat(args, "x", 0.0f);
        s.y = ArgFloat(args, "y", 0.0f);
        s.button = ArgInt(args, "button", 0);
        s.dbl = ArgBool(args, "double", false);
        s.holdFrames = 2;
        g_inputQueue.push_back(s);
        c->responseJson = MakeOkJson(reqId, "\"queued\":true");
    } else if (c->cmd == "move") {
        SynthInput s;
        s.type = SynthInput::Move;
        s.x = ArgFloat(args, "x", 0.0f);
        s.y = ArgFloat(args, "y", 0.0f);
        g_inputQueue.push_back(s);
        c->responseJson = MakeOkJson(reqId, "\"queued\":true");
    } else if (c->cmd == "key") {
        int vk = ArgInt(args, "vk", 0);
        if (vk > 0) {
            SynthInput s;
            s.type = SynthInput::Key;
            s.vk = vk;
            s.holdFrames = 2;
            g_inputQueue.push_back(s);
        }
        c->responseJson = MakeOkJson(reqId, "\"queued\":true");
    } else if (c->cmd == "optimizer") {
        auto cfg = CarSimOptimizer::GetConfig();
        auto st = CarSimOptimizer::GetStats();
        std::ostringstream ss;
        ss << "\"enabled\":" << (cfg.enabled ? "true" : "false") << ","
           << "\"max_full_sim_cars\":" << cfg.maxFullSimCars << ","
           << "\"max_full_sim_dist\":" << cfg.maxFullSimDistMeters << ","
           << "\"hooks_installed\":" << (st.hooksInstalled ? "true" : "false") << ","
           << "\"switching_calls\":" << st.switchingUpdateCalls << ","
           << "\"anti_flicker_holding\":" << (st.antiFlickerHolding ? "true" : "false") << ","
           << "\"anti_flicker_blocks\":" << st.antiFlickerBlocks << ","
           << "\"total_net_cars\":" << st.totalNetCars << ","
           << "\"full_sim_net_cars\":" << st.fullSimNetCars << ","
           << "\"snapshot_net_cars\":" << st.snapshotNetCars;
        c->responseJson = MakeOkJson(reqId, ss.str());
    } else if (c->cmd == "carfx") {
        auto st = CarFX::GetStatus(0);
        std::ostringstream ss;
        ss << "\"enabled\":" << (CarFX::IsEnabled() ? "true" : "false") << ","
           << "\"hooked_draw\":" << (st.hookedDraw ? "true" : "false") << ","
           << "\"body_valid\":" << (st.bodyTargetsValid ? "true" : "false") << ","
           << "\"compiled\":" << (st.compiled ? "true" : "false") << ","
           << "\"matched_last_frame\":" << st.matchedDrawsLastFrame << ","
           << "\"total_draws\":" << st.totalDrawsFrame << ","
           << "\"shader_path\":\"" << JsonEscape(st.shaderPath) << "\","
           << "\"last_error\":\"" << JsonEscape(st.lastError) << "\"";
        c->responseJson = MakeOkJson(reqId, ss.str());
    } else if (c->cmd == "state" || c->cmd == "windows") {
        // Answered after UI render inside AfterImGuiRender so metrics are fresh!
        return;
    } else {
        c->responseJson = MakeErrorJson(reqId, "unknown command: " + c->cmd);
    }

    if (!c->responseJson.empty()) {
        std::lock_guard<std::mutex> lk(c->mtx);
        c->done = true;
        c->cv.notify_one();
    }
}

std::string HandlePipeLine(const std::string& line) {
    std::string reqId, cmd;
    std::unordered_map<std::string, std::string> args;
    if (!ParseRequestLine(line, reqId, cmd, args)) {
        return MakeErrorJson("0", "invalid json request");
    }

    std::string instant = ExecuteInstantCommand(reqId, cmd, args);
    if (!instant.empty()) return instant;

    auto req = std::make_shared<DevCommand>();
    req->cmd = cmd;
    req->args = std::move(args);
    req->args["__req_id"] = reqId;
    if (cmd == "shot") {
        req->isShot = true;
        const bool isSim = ArgInt(req->args, "sim_w", 0) > 0 && ArgInt(req->args, "sim_h", 0) > 0;
        req->shotWaitFrames = std::clamp(ArgInt(req->args, "delay_frames", isSim ? 3 : 2), isSim ? 1 : 0, 15);
    }

    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        g_cmdQueue.push_back(req);
    }

    std::unique_lock<std::mutex> lk(req->mtx);
    bool signaled = req->cv.wait_for(lk, std::chrono::milliseconds(kRequestTimeoutMs), [&]() {
        return req->done || !g_running.load(std::memory_order_acquire);
    });
    if (!signaled || !req->done) {
        return MakeErrorJson(reqId, "render thread timeout (game minimized or unloading)");
    }
    return req->responseJson;
}

DWORD WINAPI PipeServerThread(LPVOID) {
    while (g_running.load(std::memory_order_acquire)) {
        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            4,
            kPipeBufBytes,
            kPipeBufBytes,
            0,
            nullptr
        );
        if (hPipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) break;
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = { g_stopEvent, ov.hEvent };
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(hPipe, &ov);
                    CloseHandle(ov.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ov.hEvent);

        if (!connected || !g_running.load(std::memory_order_acquire)) {
            CloseHandle(hPipe);
            continue;
        }

        // Handle newline-delimited commands over this connection
        std::string rxBuf;
        std::vector<char> chunk(8192);
        while (g_running.load(std::memory_order_acquire)) {
            OVERLAPPED rov{};
            rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD bytesRead = 0;
            BOOL rOk = ReadFile(hPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, &rov);
            if (!rOk) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waits[2] = { g_stopEvent, rov.hEvent };
                    DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (wr != WAIT_OBJECT_0 + 1) {
                        CancelIoEx(hPipe, &rov);
                        CloseHandle(rov.hEvent);
                        break;
                    }
                    rOk = GetOverlappedResult(hPipe, &rov, &bytesRead, FALSE);
                }
            }
            CloseHandle(rov.hEvent);

            if (!rOk || bytesRead == 0) break;
            rxBuf.append(chunk.data(), bytesRead);

            size_t nl = std::string::npos;
            while ((nl = rxBuf.find('\n')) != std::string::npos) {
                std::string line = rxBuf.substr(0, nl);
                rxBuf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                std::string reply = HandlePipeLine(line) + "\n";

                DWORD totalWritten = 0;
                const char* ptr = reply.data();
                DWORD remaining = static_cast<DWORD>(reply.size());
                bool writeFailed = false;
                while (remaining > 0) {
                    OVERLAPPED wov{};
                    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    DWORD written = 0;
                    BOOL wOk = WriteFile(hPipe, ptr + totalWritten, remaining, &written, &wov);
                    if (!wOk && GetLastError() == ERROR_IO_PENDING) {
                        HANDLE waits[2] = { g_stopEvent, wov.hEvent };
                        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 5000);
                        if (wr == WAIT_OBJECT_0 + 1) {
                            wOk = GetOverlappedResult(hPipe, &wov, &written, FALSE);
                        } else {
                            CancelIoEx(hPipe, &wov);
                        }
                    }
                    CloseHandle(wov.hEvent);
                    if (!wOk || written == 0) {
                        writeFailed = true;
                        break;
                    }
                    totalWritten += written;
                    remaining -= written;
                }
                if (writeFailed) break;
            }
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

} // namespace

bool DevPipe::Init(const std::wstring& exeDir) {
    if (g_running.exchange(true, std::memory_order_acq_rel)) return true;
    g_exeDir = exeDir;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pipeThread = CreateThread(nullptr, 0, PipeServerThread, nullptr, 0, nullptr);
    if (!g_pipeThread) {
        g_running = false;
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
        return false;
    }
    Logger::Info("DevPipe initialized on \\\\.\\pipe\\urushi_dev");
    return true;
}

void DevPipe::Shutdown() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_pipeThread) {
        WaitForSingleObject(g_pipeThread, 2000);
        CloseHandle(g_pipeThread);
        g_pipeThread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        for (auto& req : g_cmdQueue) {
            std::lock_guard<std::mutex> lk(req->mtx);
            req->responseJson = MakeErrorJson("0", "DevPipe shutting down");
            req->done = true;
            req->cv.notify_all();
        }
        g_cmdQueue.clear();
        if (g_activeShot) {
            std::lock_guard<std::mutex> lk(g_activeShot->mtx);
            g_activeShot->responseJson = MakeErrorJson("0", "DevPipe shutting down");
            g_activeShot->done = true;
            g_activeShot->cv.notify_all();
            g_activeShot.reset();
        }
    }
    ReleaseD3D11();
}

void DevPipe::ReleaseD3D11() {
    SafeRelease(g_stagingTex);
    SafeRelease(g_resolveTex);
    SafeRelease(g_simRtv);
    SafeRelease(g_simTex);
    g_stageW = g_stageH = 0;
    g_resW = g_resH = 0;
    g_simTexW = g_simTexH = 0;
    g_stageFmt = g_resFmt = g_simTexFmt = DXGI_FORMAT_UNKNOWN;
}

void DevPipe::BeforeNewFrame() {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();

    // 1. Continue / issue synthetic input step
    if (!g_inputQueue.empty()) {
        auto& s = g_inputQueue.front();
        if (s.type == SynthInput::Move) {
            io.AddMousePosEvent(s.x, s.y);
            g_holdPosValid = true;
            g_holdX = s.x; g_holdY = s.y; g_holdPosFrames = 2;
            g_inputQueue.pop_front();
        } else if (s.type == SynthInput::Click) {
            io.AddMousePosEvent(s.x, s.y);
            if (s.holdFrames == 2) {
                io.AddMouseButtonEvent(s.button, true);
                s.holdFrames = 1;
            } else {
                io.AddMouseButtonEvent(s.button, false);
                g_inputQueue.pop_front();
            }
            g_holdPosValid = true;
            g_holdX = s.x; g_holdY = s.y; g_holdPosFrames = 2;
        } else if (s.type == SynthInput::Key) {
            ImGuiKey ik = ImGuiKey_None;
            if (s.vk == VK_ESCAPE) ik = ImGuiKey_Escape;
            else if (s.vk == VK_RETURN) ik = ImGuiKey_Enter;
            else if (s.vk == VK_TAB) ik = ImGuiKey_Tab;
            if (s.holdFrames == 2) {
                if (ik != ImGuiKey_None) io.AddKeyEvent(ik, true);
                s.holdFrames = 1;
            } else {
                if (ik != ImGuiKey_None) io.AddKeyEvent(ik, false);
                g_inputQueue.pop_front();
            }
        }
    } else if (g_holdPosFrames > 0 && g_holdPosValid) {
        --g_holdPosFrames;
        io.AddMousePosEvent(g_holdX, g_holdY);
    }

    // 2. Process queued non-shot / immediate commands before ImGui::NewFrame
    std::vector<std::shared_ptr<DevCommand>> batch;
    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        while (!g_cmdQueue.empty()) {
            auto c = g_cmdQueue.front();
            if (c->isShot) {
                if (!g_activeShot) {
                    g_activeShot = c;
                    g_cmdQueue.pop_front();
                }
                break; // wait for current shot to complete
            }
            if (c->cmd == "state" || c->cmd == "windows") {
                break; // defer until AfterImGuiRender for fresh geometry
            }
            batch.push_back(c);
            g_cmdQueue.pop_front();
        }
    }

    for (auto& c : batch) {
        ExecuteBeforeFrameCommand(c);
    }

    // 3. Apply simulated viewport dimensions during sim_w/sim_h shot settle frames
    std::shared_ptr<DevCommand> activeShot;
    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        activeShot = g_activeShot;
    }
    if (activeShot) {
        const int simW = ArgInt(activeShot->args, "sim_w", 0);
        const int simH = ArgInt(activeShot->args, "sim_h", 0);
        if (simW > 0 && simH > 0) {
            const float cw = static_cast<float>(std::clamp(simW, 320, 7680));
            const float ch = static_cast<float>(std::clamp(simH, 240, 4320));
            const float forcedScale = ArgFloat(activeShot->args, "sim_scale", -1.0f);
            io.DisplaySize = ImVec2(cw, ch);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            UrushiUI::SetPreviewViewport(cw, ch, forcedScale);
        } else {
            UrushiUI::ClearPreviewViewport();
        }
    } else {
        UrushiUI::ClearPreviewViewport();
    }
}

bool DevPipe::ShouldSuppressVisibleDraw() {
    std::lock_guard<std::mutex> ql(g_queueMtx);
    if (!g_activeShot) return false;
    return ArgInt(g_activeShot->args, "sim_w", 0) > 0 &&
           ArgInt(g_activeShot->args, "sim_h", 0) > 0;
}

void DevPipe::AfterImGuiRender(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context) {
    // 1. Drain any state/windows queries queued after render
    std::vector<std::shared_ptr<DevCommand>> postBatch;
    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        while (!g_cmdQueue.empty()) {
            auto c = g_cmdQueue.front();
            if (c->cmd == "state" || c->cmd == "windows") {
                postBatch.push_back(c);
                g_cmdQueue.pop_front();
            } else {
                break;
            }
        }
    }
    for (auto& c : postBatch) {
        const std::string reqId = ArgStr(c->args, "__req_id", "0");
        if (c->cmd == "state") {
            c->responseJson = MakeOkJson(reqId, SerializeStatePayload());
        } else {
            c->responseJson = MakeOkJson(reqId, "\"windows\":" + SerializeWindowsArray(UrushiUI::GetDebugSnapshot()));
        }
        std::lock_guard<std::mutex> lk(c->mtx);
        c->done = true;
        c->cv.notify_one();
    }

    // 2. Capture active shot once layout settle frames finish
    std::shared_ptr<DevCommand> shot;
    {
        std::lock_guard<std::mutex> ql(g_queueMtx);
        if (g_activeShot) {
            if (g_activeShot->shotWaitFrames > 0) {
                --g_activeShot->shotWaitFrames;
            } else {
                shot = g_activeShot;
                g_activeShot.reset();
            }
        }
    }

    if (shot) {
        const std::string reqId = ArgStr(shot->args, "__req_id", "0");
        std::vector<uint8_t> png;
        UINT w = 0, h = 0;
        std::string err;
        if (CaptureSwapChainPng(swapChain, device, context, shot->args, png, w, h, err)) {
            std::string b64 = Base64Encode(png.data(), png.size());
            std::ostringstream ss;
            ss << "\"width\":" << w << ",\"height\":" << h
               << ",\"bytes\":" << png.size()
               << ",\"mime\":\"image/png\""
               << ",\"png_b64\":\"" << b64 << "\"";
            shot->responseJson = MakeOkJson(reqId, ss.str());
        } else {
            shot->responseJson = MakeErrorJson(reqId, err);
        }
        UrushiUI::ClearPreviewViewport();
        std::lock_guard<std::mutex> lk(shot->mtx);
        shot->done = true;
        shot->cv.notify_one();
    }
}
