#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>

class DevPipe {
public:
    static bool Init(const std::wstring& exeDir);
    static void Shutdown();

    // Called from D3D11 render thread inside Hooked_Present
    static void BeforeNewFrame();
    static bool ShouldSuppressVisibleDraw();
    static void AfterImGuiRender(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);
    static void ReleaseD3D11();
};
