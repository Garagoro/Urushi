#!/usr/bin/env python3
"""
Comprehensive signature scanner for CarX2ScriptHook / URUSHI.
Validates EVERY signature used in the codebase against both Release and Alpha builds.
Modules checked:
  - GameAssembly.dll
  - UnityPlayer.dll
  - NativeCarX.dll
"""

import os
import sys
import struct
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_CORE = os.path.normpath(os.path.join(HERE, "..", "core"))
sys.path.insert(0, HERE)
from ga_pe import Img

REL_DIR = r"C:\Users\garagoro\.gemini\antigravity\scratch\carx2_backup_2026-11-11"
ALP_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2"

MODULES = {
    "GameAssembly.dll": {
        "REL": os.path.join(REL_DIR, "GameAssembly.dll"),
        "ALPHA": os.path.join(ALP_DIR, "GameAssembly.dll"),
    },
    "UnityPlayer.dll": {
        "REL": os.path.join(REL_DIR, "UnityPlayer.dll"),
        "ALPHA": os.path.join(ALP_DIR, "UnityPlayer.dll"),
    },
    "NativeCarX.dll": {
        "REL": os.path.join(REL_DIR, "CarX Drift Racing Online 2_Data", "Plugins", "x86_64", "NativeCarX.dll"),
        "ALPHA": os.path.join(ALP_DIR, "CarX Drift Racing Online 2_Data", "Plugins", "x86_64", "NativeCarX.dll"),
    }
}

# (module, name, pattern, kind, (expected_rel_rva, expected_alpha_rva), flags)
SIGS = [
    # =========================================================================
    # UnityPlayer.dll (core/unity_bridge.cpp)
    # =========================================================================
    ("UnityPlayer.dll", "Find",
     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 80 39",
     "direct", (0x669CE0, 0x669CE0), ""),

    ("UnityPlayer.dll", "CloneSingle",
     "40 53 48 83 EC 20 E8 ? ? ? ? 48 8B D8 48 85 C0 74 20 48 8B 40 18 48 85 C0 75 17 48 8B D3 48 8D 4C 24 30 E8 ? ? ? ? 48 8B 43 18 48 83 C4 20 5B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC 40 53 48 83 EC 20 8B 12 48 8B D9",
     "direct", (0xC4230, 0xC4230), ""),

    ("UnityPlayer.dll", "Destroy",
     "40 53 48 83 EC 30 33 DB 0F 29 74 24 20 0F 28 F1 48 85 C9",
     "direct", (0xC3A30, 0xC3A30), ""),

    ("UnityPlayer.dll", "SetActive",
     "40 57 48 83 EC 20 48 8B F9 84 D2 74 0A 48 83 C4 20 5F E9",
     "direct", (0xBDFC0, 0xBDFC0), ""),

    ("UnityPlayer.dll", "SetLayer",
     "4C 8B DC 57 48 81 EC B0 00 00 00 48 8B F9 39 51",
     "direct", (0x2F9270, 0x2F9270), ""),

    ("UnityPlayer.dll", "GameObject_get_transform",
     "40 53 48 83 EC 20 48 8B 41 20 48 8B 58 08 48 83 7B 18 00",
     "direct", (0xBDEC0, 0xBDEC0), ""),

    ("UnityPlayer.dll", "Transform_get_rotation",
     "40 53 48 83 EC 30 48 83 79 38 00 48 8B DA 75 19 8B 41 08 89 44 24 28 48 8D 41 28 48 89 44 24 20 0F 10 44 24 20 0F 11 41 38 4C 8D 41 28 48",
     "direct", (0xCCB40, 0xCCB40), ""),

    ("UnityPlayer.dll", "Transform_pos_anchor",
     "88 81 F2 00 00 00 C3 CC CC CC CC E9 ? ? ? ? CC CC CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 30 48 8B DA 48 8D 54 24 20",
     "rva_add:27", (0xCCA20, 0xCCA20), ""),

    ("UnityPlayer.dll", "GetMainCameraNative",
     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 0D ? ? ? ? BA 05 00 00 00",
     "direct", (0x592900, 0x592900), ""),

    ("UnityPlayer.dll", "InternalRaycast",
     "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56 48 81 EC A0 00 00 00 48 8B 3D ? ? ? ? 49 8B D9 0F 29 70 E8 48 8B F2 0F 28 F2",
     "direct", (0x6B6140, 0x6B6140), ""),

    ("UnityPlayer.dll", "ExecutePlayerLoop",
     "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 4C 8B F1",
     "direct", (0x5AC580, 0x5AC580), ""),

    ("UnityPlayer.dll", "rootLoop",
     "48 89 35 ? ? ? ? 48 89 3D ? ? ? ? EB 07 48 8B 35",
     "rip:0:7:3", (0x2232F50, 0x2232F50), ""),

    ("UnityPlayer.dll", "InstanceIDToObject",
     "40 56 48 83 EC 20 83 39 00 48 8B F1 75 08 33 C0",
     "direct", (0x107480, 0x107480), ""),

    ("UnityPlayer.dll", "MatGetRenderQueue",
     "48 83 EC 28 83 79 2C 00 7C ? 8B 41 2C 48 83 C4 28 C3",
     "direct", (0x95EB0, 0x95EB0), ""),

    ("UnityPlayer.dll", "MatSetRenderQueue",
     "40 57 48 83 EC 20 48 8B F9 39 51 2C 74 ? 80 3D ? ? ? ? 00 89 51 2C",
     "direct", (0x95EF0, 0x95EF0), ""),

    ("UnityPlayer.dll", "MatSetShaderPassEnabled",
     "40 53 57 48 83 EC 68 48 8B F9 C6 44 24 30 00",
     "direct", (0x98530, 0x98530), ""),

    ("UnityPlayer.dll", "GetOrAssignInstantiatedMat",
     "48 89 5C 24 10 48 89 6C 24 20 44 88 44 24 18 56 41 56 41 57",
     "direct", (0x47AA00, 0x47AA00), ""),

    ("UnityPlayer.dll", "RendererSetMat0",
     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B F9 48 8B F2",
     "direct", (0x8CE50, 0x8CE50), ""),

    ("UnityPlayer.dll", "GetNativeTexturePtr",
     "48 89 5C 24 08 57 48 83 EC 20 48 8B 99 10 01 00 00",
     "direct", (0x4824F0, 0x4824F0), ""),

    ("UnityPlayer.dll", "MatGetTexture",
     "89 54 24 10 55 56 48 8D 6C 24 B1 48 81 EC D8 00 00 00 48 8B 41 48",
     "direct", (0x603830, 0x603830), ""),

    ("UnityPlayer.dll", "MatSetTexture",
     "48 8B C4 89 50 10 55 53 56 48 8D 68 A1 48 81 EC F0 00 00 00",
     "direct", (0x602EB0, 0x602EB0), ""),

    ("UnityPlayer.dll", "GfxCopyTextureFull",
     "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00",
     "direct", (0x7E620, 0x7E620), ""),

    ("UnityPlayer.dll", "GameObjectManager",
     "B8 FF FF FF FF 48 8B 0D ? ? ? ? 8B D0",
     "rip:5:7:3", (0x2232770, 0x2232770), ""),

    ("UnityPlayer.dll", "GCHandleTarget",
     "C3 48 8B 05 ? ? ? ? 48 8B 49 18 FF D0",
     "rip:1:7:3", (0x22339F0, 0x22339F0), ""),

    # =========================================================================
    # NativeCarX.dll (core/steer_assist.cpp)
    # =========================================================================
    ("NativeCarX.dll", "SteerAssist_CenterScaleMul",
     "0F 28 C4 F3 0F 59 05 ? ? ? ? F3 41 0F 5D D0",
     "rva_add:3", (0x90DE, 0x90DE), ""),

    ("NativeCarX.dll", "SteerAssist_Mode0SlipJump",
     "F3 0F 11 8B F8 00 00 00 76 40 44 0F 2F 15",
     "rva_add:8", (0x91F9, 0x91F9), ""),

    ("NativeCarX.dll", "SteerAssist_SlipGatePosJump",
     "F3 0F 59 15 ? ? ? ? 76 1B F3 0F 10 05",
     "rva_add:8", (0x92C8, 0x92C8), ""),

    ("NativeCarX.dll", "SteerAssist_SlipGateNegJump",
     "45 0F 2F D4 73 1B F3 0F 10 05",
     "rva_add:4", (0x92E9, 0x92E9), ""),

    ("NativeCarX.dll", "SteerAssist_SteerFlipComiss",
     "0F 28 C8 0F 54 CA 0F 2F 0D ? ? ? ? 76 25",
     "rva_add:6", (0xA5C5, 0xA5C5), ""),

    ("NativeCarX.dll", "SteerAssist_UncondFlipComiss",
     "B3 01 41 88 1F 0F 2F 0D ? ? ? ? 0F B6 DB",
     "rva_add:5", (0xA5F3, 0xA5F3), ""),

    # =========================================================================
    # GameAssembly.dll (core/car_sim_optimizer.cpp, ecs_collision.cpp, unity_bridge)
    # =========================================================================
    ("GameAssembly.dll", "TypeInfoDefinitionTable",
     "48 63 C8 48 8B 05 ? ? ? ? 48 8B 04 C8 C3",
     "rip:3:7:3", (0x87EAEA0, 0x9C2F5C0), ""),

    ("GameAssembly.dll", "SetEffectsEnabled",
     "48 8B 89 68 01 00 00 48 85 C9 74 ? 45 33 C0 E8 ? ? ? ? 48 8B BF 70 01 00 00",
     "entry", (0x1E02570, 0x23D2460), ""),

    ("GameAssembly.dll", "SwitchingUpdate",
     "4C 8B 46 28 4D 85 C0 0F 84 ? ? ? ? 33 C9 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 89 44 24 50",
     "entry", (0xE4FA70, 0x13110F0), ""),

    ("GameAssembly.dll", "IsFarCar",
     "48 89 5C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC B0 01 00 00 4C 8B E9",
     "direct", (0xE50760, 0x1311E50), ""),

    ("GameAssembly.dll", "Apply[release sig]",
     "80 79 20 00 75 ? 80 79 21 00 C6 41 20 01 74 ? 48 8B 01 48 8B 90 C0 01 00 00 48 FF A0 B8 01 00 00",
     "direct", (0xEA05F0, None), ""),

    ("GameAssembly.dll", "Apply[alpha sig]",
     "80 79 20 00 75 1E 80 79 21 00 C6 41 20 01 74 14 48 8B 11 48 8B 82 B8 01 00 00",
     "direct", (None, 0x1362FD0), ""),

    ("GameAssembly.dll", "ProviderGetTransform",
     "48 83 C1 18 E8 ? ? ? ? 48 8B 7B 20 48 85 FF 74 ? 33 D2 48 8B CB E8 ? ? ? ?",
     "rip:0x17:5:1", (0xE57BA0, 0x1319350), ""),

    ("GameAssembly.dll", "TransformGetPosInj",
     "48 8B D5 E8 ? ? ? ? 48 85 DB 74 ? F2 0F 10 00",
     "rip:3:5:1", (0x5F72710, 0x6FB9930), ""),

    ("GameAssembly.dll", "VirtualInvokeData",
     "45 0F B7 C3 48 8B CB E8 ? ? ? ? 48 8B 48 08 48 8B D7 E8 ? ? ? ? 4C 8B C0 0F 28 CE",
     "rip:7:5:1", (0x9238B0, 0xA736D0), ""),

    ("GameAssembly.dll", "INetCarContainerTI",
     "48 8B 15 ? ? ? ? B9 11 00 00 00 4C 8B C3 48 8B 5C 24 30",
     "rip:0:7:3", (0x843A128, 0x9797458), ""),

    ("GameAssembly.dll", "DeformSleepTI",
     "74 ? 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 83 B9 ? ? ? ? 00 75 ? E8 ? ? ? ? 80 3D ? ? ? ? 00",
     "rip:0xE:7:3", (0x83E7018, 0x984C918), ""),

    ("GameAssembly.dll", "CastRay.mi",
     "4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE",
     "rip:0:7:3", (0x8387960, 0x9849F98), ""),

    ("GameAssembly.dll", "CastRay.fn",
     "4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE",
     "rip:0x41:5:1", (0x2717C40, 0x2F28300), ""),

    ("GameAssembly.dll", "InitRuntimeMetadata",
     "48 83 EC 28 E8 ? ? ? ? F0 83 0C 24 00 48 83 C4 28 C3 CC",
     "direct", (0x9264A0, 0xA76750), ""),

    ("GameAssembly.dll", "TypeNamesSS",
     "48 BA E6 2A 33 62 A9 C5 B6 9D 49 B8 80 32 50 01 13 1B EB 85 33 C9 E8 ? ? ? ? 48 8B 0D",
     "rip:0x1B:7:3", (0x83F57D8, 0x97DCAC8), ""),

    ("GameAssembly.dll", "ChunkMetaClass",
     "48 BA 90 60 B6 2D 2B 4E 0F E0 33 C9 E8 ? ? ? ? 48 8B 0D",
     "rip:0x11:7:3", (0x843CB48, 0x982E7E0), ""),

    ("GameAssembly.dll", "EcsClass",
     "48 8B 01 81 E2 FF 1F 00 00 8B 04 90 C3",
     "rip:0x45:7:3", (0x8428C98, 0x9770E30), ""),

    ("GameAssembly.dll", "SphereCast*",
     "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 90 00 00 00 80 3D ? ? ? ? 00 49 8B D9 0F 29 B4 24 80 00 00 00 48 8B FA 0F 28 F2 48 8B F1 75 ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? 01 48 8B 84 24 D0 00 00 00 4C 8D 4C 24 60 4C 8B 05 ? ? ? ? 48 8D 54 24 70 4C 89 44 24 40 0F 28 D6 48 8B CE",
     "direct", (0x5B57270, 0x6B97C00), ""),

    ("GameAssembly.dll", "ForceVelocityChange",
     "48 83 EC 38 4C 8B 89 48 01 00 00 4D 85 C9 ? ? F2 0F 10 02",
     "direct", (0x1DCE130, 0x239D160), ""),

    ("GameAssembly.dll", "ForceTransformChangeMat",
     "48 89 5C 24 08 57 48 83 EC 60 48 8B F9 48 8B DA 48 8B 89 40 01 00 00",
     "direct", (0x1DCE0B0, 0x239D0E0), ""),
]

def resolve_target(img, hit, kind):
    if kind == "direct":
        return hit
    elif kind == "entry":
        return img.entry(hit)
    elif kind.startswith("rva_add:"):
        add = int(kind.split(":")[1], 0)
        return hit + add
    elif kind.startswith("rip:"):
        parts = kind.split(":")
        pat_off = int(parts[1], 0)
        length = int(parts[2], 0)
        disp_off = int(parts[3], 0)
        site = hit + pat_off
        return img.rip(site, length, disp_off)
    raise ValueError(f"Unknown kind {kind}")

def audit_code_for_unlisted_patterns():
    """Scans core/*.cpp and core/*.h to detect any pattern string not covered in SIGS."""
    listed_patterns = {s[2].replace(" ", "").upper() for s in SIGS}
    unlisted = []
    
    pat_regex = re.compile(r'"(([0-9A-Fa-f\?]{2}\s+){3,}[0-9A-Fa-f\?]{2})"')
    for root, _, files in os.walk(REPO_CORE):
        for f in files:
            if not f.endswith((".cpp", ".h")): continue
            path = os.path.join(root, f)
            with open(path, "r", encoding="utf-8", errors="ignore") as fh:
                for line_idx, line in enumerate(fh, 1):
                    for match in pat_regex.finditer(line):
                        pat_str = match.group(1).replace(" ", "").upper()
                        if pat_str not in listed_patterns:
                            unlisted.append((os.path.relpath(path, REPO_CORE), line_idx, match.group(1)))
    return unlisted

def main():
    images = {}
    for mod_name, paths in MODULES.items():
        images[mod_name] = {}
        for branch, path in paths.items():
            if not os.path.isfile(path):
                print(f"[-] Missing binary: {path}")
                images[mod_name][branch] = None
            else:
                images[mod_name][branch] = Img(path)

    total_checks = 0
    failures = 0
    warnings = 0

    header = f"{'Module':<16} {'Signature Name':<30} {'REL Verdict':<24} {'ALPHA Verdict':<24}"
    print("=" * len(header))
    print(header)
    print("=" * len(header))

    current_mod = None
    for mod_name, name, pat, kind, (exp_rel, exp_alp), flags in SIGS:
        if mod_name != current_mod:
            current_mod = mod_name
            print(f"\n--- [{mod_name}] ---")

        results = []
        for branch, exp in [("REL", exp_rel), ("ALPHA", exp_alp)]:
            total_checks += 1
            img = images[mod_name][branch]
            if img is None:
                results.append("BINARY_MISSING")
                warnings += 1
                continue

            hits = img.find(pat, limit=10)
            if not hits:
                if exp is None:
                    verdict = "OK (none/expected)"
                else:
                    verdict = "DEAD (0 hits)"
                    failures += 1
            else:
                resolved = [resolve_target(img, h, kind) for h in hits]
                first = resolved[0]
                count_str = f"x{len(hits)}" if len(hits) > 1 else "1 hit"
                if exp is None:
                    verdict = f"UNEXPECTED ({count_str} @ 0x{first:X})"
                    failures += 1
                elif first == exp:
                    verdict = f"OK ({count_str} @ 0x{first:X})"
                else:
                    verdict = f"MISMATCH ({count_str} 0x{first:X}!=0x{exp:X})"
                    failures += 1
            results.append(verdict)

        print(f"{mod_name:<16} {name:<30} {results[0]:<24} {results[1]:<24}")

    # Check for unlisted patterns in source code
    unlisted = audit_code_for_unlisted_patterns()
    if unlisted:
        print("\n[!] WARNING: Unlisted patterns found in source code:")
        for rel_file, line_num, pat in unlisted:
            print(f"    {rel_file}:{line_num} -> {pat}")
    else:
        print("\n[+] Source Code Audit: 100% of pattern signatures in core/ are tracked and verified!")

    print("\n" + "=" * len(header))
    print(f"Summary: {total_checks} checks run across {len(SIGS)} signatures on BOTH branches.")
    print(f"Failures: {failures}, Warnings: {warnings}")
    print("=" * len(header))
    return failures

if __name__ == "__main__":
    sys.exit(main())
