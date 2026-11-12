#pragma once
#include <windows.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>

struct ImFont;

struct AppFonts {
    static ImFont* Regular;
    static ImFont* Large;
    static ImFont* Small;
    static ImFont* Huge;
    static ImFont* Mono;
};

class D3D11Hook {
public:
    static bool Init();
    static void DetachWndProc();
    static void Shutdown();

    static bool IsMenuOpen();
    static void SetMenuOpen(bool open);
    static void ToggleMenu();

    static ID3D11Device* GetDevice();
    static ID3D11DeviceContext* GetContext();
    static HWND GetGameWindow();

    // ImGui is not thread-safe and only exists after the first Present. Lua callbacks run on the Unity main thread,
    // so scripts must never touch ImGui directly outside the render frame:
    static bool IsInImGuiFrame();   // true only on the render thread, between NewFrame and Render
    static bool AnyItemActive();    // value cached by the render thread at the end of the last frame (safe from any thread)

private:
    static void HookContextVTable(ID3D11DeviceContext* ctx);
    static HRESULT WINAPI Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    static HRESULT WINAPI Hooked_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    static LRESULT WINAPI Hooked_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    typedef HRESULT(WINAPI* t_Present)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(WINAPI* t_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    static t_Present s_originalPresent;
    static t_ResizeBuffers s_originalResizeBuffers;
    static WNDPROC s_originalWndProc;

    static ID3D11Device* s_device;
    static ID3D11DeviceContext* s_context;
    static ID3D11RenderTargetView* s_renderTargetView;
    static HWND s_hwnd;
    static bool s_imguiInitialized;
    static bool s_menuOpen;
    static bool s_initialized;
    static std::atomic<bool> s_inFrame;
    static std::atomic<bool> s_anyItemActive;
    static std::atomic<DWORD> s_frameThread;

    static void ApplyTheme();
};
