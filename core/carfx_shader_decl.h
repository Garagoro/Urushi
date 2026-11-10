#pragma once

#include <cstddef>

namespace CarFX {

constexpr int kParams = 8;        // p0..p7 -> float4 g_fxParam[8]
constexpr int kTextures = 4;      // 0..3   -> g_fxTex0..3
constexpr int kGameInputs = 16;   // g_fxGame[4] (16 floats from car telemetry / Lua)

constexpr int kDeclNameCap = 32;
constexpr int kDeclDescCap = 96;
constexpr int kDeclMaxParams = kParams * 4;
constexpr int kDeclWarnCap = 160;
constexpr size_t kDeclMaxBytes = 1u << 20;

struct ParamDecl {
    char name[kDeclNameCap];
    char desc[kDeclDescCap];
    int slot;          // g_fxParam[slot]
    int first;         // 0=x .. 3=w
    int count;         // 1..4, first + count <= 4
    bool color;
    float lo, hi;      // lo == hi: unbounded
    float def[4];
};

struct TexDecl {
    bool declared;
    bool coverage;     // false: colour
    char name[kDeclNameCap];
    char desc[kDeclDescCap];
};

enum DeclMode : int {
    kDeclModeNone = 0,
    kDeclModeReplace = 1,
    kDeclModeOverlay = 2
};

struct ShaderDecl {
    int paramCount;
    ParamDecl param[kDeclMaxParams];
    TexDecl tex[kTextures];
    int mode;          // DeclMode
    bool stealth;      // @stealth 1: hide internal/external non-body parts (true invisible car)
    int warnings;
    int warningLine;
    char warning[kDeclWarnCap];
};

bool ParseShaderDecl(const char* text, size_t len, ShaderDecl* out);
bool ReadShaderDecl(const char* utf8Path, ShaderDecl* out);

const ParamDecl* FindParam(const ShaderDecl& d, const char* name);
void DeclDefaults(const ShaderDecl& d, float out[kParams][4]);
float ClampToDecl(const ParamDecl& p, float v);
void PutParam(const ParamDecl& p, const float* values, float out[kParams][4]);

} // namespace CarFX
