#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

class PatternScanner {
public:
    struct SectionInfo {
        uintptr_t start = 0;
        size_t size = 0;
        bool valid = false;
    };

    // Get all executable sections for a loaded module (e.g. .text, il2cpp)
    static std::vector<SectionInfo> GetCodeSections(HMODULE hMod);

    // Find first occurrence of IDA-style pattern across all executable sections
    // (e.g. "48 89 5C 24 ? 57 48 83 EC")
    // Returns VA (virtual address), or 0 if not found.
    static uintptr_t Find(HMODULE hMod, const char* idaPattern, const char* name = nullptr);

    // Find with fallback RVA: if pattern fails, tests fallback RVA.
    static uintptr_t FindOrFallback(HMODULE hMod, const char* idaPattern, uintptr_t fallbackRva, const char* name = nullptr);

    // Resolve RIP-relative displacement: [instrAddr + ripOffset] -> target VA
    // instrLen is the full instruction length (e.g. 7 for "mov rax, [rip + disp32]")
    // ripOffset is offset of the 32-bit displacement within the instruction (e.g. 3)
    static uintptr_t ResolveRip(uintptr_t instrAddr, int instrLen, int ripOffset);

    // One-call helper: find pattern and resolve RIP-relative target.
    // instrOffsetInPattern is offset of the RIP instruction within the matched pattern.
    static uintptr_t FindRip(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, const char* name = nullptr);
    static uintptr_t FindRip(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, size_t instrOffsetInPattern, const char* name = nullptr);

    // Find RIP with fallback RVA for the resolved target
    static uintptr_t FindRipOrFallback(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, uintptr_t fallbackTargetRva, const char* name = nullptr);
    static uintptr_t FindRipOrFallback(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, uintptr_t fallbackTargetRva, size_t instrOffsetInPattern, const char* name = nullptr);

    // Find a unique pattern within a function body, then walk backwards to the function entry point
    // (stops after previous function's 0xCC padding or 0xC3 ret).
    static uintptr_t FindFunctionEntry(HMODULE hMod, const char* bodyPattern, size_t maxBacktrack = 256, const char* name = nullptr);

    // Find function entry with fallback RVA
    static uintptr_t FindFunctionEntryOrFallback(HMODULE hMod, const char* bodyPattern, uintptr_t fallbackRva, size_t maxBacktrack = 256, const char* name = nullptr);

    // Find all occurrences of pattern across executable sections
    static std::vector<uintptr_t> FindAll(HMODULE hMod, const char* idaPattern, size_t maxMatches = 100);

    // Count occurrences of pattern (stops counting after stopAt to save time)
    static size_t CountMatches(HMODULE hMod, const char* idaPattern, size_t stopAt = 100);

    // Cached pattern find (transparent O(1) for repeated scans in game loop)
    static uintptr_t FindCached(HMODULE hMod, const char* idaPattern, const char* name = nullptr);
    static void ClearCache();
};
