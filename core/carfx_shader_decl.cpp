#include "carfx_shader_decl.h"

#include <windows.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CarFX {
namespace {

constexpr size_t kLineCap = 512;
constexpr int kMaxTokens = 16;

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

void Warn(ShaderDecl* d, int line, const char* fmt, const char* arg) {
    if (d->warnings++ == 0) {
        d->warningLine = line;
        _snprintf_s(d->warning, sizeof(d->warning), _TRUNCATE, fmt, arg ? arg : "");
    }
}

bool NameOk(const char* s) {
    const size_t n = strlen(s);
    if (n == 0 || n >= static_cast<size_t>(kDeclNameCap)) return false;
    if (!(isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (size_t i = 1; i < n; ++i) {
        if (!(isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) return false;
    }
    return true;
}

bool ParseFloat(const char* s, float* out) {
    char* end = nullptr;
    const float v = strtof(s, &end);
    if (end == s || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool ParseWhere(const char* s, int* slot, int* first, int* count) {
    if ((s[0] != 'p' && s[0] != 'P') || s[1] < '0' || s[1] >= '0' + kParams) return false;
    *slot = s[1] - '0';
    if (!s[2]) {
        *first = 0;
        *count = 4;
        return true;
    }
    if (s[2] != '.' || !s[3]) return false;
    static const char kAxes[] = "xyzw";
    const char* at = strchr(kAxes, s[3]);
    if (!at) return false;
    *first = static_cast<int>(at - kAxes);
    int n = 0;
    for (const char* p = s + 3; *p; ++p, ++n) {
        if (*first + n >= 4 || *p != kAxes[*first + n]) return false;
    }
    *count = n;
    return true;
}

bool ParseRange(const char* s, float* lo, float* hi) {
    const char* dots = strstr(s, "..");
    if (!dots || dots == s) return false;
    char a[64];
    const size_t n = static_cast<size_t>(dots - s);
    if (n >= sizeof(a)) return false;
    memcpy(a, s, n);
    a[n] = 0;
    return ParseFloat(a, lo) && ParseFloat(dots + 2, hi) && *hi > *lo;
}

bool Overlaps(const ShaderDecl& d, int slot, int first, int count) {
    for (int i = 0; i < d.paramCount; ++i) {
        const ParamDecl& p = d.param[i];
        if (p.slot == slot && first < p.first + p.count && p.first < first + count) return true;
    }
    return false;
}

void LineParam(ShaderDecl* d, int line, char** tok, int n, const char* desc, bool color) {
    if (n < 3) { Warn(d, line, "%s: name and place (p0.x) expected", tok[0]); return; }
    if (d->paramCount >= kDeclMaxParams) { Warn(d, line, "%s: too many parameters", tok[1]); return; }
    if (!NameOk(tok[1])) { Warn(d, line, "\"%s\": invalid name", tok[1]); return; }
    if (FindParam(*d, tok[1])) { Warn(d, line, "\"%s\" declared twice", tok[1]); return; }

    ParamDecl p = {};
    strncpy_s(p.name, tok[1], _TRUNCATE);
    strncpy_s(p.desc, desc, _TRUNCATE);
    p.color = color;

    if (!ParseWhere(tok[2], &p.slot, &p.first, &p.count)) {
        Warn(d, line, "\"%s\": place must be p<0..7>.<xyzw run>", tok[2]);
        return;
    }
    if (color && p.count < 3) { Warn(d, line, "%s: color needs 3 or 4 components", tok[1]); return; }
    if (Overlaps(*d, p.slot, p.first, p.count)) { Warn(d, line, "%s: overlaps another param", tok[1]); return; }
    if (color) { p.lo = 0.0f; p.hi = 1.0f; }

    int i = 3;
    if (i < n && strcmp(tok[i], "=") != 0) {
        if (!ParseRange(tok[i], &p.lo, &p.hi)) { Warn(d, line, "\"%s\": range must be lo..hi", tok[i]); return; }
        ++i;
    }
    int given = 0;
    if (i < n) {
        if (strcmp(tok[i], "=") != 0) { Warn(d, line, "\"%s\": '=' expected", tok[i]); return; }
        for (++i; i < n; ++i) {
            if (given >= p.count) { Warn(d, line, "%s: too many defaults", tok[1]); return; }
            if (!ParseFloat(tok[i], &p.def[given])) { Warn(d, line, "\"%s\": not a float", tok[i]); return; }
            ++given;
        }
        if (!given) { Warn(d, line, "%s: '=' with no value", tok[1]); return; }
    }
    for (int k = given; k < p.count; ++k) p.def[k] = given ? p.def[given - 1] : 0.0f;
    for (int k = 0; k < p.count; ++k) p.def[k] = ClampToDecl(p, p.def[k]);
    d->param[d->paramCount++] = p;
}

void LineTex(ShaderDecl* d, int line, char** tok, int n, const char* desc) {
    if (n < 3) { Warn(d, line, "%s: slot and name expected", tok[0]); return; }
    const int slot = tok[1][0] - '0';
    if (tok[1][1] || slot < 0 || slot >= kTextures) { Warn(d, line, "\"%s\": texture slot is 0..3", tok[1]); return; }
    TexDecl& t = d->tex[slot];
    if (t.declared) { Warn(d, line, "texture slot %s declared twice", tok[1]); return; }
    if (!NameOk(tok[2])) { Warn(d, line, "\"%s\": invalid name", tok[2]); return; }
    bool coverage = true;
    if (n >= 4) {
        if (!_stricmp(tok[3], "colour") || !_stricmp(tok[3], "color")) coverage = false;
        else if (_stricmp(tok[3], "coverage")) { Warn(d, line, "\"%s\": coverage | colour", tok[3]); return; }
    }
    if (n > 4) { Warn(d, line, "\"%s\": unexpected token", tok[4]); return; }
    t.declared = true;
    t.coverage = coverage;
    strncpy_s(t.name, tok[2], _TRUNCATE);
    strncpy_s(t.desc, desc, _TRUNCATE);
}

void LineMode(ShaderDecl* d, int line, char** tok, int n) {
    if (n != 2) { Warn(d, line, "%s: replace | overlay", tok[0]); return; }
    if (!_stricmp(tok[1], "replace")) d->mode = kDeclModeReplace;
    else if (!_stricmp(tok[1], "overlay")) d->mode = kDeclModeOverlay;
    else Warn(d, line, "\"%s\": replace | overlay", tok[1]);
}

void LineStealth(ShaderDecl* d, int line, char** tok, int n) {
    if (n == 1) { d->stealth = true; return; }
    if (n != 2) { Warn(d, line, "%s: 0 | 1", tok[0]); return; }
    if (!_stricmp(tok[1], "1") || !_stricmp(tok[1], "true") || !_stricmp(tok[1], "on")) d->stealth = true;
    else if (!_stricmp(tok[1], "0") || !_stricmp(tok[1], "false") || !_stricmp(tok[1], "off")) d->stealth = false;
    else Warn(d, line, "\"%s\": 0 | 1", tok[1]);
}

void Line(ShaderDecl* d, int line, char* s) {
    while (IsSpace(*s)) ++s;
    if (s[0] != '/' || s[1] != '/') return;
    s += 2;
    while (IsSpace(*s)) ++s;
    if (*s != '@') return;

    const char* desc = "";
    for (char* p = s; *p; ++p) {
        if (*p == ':' && p > s && IsSpace(p[-1])) {
            *p = 0;
            desc = p + 1;
            while (IsSpace(*desc)) ++desc;
            break;
        }
    }

    char* tok[kMaxTokens];
    int n = 0;
    for (char* p = s; *p && n < kMaxTokens;) {
        while (IsSpace(*p)) *p++ = 0;
        if (!*p) break;
        tok[n++] = p;
        while (*p && !IsSpace(*p)) ++p;
    }
    if (!n) return;

    for (char* e = const_cast<char*>(desc) + strlen(desc); e > desc && IsSpace(e[-1]);) *--e = 0;
    if (!_stricmp(tok[0], "@param")) LineParam(d, line, tok, n, desc, false);
    else if (!_stricmp(tok[0], "@color") || !_stricmp(tok[0], "@colour")) LineParam(d, line, tok, n, desc, true);
    else if (!_stricmp(tok[0], "@tex")) LineTex(d, line, tok, n, desc);
    else if (!_stricmp(tok[0], "@mode")) LineMode(d, line, tok, n);
    else if (!_stricmp(tok[0], "@stealth") || !_stricmp(tok[0], "@hideInternals") || !_stricmp(tok[0], "@hide_internals")) {
        LineStealth(d, line, tok, n);
    }
    else Warn(d, line, "\"%s\": expected @param | @color | @tex | @mode | @stealth", tok[0]);
}

} // namespace

bool ParseShaderDecl(const char* text, size_t len, ShaderDecl* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text) return false;
    if (len > kDeclMaxBytes) len = kDeclMaxBytes;

    char buf[kLineCap + 1];
    int line = 1;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n' && text[j]) ++j;
        if (j - i <= kLineCap) {
            memcpy(buf, text + i, j - i);
            buf[j - i] = 0;
            Line(out, line, buf);
        }
        if (j < len && !text[j]) break;
        i = j + 1;
        ++line;
    }
    return true;
}

bool ReadShaderDecl(const char* utf8Path, ShaderDecl* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!utf8Path || !utf8Path[0]) return false;

    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, w, MAX_PATH)) return false;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0;
    const DWORD want = ok ? static_cast<DWORD>(size.QuadPart > static_cast<LONGLONG>(kDeclMaxBytes) ? kDeclMaxBytes : size.QuadPart) : 0;
    char* buf = ok ? static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, want + 1)) : nullptr;
    DWORD got = 0;
    ok = buf && ReadFile(f, buf, want, &got, nullptr) && got == want;
    CloseHandle(f);
    if (ok) {
        buf[got] = 0;
        ParseShaderDecl(buf, got, out);
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

const ParamDecl* FindParam(const ShaderDecl& d, const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < d.paramCount && i < kDeclMaxParams; ++i) {
        if (!_stricmp(d.param[i].name, name)) return &d.param[i];
    }
    return nullptr;
}

void DeclDefaults(const ShaderDecl& d, float out[kParams][4]) {
    memset(out, 0, sizeof(float) * kParams * 4);
    for (int i = 0; i < d.paramCount && i < kDeclMaxParams; ++i) {
        PutParam(d.param[i], d.param[i].def, out);
    }
}

float ClampToDecl(const ParamDecl& p, float v) {
    if (!(p.hi > p.lo)) return v;
    return v < p.lo ? p.lo : (v > p.hi ? p.hi : v);
}

void PutParam(const ParamDecl& p, const float* values, float out[kParams][4]) {
    if (!values || p.slot < 0 || p.slot >= kParams || p.first < 0 || p.count < 1 || p.first + p.count > 4) return;
    for (int k = 0; k < p.count; ++k) {
        out[p.slot][p.first + k] = values[k];
    }
}

} // namespace CarFX
