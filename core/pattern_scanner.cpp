#include "pattern_scanner.h"
#include "logger.h"
#include <mutex>
#include <unordered_map>
#include <sstream>

namespace {

struct ParsedPattern {
    std::vector<int16_t> bytes; // -1 for wildcard, 0x00..0xFF for exact byte
    bool valid = false;
};

ParsedPattern ParseIdaPattern(const char* pattern) {
    ParsedPattern res;
    if (!pattern) return res;

    std::istringstream stream(pattern);
    std::string token;
    while (stream >> token) {
        if (token == "?" || token == "??") {
            res.bytes.push_back(-1);
        } else {
            try {
                res.bytes.push_back(static_cast<int16_t>(std::stoul(token, nullptr, 16)));
            } catch (...) {
                return ParsedPattern{};
            }
        }
    }
    res.valid = !res.bytes.empty();
    return res;
}

std::mutex s_cacheMutex;
std::unordered_map<HMODULE, std::vector<PatternScanner::SectionInfo>> s_codeSectionsCache;
std::unordered_map<std::string, uintptr_t> s_scanCache;

// Isolated SEH helper for reading PE headers
static bool SafeGetCodeSectionsFromPe(uintptr_t base, std::vector<PatternScanner::SectionInfo>* outSections) {
    if (!base || !outSections) return false;
    __try {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const auto sections = IMAGE_FIRST_SECTION(nt);
        const WORD numSections = nt->FileHeader.NumberOfSections;

        for (WORD i = 0; i < numSections; ++i) {
            const auto& sec = sections[i];
            if ((sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && sec.Misc.VirtualSize > 0) {
                PatternScanner::SectionInfo info;
                info.start = base + sec.VirtualAddress;
                info.size = sec.Misc.VirtualSize;
                info.valid = true;
                outSections->push_back(info);
            }
        }

        if (outSections->empty() && nt->OptionalHeader.SizeOfCode > 0) {
            PatternScanner::SectionInfo info;
            info.start = base + nt->OptionalHeader.BaseOfCode;
            info.size = nt->OptionalHeader.SizeOfCode;
            info.valid = true;
            outSections->push_back(info);
        }

        return !outSections->empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Isolated SEH helper for memory scanning
static const uint8_t* SafeScanRaw(const uint8_t* scanStart, const uint8_t* end,
                                  const int16_t* patBytes, size_t patLen) {
    const int16_t firstByte = patBytes[0];
    __try {
        for (const uint8_t* p = scanStart; p <= end; ++p) {
            if (firstByte != -1 && *p != static_cast<uint8_t>(firstByte)) continue;

            bool match = true;
            for (size_t i = 1; i < patLen; ++i) {
                const int16_t expected = patBytes[i];
                if (expected != -1 && p[i] != static_cast<uint8_t>(expected)) {
                    match = false;
                    break;
                }
            }
            if (match) return p;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Isolated SEH helper for scanning multiple matches without C++ objects in frame
static size_t SafeScanAllRawSEH(const uint8_t* scanStart, const uint8_t* end,
                                const int16_t* patBytes, size_t patLen,
                                uintptr_t* outMatches, size_t maxMatches) {
    const int16_t firstByte = patBytes[0];
    size_t count = 0;
    __try {
        for (const uint8_t* p = scanStart; p <= end; ++p) {
            if (firstByte != -1 && *p != static_cast<uint8_t>(firstByte)) continue;

            bool match = true;
            for (size_t i = 1; i < patLen; ++i) {
                const int16_t expected = patBytes[i];
                if (expected != -1 && p[i] != static_cast<uint8_t>(expected)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (outMatches && count < maxMatches) {
                    outMatches[count] = reinterpret_cast<uintptr_t>(p);
                }
                count++;
                if (count >= maxMatches) break;
            }
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
}

// Isolated SEH helper for walking back to function prologue
static uintptr_t SafeFindEntryBacktrack(uintptr_t matchVa, size_t maxBacktrack) {
    if (!matchVa || maxBacktrack == 0) return 0;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(matchVa);
        const uint8_t* lowerBound = p - maxBacktrack;

        // Walk backwards looking for 0xCC (int3 padding) or 0xC3 (ret)
        const uint8_t* cur = p - 1;
        while (cur > lowerBound && *cur != 0xCC && *cur != 0xC3) {
            --cur;
        }

        if (*cur == 0xCC || *cur == 0xC3) {
            const uint8_t* entry = cur + 1;
            while (entry < p && *entry == 0xCC) {
                ++entry;
            }
            return reinterpret_cast<uintptr_t>(entry);
        }

        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace

std::vector<PatternScanner::SectionInfo> PatternScanner::GetCodeSections(HMODULE hMod) {
    if (!hMod) return {};

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_codeSectionsCache.find(hMod);
        if (it != s_codeSectionsCache.end()) return it->second;
    }

    std::vector<SectionInfo> sections;
    const uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
    if (SafeGetCodeSectionsFromPe(base, &sections)) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_codeSectionsCache[hMod] = sections;
    }
    return sections;
}

uintptr_t PatternScanner::Find(HMODULE hMod, const char* idaPattern, const char* name) {
    if (!hMod || !idaPattern) return 0;

    const ParsedPattern parsed = ParseIdaPattern(idaPattern);
    if (!parsed.valid) {
        Logger::Error(std::string("PatternScanner: invalid pattern syntax for ") + (name ? name : "unknown"));
        return 0;
    }

    const auto sections = GetCodeSections(hMod);
    if (sections.empty()) {
        Logger::Error(std::string("PatternScanner: no executable sections found for ") + (name ? name : "unknown"));
        return 0;
    }

    const size_t patLen = parsed.bytes.size();
    const uintptr_t base = reinterpret_cast<uintptr_t>(hMod);

    for (const auto& sec : sections) {
        if (!sec.valid || sec.size < patLen) continue;

        const uint8_t* scanStart = reinterpret_cast<const uint8_t*>(sec.start);
        const uint8_t* end = scanStart + sec.size - patLen;
        const uint8_t* match = SafeScanRaw(scanStart, end, parsed.bytes.data(), patLen);

        if (match) {
            const uintptr_t foundVa = reinterpret_cast<uintptr_t>(match);
            if (name) {
                char buf[80];
                sprintf_s(buf, "PatternScanner: matched %s at RVA 0x%X", name, static_cast<uint32_t>(foundVa - base));
                Logger::Info(buf);
            }
            return foundVa;
        }
    }

    if (name) {
        Logger::Warn(std::string("PatternScanner: no match for ") + name);
    }
    return 0;
}

uintptr_t PatternScanner::FindOrFallback(HMODULE hMod, const char* idaPattern, uintptr_t fallbackRva, const char* name) {
    const uintptr_t va = Find(hMod, idaPattern, name);
    if (va != 0) return va;

    if (fallbackRva != 0 && hMod) {
        const uintptr_t fallbackVa = reinterpret_cast<uintptr_t>(hMod) + fallbackRva;
        if (name) {
            char buf[96];
            sprintf_s(buf, "PatternScanner: using fallback RVA 0x%X for %s", static_cast<uint32_t>(fallbackRva), name);
            Logger::Warn(buf);
        }
        return fallbackVa;
    }
    return 0;
}

uintptr_t PatternScanner::ResolveRip(uintptr_t instrAddr, int instrLen, int ripOffset) {
    if (!instrAddr || instrLen <= 0 || ripOffset < 0 || ripOffset + 4 > instrLen) return 0;
    __try {
        const int32_t disp = *reinterpret_cast<const int32_t*>(instrAddr + ripOffset);
        return instrAddr + static_cast<uintptr_t>(instrLen) + static_cast<uintptr_t>(disp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t PatternScanner::FindRip(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, const char* name) {
    return FindRip(hMod, idaPattern, instrLen, ripOffset, 0, name);
}

uintptr_t PatternScanner::FindRip(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, size_t instrOffsetInPattern, const char* name) {
    const uintptr_t matchVa = Find(hMod, idaPattern, name);
    if (!matchVa) return 0;
    return ResolveRip(matchVa + instrOffsetInPattern, instrLen, ripOffset);
}

uintptr_t PatternScanner::FindRipOrFallback(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, uintptr_t fallbackTargetRva, const char* name) {
    return FindRipOrFallback(hMod, idaPattern, instrLen, ripOffset, fallbackTargetRva, 0, name);
}

uintptr_t PatternScanner::FindRipOrFallback(HMODULE hMod, const char* idaPattern, int instrLen, int ripOffset, uintptr_t fallbackTargetRva, size_t instrOffsetInPattern, const char* name) {
    const uintptr_t targetVa = FindRip(hMod, idaPattern, instrLen, ripOffset, instrOffsetInPattern, name);
    if (targetVa != 0) return targetVa;

    if (fallbackTargetRva != 0 && hMod) {
        const uintptr_t fallbackVa = reinterpret_cast<uintptr_t>(hMod) + fallbackTargetRva;
        if (name) {
            char buf[96];
            sprintf_s(buf, "PatternScanner: using fallback target RVA 0x%X for %s", static_cast<uint32_t>(fallbackTargetRva), name);
            Logger::Warn(buf);
        }
        return fallbackVa;
    }
    return 0;
}

uintptr_t PatternScanner::FindFunctionEntry(HMODULE hMod, const char* bodyPattern, size_t maxBacktrack, const char* name) {
    const uintptr_t bodyVa = Find(hMod, bodyPattern, nullptr);
    if (!bodyVa) {
        if (name) Logger::Warn(std::string("PatternScanner: body pattern not found for ") + name);
        return 0;
    }

    const uintptr_t entryVa = SafeFindEntryBacktrack(bodyVa, maxBacktrack);
    if (entryVa && hMod) {
        if (name) {
            char buf[96];
            sprintf_s(buf, "PatternScanner: resolved %s entry at RVA 0x%X", name, static_cast<uint32_t>(entryVa - reinterpret_cast<uintptr_t>(hMod)));
            Logger::Info(buf);
        }
        return entryVa;
    }

    if (name) {
        Logger::Warn(std::string("PatternScanner: failed to backtrack entry for ") + name);
    }
    return 0;
}

uintptr_t PatternScanner::FindFunctionEntryOrFallback(HMODULE hMod, const char* bodyPattern, uintptr_t fallbackRva, size_t maxBacktrack, const char* name) {
    const uintptr_t entryVa = FindFunctionEntry(hMod, bodyPattern, maxBacktrack, name);
    if (entryVa != 0) return entryVa;

    if (fallbackRva != 0 && hMod) {
        const uintptr_t fallbackVa = reinterpret_cast<uintptr_t>(hMod) + fallbackRva;
        if (name) {
            char buf[96];
            sprintf_s(buf, "PatternScanner: using fallback entry RVA 0x%X for %s", static_cast<uint32_t>(fallbackRva), name);
            Logger::Warn(buf);
        }
        return fallbackVa;
    }
    return 0;
}

std::vector<uintptr_t> PatternScanner::FindAll(HMODULE hMod, const char* idaPattern, size_t maxMatches) {
    if (!hMod || !idaPattern || maxMatches == 0) return {};

    const ParsedPattern parsed = ParseIdaPattern(idaPattern);
    if (!parsed.valid) return {};

    const auto sections = GetCodeSections(hMod);
    if (sections.empty()) return {};

    const size_t patLen = parsed.bytes.size();
    std::vector<uintptr_t> matches;
    matches.reserve(std::min<size_t>(maxMatches, 64));

    std::vector<uintptr_t> tempBuf(maxMatches);

    for (const auto& sec : sections) {
        if (!sec.valid || sec.size < patLen) continue;

        const uint8_t* scanStart = reinterpret_cast<const uint8_t*>(sec.start);
        const uint8_t* end = scanStart + sec.size - patLen;
        const size_t needed = maxMatches - matches.size();

        size_t found = SafeScanAllRawSEH(scanStart, end, parsed.bytes.data(), patLen, tempBuf.data(), needed);
        for (size_t i = 0; i < found; ++i) {
            matches.push_back(tempBuf[i]);
        }
        if (matches.size() >= maxMatches) break;
    }

    return matches;
}

size_t PatternScanner::CountMatches(HMODULE hMod, const char* idaPattern, size_t stopAt) {
    if (!hMod || !idaPattern || stopAt == 0) return 0;

    const ParsedPattern parsed = ParseIdaPattern(idaPattern);
    if (!parsed.valid) return 0;

    const auto sections = GetCodeSections(hMod);
    if (sections.empty()) return 0;

    const size_t patLen = parsed.bytes.size();
    size_t count = 0;

    for (const auto& sec : sections) {
        if (!sec.valid || sec.size < patLen) continue;

        const uint8_t* scanStart = reinterpret_cast<const uint8_t*>(sec.start);
        const uint8_t* end = scanStart + sec.size - patLen;
        const size_t needed = stopAt - count;

        size_t found = SafeScanAllRawSEH(scanStart, end, parsed.bytes.data(), patLen, nullptr, needed);
        count += found;
        if (count >= stopAt) break;
    }

    return count;
}

uintptr_t PatternScanner::FindCached(HMODULE hMod, const char* idaPattern, const char* name) {
    if (!hMod || !idaPattern) return 0;
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(hMod)) + ":" + idaPattern;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_scanCache.find(key);
        if (it != s_scanCache.end()) return it->second;
    }

    uintptr_t va = Find(hMod, idaPattern, name);
    if (va != 0) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_scanCache[key] = va;
    }
    return va;
}

void PatternScanner::ClearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_scanCache.clear();
    s_codeSectionsCache.clear();
}
