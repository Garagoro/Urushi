import struct
import os

dmp_path = r"C:\Users\garagoro\AppData\Local\CrashDumps\CarX Drift Racing Online 2.exe.18484.dmp"

with open(dmp_path, "rb") as f:
    data = f.read()

num_streams, dir_rva = struct.unpack_from("<II", data, 8)
mem_ranges = []
modules = []

for i in range(num_streams):
    st_type, st_size, st_rva = struct.unpack_from("<III", data, dir_rva + i * 12)
    if st_type == 3: # ThreadList
        th_count = struct.unpack_from("<I", data, st_rva)[0]
        for t in range(th_count):
            off = st_rva + 4 + t * 48
            tid, susp, prio_cls, prio, teb, stack_base, stack_size, stack_rva, ctx_size, ctx_rva = struct.unpack_from("<IIIIQQIIII", data, off)
            mem_ranges.append((stack_base, stack_size, stack_rva))
    elif st_type == 4: # ModuleList
        mod_count = struct.unpack_from("<I", data, st_rva)[0]
        for m in range(mod_count):
            off = st_rva + 4 + m * 108
            base, size, chk, ts, name_rva = struct.unpack_from("<QIIII", data, off)
            str_len = struct.unpack_from("<I", data, name_rva)[0]
            name = data[name_rva+4 : name_rva+4+str_len].decode("utf-16le", errors="replace")
            modules.append((base, size, name))
    elif st_type == 9: # Memory64List
        num_ranges, base_rva = struct.unpack_from("<QQ", data, st_rva)
        curr_rva = base_rva
        for m in range(num_ranges):
            start, size = struct.unpack_from("<QQ", data, st_rva + 16 + m * 16)
            mem_ranges.append((start, size, curr_rva))
            curr_rva += size

def read_mem(addr, length):
    for start, size, rva in mem_ranges:
        if start <= addr < start + size:
            offset_in_range = addr - start
            available = size - offset_in_range
            to_read = min(length, available)
            file_off = rva + offset_in_range
            return data[file_off : file_off + to_read]
    return None

def find_module(addr):
    for b, s, n in modules:
        if b <= addr < b + s:
            return (b, s, os.path.basename(n))
    return None

rsp = 0x6d16dfe0
rbp = 0x6d16e0e0

print(f"Reading stack around RSP={hex(rsp)}, RBP={hex(rbp)}")
raw_stack = read_mem(rsp, 0x600)
if raw_stack:
    for off in range(0, len(raw_stack) - 7, 8):
        val = struct.unpack_from("<Q", raw_stack, off)[0]
        curr_addr = rsp + off
        mod = find_module(val)
        mod_str = f" -> {mod[2]}+0x{val-mod[0]:x}" if mod else ""
        print(f"  [RSP+0x{off:03x}] (0x{curr_addr:x}) = 0x{val:016x}{mod_str}")
else:
    print("Could not read stack memory!")
