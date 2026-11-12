#include "d3d11_hook.h"
#include "input_manager.h"
#include "carfx.h"
#include "dev_pipe.h"
#include "hook_manager.h"
#include "logger.h"
#include "lua_engine.h"
#include "ui_schema.h"
#include "urushi_ui.h"
#include "updater.h"
#include "safe_mode.h"
#include "crash_handler.h"

#include <shlwapi.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef void (STDMETHODCALLTYPE* t_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* t_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

constexpr int kMaxDrawSlots = 4;
static void* s_diTargets[kMaxDrawSlots] = {};
static t_DrawIndexed s_diOrig[kMaxDrawSlots] = {};
static int s_diCount = 0;

static void* s_diiTargets[kMaxDrawSlots] = {};
static t_DrawIndexedInstanced s_diiOrig[kMaxDrawSlots] = {};
static int s_diiCount = 0;

static thread_local bool t_inDrawHook = false;
static bool s_carfxInit = false;   // CarFX is only brought up once Safe mode is left

template <int Slot>
static void STDMETHODCALLTYPE Hooked_DrawIndexed(ID3D11DeviceContext* ctx, UINT IndexCount,
                                                 UINT StartIndexLocation, INT BaseVertexLocation) {
    HookManager::ScopedHookGuard hookGuard;
    t_DrawIndexed orig = s_diOrig[Slot];
    if (!orig) return;
    if (HookManager::IsUnloading() || t_inDrawHook || !ctx || !SafeMode::IsFull()) {
        orig(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
        return;
    }

    t_inDrawHook = true;
    struct IssueCtx {
        t_DrawIndexed orig;
        ID3D11DeviceContext* ctx;
        UINT idxCount;
        UINT startIdx;
        INT baseVtx;
    } ic{ orig, ctx, IndexCount, StartIndexLocation, BaseVertexLocation };

    auto issue = [](void* p) {
        auto* d = static_cast<IssueCtx*>(p);
        d->orig(d->ctx, d->idxCount, d->startIdx, d->baseVtx);
    };

    if (!CarFX::TryHandleDraw(ctx, IndexCount, issue, &ic)) {
        orig(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
    }
    t_inDrawHook = false;
}

template <int Slot>
static void STDMETHODCALLTYPE Hooked_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation) {
    HookManager::ScopedHookGuard hookGuard;
    t_DrawIndexedInstanced orig = s_diiOrig[Slot];
    if (!orig) return;
    if (HookManager::IsUnloading() || t_inDrawHook || !ctx || !SafeMode::IsFull()) {
        orig(ctx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    t_inDrawHook = true;
    struct IssueCtx {
        t_DrawIndexedInstanced orig;
        ID3D11DeviceContext* ctx;
        UINT idxCount;
        UINT instCount;
        UINT startIdx;
        INT baseVtx;
        UINT startInst;
    } ic{ orig, ctx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation };

    auto issue = [](void* p) {
        auto* d = static_cast<IssueCtx*>(p);
        d->orig(d->ctx, d->idxCount, d->instCount, d->startIdx, d->baseVtx, d->startInst);
    };

    if (!CarFX::TryHandleDraw(ctx, IndexCountPerInstance, issue, &ic)) {
        orig(ctx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
    t_inDrawHook = false;
}

static std::wstring GetExeShadersDirectory() {
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path) + L"\\shaders";
}

D3D11Hook::t_Present D3D11Hook::s_originalPresent = nullptr;
D3D11Hook::t_ResizeBuffers D3D11Hook::s_originalResizeBuffers = nullptr;
WNDPROC D3D11Hook::s_originalWndProc = nullptr;

ID3D11Device* D3D11Hook::s_device = nullptr;
ID3D11DeviceContext* D3D11Hook::s_context = nullptr;
ID3D11RenderTargetView* D3D11Hook::s_renderTargetView = nullptr;
HWND D3D11Hook::s_hwnd = nullptr;
bool D3D11Hook::s_imguiInitialized = false;
bool D3D11Hook::s_menuOpen = true;
bool D3D11Hook::s_initialized = false;

ImFont* AppFonts::Regular = nullptr;
ImFont* AppFonts::Large = nullptr;
ImFont* AppFonts::Small = nullptr;
ImFont* AppFonts::Huge = nullptr;
ImFont* AppFonts::Mono = nullptr;

static HWND FindGameWindow() {
    HWND hwnd = FindWindowW(L"UnityWndClass", nullptr);
    if (!hwnd) {
        hwnd = GetForegroundWindow();
    }
    return hwnd;
}

static ImFont* SafeLoadFont(ImFontAtlas* atlas, const char* path, float size, const ImWchar* ranges) {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return nullptr;
    }
    ImFontConfig cfg;
    cfg.Flags |= ImFontFlags_NoLoadError;
    return atlas->AddFontFromFileTTF(path, size, &cfg, ranges);
}

bool D3D11Hook::Init() {
    if (s_initialized) return true;

    // Create a dummy DirectX 11 device and swap chain to get the virtual function table
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, TEXT("DX11DummyWnd"), nullptr };
    RegisterClassEx(&wc);
    HWND dummyHwnd = CreateWindow(TEXT("DX11DummyWnd"), TEXT(""), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyHwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &featureLevel,
        1,
        D3D11_SDK_VERSION,
        &sd,
        &dummySwapChain,
        &dummyDevice,
        nullptr,
        &dummyContext
    );

    if (FAILED(hr) || !dummySwapChain) {
        DestroyWindow(dummyHwnd);
        UnregisterClass(TEXT("DX11DummyWnd"), wc.hInstance);
        Logger::Error("Failed to create dummy D3D11 device and swap chain!");
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(dummySwapChain);
    void* presentAddress = vtbl[8];
    void* resizeBuffersAddress = vtbl[13];

    HookContextVTable(dummyContext);

    dummySwapChain->Release();
    dummyContext->Release();
    dummyDevice->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClass(TEXT("DX11DummyWnd"), wc.hInstance);

    // Install hooks on Present and ResizeBuffers
    if (!HookManager::CreateHook(presentAddress, (void*)Hooked_Present, (void**)&s_originalPresent, "IDXGISwapChain::Present")) {
        return false;
    }

    if (!HookManager::CreateHook(resizeBuffersAddress, (void*)Hooked_ResizeBuffers, (void**)&s_originalResizeBuffers, "IDXGISwapChain::ResizeBuffers")) {
        return false;
    }

    s_initialized = true;
    Logger::Info("D3D11 hooks successfully installed.");
    return true;
}

void D3D11Hook::HookContextVTable(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    void** ctxVtbl = *reinterpret_cast<void***>(ctx);
    if (!ctxVtbl) return;

    void* diAddr = ctxVtbl[12];   // ID3D11DeviceContext::DrawIndexed
    void* diiAddr = ctxVtbl[20];  // ID3D11DeviceContext::DrawIndexedInstanced

    auto alreadyHooked = [](void* addr, void* const* list, int count) {
        for (int i = 0; i < count; ++i) {
            if (list[i] == addr) return true;
        }
        return false;
    };

    if (diAddr && !alreadyHooked(diAddr, s_diTargets, s_diCount) && s_diCount < kMaxDrawSlots) {
        static const t_DrawIndexed kDiDetours[kMaxDrawSlots] = {
            &Hooked_DrawIndexed<0>, &Hooked_DrawIndexed<1>, &Hooked_DrawIndexed<2>, &Hooked_DrawIndexed<3>
        };
        const int idx = s_diCount;
        if (HookManager::CreateHook(diAddr, reinterpret_cast<void*>(kDiDetours[idx]),
                                    reinterpret_cast<void**>(&s_diOrig[idx]),
                                    "ID3D11DeviceContext::DrawIndexed#" + std::to_string(idx))) {
            s_diTargets[idx] = diAddr;
            s_diCount++;
        }
    }

    if (diiAddr && !alreadyHooked(diiAddr, s_diiTargets, s_diiCount) && s_diiCount < kMaxDrawSlots) {
        static const t_DrawIndexedInstanced kDiiDetours[kMaxDrawSlots] = {
            &Hooked_DrawIndexedInstanced<0>, &Hooked_DrawIndexedInstanced<1>, &Hooked_DrawIndexedInstanced<2>, &Hooked_DrawIndexedInstanced<3>
        };
        const int idx = s_diiCount;
        if (HookManager::CreateHook(diiAddr, reinterpret_cast<void*>(kDiiDetours[idx]),
                                    reinterpret_cast<void**>(&s_diiOrig[idx]),
                                    "ID3D11DeviceContext::DrawIndexedInstanced#" + std::to_string(idx))) {
            s_diiTargets[idx] = diiAddr;
            s_diiCount++;
        }
    }

    CarFX::SetHookedDrawFlag(s_diCount > 0 || s_diiCount > 0);
}

void D3D11Hook::DetachWndProc() {
    if (s_originalWndProc && s_hwnd) {
        if (IsWindow(s_hwnd) && GetWindowLongPtrW(s_hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(Hooked_WndProc)) {
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
        }
        s_originalWndProc = nullptr;
    }
}

void D3D11Hook::Shutdown() {
    if (!s_initialized) return;

    InputManager::Shutdown();
    DetachWndProc();
    DevPipe::ReleaseD3D11();
    if (s_carfxInit) {
        CarFX::Shutdown();
        s_carfxInit = false;
    }

    if (s_imguiInitialized) {
        UrushiUI::Shutdown();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        s_imguiInitialized = false;
    }

    if (s_renderTargetView) {
        s_renderTargetView->Release();
        s_renderTargetView = nullptr;
    }
    if (s_context) {
        s_context->Release();
        s_context = nullptr;
    }
    if (s_device) {
        s_device->Release();
        s_device = nullptr;
    }

    s_diCount = 0;
    s_diiCount = 0;
    s_initialized = false;
    Logger::Info("D3D11 hook shut down.");
}

void D3D11Hook::ApplyTheme() {
    UrushiUI::ApplyBaseTheme();
}

HRESULT WINAPI D3D11Hook::Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        return s_originalPresent(pSwapChain, SyncInterval, Flags);
    }
    try {
        if (!s_imguiInitialized && pSwapChain) {
            // Retry after a failed init: drop refs from the previous attempt instead of leaking them
            if (s_renderTargetView) { s_renderTargetView->Release(); s_renderTargetView = nullptr; }
            if (s_context) { s_context->Release(); s_context = nullptr; }
            if (s_device) { s_device->Release(); s_device = nullptr; }
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&s_device))) {
                s_device->GetImmediateContext(&s_context);

                DXGI_SWAP_CHAIN_DESC desc;
                pSwapChain->GetDesc(&desc);
                s_hwnd = desc.OutputWindow;
                if (!s_hwnd) s_hwnd = FindGameWindow();

                ID3D11Texture2D* pBackBuffer = nullptr;
                pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
                if (pBackBuffer) {
                    s_device->CreateRenderTargetView(pBackBuffer, nullptr, &s_renderTargetView);
                    pBackBuffer->Release();
                }

                // Hook window procedure for inputs
                // Never subclass twice: if a previous half-finished init already installed us,
                // s_originalWndProc would become our own proc and recurse forever.
                if (s_hwnd && !s_originalWndProc) {
                    s_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hooked_WndProc)));
                }

                // Setup Dear ImGui
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                // No ImGui keyboard/gamepad navigation: arrow keys / WASD drive the CAR. With nav enabled the arrows switched
                // ImGui into nav mode (mouse hover disabled) and WantCaptureKeyboard swallowed every key while the menu was open.
                io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
                io.IniFilename = nullptr;

                // Disable assert dialogs and popups that could crash the game
                io.ConfigErrorRecoveryEnableAssert = false;
                io.ConfigErrorRecoveryEnableTooltip = false;

                // Build full Cyrillic + Extra Symbols glyph range atlas
                static ImVector<ImWchar> s_Ranges;
                if (s_Ranges.empty()) {
                    ImFontGlyphRangesBuilder builder;
                    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
                    static const ImWchar kExtra[] = {
                        0x2010, 0x205E, // Dashes, bullets, quotes
                        0x2190, 0x21FF, // Arrows (←, ↑, →, ↓, etc.)
                        0x2200, 0x22FF, // Mathematical symbols
                        0x2500, 0x25FF, // Geometric shapes (●, ■, ►, ❚, etc.)
                        0
                    };
                    builder.AddRanges(kExtra);
                    builder.BuildRanges(&s_Ranges);
                }
                const ImWchar* cyr = s_Ranges.Data;

                // Safely load fonts from existing Windows system files
                AppFonts::Regular = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, cyr);

                AppFonts::Large   = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeuib.ttf", 20.0f, cyr);
                if (!AppFonts::Large) AppFonts::Large = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeui.ttf", 20.0f, cyr);

                AppFonts::Small   = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeui.ttf", 13.0f, cyr);

                AppFonts::Huge    = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeuib.ttf", 26.0f, cyr);
                if (!AppFonts::Huge) AppFonts::Huge = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\segoeui.ttf", 26.0f, cyr);

                AppFonts::Mono    = SafeLoadFont(io.Fonts, "C:\\Windows\\Fonts\\consola.ttf", 14.0f, cyr);

                if (!AppFonts::Regular) AppFonts::Regular = io.Fonts->AddFontDefault();
                if (!AppFonts::Large)   AppFonts::Large   = AppFonts::Regular;
                if (!AppFonts::Small)   AppFonts::Small   = AppFonts::Regular;
                if (!AppFonts::Huge)    AppFonts::Huge    = AppFonts::Large;
                if (!AppFonts::Mono)    AppFonts::Mono    = AppFonts::Regular;

                io.FontDefault = AppFonts::Regular;

                // Apply URUSHI Lacquered theme
                UrushiUI::Init();

                ImGui_ImplWin32_Init(s_hwnd);
                ImGui_ImplDX11_Init(s_device, s_context);

                s_imguiInitialized = true;
                Logger::Info("ImGui initialized in D3D11 context with full Cyrillic + Segoe UI typography.");
            }
        }

        if (s_imguiInitialized) {
            static ULONGLONG s_nextCrashTick = 0;
            const ULONGLONG nowTick = GetTickCount64();
            if (nowTick >= s_nextCrashTick) {
                s_nextCrashTick = nowTick + 2000;
                CrashHandler::Tick(); // keep our crash filter on top if somebody replaced it
            }

            // Everything below the dividing line is a "feature" and stays off in Safe mode
            const bool full = SafeMode::IsFull();
            if (full && !s_carfxInit && s_context) {
                HookContextVTable(s_context);
                CarFX::Init(s_device, s_context, GetExeShadersDirectory());
                s_carfxInit = true;
            }

            if (full) {
                InputManager::Update();

                if (s_context) {
                    HookContextVTable(s_context);
                    CarFX::OnNewFrame(s_context);
                }
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            DevPipe::BeforeNewFrame();
            UrushiUI::PrepareFrame();
            ImGui::NewFrame();

            if (full) {
                // 1. Let Lua Engine render imperative UI (HUD, graphs)
                LuaEngine::OnRender();

                // 2. Render all declarative schema panels
                UiSchema::RenderAll(s_menuOpen);

                // 3. Render floating update notification banner if a newer build is ready
                Updater::RenderUI(s_menuOpen);
            }

            // Crash prompt / Safe mode window (native, no Lua involved)
            SafeMode::RenderUI(s_menuOpen);

            ImGui::Render();
            if (!DevPipe::ShouldSuppressVisibleDraw()) {
                s_context->OMSetRenderTargets(1, &s_renderTargetView, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            DevPipe::AfterImGuiRender(pSwapChain, s_device, s_context);
        }
    }
    catch (const std::exception& e) {
        Logger::Error("Exception in Hooked_Present: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Error("Unknown exception in Hooked_Present");
    }

    return s_originalPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI D3D11Hook::Hooked_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        return s_originalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    if (s_renderTargetView) {
        if (s_context) s_context->OMSetRenderTargets(0, nullptr, nullptr);
        s_renderTargetView->Release();
        s_renderTargetView = nullptr;
    }
    DevPipe::ReleaseD3D11();

    HRESULT hr = s_originalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr) && pSwapChain && s_device) {
        ID3D11Texture2D* pBackBuffer = nullptr;
        pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
        if (pBackBuffer) {
            s_device->CreateRenderTargetView(pBackBuffer, nullptr, &s_renderTargetView);
            pBackBuffer->Release();
        }
    }

    return hr;
}

LRESULT WINAPI D3D11Hook::Hooked_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    HookManager::ScopedHookGuard hookGuard;
    WNDPROC origWndProc = s_originalWndProc;
    if (HookManager::IsUnloading()) {
        return origWndProc ? CallWindowProc(origWndProc, hWnd, uMsg, wParam, lParam)
                           : DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    // Forward key events to Lua scripts and InputManager
    const bool isRepeat = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && (lParam & (1 << 30));
    const bool typingInUi = s_imguiInitialized && ImGui::GetCurrentContext() &&
                            ImGui::GetIO().WantTextInput;
    if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && !isRepeat && !typingInUi) {
        // Don't fire script hotkeys while the user is typing into a text field,
        // and ignore OS auto-repeat (would re-trigger toggles every ~30 ms).
        InputManager::OnKeyEvent(static_cast<int>(wParam), uMsg == WM_KEYDOWN);
        LuaEngine::OnKeyEvent(static_cast<int>(wParam), uMsg == WM_KEYDOWN);
    }

    // Toggle menu on F4 or Insert (F4 arrives as WM_SYSKEYDOWN when Alt is held; Alt+F4 must still close the game)
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_F4 || wParam == VK_INSERT) {
            if (!isRepeat && !(wParam == VK_INSERT && typingInUi)) ToggleMenu();   // Insert is an editing key inside text fields
            return 0;
        }
    }

    if (s_imguiInitialized && ImGui::GetCurrentContext()) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return true;
        }

        // If ImGui is capturing keyboard/mouse (dock open or hovering a pinned interactive window), block inputs to game
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)) {
            return true;
        }
        // Keyboard goes to the game unless the user is typing into a text field. Key-ups are never swallowed,
        // otherwise a key pressed before the field got focus would stay "stuck" in the game.
        if (io.WantTextInput && (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) &&
            uMsg != WM_KEYUP && uMsg != WM_SYSKEYUP) {
            return true;
        }
    }

    if (s_originalWndProc) {
        return CallWindowProc(s_originalWndProc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool D3D11Hook::IsMenuOpen() {
    return s_menuOpen;
}

void D3D11Hook::SetMenuOpen(bool open) {
    s_menuOpen = open;
}

void D3D11Hook::ToggleMenu() {
    s_menuOpen = !s_menuOpen;
}

ID3D11Device* D3D11Hook::GetDevice() {
    return s_device;
}

ID3D11DeviceContext* D3D11Hook::GetContext() {
    return s_context;
}

HWND D3D11Hook::GetGameWindow() {
    return s_hwnd;
}
