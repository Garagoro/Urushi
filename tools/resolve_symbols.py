import ctypes
from ctypes import wintypes
import bisect

dbghelp = ctypes.windll.dbghelp

hProcess = ctypes.c_void_p(0x12345678)
dbghelp.SymInitializeW(hProcess, None, False)

mod_path = r'C:\Users\garagoro\.gemini\antigravity\scratch\CarX2ScriptHook\tools\UnityPlayer_Win64_player_il2cpp_x64.pdb'
dbghelp.SymLoadModuleExW.argtypes = [ctypes.c_void_p, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymLoadModuleExW(hProcess, None, mod_path, None, 0x180000000, 0x2428000, None, 0)

all_symbols = []

def enum_cb(pSymInfo, size, user_ctx):
    addr = ctypes.c_uint64.from_address(pSymInfo + 56).value
    name = ctypes.wstring_at(pSymInfo + 84)
    all_symbols.append((addr, name))
    return True

CB_TYPE = ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p)
cb = CB_TYPE(enum_cb)

dbghelp.SymEnumSymbolsW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wintypes.LPCWSTR, CB_TYPE, ctypes.c_void_p]
dbghelp.SymEnumSymbolsW.restype = wintypes.BOOL

print('Loading all symbols from PDB...')
dbghelp.SymEnumSymbolsW(hProcess, 0x180000000, '*', cb, None)
all_symbols.sort(key=lambda x: x[0])
print(f'Total symbols loaded: {len(all_symbols)}')

addrs = [x[0] for x in all_symbols]

def resolve(rva):
    target = 0x180000000 + rva
    idx = bisect.bisect_right(addrs, target) - 1
    if idx >= 0:
        sym_addr, name = all_symbols[idx]
        disp = target - sym_addr
        return f'{name} + 0x{disp:x} (sym: 0x{sym_addr:x})'
    return '<unknown>'

rvas = [
    0x6faa54, # return addr in crash.dmp (UnityPlayer.dll+0x6faa54)
    0x6faa4e, # call instruction
    0x4cddfd, # caller
    0x454a6f, # caller
    0x53d5af, # caller
    0x53d768, # RIP in CarX Drift Racing Online 2.exe.18484.dmp
    0x52fe98, # caller
    0x531377, # caller
    0x5696f3, # caller
    0x569fc0, # caller
    0x56a049, # caller
    0xa24f7d, # caller
    0x1d08e38, # pointer on stack
]

print('\n' + '=' * 80)
print('RESOLVED STACKTRACE:')
print('=' * 80)
for r in rvas:
    print(f'UnityPlayer.dll+0x{r:06x} -> {resolve(r)}')
