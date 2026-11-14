#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaDisasm {
public:
    static void Register(lua_State* L);

    // re.decode(address, [count = 10]) -> table of decoded instructions
    static int Decode(lua_State* L);

    // re.disasm(address, [count = 15]) -> clean text listing of disassembled instructions
    static int DisasmText(lua_State* L);

    // re.make_signature(address, [module_name], [min_len = 14], [max_len = 64]) -> pattern, match_count, byte_count
    static int MakeSignature(lua_State* L);

    // re.nop(address, [instruction_count = 1]) -> ok, total_bytes_nopped
    static int Nop(lua_State* L);

    // re.resolve_rip(address) -> target_address, target_hex
    static int ResolveRip(lua_State* L);

    // re.dump_for_ai(comp_or_obj_ptr, [opts]) -> markdown_text
    static int DumpForAI(lua_State* L);
};
