import pefile
import capstone

old_ga_path = r'C:\Users\garagoro\.gemini\antigravity\scratch\carx2_backup_2026-11-11\GameAssembly.dll'
old_pe = pefile.PE(old_ga_path, fast_load=True)
old_data = old_pe.get_memory_mapped_image()

cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def dump_func(name, rva, length=60):
    print(f'=== {name} (0x{rva:X}) ===')
    code = old_data[rva:rva+length]
    for ins in cs.disasm(code, rva):
        print(f'{ins.address:X}: {ins.mnemonic:10s} {ins.op_str:30s} [{bytes(ins.bytes).hex()}]')

dump_func('ApplyForceToDeformUpdate', 0xEBF340, 50)
dump_func('BreakPhysicsJointUpdate', 0xEC2440, 50)
dump_func('CarBehaviourSetEffectsEnabled', 0x1E02570, 70)
dump_func('CarRepair', 0x12FEC20, 60)
dump_func('VisualRepair', 0x12FF9A0, 60)
dump_func('FullCarRepair', 0x1E570D0, 60)
dump_func('CollisionWorld_CastRay', 0x2717C40, 70)
dump_func('CollisionWorld_SphereCast', 0x5B57270, 70)
