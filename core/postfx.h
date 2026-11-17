#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include "carfx_shader_decl.h"

namespace PostFX {

// Lifecycle & Direct3D 11 Render Hook
bool Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& shadersDir);
void Shutdown();
void OnResizeBuffers();
void OnNewFrame(ID3D11DeviceContext* context);
void Render(IDXGISwapChain* swapChain, ID3D11RenderTargetView* backBufferRtv = nullptr);

// Shader Management
bool LoadShader(const std::string& filenameUtf8, std::string* outError = nullptr);
void UnloadShader();
bool ReloadShader(std::string* outError = nullptr);

// Runtime Controls
void SetEnabled(bool enabled);
bool IsEnabled();

// Parameter Controls
bool SetParamSlot(int slot, const float* values, int count);
bool SetParamByName(const char* name, const float* values, int count);
bool GetParamSlot(int slot, float out[4]);
CarFX::ShaderDecl GetCurrentDecl();

// Inspection
std::vector<std::string> ListShaders();
std::wstring GetPostShadersDirectory();

} // namespace PostFX
