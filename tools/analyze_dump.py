import struct
import os
import sys

def parse_dump(dmp_path):
    print("=" * 70)
    print("ANALYZING:", dmp_path)
    print("=" * 70)
    if not os.path.exists(dmp_path):
        print("File does not exist!")
        return

    with open(dmp_path, "rb") as f:
        data = f.read()

    if len(data) < 32 or data[:4] != b"MDMP":
        print("Invalid minidump signature")
        return

    num_streams, dir_rva = struct.unpack_from("<II", data, 8)
    print(f"Streams: {num_streams}, Directory RVA: {hex(dir_rva)}")

    modules = []
    ex_info = None
    threads = []
    mem_ranges = [] # list of (start, size, rva)

    for i in range(num_streams):
        st_type, st_size, st_rva = struct.unpack_from("<III", data, dir_rva + i * 12)

        if st_type == 3: # ThreadList
            th_count = struct.unpack_from("<I", data, st_rva)[0]
            for t in range(th_count):
                off = st_rva + 4 + t * 48
                tid, susp, prio_cls, prio, teb, stack_base, stack_size, stack_rva, ctx_size, ctx_rva = struct.unpack_from("<IIIIQQIIII", data, off)
                threads.append({
                    "tid": tid,
                    "teb": teb,
                    "stack_base": stack_base,
                    "stack_size": stack_size,
                    "stack_rva": stack_rva,
                    "ctx_rva": ctx_rva
                })
                mem_ranges.append((stack_base, stack_size, stack_rva))

        elif st_type == 4: # ModuleList
            mod_count = struct.unpack_from("<I", data, st_rva)[0]
            for m in range(mod_count):
                off = st_rva + 4 + m * 108
                base, size, chk, ts, name_rva = struct.unpack_from("<QIIII", data, off)
                str_len = struct.unpack_from("<I", data, name_rva)[0]
                name = data[name_rva+4 : name_rva+4+str_len].decode("utf-16le", errors="replace")
                modules.append((base, size, name))

        elif st_type == 5: # MemoryList
            mem_count = struct.unpack_from("<I", data, st_rva)[0]
            for m in range(mem_count):
                off = st_rva + 4 + m * 16
                start, size, rva = struct.unpack_from("<QII", data, off)
                mem_ranges.append((start, size, rva))

        elif st_type == 6: # ExceptionStream
            th_id = struct.unpack_from("<I", data, st_rva)[0]
            ex_code, ex_flags, ex_rec_ptr, ex_addr, num_params = struct.unpack_from("<IIQQI", data, st_rva + 8)
            params = []
            for p in range(min(num_params, 15)):
                params.append(struct.unpack_from("<Q", data, st_rva + 8 + 28 + p * 8)[0])
            ctx_size, ctx_rva = struct.unpack_from("<II", data, st_rva + 8 + 28 + 15 * 8 + 4)

            ex_info = {
                "thread_id": th_id,
                "code": ex_code,
                "flags": ex_flags,
                "addr": ex_addr,
                "params": params,
                "ctx_rva": ctx_rva,
                "ctx_size": ctx_size
            }

        elif st_type == 9: # Memory64List
            num_ranges, base_rva = struct.unpack_from("<QQ", data, st_rva)
            curr_rva = base_rva
            for m in range(num_ranges):
                start, size = struct.unpack_from("<QQ", data, st_rva + 16 + m * 16)
                mem_ranges.append((start, size, curr_rva))
                curr_rva += size

    modules.sort(key=lambda x: x[0])

    def find_module(addr):
        for b, s, n in modules:
            if b <= addr < b + s:
                return (b, s, n)
        return None

    def read_mem(addr, length):
        for start, size, rva in mem_ranges:
            if start <= addr < start + size:
                offset_in_range = addr - start
                available = size - offset_in_range
                to_read = min(length, available)
                file_off = rva + offset_in_range
                return data[file_off : file_off + to_read]
        return None

    if ex_info:
        code_str = {
            0xC0000005: "STATUS_ACCESS_VIOLATION",
            0x80000003: "STATUS_BREAKPOINT",
            0xC00000FD: "STATUS_STACK_OVERFLOW",
            0xC000001D: "STATUS_ILLEGAL_INSTRUCTION"
        }.get(ex_info["code"], hex(ex_info["code"]))

        print(f"Exception Code   : {code_str} ({hex(ex_info['code'])})")
        print(f"Exception Address: {hex(ex_info['addr'])}")
        print(f"Thread ID        : {ex_info['thread_id']}")
        if ex_info["code"] == 0xC0000005 and len(ex_info["params"]) >= 2:
            access_type = "Read" if ex_info["params"][0] == 0 else ("Write" if ex_info["params"][0] == 1 else "Execute")
            target_addr = hex(ex_info["params"][1])
            print(f"Access Violation : Attempted to {access_type} address {target_addr}")

        fault_mod = find_module(ex_info["addr"])
        if fault_mod:
            b, s, n = fault_mod
            print(f"Faulting Module  : {n}")
            print(f"Module Base      : {hex(b)}, Size: {hex(s)}, RVA: {hex(ex_info['addr'] - b)}")
        else:
            print("Faulting Address is NOT in any mapped module!")

        ctx_rva = ex_info["ctx_rva"]
        if ctx_rva and ctx_rva + 256 < len(data):
            rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
            rsp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
            rbp = struct.unpack_from("<Q", data, ctx_rva + 0xA0)[0]
            rax = struct.unpack_from("<Q", data, ctx_rva + 0x78)[0]
            rcx = struct.unpack_from("<Q", data, ctx_rva + 0x80)[0]
            rdx = struct.unpack_from("<Q", data, ctx_rva + 0x88)[0]
            rbx = struct.unpack_from("<Q", data, ctx_rva + 0x90)[0]
            rsi = struct.unpack_from("<Q", data, ctx_rva + 0xA8)[0]
            rdi = struct.unpack_from("<Q", data, ctx_rva + 0xB0)[0]
            r8  = struct.unpack_from("<Q", data, ctx_rva + 0xD0)[0]
            r9  = struct.unpack_from("<Q", data, ctx_rva + 0xD8)[0]
            r10 = struct.unpack_from("<Q", data, ctx_rva + 0xE0)[0]
            r11 = struct.unpack_from("<Q", data, ctx_rva + 0xE8)[0]
            r12 = struct.unpack_from("<Q", data, ctx_rva + 0x58)[0]
            r13 = struct.unpack_from("<Q", data, ctx_rva + 0x60)[0]
            r14 = struct.unpack_from("<Q", data, ctx_rva + 0x68)[0]
            r15 = struct.unpack_from("<Q", data, ctx_rva + 0x70)[0]

            print("\nRegisters:")
            print(f"  RIP: {hex(rip):<18} RSP: {hex(rsp):<18} RBP: {hex(rbp):<18}")
            print(f"  RAX: {hex(rax):<18} RBX: {hex(rbx):<18} RCX: {hex(rcx):<18}")
            print(f"  RDX: {hex(rdx):<18} RSI: {hex(rsi):<18} RDI: {hex(rdi):<18}")
            print(f"  R8 : {hex(r8):<18}  R9 : {hex(r9):<18}  R10: {hex(r10):<18}")
            print(f"  R11: {hex(r11):<18} R12: {hex(r12):<18} R13: {hex(r13):<18}")
            print(f"  R14: {hex(r14):<18} R15: {hex(r15):<18}")

            # Inspect instruction bytes at RIP
            code_bytes = read_mem(rip, 16)
            if code_bytes:
                print(f"Code at RIP: {code_bytes.hex(' ')}")
            else:
                print("Could not read memory at RIP (not stored in dump or invalid addr)")

            # Stack Walk / Scan
            print(f"\nStack Scan starting at RSP={hex(rsp)} (next 0x400 bytes):")
            stack_data = read_mem(rsp, 0x500)
            if stack_data:
                for off in range(0, len(stack_data) - 7, 8):
                    val = struct.unpack_from("<Q", stack_data, off)[0]
                    mod = find_module(val)
                    if mod:
                        mb, ms, mn = mod
                        short_name = os.path.basename(mn)
                        rva = val - mb
                        print(f"  [RSP+{hex(off):>4}] = {hex(val):<18} -> {short_name}+0x{rva:x}")
            else:
                print("Stack memory not found in dump!")

    print(f"\nTotal threads recorded: {len(threads)}")
    for t in threads:
        if ex_info and t["tid"] == ex_info["thread_id"]:
            print(f" * Thread TID {t['tid']} (CRASH THREAD) stack: {hex(t['stack_base'])} - {hex(t['stack_base'] + t['stack_size'])}")

if __name__ == "__main__":
    dmp1 = r"C:\Users\garagoro\AppData\Local\Temp\CarX Technologies\CarX Drift Racing Online 2\Crashes\Crash_2026-11-12_164129992\crash.dmp"
    dmp2 = r"C:\Users\garagoro\AppData\Local\CrashDumps\CarX Drift Racing Online 2.exe.17920.dmp"
    parse_dump(dmp1)
    if os.path.exists(dmp2):
        parse_dump(dmp2)
