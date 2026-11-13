import pefile
import capstone

path = r'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2\UnityPlayer.dll'
with open(path, 'rb') as f:
    raw = f.read()

pe = pefile.PE(data=raw)

def rva_to_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def get_str(rva):
    off = rva_to_off(rva)
    if off is None or off >= len(raw):
        return None
    end = raw.find(b'\x00', off)
    if end != -1 and 3 <= end - off < 120:
        try:
            s = raw[off:end].decode('ascii')
            if all(32 <= ord(c) < 127 for c in s):
                return s
        except:
            pass
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

for target_rva, func_len in [(0x6fa960, 0x200), (0x4cdda0, 0x100), (0x53d500, 0x400)]:
    off = rva_to_off(target_rva)
    code = raw[off : off + func_len]
    print(f'=== Strings referenced in {hex(target_rva)} ===')
    for ins in md.disasm(code, target_rva):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp
                s = get_str(target)
                if s:
                    print(f'  0x{ins.address:x}: {ins.mnemonic} {ins.op_str} -> "{s}"')
