import pefile
import capstone
import re

old_ga_path = r'C:\Users\garagoro\.gemini\antigravity\scratch\carx2_backup_2026-11-11\GameAssembly.dll'
new_ga_path = r'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2\GameAssembly.dll'

old_pe = pefile.PE(old_ga_path, fast_load=True)
old_data = old_pe.get_memory_mapped_image()

new_pe = pefile.PE(new_ga_path, fast_load=True)
new_data = new_pe.get_memory_mapped_image()

exec_secs = []
for s in new_pe.sections:
    if s.Characteristics & 0x20000000:
        exec_secs.append((s.VirtualAddress, s.Misc_VirtualSize))

cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
cs.detail = True

def build_pattern_from_func(data, rva, min_len=24, max_len=64):
    code = data[rva:rva+max_len]
    pat_bytes = []
    pat_mask = []
    for ins in cs.disasm(code, rva):
        is_rip_rel = False
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                is_rip_rel = True
        ins_bytes = list(ins.bytes)
        if is_rip_rel or ins.mnemonic.startswith('call') or ins.mnemonic.startswith('j'):
            if len(ins_bytes) >= 5:
                mask = [True] * (len(ins_bytes) - 4) + [False] * 4
            else:
                mask = [False] * len(ins_bytes)
        else:
            mask = [True] * len(ins_bytes)
        pat_bytes.extend(ins_bytes)
        pat_mask.extend(mask)
        if len(pat_bytes) >= min_len:
            break
    return pat_bytes, pat_mask

def pattern_to_regex(pat_bytes, pat_mask):
    re_parts = []
    i = 0
    while i < len(pat_bytes):
        if pat_mask[i]:
            re_parts.append(re.escape(bytes([pat_bytes[i]])))
            i += 1
        else:
            wc = 0
            while i < len(pat_bytes) and not pat_mask[i]:
                wc += 1
                i += 1
            re_parts.append(b'.' * wc)
    return b''.join(re_parts)

def pattern_to_ida(pat_bytes, pat_mask):
    return ' '.join(f'{b:02X}' if m else '?' for b, m in zip(pat_bytes, pat_mask))

funcs = {
    'SwitchingUpdate': 0xE4FA70,
    'IsFarCar': 0xE4F880,
    'ProviderGetTransform': 0xE57BA0,
    'TransformGetPosInjected': 0x5F72710,
    'Il2CppVirtualInvokeData': 0x9238B0,
    'CarBehaviourSetEffectsEnabled': 0x1E02570,
    'ApplyForceToDeformUpdate': 0xEBF340,
    'BreakPhysicsJointUpdate': 0xEC2440,
    'CollisionWorld_CastRay': 0x2717C40,
    'CollisionWorld_SphereCast': 0x5B57270,
    'InitRuntimeMetadata': 0x09264A0,
    'CarRepair': 0x12FEC20,
    'VisualRepair': 0x12FF9A0,
    'FullCarRepair': 0x1E570D0,
    'ForceVelocityChange': 0x1DCE130,
    'ForceTransformChangeMat': 0x1DCE0B0
}

print('=== SCANNING FUNCTIONS ACROSS ALL EXECUTABLE SECTIONS ===', flush=True)
for name, old_rva in funcs.items():
    found = False
    for plen in [20, 28, 36, 48, 64, 80]:
        pb, pm = build_pattern_from_func(old_data, old_rva, min_len=plen, max_len=plen+20)
        regex_pat = pattern_to_regex(pb, pm)
        compiled = re.compile(regex_pat, re.DOTALL)
        matches = []
        for vstart, vsize in exec_secs:
            sec_bytes = new_data[vstart:vstart+vsize]
            for m in compiled.finditer(sec_bytes):
                matches.append(vstart + m.start())
        if len(matches) == 1:
            new_rva = matches[0]
            ida_sig = pattern_to_ida(pb, pm)
            print(f'[FOUND 1:1] {name}: old=0x{old_rva:X} -> new=0x{new_rva:X} (delta=+0x{new_rva-old_rva:X})', flush=True)
            print(f'   Pattern: "{ida_sig}"', flush=True)
            found = True
            break
    if not found:
        print(f'[NOT FOUND] {name} (old=0x{old_rva:X}), hits={len(matches)}', flush=True)
