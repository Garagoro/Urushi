#!/usr/bin/env python3
"""resolve_crash.py - turn the stack of a URUSHI crash report into function names.

Usage:
    python tools/resolve_crash.py urushi_crash/crash_<stamp>.txt path/to/urushi.map

The .map file must come from the SAME build as the urushi.dll that crashed (the linker writes it next to urushi.dll
when you build with this repo's CMake). Frames inside other modules (UnityPlayer.dll, ...) are printed unchanged.
Pure Python stdlib.
"""
import bisect
import re
import sys


def load_map(path):
    base = None
    syms = []
    with open(path, encoding="latin-1") as f:
        for line in f:
            m = re.match(r"\s*Preferred load address is ([0-9a-f]+)", line)
            if m:
                base = int(m.group(1), 16)
            m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(.*)$", line)
            if m and base is not None:
                syms.append((int(m.group(2), 16) - base, m.group(1), m.group(3).strip()))
    syms.sort()
    return syms


def resolve(syms, keys, rva):
    i = bisect.bisect_right(keys, rva) - 1
    if i < 0:
        return "?"
    off, name, obj = syms[i]
    return f"{name} +0x{rva - off:X}  [{obj}]"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    report, mp = sys.argv[1], sys.argv[2]
    syms = load_map(mp)
    keys = [s[0] for s in syms]
    pat = re.compile(r"(urushi[^\s+]*\.dll)\+0x([0-9A-Fa-f]+)")
    with open(report, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = pat.search(line)
            if m:
                print(f"{line}\n      -> {resolve(syms, keys, int(m.group(2), 16))}")
            else:
                print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
