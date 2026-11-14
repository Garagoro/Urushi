#include "lua_disasm.h"
#include "../pattern_scanner.h"
#include "../logger.h"
#include <Zydis/Zydis.h>
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

static bool SafeReadBytes(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || reinterpret_cast<uintptr_t>(addr) < 0x10000 || !outBuf) return false;
        memcpy(outBuf, addr, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uintptr_t GetAddr(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return reinterpret_cast<uintptr_t>(lua_touserdata(L, idx));
    }
    if (lua_isinteger(L, idx)) {
        return static_cast<uintptr_t>(lua_tointeger(L, idx));
    }
    if (lua_isnumber(L, idx)) {
        return static_cast<uintptr_t>(lua_tonumber(L, idx));
    }
    if (lua_isstring(L, idx)) {
        const char* s = lua_tostring(L, idx);
        if (s) {
            return static_cast<uintptr_t>(std::strtoull(s, nullptr, 0));
        }
    }
    return 0;
}

static HMODULE GetModuleFromAddress(uintptr_t addr) {
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &hMod) && hMod) {
        return hMod;
    }
    hMod = GetModuleHandleA("GameAssembly.dll");
    if (!hMod) hMod = GetModuleHandleA(nullptr);
    return hMod;
}

static std::string FormatPattern(const std::vector<int16_t>& bytes) {
    std::ostringstream ss;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) ss << " ";
        if (bytes[i] < 0) {
            ss << "?";
        } else {
            ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (bytes[i] & 0xFF);
        }
    }
    return ss.str();
}

static bool SetClipboardTextWin32(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hGlob) {
        CloseClipboard();
        return false;
    }
    char* dst = static_cast<char*>(GlobalLock(hGlob));
    if (dst) {
        memcpy(dst, text.c_str(), text.size() + 1);
        GlobalUnlock(hGlob);
        SetClipboardData(CF_TEXT, hGlob);
    }
    CloseClipboard();
    return true;
}

static int Lua_CopyToClipboard(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    bool ok = SetClipboardTextWin32(str ? str : "");
    lua_pushboolean(L, ok);
    return 1;
}

static const luaL_Reg kReFuncs[] = {
    { "decode",            LuaDisasm::Decode },
    { "disasm",            LuaDisasm::DisasmText },
    { "make_signature",    LuaDisasm::MakeSignature },
    { "nop",               LuaDisasm::Nop },
    { "resolve_rip",       LuaDisasm::ResolveRip },
    { "copy_to_clipboard", Lua_CopyToClipboard },
    { nullptr, nullptr }
};

void LuaDisasm::Register(lua_State* L) {
    luaL_newlib(L, kReFuncs);
    lua_setglobal(L, "re");

    // Backward compatibility alias: disasm -> re
    lua_getglobal(L, "re");
    lua_setglobal(L, "disasm");
}

int LuaDisasm::Decode(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_newtable(L);
        return 1;
    }

    int count = static_cast<int>(luaL_optinteger(L, 2, 10));
    if (count <= 0) count = 1;
    if (count > 256) count = 256;

    HMODULE hMod = GetModuleFromAddress(addr);
    uintptr_t modBase = reinterpret_cast<uintptr_t>(hMod);

    lua_createtable(L, count, 0);

    uintptr_t curAddr = addr;
    int tableIdx = 1;

    for (int i = 0; i < count; ++i) {
        uint8_t codeBuf[16];
        if (!SafeReadBytes(reinterpret_cast<void*>(curAddr), codeBuf, sizeof(codeBuf))) {
            break;
        }

        ZydisDisassembledInstruction insn;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, curAddr, codeBuf, sizeof(codeBuf), &insn))) {
            break;
        }

        lua_createtable(L, 0, 10);

        lua_pushinteger(L, static_cast<lua_Integer>(insn.runtime_address));
        lua_setfield(L, -2, "address");

        char hexBuf[32];
        sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(insn.runtime_address));
        lua_pushstring(L, hexBuf);
        lua_setfield(L, -2, "address_hex");

        if (modBase && insn.runtime_address >= modBase) {
            uintptr_t rva = insn.runtime_address - modBase;
            lua_pushinteger(L, static_cast<lua_Integer>(rva));
            lua_setfield(L, -2, "rva");

            sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(rva));
            lua_pushstring(L, hexBuf);
            lua_setfield(L, -2, "rva_hex");
        }

        lua_pushinteger(L, insn.info.length);
        lua_setfield(L, -2, "length");

        std::string byteStr;
        for (ZyanU8 b = 0; b < insn.info.length; ++b) {
            if (b > 0) byteStr += " ";
            char bHex[4];
            sprintf_s(bHex, "%02X", codeBuf[b]);
            byteStr += bHex;
        }
        lua_pushstring(L, byteStr.c_str());
        lua_setfield(L, -2, "bytes");

        const char* mnem = ZydisMnemonicGetString(insn.info.mnemonic);
        lua_pushstring(L, mnem ? mnem : "unknown");
        lua_setfield(L, -2, "mnemonic");

        lua_pushstring(L, insn.text);
        lua_setfield(L, -2, "text");

        bool isRelative = false;
        ZyanU64 targetAddr = 0;

        for (ZyanU8 opIdx = 0; opIdx < insn.info.operand_count_visible; ++opIdx) {
            const auto& op = insn.operands[opIdx];
            if ((op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) ||
                (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative)) {
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn.info, &op, curAddr, &targetAddr))) {
                    isRelative = true;
                    break;
                }
            }
        }

        lua_pushboolean(L, isRelative);
        lua_setfield(L, -2, "is_relative");

        if (isRelative) {
            lua_pushinteger(L, static_cast<lua_Integer>(targetAddr));
            lua_setfield(L, -2, "target");

            sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(targetAddr));
            lua_pushstring(L, hexBuf);
            lua_setfield(L, -2, "target_hex");
        }

        lua_rawseti(L, -2, tableIdx++);
        curAddr += insn.info.length;
    }

    return 1;
}

int LuaDisasm::DisasmText(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushstring(L, "");
        return 1;
    }

    int count = static_cast<int>(luaL_optinteger(L, 2, 15));
    if (count <= 0) count = 1;
    if (count > 256) count = 256;

    HMODULE hMod = GetModuleFromAddress(addr);
    uintptr_t modBase = reinterpret_cast<uintptr_t>(hMod);

    std::ostringstream ss;
    uintptr_t curAddr = addr;

    for (int i = 0; i < count; ++i) {
        uint8_t codeBuf[16];
        if (!SafeReadBytes(reinterpret_cast<void*>(curAddr), codeBuf, sizeof(codeBuf))) {
            break;
        }

        ZydisDisassembledInstruction insn;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, curAddr, codeBuf, sizeof(codeBuf), &insn))) {
            break;
        }

        char hexAddr[32];
        if (modBase && curAddr >= modBase) {
            sprintf_s(hexAddr, "+0x%06llX", static_cast<unsigned long long>(curAddr - modBase));
        } else {
            sprintf_s(hexAddr, "0x%llX", static_cast<unsigned long long>(curAddr));
        }

        std::string byteStr;
        for (ZyanU8 b = 0; b < insn.info.length; ++b) {
            if (b > 0) byteStr += " ";
            char bHex[4];
            sprintf_s(bHex, "%02X", codeBuf[b]);
            byteStr += bHex;
        }

        char line[256];
        sprintf_s(line, "  %-12s | %-24s | %s", hexAddr, byteStr.c_str(), insn.text);
        ss << line;

        for (ZyanU8 opIdx = 0; opIdx < insn.info.operand_count_visible; ++opIdx) {
            const auto& op = insn.operands[opIdx];
            if ((op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) ||
                (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative)) {
                ZyanU64 target = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn.info, &op, curAddr, &target))) {
                    char tBuf[48];
                    sprintf_s(tBuf, " -> 0x%llX", static_cast<unsigned long long>(target));
                    ss << tBuf;
                    break;
                }
            }
        }
        ss << "\n";
        curAddr += insn.info.length;
    }

    std::string res = ss.str();
    lua_pushstring(L, res.c_str());
    return 1;
}

int LuaDisasm::MakeSignature(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid address");
        return 2;
    }

    HMODULE hMod = nullptr;
    if (lua_isstring(L, 2) && lua_tostring(L, 2)[0] != '\0') {
        hMod = GetModuleHandleA(lua_tostring(L, 2));
    }
    if (!hMod) {
        hMod = GetModuleFromAddress(addr);
    }
    if (!hMod) {
        lua_pushnil(L);
        lua_pushstring(L, "module not found");
        return 2;
    }

    size_t minLen = static_cast<size_t>(luaL_optinteger(L, 3, 14));
    size_t maxLen = static_cast<size_t>(luaL_optinteger(L, 4, 64));
    if (minLen < 4) minLen = 4;
    if (maxLen < minLen) maxLen = minLen;
    if (maxLen > 256) maxLen = 256;

    std::vector<int16_t> sigBytes;
    uintptr_t curAddr = addr;
    size_t lastMatchCount = 0;
    std::string finalPattern;

    while (sigBytes.size() < maxLen) {
        uint8_t codeBuf[16];
        if (!SafeReadBytes(reinterpret_cast<void*>(curAddr), codeBuf, sizeof(codeBuf))) {
            break;
        }

        ZydisDisassembledInstruction insn;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, curAddr, codeBuf, sizeof(codeBuf), &insn))) {
            break;
        }

        bool isWildcard[16] = { false };
        for (ZyanU8 opIdx = 0; opIdx < insn.info.operand_count_visible; ++opIdx) {
            const auto& op = insn.operands[opIdx];
            if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                if (op.mem.base == ZYDIS_REGISTER_RIP && op.mem.disp.size > 0) {
                    ZyanU8 off = op.mem.disp.offset;
                    ZyanU8 sz = op.mem.disp.size / 8;
                    for (ZyanU8 b = 0; b < sz && (off + b) < insn.info.length; ++b) {
                        isWildcard[off + b] = true;
                    }
                }
            } else if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                if (op.imm.is_relative && op.imm.size > 0) {
                    ZyanU8 off = op.imm.offset;
                    ZyanU8 sz = op.imm.size / 8;
                    for (ZyanU8 b = 0; b < sz && (off + b) < insn.info.length; ++b) {
                        isWildcard[off + b] = true;
                    }
                }
            }
        }

        for (ZyanU8 b = 0; b < insn.info.length; ++b) {
            if (isWildcard[b]) {
                sigBytes.push_back(-1);
            } else {
                sigBytes.push_back(static_cast<int16_t>(codeBuf[b]));
            }
        }

        curAddr += insn.info.length;

        if (sigBytes.size() >= minLen) {
            finalPattern = FormatPattern(sigBytes);
            lastMatchCount = PatternScanner::CountMatches(hMod, finalPattern.c_str(), 2);
            if (lastMatchCount == 1) {
                break;
            }
        }
    }

    if (finalPattern.empty()) {
        finalPattern = FormatPattern(sigBytes);
        lastMatchCount = PatternScanner::CountMatches(hMod, finalPattern.c_str(), 2);
    }

    lua_pushstring(L, finalPattern.c_str());
    lua_pushinteger(L, static_cast<lua_Integer>(lastMatchCount));
    lua_pushinteger(L, static_cast<lua_Integer>(sigBytes.size()));
    return 3;
}

int LuaDisasm::Nop(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "invalid address");
        return 2;
    }

    int instrCount = static_cast<int>(luaL_optinteger(L, 2, 1));
    if (instrCount <= 0) instrCount = 1;
    if (instrCount > 64) instrCount = 64;

    size_t totalBytes = 0;
    uintptr_t curAddr = addr;

    for (int i = 0; i < instrCount; ++i) {
        uint8_t codeBuf[16];
        if (!SafeReadBytes(reinterpret_cast<void*>(curAddr), codeBuf, sizeof(codeBuf))) {
            break;
        }

        ZydisDisassembledInstruction insn;
        if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, curAddr, codeBuf, sizeof(codeBuf), &insn))) {
            break;
        }

        totalBytes += insn.info.length;
        curAddr += insn.info.length;
    }

    if (totalBytes == 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "failed to decode instructions at address");
        return 2;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "VirtualProtect failed");
        return 2;
    }

    memset(reinterpret_cast<void*>(addr), 0x90, totalBytes);

    DWORD temp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, oldProtect, &temp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), totalBytes);

    lua_pushboolean(L, true);
    lua_pushinteger(L, static_cast<lua_Integer>(totalBytes));
    return 2;
}

int LuaDisasm::ResolveRip(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t codeBuf[16];
    if (!SafeReadBytes(reinterpret_cast<void*>(addr), codeBuf, sizeof(codeBuf))) {
        lua_pushnil(L);
        return 1;
    }

    ZydisDisassembledInstruction insn;
    if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, addr, codeBuf, sizeof(codeBuf), &insn))) {
        lua_pushnil(L);
        return 1;
    }

    for (ZyanU8 opIdx = 0; opIdx < insn.info.operand_count_visible; ++opIdx) {
        const auto& op = insn.operands[opIdx];
        if ((op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) ||
            (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative)) {
            ZyanU64 targetAddr = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn.info, &op, addr, &targetAddr))) {
                lua_pushinteger(L, static_cast<lua_Integer>(targetAddr));
                char hexBuf[32];
                sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(targetAddr));
                lua_pushstring(L, hexBuf);
                return 2;
            }
        }
    }

    lua_pushnil(L);
    return 1;
}
